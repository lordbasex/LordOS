/* secctx.c — wp_security_context_v1: a sandbox engine (Flatpak 1.15 and later) hands lord-server a listening
 * socket it made for one app, with its engine name, app id and instance id, and mounts that socket in the
 * sandbox instead of the normal one. lord-server accepts on it from its event loop (non-blocking) until the
 * engine closes close_fd, and every client that connects there carries that metadata (secctx_of).
 *
 * Such a client counts as sandboxed everywhere in wl.c (its drags and clipboard file lists never hand files
 * to LordOS), and the display's global filter hides from it the manager itself (no nesting) and every
 * interface that only makes sense for a trusted client (secctx_hidden). A context created from inside a
 * context is the nested error. The manager is offered only to clients the caller says are unsandboxed
 * (wl.c: not an isolated LordOS app, same root and namespaces as lord-server), so a sandboxed app can't
 * open connections tagged as another app. Tests: tests/secctx_test.c. */
#include "secctx.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include "security-context-v1-protocol.h"

/* Interfaces a client in a security context never gets, like KWin and wlroots do for sandboxed clients.
 * lord-server offers none of them today but the manager; the rest are named so that one added later is
 * hidden from the start. zwp_keyboard_shortcuts_inhibit_manager_v1 is left out on purpose: games and
 * virtual machines in a sandbox need it, so the person's permission gates it instead. */
static const char *const hidden[] = {
    "wp_security_context_manager_v1",                   /* no nesting */
    "xwayland_shell_v1",                                /* only Xwayland, which lord-server starts */
    /* read or control other windows, the screen and the outputs */
    "ext_foreign_toplevel_list_v1", "zwlr_foreign_toplevel_manager_v1", "ext_workspace_manager_v1",
    "ext_image_copy_capture_manager_v1", "ext_output_image_capture_source_manager_v1",
    "ext_foreign_toplevel_image_capture_source_manager_v1", "zwlr_screencopy_manager_v1",
    "zwlr_export_dmabuf_manager_v1", "zwlr_gamma_control_manager_v1", "zwlr_output_manager_v1",
    "zwlr_output_power_manager_v1", "wp_drm_lease_device_v1",
    /* the clipboard without keyboard focus */
    "ext_data_control_manager_v1", "zwlr_data_control_manager_v1",
    /* make input, or read it as an input method */
    "zwp_virtual_keyboard_manager_v1", "zwlr_virtual_pointer_manager_v1", "zwp_input_method_manager_v2",
    "zwp_input_method_v1", "zwp_input_panel_v1", "ext_transient_seat_manager_v1",
    /* surfaces above every window, or the lock screen */
    "zwlr_layer_shell_v1", "ext_session_lock_manager_v1",
};

int secctx_str_ok(const char *s, size_t max) {
    if (!s || !s[0]) return 0;
    size_t n = 0;
    for (; s[n]; n++) {
        if (n + 1 >= max) return 0;
        unsigned char ch = (unsigned char)s[n];
        if (ch <= 0x20 || ch >= 0x7f) return 0;
    }
    return 1;
}

int secctx_meta_set(SecMeta *m, unsigned *set, int field, const char *s) {
    char *dst = field == SECCTX_ENGINE ? m->engine : field == SECCTX_APP_ID ? m->app_id
              : field == SECCTX_INSTANCE ? m->instance_id : NULL;
    if (!dst) return SECCTX_ERR_BAD;
    if (*set & (1u << field)) return SECCTX_ERR_SET;
    if (!secctx_str_ok(s, SECCTX_STR_MAX)) return SECCTX_ERR_BAD;
    memcpy(dst, s, strlen(s) + 1);
    *set |= 1u << field;
    return SECCTX_OK;
}

int secctx_hidden(const char *iface) {
    if (!iface) return 1;
    for (size_t i = 0; i < sizeof hidden / sizeof hidden[0]; i++)
        if (!strcmp(iface, hidden[i])) return 1;
    return 0;
}

int secctx_listen_ok(int fd) {
    int v = 0;
    socklen_t l = sizeof v;
    if (fd < 0 || getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &v, &l) != 0 || v != AF_UNIX) return 0;
    l = sizeof v;
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &v, &l) != 0 || v != SOCK_STREAM) return 0;
    l = sizeof v;
    return getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &v, &l) == 0 && v == 1;
}

/* ---- the protocol ---- */

