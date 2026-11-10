/* secctx_test_client.c — the client side of tests/secctx_test.c: a sandbox engine's launcher making a security
 * context on a listening socket, an app connecting through it, and each protocol error on a connection of its
 * own. Kept apart from the server file: the client and server headers don't mix in one file. */
#include "secctx_test.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>

#include "security-context-v1-client-protocol.h"

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

static const struct wl_interface visible_iface = { SECT_VISIBLE, 1, 0, NULL, 0, NULL };

/* What a connection's registry shows */
typedef struct { uint32_t mgr, visible, hidden; } Seen;
static void reg_global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)r; (void)ver;
    Seen *s = data;
    if (!strcmp(iface, wp_security_context_manager_v1_interface.name)) s->mgr = name;
    else if (!strcmp(iface, SECT_VISIBLE)) s->visible = name;
    else if (!strcmp(iface, SECT_HIDDEN)) s->hidden = name;
}
static void reg_remove(void *data, struct wl_registry *r, uint32_t name) { (void)data; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static struct wl_registry *look(struct wl_display *d, Seen *s) {
    memset(s, 0, sizeof *s);
    struct wl_registry *r = wl_display_get_registry(d);
    wl_registry_add_listener(r, &reg_listener, s);
    CHECK(wl_display_roundtrip(d) >= 0);
    return r;
}

static struct wp_security_context_manager_v1 *manager(struct wl_display *d, struct wl_registry *r, const Seen *s) {
    (void)d;
    return s->mgr ? wl_registry_bind(r, s->mgr, &wp_security_context_manager_v1_interface, 1) : NULL;
}

/* A listening socket on an abstract address of its own; *a and *len say where */
static int listener(struct sockaddr_un *a, socklen_t *len) {
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    if (s < 0 || bind(s, (struct sockaddr *)a, sizeof(sa_family_t)) || listen(s, 8)) return -1;
    *len = sizeof *a;
    return getsockname(s, (struct sockaddr *)a, len) == 0 ? s : -1;
}

/* The error a connection died of: the interface's name and the code, or "" */
static uint32_t died_of(struct wl_display *d, const char **iface) {
    const struct wl_interface *i = NULL;
    uint32_t id = 0, code = 0;
    *iface = "";
    if (wl_display_roundtrip(d) >= 0) return 0;
    if (wl_display_get_error(d) == EPROTO) code = wl_display_get_protocol_error(d, &i, &id);
    if (i) *iface = i->name;
    return code;
}

/* A security context made on d, on a listener and a close pipe of its own; *close_w gets the pipe's write end */
static struct wp_security_context_v1 *context_on(struct wl_display *d, int *close_w) {
    Seen s;
    struct wl_registry *r = look(d, &s);
    struct wp_security_context_manager_v1 *m = manager(d, r, &s);
    struct sockaddr_un a;
    socklen_t len;
    int l = listener(&a, &len), p[2];
    CHECK(m && l >= 0 && pipe2(p, O_CLOEXEC) == 0);
    if (!m || l < 0) return NULL;
    struct wp_security_context_v1 *x = wp_security_context_manager_v1_create_listener(m, l, p[0]);
    wl_display_flush(d);
    close(l);
    close(p[0]);
    *close_w = p[1];
    return x;
}

static int connect_to(const struct sockaddr_un *a, socklen_t len) {
    int c = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (c >= 0 && connect(c, (const struct sockaddr *)a, len) == 0) return c;
    if (c >= 0) close(c);
    return -1;
}

static void main_flow(int fd) {
    struct wl_display *d = wl_display_connect_to_fd(fd);
    CHECK(d != NULL);
    if (!d) return;
    Seen s;
    struct wl_registry *r = look(d, &s);
    /* the launcher sees everything */
    CHECK(s.mgr && s.visible && s.hidden);
    if (s.visible) wl_registry_bind(r, s.visible, &visible_iface, 1);
    struct wp_security_context_manager_v1 *m = manager(d, r, &s);
    struct sockaddr_un a;
    socklen_t len = 0;
    int l = listener(&a, &len), p[2];
    CHECK(m && l >= 0 && pipe2(p, O_CLOEXEC) == 0);
    if (!m || l < 0) return;
    struct wp_security_context_v1 *x = wp_security_context_manager_v1_create_listener(m, l, p[0]);
    wp_security_context_v1_set_sandbox_engine(x, "org.flatpak");
    wp_security_context_v1_set_app_id(x, "org.example.App");
    wp_security_context_v1_set_instance_id(x, "42");
    wp_security_context_v1_commit(x);
    wp_security_context_v1_destroy(x);
    CHECK(wl_display_roundtrip(d) >= 0);
    close(l);                               /* like Flatpak: only lord-server holds it now */
    close(p[0]);
    uint32_t mgr_name = s.mgr;
    /* the launcher goes away: the context keeps listening */
    wl_display_disconnect(d);

    int c = connect_to(&a, len);
    CHECK(c >= 0);
    struct wl_display *app = c >= 0 ? wl_display_connect_to_fd(c) : NULL;
    CHECK(app != NULL);
    if (!app) return;
    r = look(app, &s);
    CHECK(s.visible);
    CHECK(!s.mgr);                          /* no nesting */
    CHECK(!s.hidden);                       /* no trusted-only interface */
    if (s.visible) wl_registry_bind(r, s.visible, &visible_iface, 1);
    CHECK(wl_display_roundtrip(app) >= 0);
    /* binding the manager by its name anyway is refused */
    wl_registry_bind(r, mgr_name, &wp_security_context_manager_v1_interface, 1);
    const char *iface;
    died_of(app, &iface);
    CHECK(wl_display_get_error(app) != 0);
    wl_display_disconnect(app);

    /* the engine hangs up: no more connections */
    close(p[1]);
    int refused = 0;
    for (int i = 0; i < 200 && !refused; i++) {
        int t = connect_to(&a, len);
        if (t < 0) refused = 1;
        else { close(t); usleep(10000); }
    }
    CHECK(refused);
}

static void bad_fd(int fd) {
    struct wl_display *d = wl_display_connect_to_fd(fd);
    Seen s;
    struct wl_registry *r = look(d, &s);
    struct wp_security_context_manager_v1 *m = manager(d, r, &s);
    int sv[2], p[2];
    CHECK(m && socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0 && pipe2(p, O_CLOEXEC) == 0);
    if (!m) return;
    wp_security_context_manager_v1_create_listener(m, sv[0], p[0]);   /* a socketpair is not a listener */
    const char *iface;
    CHECK(died_of(d, &iface) == WP_SECURITY_CONTEXT_MANAGER_V1_ERROR_INVALID_LISTEN_FD);
    CHECK(!strcmp(iface, "wp_security_context_manager_v1"));
    wl_display_disconnect(d);
}

static void denied_flow(int fd) {
    struct wl_display *d = wl_display_connect_to_fd(fd);
    Seen s;
    struct wl_registry *r = look(d, &s);
    CHECK(!s.mgr);                          /* not known to be unsandboxed: no manager */
    CHECK(s.visible && s.hidden);           /* but it is no context's client */
    if (s.visible) wl_registry_bind(r, s.visible, &visible_iface, 1);
    CHECK(wl_display_roundtrip(d) >= 0);
    wl_display_disconnect(d);
}

/* case: 0 the app id twice, 1 bad metadata, 2 a request after commit */
static void ctx_error(int fd, int which, uint32_t want) {
    struct wl_display *d = wl_display_connect_to_fd(fd);
    int w = -1;
    struct wp_security_context_v1 *x = context_on(d, &w);
    if (!x) return;
    if (which == 0) { wp_security_context_v1_set_app_id(x, "org.example.A"); wp_security_context_v1_set_app_id(x, "org.example.B"); }
    if (which == 1) wp_security_context_v1_set_instance_id(x, "4 2");
    if (which == 2) { wp_security_context_v1_commit(x); wp_security_context_v1_set_app_id(x, "org.example.A"); }
    const char *iface;
    CHECK(died_of(d, &iface) == want);
    CHECK(!strcmp(iface, "wp_security_context_v1"));
    wl_display_disconnect(d);
    close(w);
}

int sect_client(const int *fds) {
    main_flow(fds[SECT_MAIN]);
    bad_fd(fds[SECT_BAD_FD]);
    denied_flow(fds[SECT_DENIED]);
    ctx_error(fds[SECT_TWICE], 0, WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_SET);
    ctx_error(fds[SECT_BAD_META], 1, WP_SECURITY_CONTEXT_V1_ERROR_INVALID_METADATA);
    ctx_error(fds[SECT_AFTER_COMMIT], 2, WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_USED);
    fflush(stdout);
    return fails;
}