/* A context: before commit only its resource holds it; after commit it listens until close_fd hangs up,
 * also once its creator let go of the resource or disconnected (the protocol asks for that). */
typedef struct {
    struct wl_list link;
    struct wl_resource *res;            /* NULL once destroyed */
    SecMeta meta;
    unsigned set;
    int committed;
    int listen_fd, close_fd;            /* -1 once stopped */
    struct wl_event_source *listen_ev, *close_ev;
} Ctx;
/* What a client that connected through a context carries, until it is gone */
typedef struct { struct wl_listener gone; SecMeta meta; } Tag;

static struct wl_display *sdpy;
static struct wl_list ctxs;
static int nctx;
static int (*may_create_fn)(struct wl_client *c);
static struct wl_listener display_gone;

static void tag_gone(struct wl_listener *l, void *data) {
    (void)data;
    wl_list_remove(&l->link);
    free(wl_container_of(l, (Tag *)NULL, gone));
}

const SecMeta *secctx_of(struct wl_client *c) {
    struct wl_listener *l = c ? wl_client_get_destroy_listener(c, tag_gone) : NULL;
    if (!l) return NULL;
    Tag *t = wl_container_of(l, (Tag *)NULL, gone);
    return &t->meta;
}

/* No more connections: its sources and fds go (the resource, if any, stays until the client destroys it) */
static void ctx_stop(Ctx *x) {
    if (x->listen_ev) wl_event_source_remove(x->listen_ev);
    if (x->close_ev) wl_event_source_remove(x->close_ev);
    x->listen_ev = x->close_ev = NULL;
    if (x->listen_fd >= 0) close(x->listen_fd);
    if (x->close_fd >= 0) close(x->close_fd);
    x->listen_fd = x->close_fd = -1;
}
static void ctx_free(Ctx *x) {
    ctx_stop(x);
    if (x->res) wl_resource_set_user_data(x->res, NULL);
    wl_list_remove(&x->link);
    nctx--;
    free(x);
}

static int on_close(int fd, uint32_t mask, void *data) {
    (void)fd; (void)mask;
    Ctx *x = data;
    ctx_stop(x);
    if (!x->res) ctx_free(x);
    return 0;
}

static int on_listen(int fd, uint32_t mask, void *data) {
    Ctx *x = data;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) return on_close(fd, mask, data);
    for (int n = 0; n < SECCTX_ACCEPT_BURST; n++) {
        int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            break;                      /* EAGAIN: none left (the fd is non-blocking) */
        }
        Tag *t = calloc(1, sizeof *t);
        struct wl_client *cl = t ? wl_client_create(sdpy, c) : NULL;
        if (!cl) { close(c); free(t); continue; }
        t->meta = x->meta;
        t->gone.notify = tag_gone;
        wl_client_add_destroy_listener(cl, &t->gone);
    }
    return 0;
}

static void ctx_res_gone(struct wl_resource *r) {
    Ctx *x = wl_resource_get_user_data(r);
    if (!x) return;
    x->res = NULL;
    if (!x->committed || x->listen_fd < 0) ctx_free(x);
}
static void ctx_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }

static void ctx_set(struct wl_resource *r, int field, const char *s) {
    Ctx *x = wl_resource_get_user_data(r);
    if (!x || x->committed) { wl_resource_post_error(r, WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_USED, "the security context was committed"); return; }
    int e = secctx_meta_set(&x->meta, &x->set, field, s);
    if (e == SECCTX_ERR_SET) wl_resource_post_error(r, WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_SET, "that metadata is already set");
    else if (e != SECCTX_OK) wl_resource_post_error(r, WP_SECURITY_CONTEXT_V1_ERROR_INVALID_METADATA, "metadata must be 1 to %d printable ASCII bytes", SECCTX_STR_MAX - 1);
}
static void ctx_engine(struct wl_client *c, struct wl_resource *r, const char *s) { (void)c; ctx_set(r, SECCTX_ENGINE, s); }
static void ctx_app_id(struct wl_client *c, struct wl_resource *r, const char *s) { (void)c; ctx_set(r, SECCTX_APP_ID, s); }
static void ctx_instance(struct wl_client *c, struct wl_resource *r, const char *s) { (void)c; ctx_set(r, SECCTX_INSTANCE, s); }
static void ctx_commit(struct wl_client *c, struct wl_resource *r) {
    Ctx *x = wl_resource_get_user_data(r);
    if (!x || x->committed) { wl_resource_post_error(r, WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_USED, "the security context was committed"); return; }
    x->committed = 1;
    if (x->listen_fd < 0) return;       /* the engine already hung up: nothing to accept */
    x->listen_ev = wl_event_loop_add_fd(wl_display_get_event_loop(sdpy), x->listen_fd, WL_EVENT_READABLE, on_listen, x);
    if (!x->listen_ev) { ctx_stop(x); wl_client_post_no_memory(c); }
}
static const struct wp_security_context_v1_interface ctx_impl = {
    .destroy = ctx_destroy, .set_sandbox_engine = ctx_engine, .set_app_id = ctx_app_id,
    .set_instance_id = ctx_instance, .commit = ctx_commit,
};

static void mgr_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void mgr_create(struct wl_client *c, struct wl_resource *r, uint32_t id, int32_t listen_fd, int32_t close_fd) {
    if (secctx_of(c)) {
        wl_resource_post_error(r, WP_SECURITY_CONTEXT_MANAGER_V1_ERROR_NESTED, "nested security contexts are forbidden");
        goto fail;
    }
    if (!secctx_listen_ok(listen_fd)) {
        wl_resource_post_error(r, WP_SECURITY_CONTEXT_MANAGER_V1_ERROR_INVALID_LISTEN_FD, "listen_fd is not a listening unix stream socket");
        goto fail;
    }
    int fl = fcntl(listen_fd, F_GETFL);
    if (fl < 0 || fcntl(listen_fd, F_SETFL, fl | O_NONBLOCK) < 0) {
        wl_resource_post_error(r, WP_SECURITY_CONTEXT_MANAGER_V1_ERROR_INVALID_LISTEN_FD, "listen_fd can't be made non-blocking");
        goto fail;
    }
    if (nctx >= SECCTX_MAX) { wl_client_post_no_memory(c); goto fail; }
    Ctx *x = calloc(1, sizeof *x);
    struct wl_resource *cr = x ? wl_resource_create(c, &wp_security_context_v1_interface, wl_resource_get_version(r), id) : NULL;
    if (!cr) { free(x); wl_client_post_no_memory(c); goto fail; }
    x->listen_fd = listen_fd;
    x->close_fd = close_fd;
    x->res = cr;
    wl_list_insert(&ctxs, &x->link);
    nctx++;
    wl_resource_set_implementation(cr, &ctx_impl, x, ctx_res_gone);
    /* mask 0: only the hangup (and errors), which epoll always reports; a fd epoll can't watch is refused */
    x->close_ev = wl_event_loop_add_fd(wl_display_get_event_loop(sdpy), close_fd, 0, on_close, x);
    if (!x->close_ev) wl_resource_post_error(r, WP_SECURITY_CONTEXT_MANAGER_V1_ERROR_INVALID_LISTEN_FD, "close_fd can't be polled");
    return;
fail:
    close(listen_fd);
    close(close_fd);
}
static const struct wp_security_context_manager_v1_interface mgr_impl = { .destroy = mgr_destroy, .create_listener = mgr_create };
static void bind_mgr(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wp_security_context_manager_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &mgr_impl, NULL, NULL);
}

/* Which globals a client sees: the manager only outside a context and when may_create agrees; the
 * trusted-only interfaces never inside one */
static bool global_filter(const struct wl_client *client, const struct wl_global *g, void *data) {
    (void)data;
    struct wl_client *c = (struct wl_client *)client;
    const struct wl_interface *i = wl_global_get_interface(g);
    int in_ctx = secctx_of(c) != NULL;
    if (i == &wp_security_context_manager_v1_interface) return !in_ctx && (!may_create_fn || may_create_fn(c));
    return !(in_ctx && secctx_hidden(i->name));
}

static void on_display_gone(struct wl_listener *l, void *data) {
    (void)l; (void)data;
    Ctx *x, *t;
    wl_list_for_each_safe(x, t, &ctxs, link) ctx_free(x);
    sdpy = NULL;
}

int secctx_init(struct wl_display *dpy, int (*may_create)(struct wl_client *c)) {
    sdpy = dpy;
    may_create_fn = may_create;
    wl_list_init(&ctxs);
    nctx = 0;
    if (!wl_global_create(dpy, &wp_security_context_manager_v1_interface, 1, NULL, bind_mgr)) return -1;
    wl_display_set_global_filter(dpy, global_filter, NULL);
    display_gone.notify = on_display_gone;
    wl_display_add_destroy_listener(dpy, &display_gone);
    return 0;
}
