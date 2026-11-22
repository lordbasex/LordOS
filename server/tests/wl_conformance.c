/* wl_conformance.c — the dynamic half of tools/wl-conformance.mjs (docs/WAYLAND.md, "Conformance"): a Wayland
 * client that exercises lord-server's protocols point by point and prints one line per check:
 *
 *   CHECK <global> <check-id> PASS|FAIL|SKIP <hint> <message>
 *
 * <global> is the advertised interface the check is scored under, <hint> the request, event or error it is
 * about ("wl_surface.set_buffer_scale", "xdg_surface.error.unconfigured_buffer") so the driver can point at
 * the handler's file and line. Each check that expects a protocol error runs on its own connection.
 *
 *   build/wl-conformance-client generic wl_compositor:6:-1:0 xdg_wm_base:7:0:1 ...   bind at every version
 *   build/wl-conformance-client flows [global ...]                                  the protocol-specific flows
 *   build/wl-conformance-client clip-read <mime> <bytes> <md5|->                  X11 → Wayland selections
 *   build/wl-conformance-client clip-own <bytes>                                   Wayland → X11 selections
 *
 * It is built by the driver, not by the Makefile: the driver runs wayland-scanner on every protocol XML it
 * found and generates conf_have.h (CONF_HAVE_<FILE> for each protocol present) and conf_table.c (interface
 * name → struct wl_interface, for the generic checks), so new protocols are picked up without editing this
 * file. Run it only against the headless lord-server the driver starts (never a live session). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "conf_have.h"

#ifdef CONF_HAVE_XDG_SHELL
#include "xdg-shell-client-protocol.h"
#endif
#ifdef CONF_HAVE_XDG_DECORATION_UNSTABLE_V1
#include "xdg-decoration-unstable-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_VIEWPORTER
#include "viewporter-client-protocol.h"
#endif
#ifdef CONF_HAVE_LINUX_DMABUF_V1
#include "linux-dmabuf-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_PRESENTATION_TIME
#include "presentation-time-client-protocol.h"
#endif
#ifdef CONF_HAVE_FRACTIONAL_SCALE_V1
#include "fractional-scale-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_XDG_OUTPUT_UNSTABLE_V1
#include "xdg-output-unstable-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_PRIMARY_SELECTION_UNSTABLE_V1
#include "primary-selection-unstable-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_XDG_FOREIGN_UNSTABLE_V2
#include "xdg-foreign-unstable-v2-client-protocol.h"
#endif
#ifdef CONF_HAVE_XDG_DIALOG_V1
#include "xdg-dialog-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_XDG_TOPLEVEL_ICON_V1
#include "xdg-toplevel-icon-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_EXT_IDLE_NOTIFY_V1
#include "ext-idle-notify-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_SINGLE_PIXEL_BUFFER_V1
#include "single-pixel-buffer-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_ALPHA_MODIFIER_V1
#include "alpha-modifier-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_RELATIVE_POINTER_UNSTABLE_V1
#include "relative-pointer-unstable-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_POINTER_CONSTRAINTS_UNSTABLE_V1
#include "pointer-constraints-unstable-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_CURSOR_SHAPE_V1
#include "cursor-shape-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_IDLE_INHIBIT_UNSTABLE_V1
#include "idle-inhibit-unstable-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_KEYBOARD_SHORTCUTS_INHIBIT_UNSTABLE_V1
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_XDG_ACTIVATION_V1
#include "xdg-activation-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_POINTER_GESTURES_UNSTABLE_V1
#include "pointer-gestures-unstable-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_TEXT_INPUT_UNSTABLE_V3
#include "text-input-unstable-v3-client-protocol.h"
#endif
#ifdef CONF_HAVE_SECURITY_CONTEXT_V1
#include "security-context-v1-client-protocol.h"
#endif

struct conf_iface { const char *name; const struct wl_interface *iface; };
extern const struct conf_iface conf_ifaces[];    /* conf_table.c, generated */

/* ---------------------------------------------------------------- reporting */
static const char *only[64];
static int nonly;
static int nfail;

static void report(const char *item, const char *id, const char *res, const char *hint, const char *fmt, va_list ap) {
    printf("CHECK %s %s %s %s ", item, id, res, hint && *hint ? hint : "-");
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
}
static void check(const char *item, const char *id, int ok, const char *hint, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    report(item, id, ok ? "PASS" : "FAIL", hint, fmt, ap);
    va_end(ap);
    if (!ok) nfail++;
}
static void skip(const char *item, const char *id, const char *hint, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    report(item, id, "SKIP", hint, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- connection */
typedef struct Conn Conn;
struct Conn {
    struct wl_display *d;
    struct wl_registry *reg;
    struct { uint32_t name, ver; char iface[80]; } g[128];
    int ng;
    struct wl_compositor *comp;
    struct wl_subcompositor *subc;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_output *out;
    struct wl_keyboard *kb;
    struct wl_pointer *ptr;
    uint32_t caps, kb_serial, ptr_serial, shm_formats[64];
    int nshm;
    struct wl_surface *kb_focus;
    int kb_enters, ptr_enters;
#ifdef CONF_HAVE_XDG_SHELL
    struct xdg_wm_base *wm;
#endif
};

static void reg_global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)r;
    Conn *c = data;
    if (c->ng >= (int)(sizeof c->g / sizeof c->g[0])) return;
    c->g[c->ng].name = name;
    c->g[c->ng].ver = ver;
    snprintf(c->g[c->ng].iface, sizeof c->g[c->ng].iface, "%s", iface);
    c->ng++;
}
static void reg_remove(void *data, struct wl_registry *r, uint32_t name) { (void)data; (void)r; (void)name; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static uint32_t adv(Conn *c, const char *iface) {
    for (int i = 0; i < c->ng; i++) if (!strcmp(c->g[i].iface, iface)) return c->g[i].ver;
    return 0;
}
/* binds the first global of that interface at min(advertised, ours, want); NULL if not advertised */
static void *bindv(Conn *c, const struct wl_interface *iface, uint32_t want) {
    for (int i = 0; i < c->ng; i++)
        if (!strcmp(c->g[i].iface, iface->name)) {
            uint32_t v = c->g[i].ver;
            if ((uint32_t)iface->version < v) v = (uint32_t)iface->version;
            if (want && want < v) v = want;
            return wl_registry_bind(c->reg, c->g[i].name, iface, v);
        }
    return NULL;
}

static void shm_format(void *data, struct wl_shm *s, uint32_t f) {
    (void)s;
    Conn *c = data;
    if (c->nshm < 64) c->shm_formats[c->nshm++] = f;
}
static const struct wl_shm_listener shm_l = { shm_format };

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int fd, uint32_t size) { (void)d; (void)k; (void)fmt; (void)size; close(fd); }
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys) {
    (void)k; (void)keys;
    Conn *c = d;
    c->kb_serial = serial;
    c->kb_focus = s;
    c->kb_enters++;
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) { (void)k; (void)serial; (void)s; ((Conn *)d)->kb_focus = NULL; }
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t st) { (void)k; (void)t; (void)key; (void)st; ((Conn *)d)->kb_serial = serial; }
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t a, uint32_t b, uint32_t e, uint32_t g) { (void)d; (void)k; (void)serial; (void)a; (void)b; (void)e; (void)g; }
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) { (void)d; (void)k; (void)rate; (void)delay; }
static const struct wl_keyboard_listener kb_l = { kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat };

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) { (void)p; (void)s; (void)x; (void)y; ((Conn *)d)->ptr_serial = serial; ((Conn *)d)->ptr_enters++; }
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) { (void)d; (void)p; (void)serial; (void)s; }
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)t; (void)x; (void)y; }
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t st) { (void)d; (void)p; (void)serial; (void)t; (void)b; (void)st; }
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d; (void)p; (void)t; (void)a; (void)v; }
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void p_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void p_v120(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void p_dir(void *d, struct wl_pointer *p, uint32_t a, uint32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener ptr_l = { p_enter, p_leave, p_motion, p_button, p_axis, p_frame, p_source, p_stop, p_discrete, p_v120, p_dir };

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) { (void)s; ((Conn *)d)->caps = caps; }
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_l = { seat_caps, seat_name };

#ifdef CONF_HAVE_XDG_SHELL
static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t serial) { (void)d; xdg_wm_base_pong(wm, serial); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };
#endif

static void cx_close(Conn *c) {
    if (!c) return;
    wl_display_disconnect(c->d);
    free(c);
}
static Conn *cx_open(void) {
    Conn *c = calloc(1, sizeof *c);
    c->d = wl_display_connect(NULL);
    if (!c->d) { free(c); return NULL; }
    c->reg = wl_display_get_registry(c->d);
    wl_registry_add_listener(c->reg, &reg_l, c);
    wl_display_roundtrip(c->d);
    c->comp = bindv(c, &wl_compositor_interface, 0);
    c->subc = bindv(c, &wl_subcompositor_interface, 0);
    c->shm = bindv(c, &wl_shm_interface, 1);
    if (c->shm) wl_shm_add_listener(c->shm, &shm_l, c);
    c->seat = bindv(c, &wl_seat_interface, 0);
    if (c->seat) wl_seat_add_listener(c->seat, &seat_l, c);
    c->out = bindv(c, &wl_output_interface, 0);
#ifdef CONF_HAVE_XDG_SHELL
    c->wm = bindv(c, &xdg_wm_base_interface, 0);
    if (c->wm) xdg_wm_base_add_listener(c->wm, &wm_l, c);
#endif
    wl_display_roundtrip(c->d);
    if (c->seat && (c->caps & WL_SEAT_CAPABILITY_KEYBOARD)) {
        c->kb = wl_seat_get_keyboard(c->seat);
        wl_keyboard_add_listener(c->kb, &kb_l, c);
    }
    if (c->seat && (c->caps & WL_SEAT_CAPABILITY_POINTER)) { c->ptr = wl_seat_get_pointer(c->seat); wl_pointer_add_listener(c->ptr, &ptr_l, c); }
    wl_display_roundtrip(c->d);
    return c;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
/* dispatches until *flag is non-zero or ms passed: 1 if it became set, 0 on timeout, -1 on a connection error */
static int cx_wait(Conn *c, volatile int *flag, int ms) {
    double end = now_ms() + ms;
    for (;;) {
        if (wl_display_dispatch_pending(c->d) < 0) return -1;
        if (flag && *flag) return 1;
        double left = end - now_ms();
        if (left <= 0) return 0;
        while (wl_display_prepare_read(c->d) != 0)
            if (wl_display_dispatch_pending(c->d) < 0) return -1;
        if (wl_display_flush(c->d) < 0 && errno != EAGAIN) { wl_display_cancel_read(c->d); return -1; }
        struct pollfd p = { wl_display_get_fd(c->d), POLLIN, 0 };
        int n = poll(&p, 1, (int)left + 1);
        if (n > 0) { if (wl_display_read_events(c->d) < 0) return -1; }
        else wl_display_cancel_read(c->d);
    }
}
static int cx_sync(Conn *c) { return wl_display_roundtrip(c->d) < 0 ? -1 : 0; }

/* the protocol error the connection died with, as text */
static char errbuf[256];
static const char *cx_errtext(Conn *c) {
    int e = wl_display_get_error(c->d);
    if (!e) return "no error";
    if (e != EPROTO) { snprintf(errbuf, sizeof errbuf, "connection error %s", strerror(e)); return errbuf; }
    const struct wl_interface *i = NULL;
    uint32_t id = 0;
    uint32_t code = wl_display_get_protocol_error(c->d, &i, &id);
    snprintf(errbuf, sizeof errbuf, "protocol error %s#%u code %u", i ? i->name : "?", id, code);
    return errbuf;
}
/* after a roundtrip: did the connection die with exactly that error? */
static int cx_expect(Conn *c, const struct wl_interface *iface, uint32_t code) {
    wl_display_roundtrip(c->d);
    if (wl_display_get_error(c->d) != EPROTO) return 0;
    const struct wl_interface *i = NULL;
    uint32_t id;
    uint32_t got = wl_display_get_protocol_error(c->d, &i, &id);
    return i && !strcmp(i->name, iface->name) && got == code;
}
/* the same, raised on either of two objects (an error defined on a new object is often posted on its factory) */
static int cx_expect2(Conn *c, const struct wl_interface *a, const struct wl_interface *b, uint32_t code) {
    wl_display_roundtrip(c->d);
    if (wl_display_get_error(c->d) != EPROTO) return 0;
    const struct wl_interface *i = NULL;
    uint32_t id;
    uint32_t got = wl_display_get_protocol_error(c->d, &i, &id);
    return i && (!strcmp(i->name, a->name) || !strcmp(i->name, b->name)) && got == code;
}
/* the same, for an error posted on an object the client has just destroyed (a destructor the compositor refused,
 * as defunct_role_object): libwayland-client has made the proxy a zombie and reports the error with no interface and
 * id 0, so only the code can be compared (and the interface and id, when they are known) */
static int cx_expect_id(Conn *c, const struct wl_interface *iface, uint32_t want_id, uint32_t code) {
    wl_display_roundtrip(c->d);
    if (wl_display_get_error(c->d) != EPROTO) return 0;
    const struct wl_interface *i = NULL;
    uint32_t id = 0;
    uint32_t got = wl_display_get_protocol_error(c->d, &i, &id);
    return got == code && (i ? !strcmp(i->name, iface->name) && id == want_id : 1);
}
static int cx_ok(Conn *c) { wl_display_roundtrip(c->d); return wl_display_get_error(c->d) == 0; }

/* ---------------------------------------------------------------- buffers and windows */
static int shm_fd(size_t size) {
    int fd = memfd_create("wlconf", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size)) { if (fd >= 0) close(fd); return -1; }
    return fd;
}
static int released;
static void buf_release(void *d, struct wl_buffer *b) { (void)d; (void)b; released++; }
static const struct wl_buffer_listener buf_l = { buf_release };
static struct wl_buffer *shm_buffer(Conn *c, int w, int h, uint32_t color) {
    size_t size = (size_t)w * (size_t)h * 4;
    int fd = shm_fd(size);
    if (fd < 0) return NULL;
    uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px != MAP_FAILED) { for (size_t i = 0; i < size / 4; i++) px[i] = color; munmap(px, size); }
    struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, (int32_t)size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b, &buf_l, NULL);
    return b;
}

#ifdef CONF_HAVE_XDG_SHELL
typedef struct {
    struct wl_surface *s;
    struct xdg_surface *xs;
    struct xdg_toplevel *top;
    int configured, top_configs, xs_configs, order_bad, caps_seen, bounds_seen, closed, has_acked;
    uint32_t serial, acked;     /* the last configure's serial; the last one acked (each is acked once: xdg-shell) */
    int32_t cw, ch;
    int maximized, fullscreen;
} Win;
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
    (void)xs;
    Win *w = d;
    if (w->top_configs <= w->xs_configs) w->order_bad = 1;     /* the role's configure must come first */
    w->xs_configs++;
    w->serial = serial;
    w->configured = 1;
}
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states) {
    (void)t;
    Win *win = d;
    win->top_configs++;
    win->cw = w;
    win->ch = h;
    win->maximized = win->fullscreen = 0;
    uint32_t *s;
    wl_array_for_each(s, states) {
        if (*s == XDG_TOPLEVEL_STATE_MAXIMIZED) win->maximized = 1;
        if (*s == XDG_TOPLEVEL_STATE_FULLSCREEN) win->fullscreen = 1;
    }
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)t; ((Win *)d)->closed = 1; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)t; (void)w; (void)h; ((Win *)d)->bounds_seen = 1; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *caps) { (void)t; (void)caps; ((Win *)d)->caps_seen = 1; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_bounds, top_caps };

/* a toplevel role, committed once, its first configure acked (no buffer yet); 0 on success */
static int win_role(Conn *c, Win *w, const char *title) {
    memset(w, 0, sizeof *w);
    if (!c->wm || !c->comp) return -1;
    w->s = wl_compositor_create_surface(c->comp);
    w->xs = xdg_wm_base_get_xdg_surface(c->wm, w->s);
    xdg_surface_add_listener(w->xs, &xs_l, w);
    w->top = xdg_surface_get_toplevel(w->xs);
    xdg_toplevel_add_listener(w->top, &top_l, w);
    xdg_toplevel_set_title(w->top, title);
    xdg_toplevel_set_app_id(w->top, "org.lordos.wlconformance");
    wl_surface_commit(w->s);
    if (cx_wait(c, &w->configured, 3000) != 1) return -1;
    xdg_surface_ack_configure(w->xs, w->serial);
    w->acked = w->serial;
    w->has_acked = 1;
    return 0;
}
/* ... and mapped with a w×h buffer */
static int win_map(Conn *c, Win *w, const char *title, int width, int height) {
    if (win_role(c, w, title)) return -1;
    int bw = w->cw > 0 ? w->cw : width, bh = w->ch > 0 ? w->ch : height;
    struct wl_buffer *b = shm_buffer(c, bw, bh, 0xff3366aa);
    if (!b) return -1;
    wl_surface_attach(w->s, b, 0, 0);
    wl_surface_damage_buffer(w->s, 0, 0, bw, bh);
    wl_surface_commit(w->s);
    return cx_sync(c);
}
/* acks the last configure unless it was acked already: acking a serial twice is invalid_serial (xdg-shell) */
static void win_ack_commit(Conn *c, Win *w, int width, int height) {
    if (!w->has_acked || w->acked != w->serial) {
        xdg_surface_ack_configure(w->xs, w->serial);
        w->acked = w->serial;
        w->has_acked = 1;
    }
    struct wl_buffer *b = shm_buffer(c, width, height, 0xff113355);
    wl_surface_attach(w->s, b, 0, 0);
    wl_surface_damage_buffer(w->s, 0, 0, width, height);
    wl_surface_commit(w->s);
}
#endif

/* ---------------------------------------------------------------- generic: bind at every version */
static const struct wl_interface *iface_by_name(const char *name) {
    for (int i = 0; conf_ifaces[i].name; i++) if (!strcmp(conf_ifaces[i].name, name)) return conf_ifaces[i].iface;
    return NULL;
}
static void generic(int argc, char **argv) {
    for (int a = 0; a < argc; a++) {
        char name[96];
        int ver = 0, dop = -1, dsince = 1;
        if (sscanf(argv[a], "%95[^:]:%d:%d:%d", name, &ver, &dop, &dsince) < 2) continue;
        const struct wl_interface *iface = iface_by_name(name);
        if (!iface) { skip(name, "bind", name, "no client code for this interface (its XML wasn't found)"); continue; }
        int top = ver < iface->version ? ver : iface->version;
        if (top < ver) skip(name, "bind.above", name, "advertised v%d, the XML found only goes to v%d", ver, iface->version);
        for (int v = 1; v <= top; v++) {
            char id[32];
            snprintf(id, sizeof id, "bind.v%d", v);
            Conn *c = cx_open();
            if (!c) { check(name, id, 0, name, "can't connect"); continue; }
            uint32_t gname = 0;
            for (int i = 0; i < c->ng; i++) if (!strcmp(c->g[i].iface, name)) { gname = c->g[i].name; break; }
            if (!gname) { check(name, id, 0, name, "not advertised to this client"); cx_close(c); continue; }
            struct wl_proxy *p = wl_registry_bind(c->reg, gname, iface, (uint32_t)v);
            int ok = cx_ok(c) && wl_proxy_get_version(p) == (uint32_t)v;
            check(name, id, ok, name, ok ? "bound at v%d" : "binding at v%d: %s", v, cx_errtext(c));
            if (ok && dop >= 0 && v >= dsince) {
                snprintf(id, sizeof id, "destroy.v%d", v);
                wl_proxy_marshal_flags(p, (uint32_t)dop, NULL, (uint32_t)v, WL_MARSHAL_FLAG_DESTROY);
                int ok2 = cx_ok(c);
                check(name, id, ok2, iface->methods[dop].name, ok2 ? "%s at v%d" : "%s at v%d: %s", iface->methods[dop].name, v, cx_errtext(c));
            }
            cx_close(c);
        }
    }
}

/* ---------------------------------------------------------------- flows */
#define ITEM(n) static const char *const item = n
#define OPEN_OR_SKIP(id) Conn *c = cx_open(); if (!c) { skip(item, id, item, "can't connect"); return; }

static void f_compositor(void) {
    ITEM("wl_compositor");
    {
        OPEN_OR_SKIP("surface.create");
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        struct wl_region *r = wl_compositor_create_region(c->comp);
        wl_region_add(r, 0, 0, 100, 100);
        wl_region_subtract(r, 10, 10, 20, 20);
        wl_surface_set_input_region(s, r);
        wl_surface_set_opaque_region(s, r);
        wl_region_destroy(r);
        wl_surface_commit(s);
        int ok = cx_ok(c);
        check(item, "surface.create", ok, "wl_compositor.create_surface", ok ? "surface and region requests accepted" : "%s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("surface.invalid_scale");
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        wl_surface_set_buffer_scale(s, 0);
        int ok = cx_expect(c, &wl_surface_interface, WL_SURFACE_ERROR_INVALID_SCALE);
        check(item, "surface.invalid_scale", ok, "wl_surface.error.invalid_scale", "set_buffer_scale(0): %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("surface.invalid_transform");
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        wl_surface_set_buffer_transform(s, 99);
        int ok = cx_expect(c, &wl_surface_interface, WL_SURFACE_ERROR_INVALID_TRANSFORM);
        check(item, "surface.invalid_transform", ok, "wl_surface.error.invalid_transform", "set_buffer_transform(99): %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("surface.invalid_offset");
        if (wl_proxy_get_version((struct wl_proxy *)c->comp) < 5) skip(item, "surface.invalid_offset", "wl_surface.error.invalid_offset", "needs v5");
        else {
            struct wl_surface *s = wl_compositor_create_surface(c->comp);
            struct wl_buffer *b = shm_buffer(c, 8, 8, 0);
            wl_surface_attach(s, b, 4, 4);
            int ok = cx_expect(c, &wl_surface_interface, WL_SURFACE_ERROR_INVALID_OFFSET);
            check(item, "surface.invalid_offset", ok, "wl_surface.error.invalid_offset", "attach with x,y ≠ 0 at v5+: %s", cx_errtext(c));
        }
        cx_close(c);
    }
#ifdef CONF_HAVE_XDG_SHELL
    {
        OPEN_OR_SKIP("surface.invalid_size");
        Win w;
        if (win_role(c, &w, "invalid-size")) skip(item, "surface.invalid_size", "wl_surface.error.invalid_size", "no toplevel");
        else {
            struct wl_buffer *b = shm_buffer(c, 33, 33, 0);
            wl_surface_set_buffer_scale(w.s, 2);
            wl_surface_attach(w.s, b, 0, 0);
            wl_surface_commit(w.s);
            int ok = cx_expect(c, &wl_surface_interface, WL_SURFACE_ERROR_INVALID_SIZE);
            check(item, "surface.invalid_size", ok, "wl_surface.error.invalid_size", "a 33×33 buffer at scale 2 on a toplevel: %s", cx_errtext(c));
        }
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("surface.defunct_role_object");
        Win w;
        if (wl_proxy_get_version((struct wl_proxy *)c->comp) < 6 || win_role(c, &w, "defunct")) skip(item, "surface.defunct_role_object", "wl_surface.error.defunct_role_object", "needs v6 and a toplevel");
        else {
            uint32_t sid = wl_proxy_get_id((struct wl_proxy *)w.s);
            wl_surface_destroy(w.s);
            int ok = cx_expect_id(c, &wl_surface_interface, sid, WL_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT);
            check(item, "surface.defunct_role_object", ok, "wl_surface.error.defunct_role_object", "wl_surface destroyed before its xdg_surface: %s", cx_errtext(c));
        }
        cx_close(c);
    }
#endif
}

static int cb_done;
static void cb_fire(void *d, struct wl_callback *cb, uint32_t t) { (void)d; (void)t; cb_done++; wl_callback_destroy(cb); }
static const struct wl_callback_listener cb_l = { cb_fire };

static int enters, scales;
static void srf_enter(void *d, struct wl_surface *s, struct wl_output *o) { (void)d; (void)s; (void)o; enters++; }
static void srf_leave(void *d, struct wl_surface *s, struct wl_output *o) { (void)d; (void)s; (void)o; }
static void srf_scale(void *d, struct wl_surface *s, int32_t f) { (void)d; (void)s; (void)f; scales++; }
static void srf_transform(void *d, struct wl_surface *s, uint32_t t) { (void)d; (void)s; (void)t; }
static const struct wl_surface_listener srf_l = { srf_enter, srf_leave, srf_scale, srf_transform };

#ifdef CONF_HAVE_XDG_SHELL
static void f_surface_events(void) {
    ITEM("wl_compositor");
    OPEN_OR_SKIP("surface.frame_done");
    Win w;
    memset(&w, 0, sizeof w);
    if (win_role(c, &w, "events")) { check(item, "surface.frame_done", 0, "wl_surface.frame", "no toplevel: %s", cx_errtext(c)); cx_close(c); return; }
    enters = scales = 0;
    wl_surface_add_listener(w.s, &srf_l, NULL);
    int bw = w.cw > 0 ? w.cw : 120, bh = w.ch > 0 ? w.ch : 90;
    struct wl_buffer *b = shm_buffer(c, bw, bh, 0xff3366aa);
    released = 0;
    wl_surface_attach(w.s, b, 0, 0);
    wl_surface_damage_buffer(w.s, 0, 0, bw, bh);
    cb_done = 0;
    wl_callback_add_listener(wl_surface_frame(w.s), &cb_l, NULL);
    wl_surface_commit(w.s);
    cx_wait(c, &cb_done, 2000);
    check(item, "surface.frame_done", cb_done > 0, "wl_callback.done", "frame callback on a mapped surface: %s", cb_done ? "done" : "never done in 2 s");
    /* a second buffer: the first one must be released (lord-server copies on commit) */
    struct wl_buffer *b2 = shm_buffer(c, bw, bh, 0xff553311);
    wl_surface_attach(w.s, b2, 0, 0);
    wl_surface_damage(w.s, 0, 0, bw, bh);
    wl_surface_commit(w.s);
    cx_wait(c, &released, 2000);
    check(item, "buffer.release", released > 0, "wl_buffer.release", "a replaced buffer is released: %d release(s)", released);
    cx_wait(c, &enters, 1500);
    check(item, "surface.enter", enters > 0, "wl_surface.enter", "wl_surface.enter for the output of a mapped surface: %d", enters);
    if (wl_proxy_get_version((struct wl_proxy *)w.s) >= 6) {
        cx_wait(c, &scales, 1500);
        check(item, "surface.preferred_buffer_scale", scales > 0, "wl_surface.preferred_buffer_scale", "preferred_buffer_scale sent (v6): %d", scales);
    } else skip(item, "surface.preferred_buffer_scale", "wl_surface.preferred_buffer_scale", "needs v6");
    int ok = cx_ok(c);
    check(item, "surface.damage_requests", ok, "wl_surface.damage", ok ? "damage, damage_buffer and commit accepted" : "%s", cx_errtext(c));
    cx_close(c);
}
#endif

static void f_subcompositor(void) {
    ITEM("wl_subcompositor");
    {
        OPEN_OR_SKIP("subsurface.flow");
        if (!c->subc) { skip(item, "subsurface.flow", item, "not advertised"); cx_close(c); return; }
        struct wl_surface *p = wl_compositor_create_surface(c->comp), *a = wl_compositor_create_surface(c->comp), *b = wl_compositor_create_surface(c->comp);
        struct wl_subsurface *sa = wl_subcompositor_get_subsurface(c->subc, a, p), *sb = wl_subcompositor_get_subsurface(c->subc, b, p);
        wl_subsurface_set_position(sa, 10, 10);
        wl_subsurface_place_above(sa, b);
        wl_subsurface_place_below(sb, p);
        wl_subsurface_set_sync(sa);
        wl_subsurface_set_desync(sa);
        wl_surface_commit(a);
        wl_surface_commit(p);
        wl_subsurface_destroy(sb);
        int ok = cx_ok(c);
        check(item, "subsurface.flow", ok, "wl_subcompositor.get_subsurface", ok ? "get_subsurface, set_position, place_above/below, set_sync/desync, destroy" : "%s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("subsurface.bad_parent");
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        wl_subcompositor_get_subsurface(c->subc, s, s);
        int ok = cx_expect(c, &wl_subcompositor_interface, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT);
        check(item, "subsurface.bad_parent", ok, "wl_subcompositor.error.bad_parent", "a surface as its own parent: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("subsurface.bad_surface");
        struct wl_surface *p = wl_compositor_create_surface(c->comp), *s = wl_compositor_create_surface(c->comp);
        wl_subcompositor_get_subsurface(c->subc, s, p);
        wl_subcompositor_get_subsurface(c->subc, s, p);
        int ok = cx_expect(c, &wl_subcompositor_interface, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE);
        check(item, "subsurface.bad_surface", ok, "wl_subcompositor.error.bad_surface", "a surface that already has a role: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("subsurface.place_bad_surface");
        struct wl_surface *p = wl_compositor_create_surface(c->comp), *s = wl_compositor_create_surface(c->comp), *o = wl_compositor_create_surface(c->comp);
        struct wl_subsurface *ss = wl_subcompositor_get_subsurface(c->subc, s, p);
        wl_subsurface_place_above(ss, o);
        int ok = cx_expect(c, &wl_subsurface_interface, WL_SUBSURFACE_ERROR_BAD_SURFACE);
        check(item, "subsurface.place_bad_surface", ok, "wl_subsurface.error.bad_surface", "place_above a surface that isn't a sibling: %s", cx_errtext(c));
        cx_close(c);
    }
}

static void f_shm(void) {
    ITEM("wl_shm");
    {
        OPEN_OR_SKIP("shm.formats");
        int argb = 0, xrgb = 0;
        for (int i = 0; i < c->nshm; i++) { if (c->shm_formats[i] == WL_SHM_FORMAT_ARGB8888) argb = 1; if (c->shm_formats[i] == WL_SHM_FORMAT_XRGB8888) xrgb = 1; }
        check(item, "shm.formats", argb && xrgb, "wl_shm.format", "argb8888 %s, xrgb8888 %s (%d formats)", argb ? "yes" : "no", xrgb ? "yes" : "no", c->nshm);
        struct wl_buffer *b = shm_buffer(c, 16, 16, 0);
        int ok = b && cx_ok(c);
        check(item, "shm.buffer", ok, "wl_shm.create_pool", ok ? "pool and buffer created" : "%s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("shm.invalid_stride");
        int fd = shm_fd(4096);
        struct wl_shm_pool *p = wl_shm_create_pool(c->shm, fd, 4096);
        close(fd);
        wl_shm_pool_create_buffer(p, 0, 16, 16, 8, WL_SHM_FORMAT_ARGB8888);
        int ok = cx_expect2(c, &wl_shm_interface, &wl_shm_pool_interface, WL_SHM_ERROR_INVALID_STRIDE);
        check(item, "shm.invalid_stride", ok, "wl_shm.error.invalid_stride", "stride smaller than a row: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("shm.invalid_format");
        int fd = shm_fd(4096);
        struct wl_shm_pool *p = wl_shm_create_pool(c->shm, fd, 4096);
        close(fd);
        wl_shm_pool_create_buffer(p, 0, 4, 4, 16, 0x12345678);
        int ok = cx_expect2(c, &wl_shm_interface, &wl_shm_pool_interface, WL_SHM_ERROR_INVALID_FORMAT);
        check(item, "shm.invalid_format", ok, "wl_shm.error.invalid_format", "a format never announced: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("shm.invalid_fd");
        int pp[2];
        if (pipe(pp)) { skip(item, "shm.invalid_fd", "wl_shm.error.invalid_fd", "no pipe"); cx_close(c); return; }
        wl_shm_create_pool(c->shm, pp[0], 4096);
        close(pp[0]);
        close(pp[1]);
        int ok = cx_expect(c, &wl_shm_interface, WL_SHM_ERROR_INVALID_FD);
        check(item, "shm.invalid_fd", ok, "wl_shm.error.invalid_fd", "a pipe as the pool's fd: %s", cx_errtext(c));
        cx_close(c);
    }
}

/* wl_output: everything a client needs arrives on bind */
static int o_geom, o_mode_cur, o_done, o_scale, o_name, o_desc;
static void o_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp, const char *mk, const char *md, int32_t tr) {
    (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sp; (void)mk; (void)md; (void)tr; o_geom++;
}
static void o_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t r) { (void)d; (void)o; (void)w; (void)h; (void)r; if (flags & WL_OUTPUT_MODE_CURRENT) o_mode_cur++; }
static void o_donef(void *d, struct wl_output *o) { (void)d; (void)o; o_done++; }
static void o_scalef(void *d, struct wl_output *o, int32_t f) { (void)d; (void)o; if (f >= 1) o_scale++; }
static void o_namef(void *d, struct wl_output *o, const char *n) { (void)d; (void)o; if (n && *n) o_name++; }
static void o_descf(void *d, struct wl_output *o, const char *n) { (void)d; (void)o; if (n) o_desc++; }
static const struct wl_output_listener out_l = { o_geometry, o_mode, o_donef, o_scalef, o_namef, o_descf };

static void f_output(void) {
    ITEM("wl_output");
    OPEN_OR_SKIP("output.bind_events");
    o_geom = o_mode_cur = o_done = o_scale = o_name = o_desc = 0;
    struct wl_output *o = bindv(c, &wl_output_interface, 0);
    if (!o) { skip(item, "output.bind_events", item, "not advertised (headless with no display?)"); cx_close(c); return; }
    wl_output_add_listener(o, &out_l, NULL);
    cx_sync(c);
    uint32_t v = wl_proxy_get_version((struct wl_proxy *)o);
    check(item, "output.geometry", o_geom == 1, "wl_output.geometry", "geometry events on bind: %d", o_geom);
    check(item, "output.mode_current", o_mode_cur >= 1, "wl_output.mode", "a mode with the current flag: %d", o_mode_cur);
    if (v >= 2) {
        check(item, "output.scale", o_scale == 1, "wl_output.scale", "scale on bind: %d", o_scale);
        check(item, "output.done", o_done == 1, "wl_output.done", "one done after the burst: %d", o_done);
    }
    if (v >= 4) {
        check(item, "output.name", o_name == 1, "wl_output.name", "name on bind: %d", o_name);
        check(item, "output.description", o_desc == 1, "wl_output.description", "description on bind: %d", o_desc);
    }
    if (v >= 3) { wl_output_release(o); check(item, "output.release", cx_ok(c), "wl_output.release", "release: %s", cx_errtext(c)); }
    cx_close(c);
}

#ifdef CONF_HAVE_XDG_OUTPUT_UNSTABLE_V1
static int xo_pos, xo_size, xo_name, xo_desc, xo_done;
static void xo_lpos(void *d, struct zxdg_output_v1 *o, int32_t x, int32_t y) { (void)d; (void)o; (void)x; (void)y; xo_pos++; }
static void xo_lsize(void *d, struct zxdg_output_v1 *o, int32_t w, int32_t h) { (void)d; (void)o; if (w > 0 && h > 0) xo_size++; }
static void xo_donef(void *d, struct zxdg_output_v1 *o) { (void)d; (void)o; xo_done++; }
static void xo_namef(void *d, struct zxdg_output_v1 *o, const char *n) { (void)d; (void)o; (void)n; xo_name++; }
static void xo_descf(void *d, struct zxdg_output_v1 *o, const char *n) { (void)d; (void)o; (void)n; xo_desc++; }
static const struct zxdg_output_v1_listener xo_l = { xo_lpos, xo_lsize, xo_donef, xo_namef, xo_descf };
static void f_xdg_output(void) {
    ITEM("zxdg_output_manager_v1");
    OPEN_OR_SKIP("xdg_output.events");
    struct zxdg_output_manager_v1 *m = bindv(c, &zxdg_output_manager_v1_interface, 0);
    if (!m || !c->out) { skip(item, "xdg_output.events", item, "not advertised"); cx_close(c); return; }
    xo_pos = xo_size = xo_name = xo_desc = xo_done = 0;
    o_done = 0;
    struct wl_output *o = bindv(c, &wl_output_interface, 0);
    wl_output_add_listener(o, &out_l, NULL);
    struct zxdg_output_v1 *x = zxdg_output_manager_v1_get_xdg_output(m, o);
    zxdg_output_v1_add_listener(x, &xo_l, NULL);
    cx_sync(c);
    uint32_t v = wl_proxy_get_version((struct wl_proxy *)m);
    check(item, "xdg_output.logical_position", xo_pos == 1, "zxdg_output_v1.logical_position", "logical_position: %d", xo_pos);
    check(item, "xdg_output.logical_size", xo_size == 1, "zxdg_output_v1.logical_size", "logical_size (positive): %d", xo_size);
    if (v >= 2) {
        check(item, "xdg_output.name", xo_name == 1, "zxdg_output_v1.name", "name: %d", xo_name);
        check(item, "xdg_output.description", xo_desc == 1, "zxdg_output_v1.description", "description: %d", xo_desc);
    }
    if (v >= 3) check(item, "xdg_output.done_v3", xo_done == 0 && o_done >= 1, "zxdg_output_v1.done", "v3: no xdg_output.done (%d), wl_output.done instead (%d)", xo_done, o_done);
    else check(item, "xdg_output.done", xo_done >= 1, "zxdg_output_v1.done", "done: %d", xo_done);
    zxdg_output_v1_destroy(x);
    zxdg_output_manager_v1_destroy(m);
    check(item, "xdg_output.destroy", cx_ok(c), "zxdg_output_v1.destroy", "destroy: %s", cx_errtext(c));
    cx_close(c);
}
#endif

/* wl_seat */
static int s_caps_n, s_name_n, k_keymap_ok, k_repeat;
static void s2_caps(void *d, struct wl_seat *s, uint32_t caps) { (void)d; (void)s; (void)caps; s_caps_n++; }
static void s2_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; if (n && *n) s_name_n++; }
static const struct wl_seat_listener seat2_l = { s2_caps, s2_name };
static void k2_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int fd, uint32_t size) {
    (void)d; (void)k;
    if (fmt == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0) {
        char *m = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m != MAP_FAILED) { if (strstr(m, "xkb_keymap")) k_keymap_ok = 1; munmap(m, size); }
    }
    close(fd);
}
static void k2_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) { (void)d; (void)k; if (rate >= 0 && delay >= 0) k_repeat++; }
static const struct wl_keyboard_listener kb2_l = { k2_keymap, kb_enter, kb_leave, kb_key, kb_mods, k2_repeat };

static void f_seat(void) {
    ITEM("wl_seat");
    {
        OPEN_OR_SKIP("seat.capabilities");
        struct wl_seat *s = bindv(c, &wl_seat_interface, 0);
        s_caps_n = s_name_n = 0;
        wl_seat_add_listener(s, &seat2_l, NULL);
        cx_sync(c);
        uint32_t v = wl_proxy_get_version((struct wl_proxy *)s);
        check(item, "seat.capabilities", s_caps_n == 1, "wl_seat.capabilities", "capabilities on bind: %d", s_caps_n);
        if (v >= 2) check(item, "seat.name", s_name_n == 1, "wl_seat.name", "name on bind: %d", s_name_n);
        if (c->caps & WL_SEAT_CAPABILITY_KEYBOARD) {
            k_keymap_ok = k_repeat = 0;
            struct wl_keyboard *k = wl_seat_get_keyboard(s);
            wl_keyboard_add_listener(k, &kb2_l, c);
            cx_sync(c);
            check(item, "keyboard.keymap", k_keymap_ok, "wl_keyboard.keymap", "an xkb_v1 keymap the client can map: %s", k_keymap_ok ? "yes" : "no");
            if (v >= 4) check(item, "keyboard.repeat_info", k_repeat >= 1, "wl_keyboard.repeat_info", "repeat_info (v4): %d", k_repeat);
            if (v >= 3) wl_keyboard_release(k);
        } else skip(item, "keyboard.keymap", "wl_keyboard.keymap", "no keyboard capability");
        if (c->caps & WL_SEAT_CAPABILITY_POINTER) {
            struct wl_pointer *p = wl_seat_get_pointer(s);
            if (v >= 3) wl_pointer_release(p);
        }
        if (v >= 5) wl_seat_release(s);
        int ok = cx_ok(c);
        check(item, "seat.get_release", ok, "wl_seat.get_keyboard", ok ? "get_keyboard/get_pointer and their release" : "%s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("seat.missing_capability");
        if (c->caps & WL_SEAT_CAPABILITY_TOUCH) skip(item, "seat.missing_capability", "wl_seat.error.missing_capability", "this seat has touch");
        else {
            wl_seat_get_touch(c->seat);
            int ok = cx_expect(c, &wl_seat_interface, WL_SEAT_ERROR_MISSING_CAPABILITY);
            check(item, "seat.missing_capability", ok, "wl_seat.error.missing_capability", "get_touch on a seat that never had touch: %s", cx_errtext(c));
        }
        cx_close(c);
    }
#ifdef CONF_HAVE_XDG_SHELL
    {
        OPEN_OR_SKIP("pointer.role");
        Win w;
        if (!c->ptr || win_map(c, &w, "cursor-role", 1280, 800)) skip(item, "pointer.role", "wl_pointer.error.role", "no pointer or toplevel");
        else if (cx_wait(c, &c->ptr_enters, 1500) != 1) skip(item, "pointer.role", "wl_pointer.error.role", "the pointer never entered a window (headless): set_cursor without the enter serial is ignored by the protocol");
        else {
            Win w2;
            win_role(c, &w2, "cursor-role-2");
            w.s = w2.s;
            wl_pointer_set_cursor(c->ptr, c->ptr_serial, w.s, 0, 0);
            int ok = cx_expect(c, &wl_pointer_interface, WL_POINTER_ERROR_ROLE);
            check(item, "pointer.role", ok, "wl_pointer.error.role", "a toplevel's surface as the cursor: %s", cx_errtext(c));
        }
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("keyboard.enter");
        Win w;
        if (!c->kb || win_map(c, &w, "focus", 100, 80)) skip(item, "keyboard.enter", "wl_keyboard.enter", "no keyboard or toplevel");
        else {
            cx_wait(c, &c->kb_enters, 2000);
            check(item, "keyboard.enter", c->kb_focus == w.s, "wl_keyboard.enter", "a newly mapped toplevel gets the keyboard: %s", c->kb_focus == w.s ? "yes" : "no enter in 2 s");
        }
        cx_close(c);
    }
#endif
}

/* data devices: selections round-trip through the compositor */
static int src_send_n;
static void src_target(void *d, struct wl_data_source *s, const char *m) { (void)d; (void)s; (void)m; }
static void src_send(void *d, struct wl_data_source *s, const char *mime, int fd) {
    (void)s; (void)mime;
    const char *text = d;
    if (write(fd, text, strlen(text)) < 0) {}
    close(fd);
    src_send_n++;
}
static void src_cancelled(void *d, struct wl_data_source *s) { (void)d; wl_data_source_destroy(s); }
static const struct wl_data_source_listener src_l = { src_target, src_send, src_cancelled, NULL, NULL, NULL };

static int sel_n, offer_mimes;
static struct wl_data_offer *sel_offer;
static void offer_offer(void *d, struct wl_data_offer *o, const char *m) { (void)d; (void)o; if (!strcmp(m, "text/plain;charset=utf-8")) offer_mimes++; }
static void offer_actions(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static const struct wl_data_offer_listener offer_l = { offer_offer, offer_actions, offer_actions };
static void dd_offer(void *d, struct wl_data_device *dd, struct wl_data_offer *o) { (void)d; (void)dd; wl_data_offer_add_listener(o, &offer_l, NULL); }
static void dd_enter(void *d, struct wl_data_device *dd, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o) { (void)d; (void)dd; (void)s; (void)sf; (void)x; (void)y; (void)o; }
static void dd_leave(void *d, struct wl_data_device *dd) { (void)d; (void)dd; }
static void dd_motion(void *d, struct wl_data_device *dd, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)dd; (void)t; (void)x; (void)y; }
static void dd_drop(void *d, struct wl_data_device *dd) { (void)d; (void)dd; }
static void dd_selection(void *d, struct wl_data_device *dd, struct wl_data_offer *o) { (void)d; (void)dd; sel_offer = o; if (o) sel_n++; }
static const struct wl_data_device_listener dd_l = { dd_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_selection };

/* reads an offer's mime into buf (dispatching the connection meanwhile); bytes read or -1 */
static void recv_data(void *o, const char *mime, int fd) { wl_data_offer_receive(o, mime, fd); }
static long read_from(Conn *c, void (*recv)(void *, const char *, int), void *o, const char *mime, char *buf, size_t cap, int ms) {
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return -1;
    recv(o, mime, p[1]);
    close(p[1]);
    wl_display_flush(c->d);
    size_t n = 0;
    double end = now_ms() + ms;
    for (;;) {
        struct pollfd pf[2] = { { p[0], POLLIN, 0 }, { wl_display_get_fd(c->d), POLLIN, 0 } };
        int left = (int)(end - now_ms());
        if (left <= 0) break;
        wl_display_flush(c->d);
        if (poll(pf, 2, left) <= 0) break;
        if (pf[1].revents & POLLIN) { if (wl_display_dispatch(c->d) < 0) break; }
        if (pf[0].revents & (POLLIN | POLLHUP)) {
            ssize_t r = read(p[0], buf + n, n < cap ? cap - n : 0);
            if (r <= 0) break;
            n += (size_t)r;
        }
    }
    close(p[0]);
    return (long)n;
}
static long read_offer(Conn *c, struct wl_data_offer *o, const char *mime, char *buf, size_t cap, int ms) { return read_from(c, recv_data, o, mime, buf, cap, ms); }

static void f_data_device(void) {
    ITEM("wl_data_device_manager");
    {
        OPEN_OR_SKIP("data.invalid_action_mask");
        struct wl_data_device_manager *m = bindv(c, &wl_data_device_manager_interface, 0);
        if (!m || wl_proxy_get_version((struct wl_proxy *)m) < 3) { skip(item, "data.invalid_action_mask", "wl_data_source.error.invalid_action_mask", "needs v3"); cx_close(c); return; }
        struct wl_data_source *s = wl_data_device_manager_create_data_source(m);
        wl_data_source_set_actions(s, 0x80);
        int ok = cx_expect(c, &wl_data_source_interface, WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK);
        check(item, "data.invalid_action_mask", ok, "wl_data_source.error.invalid_action_mask", "set_actions(0x80): %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("data.invalid_source");
        struct wl_data_device_manager *m = bindv(c, &wl_data_device_manager_interface, 0);
        struct wl_data_source *s = wl_data_device_manager_create_data_source(m);
        struct wl_data_device *dd = wl_data_device_manager_get_data_device(m, c->seat);
        wl_data_source_offer(s, "text/plain");
        wl_data_source_set_actions(s, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        wl_data_device_set_selection(dd, s, c->kb_serial);
        int ok = cx_expect(c, &wl_data_source_interface, WL_DATA_SOURCE_ERROR_INVALID_SOURCE);
        check(item, "data.invalid_source", ok, "wl_data_source.error.invalid_source", "a drag-and-drop source (set_actions) used as the selection: %s", cx_errtext(c));
        cx_close(c);
    }
#ifdef CONF_HAVE_XDG_SHELL
    {
        OPEN_OR_SKIP("data.selection_roundtrip");
        struct wl_data_device_manager *m = bindv(c, &wl_data_device_manager_interface, 0);
        Win w;
        if (!c->kb || win_map(c, &w, "clipboard", 100, 80) || cx_wait(c, &c->kb_enters, 2000) != 1) {
            skip(item, "data.selection_roundtrip", "wl_data_device.set_selection", "no keyboard focus for a mapped toplevel");
            skip(item, "data.offer_invalid_finish", "wl_data_offer.error.invalid_finish", "no keyboard focus");
            cx_close(c);
            return;
        }
        struct wl_data_device *dd = wl_data_device_manager_get_data_device(m, c->seat);
        sel_n = offer_mimes = 0;
        sel_offer = NULL;
        wl_data_device_add_listener(dd, &dd_l, NULL);
        cx_sync(c);
        int before = sel_n;
        struct wl_data_source *s = wl_data_device_manager_create_data_source(m);
        static char text[] = "lordos conformance clipboard";
        wl_data_source_add_listener(s, &src_l, text);
        wl_data_source_offer(s, "text/plain;charset=utf-8");
        wl_data_device_set_selection(dd, s, c->kb_serial);
        double end = now_ms() + 2000;
        while (sel_n == before && now_ms() < end) cx_wait(c, NULL, 50);
        int got_sel = sel_n > before && sel_offer && offer_mimes > 0;
        check(item, "data.selection_event", got_sel, "wl_data_device.selection", "set_selection with the keyboard's serial gives a selection offer: %s", got_sel ? "yes" : "no");
        if (got_sel) {
            char buf[256] = { 0 };
            long n = read_offer(c, sel_offer, "text/plain;charset=utf-8", buf, sizeof buf - 1, 2000);
            int ok = n == (long)strlen(text) && !memcmp(buf, text, (size_t)n);
            check(item, "data.selection_roundtrip", ok, "wl_data_offer.receive", "receive through the compositor: %ld bytes%s", n, ok ? "" : " (wrong)");
            wl_data_offer_finish(sel_offer);
            int e = cx_expect(c, &wl_data_offer_interface, WL_DATA_OFFER_ERROR_INVALID_FINISH);
            check(item, "data.offer_invalid_finish", e, "wl_data_offer.error.invalid_finish", "finish on a selection offer: %s", cx_errtext(c));
        } else skip(item, "data.offer_invalid_finish", "wl_data_offer.error.invalid_finish", "no selection offer");
        cx_close(c);
    }
#endif
}

#ifdef CONF_HAVE_PRIMARY_SELECTION_UNSTABLE_V1
static int psel_n;
static struct zwp_primary_selection_offer_v1 *psel_offer;
static void ps_offer(void *d, struct zwp_primary_selection_device_v1 *dv, struct zwp_primary_selection_offer_v1 *o) { (void)d; (void)dv; (void)o; }
static void ps_sel(void *d, struct zwp_primary_selection_device_v1 *dv, struct zwp_primary_selection_offer_v1 *o) { (void)d; (void)dv; psel_offer = o; if (o) psel_n++; }
static void recv_primary(void *o, const char *mime, int fd) { zwp_primary_selection_offer_v1_receive(o, mime, fd); }
static const struct zwp_primary_selection_device_v1_listener ps_l = { ps_offer, ps_sel };
static void pss_send(void *d, struct zwp_primary_selection_source_v1 *s, const char *m, int fd) { (void)d; (void)s; (void)m; if (write(fd, "p", 1) < 0) {} close(fd); }
static void pss_cancel(void *d, struct zwp_primary_selection_source_v1 *s) { (void)d; zwp_primary_selection_source_v1_destroy(s); }
static const struct zwp_primary_selection_source_v1_listener pss_l = { pss_send, pss_cancel };
static void f_primary(void) {
    ITEM("zwp_primary_selection_device_manager_v1");
    OPEN_OR_SKIP("primary.selection_event");
    struct zwp_primary_selection_device_manager_v1 *m = bindv(c, &zwp_primary_selection_device_manager_v1_interface, 0);
    Win w;
    if (!m) { skip(item, "primary.selection_event", item, "not advertised"); cx_close(c); return; }
    if (!c->kb || win_map(c, &w, "primary", 100, 80) || cx_wait(c, &c->kb_enters, 2000) != 1) { skip(item, "primary.selection_event", "zwp_primary_selection_device_v1.set_selection", "no keyboard focus"); cx_close(c); return; }
    struct zwp_primary_selection_device_v1 *dv = zwp_primary_selection_device_manager_v1_get_device(m, c->seat);
    psel_n = 0;
    zwp_primary_selection_device_v1_add_listener(dv, &ps_l, NULL);
    cx_sync(c);
    int before = psel_n;
    struct zwp_primary_selection_source_v1 *s = zwp_primary_selection_device_manager_v1_create_source(m);
    zwp_primary_selection_source_v1_add_listener(s, &pss_l, NULL);
    zwp_primary_selection_source_v1_offer(s, "text/plain;charset=utf-8");
    zwp_primary_selection_device_v1_set_selection(dv, s, c->kb_serial);
    double end = now_ms() + 2000;
    while (psel_n == before && now_ms() < end) cx_wait(c, NULL, 50);
    check(item, "primary.selection_event", psel_n > before, "zwp_primary_selection_device_v1.selection", "set_selection with the keyboard's serial gives a selection: %s (%s)", psel_n > before ? "yes" : "no", cx_errtext(c));
    cx_close(c);
}
#endif

#ifdef CONF_HAVE_XDG_SHELL
static int pop_conf, pop_done, pop_repos, pop_xs_conf;
static uint32_t pop_serial, pop_token;
static void popc(void *d, struct xdg_popup *p, int32_t x, int32_t y, int32_t w, int32_t h) { (void)d; (void)p; (void)x; (void)y; (void)w; (void)h; pop_conf++; }
static void popd(void *d, struct xdg_popup *p) { (void)d; (void)p; pop_done++; }
static void popr(void *d, struct xdg_popup *p, uint32_t token) { (void)d; (void)p; pop_token = token; pop_repos++; }
static const struct xdg_popup_listener pop_l = { popc, popd, popr };
static void pop_xsc(void *d, struct xdg_surface *xs, uint32_t serial) { (void)d; (void)xs; pop_serial = serial; pop_xs_conf++; }
static const struct xdg_surface_listener pop_xs_l = { pop_xsc };

static struct xdg_positioner *positioner(Conn *c) {
    struct xdg_positioner *p = xdg_wm_base_create_positioner(c->wm);
    xdg_positioner_set_size(p, 50, 40);
    xdg_positioner_set_anchor_rect(p, 10, 10, 1, 1);
    xdg_positioner_set_anchor(p, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
    xdg_positioner_set_gravity(p, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(p, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    xdg_positioner_set_offset(p, 0, 0);
    return p;
}

static void f_xdg_shell(void) {
    ITEM("xdg_wm_base");
    {
        OPEN_OR_SKIP("toplevel.configure");
        if (!c->wm) { skip(item, "toplevel.configure", item, "not advertised"); cx_close(c); return; }
        Win w;
        int r = win_role(c, &w, "configure");
        check(item, "toplevel.configure", r == 0, "xdg_surface.configure", "the initial commit is answered with a configure: %s", r ? "none in 3 s" : "yes");
        if (!r) {
            check(item, "toplevel.configure_order", !w.order_bad && w.top_configs >= 1, "xdg_toplevel.configure", "xdg_toplevel.configure before xdg_surface.configure: %s", w.order_bad ? "wrong order" : "yes");
            uint32_t v = wl_proxy_get_version((struct wl_proxy *)c->wm);
            if (v >= 5) check(item, "toplevel.wm_capabilities", w.caps_seen, "xdg_toplevel.wm_capabilities", "wm_capabilities with the first configure (v5): %s", w.caps_seen ? "yes" : "no");
            if (v >= 4) check(item, "toplevel.configure_bounds", w.bounds_seen, "xdg_toplevel.configure_bounds", "configure_bounds (v4, optional but expected here): %s", w.bounds_seen ? "yes" : "no");
            int bw = w.cw > 0 ? w.cw : 200, bh = w.ch > 0 ? w.ch : 150;
            win_ack_commit(c, &w, bw, bh);
            xdg_surface_set_window_geometry(w.xs, 0, 0, bw, bh);
            xdg_toplevel_set_min_size(w.top, 50, 40);
            xdg_toplevel_set_max_size(w.top, 4000, 3000);
            wl_surface_commit(w.s);
            check(item, "toplevel.map", cx_ok(c), "xdg_toplevel.set_title", "mapped with title, app id, geometry, min/max size: %s", cx_errtext(c));
            /* maximize and fullscreen come back as configures with the state */
            int before = w.top_configs;
            xdg_toplevel_set_maximized(w.top);
            double end = now_ms() + 2000;
            while (now_ms() < end && !(w.top_configs > before && w.maximized)) cx_wait(c, NULL, 50);
            check(item, "toplevel.set_maximized", w.maximized, "xdg_toplevel.set_maximized", "a configure with the maximized state: %s", w.maximized ? "yes" : "no");
            if (w.maximized) { win_ack_commit(c, &w, w.cw > 0 ? w.cw : bw, w.ch > 0 ? w.ch : bh); }
            xdg_toplevel_unset_maximized(w.top);
            end = now_ms() + 2000;
            while (now_ms() < end && w.maximized) cx_wait(c, NULL, 50);
            check(item, "toplevel.unset_maximized", !w.maximized, "xdg_toplevel.unset_maximized", "a configure without it: %s", !w.maximized ? "yes" : "no");
            win_ack_commit(c, &w, bw, bh);
            xdg_toplevel_set_fullscreen(w.top, NULL);
            end = now_ms() + 2000;
            while (now_ms() < end && !w.fullscreen) cx_wait(c, NULL, 50);
            check(item, "toplevel.set_fullscreen", w.fullscreen, "xdg_toplevel.set_fullscreen", "a configure with the fullscreen state (%dx%d): %s", w.cw, w.ch, w.fullscreen ? "yes" : "no");
            if (w.fullscreen) win_ack_commit(c, &w, w.cw > 0 ? w.cw : bw, w.ch > 0 ? w.ch : bh);
            xdg_toplevel_unset_fullscreen(w.top);
            end = now_ms() + 2000;
            while (now_ms() < end && w.fullscreen) cx_wait(c, NULL, 50);
            check(item, "toplevel.unset_fullscreen", !w.fullscreen, "xdg_toplevel.unset_fullscreen", "a configure without it: %s", !w.fullscreen ? "yes" : "no");
            win_ack_commit(c, &w, bw, bh);
            xdg_toplevel_set_minimized(w.top);
            check(item, "toplevel.set_minimized", cx_ok(c), "xdg_toplevel.set_minimized", "set_minimized: %s", cx_errtext(c));
            /* a popup on it */
            struct xdg_positioner *p = positioner(c);
            struct wl_surface *ps = wl_compositor_create_surface(c->comp);
            struct xdg_surface *pxs = xdg_wm_base_get_xdg_surface(c->wm, ps);
            pop_conf = pop_done = pop_repos = pop_xs_conf = 0;
            xdg_surface_add_listener(pxs, &pop_xs_l, NULL);
            struct xdg_popup *pop = xdg_surface_get_popup(pxs, w.xs, p);
            xdg_popup_add_listener(pop, &pop_l, NULL);
            wl_surface_commit(ps);
            cx_wait(c, &pop_xs_conf, 2000);
            check(item, "popup.configure", pop_conf >= 1 && pop_xs_conf >= 1, "xdg_popup.configure", "xdg_popup.configure and xdg_surface.configure: %d, %d", pop_conf, pop_xs_conf);
            if (pop_xs_conf) {
                xdg_surface_ack_configure(pxs, pop_serial);
                struct wl_buffer *b = shm_buffer(c, 50, 40, 0xffeeeeee);
                wl_surface_attach(ps, b, 0, 0);
                wl_surface_commit(ps);
                cx_sync(c);
                if (wl_proxy_get_version((struct wl_proxy *)pop) >= 3) {
                    xdg_positioner_set_offset(p, 5, 5);
                    xdg_popup_reposition(pop, p, 77);
                    cx_wait(c, &pop_repos, 2000);
                    check(item, "popup.repositioned", pop_repos >= 1 && pop_token == 77, "xdg_popup.repositioned", "reposition answered with repositioned(token): %d (token %u)", pop_repos, pop_token);
                }
            }
            xdg_popup_destroy(pop);
            xdg_surface_destroy(pxs);
            xdg_positioner_destroy(p);
            check(item, "popup.destroy", cx_ok(c), "xdg_popup.destroy", "popup destroyed: %s", cx_errtext(c));
        }
        cx_close(c);
    }
    {
        /* a grab with a serial the client never got is denied: the popup is dismissed */
        OPEN_OR_SKIP("popup.grab_denied");
        Win w;
        if (win_map(c, &w, "grab", 120, 90)) { skip(item, "popup.grab_denied", "xdg_popup.grab", "no toplevel"); cx_close(c); goto next1; }
        struct xdg_positioner *p = positioner(c);
        struct wl_surface *ps = wl_compositor_create_surface(c->comp);
        struct xdg_surface *pxs = xdg_wm_base_get_xdg_surface(c->wm, ps);
        pop_conf = pop_done = pop_xs_conf = 0;
        xdg_surface_add_listener(pxs, &pop_xs_l, NULL);
        struct xdg_popup *pop = xdg_surface_get_popup(pxs, w.xs, p);
        xdg_popup_add_listener(pop, &pop_l, NULL);
        xdg_popup_grab(pop, c->seat, 0xdead);
        wl_surface_commit(ps);
        cx_wait(c, &pop_done, 2000);
        check(item, "popup.grab_denied", pop_done >= 1, "xdg_popup.popup_done", "grab with a serial never sent: popup_done %s", pop_done ? "sent" : "not sent in 2 s");
        cx_close(c);
    }
next1:
    {
        OPEN_OR_SKIP("popup.invalid_grab");
        Win w;
        if (win_map(c, &w, "grab2", 120, 90)) { skip(item, "popup.invalid_grab", "xdg_popup.error.invalid_grab", "no toplevel"); cx_close(c); goto next2; }
        struct xdg_positioner *p = positioner(c);
        struct wl_surface *ps = wl_compositor_create_surface(c->comp);
        struct xdg_surface *pxs = xdg_wm_base_get_xdg_surface(c->wm, ps);
        pop_xs_conf = 0;
        xdg_surface_add_listener(pxs, &pop_xs_l, NULL);
        struct xdg_popup *pop = xdg_surface_get_popup(pxs, w.xs, p);
        xdg_popup_add_listener(pop, &pop_l, NULL);
        wl_surface_commit(ps);
        cx_wait(c, &pop_xs_conf, 2000);
        xdg_surface_ack_configure(pxs, pop_serial);
        wl_surface_attach(ps, shm_buffer(c, 50, 40, 0xffffffff), 0, 0);
        wl_surface_commit(ps);
        cx_sync(c);
        xdg_popup_grab(pop, c->seat, c->kb_serial);
        int ok = cx_expect(c, &xdg_popup_interface, XDG_POPUP_ERROR_INVALID_GRAB);
        check(item, "popup.invalid_grab", ok, "xdg_popup.error.invalid_grab", "grab on a popup already mapped: %s", cx_errtext(c));
        cx_close(c);
    }
next2:;
    struct { const char *id, *hint, *what; int kind; } errs[] = {
        { "positioner.invalid_input", "xdg_positioner.error.invalid_input", "set_size(0, 0)", 1 },
        { "surface.unconfigured_buffer", "xdg_surface.error.unconfigured_buffer", "a buffer attached before the first ack_configure", 2 },
        { "surface.invalid_serial", "xdg_surface.error.invalid_serial", "ack_configure with a serial never sent", 3 },
        { "surface.already_constructed", "xdg_surface.error.already_constructed", "get_toplevel twice", 4 },
        { "surface.invalid_size", "xdg_surface.error.invalid_size", "set_window_geometry 0×0", 5 },
        { "surface.defunct_role_object", "xdg_surface.error.defunct_role_object", "xdg_surface destroyed before its xdg_toplevel", 6 },
        { "wm_base.role", "xdg_wm_base.error.role", "get_xdg_surface on a subsurface", 7 },
        { "wm_base.defunct_surfaces", "xdg_wm_base.error.defunct_surfaces", "xdg_wm_base destroyed with an xdg_surface alive", 8 },
        { "wm_base.invalid_surface_state", "xdg_wm_base.error.invalid_surface_state", "get_xdg_surface on a surface with a buffer", 9 },
        { "wm_base.invalid_positioner", "xdg_wm_base.error.invalid_positioner", "get_popup with a positioner that has no size", 10 },
        { "wm_base.not_the_topmost_popup", "xdg_wm_base.error.not_the_topmost_popup", "a parent popup destroyed before its child", 11 },
        { "toplevel.invalid_parent", "xdg_toplevel.error.invalid_parent", "set_parent to itself", 12 },
        { "toplevel.invalid_size", "xdg_toplevel.error.invalid_size", "set_min_size(-1, -1)", 13 },
        { "toplevel.invalid_size_max", "xdg_toplevel.error.invalid_size", "max size smaller than the min size", 14 },
        { "toplevel.invalid_resize_edge", "xdg_toplevel.error.invalid_resize_edge", "resize with edge 99", 15 },
    };
    for (size_t i = 0; i < sizeof errs / sizeof errs[0]; i++) {
        OPEN_OR_SKIP(errs[i].id);
        Win w;
        const struct wl_interface *want = NULL;
        uint32_t code = 0, gone_id = 0;     /* gone_id: the error is on an object the request destroyed (cx_expect_id) */
        int ok = 1;
        switch (errs[i].kind) {
        case 1: { struct xdg_positioner *p = xdg_wm_base_create_positioner(c->wm); xdg_positioner_set_size(p, 0, 0); want = &xdg_positioner_interface; code = XDG_POSITIONER_ERROR_INVALID_INPUT; break; }
        case 2: {
            struct wl_surface *s = wl_compositor_create_surface(c->comp);
            struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(c->wm, s);
            xdg_surface_get_toplevel(xs);
            wl_surface_commit(s);
            cx_sync(c);
            wl_surface_attach(s, shm_buffer(c, 32, 32, 0), 0, 0);
            wl_surface_commit(s);
            want = &xdg_surface_interface; code = XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER; break;
        }
        case 3: ok = !win_role(c, &w, "serial"); if (ok) xdg_surface_ack_configure(w.xs, w.serial + 1000); want = &xdg_surface_interface; code = XDG_SURFACE_ERROR_INVALID_SERIAL; break;
        case 4: ok = !win_role(c, &w, "twice"); if (ok) xdg_surface_get_toplevel(w.xs); want = &xdg_surface_interface; code = XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED; break;
        case 5: ok = !win_role(c, &w, "geom"); if (ok) xdg_surface_set_window_geometry(w.xs, 0, 0, 0, 0); want = &xdg_surface_interface; code = XDG_SURFACE_ERROR_INVALID_SIZE; break;
        case 6: ok = !win_role(c, &w, "defunct") && wl_proxy_get_version((struct wl_proxy *)c->wm) >= 6; if (ok) { gone_id = wl_proxy_get_id((struct wl_proxy *)w.xs); xdg_surface_destroy(w.xs); } want = &xdg_surface_interface; code = XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT; break;
        case 7: {
            struct wl_surface *p = wl_compositor_create_surface(c->comp), *s = wl_compositor_create_surface(c->comp);
            wl_subcompositor_get_subsurface(c->subc, s, p);
            xdg_wm_base_get_xdg_surface(c->wm, s);
            want = &xdg_wm_base_interface; code = XDG_WM_BASE_ERROR_ROLE; break;
        }
        case 8: ok = !win_role(c, &w, "defunct-surfaces"); if (ok) { gone_id = wl_proxy_get_id((struct wl_proxy *)c->wm); xdg_wm_base_destroy(c->wm); c->wm = NULL; } want = &xdg_wm_base_interface; code = XDG_WM_BASE_ERROR_DEFUNCT_SURFACES; break;
        case 9: {
            struct wl_surface *s = wl_compositor_create_surface(c->comp);
            wl_surface_attach(s, shm_buffer(c, 16, 16, 0), 0, 0);
            wl_surface_commit(s);
            xdg_wm_base_get_xdg_surface(c->wm, s);
            want = &xdg_wm_base_interface; code = XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE; break;
        }
        case 10: {
            ok = !win_role(c, &w, "bad-positioner");
            if (ok) {
                struct xdg_positioner *p = xdg_wm_base_create_positioner(c->wm);
                xdg_positioner_set_anchor_rect(p, 0, 0, 1, 1);
                struct wl_surface *s = wl_compositor_create_surface(c->comp);
                xdg_surface_get_popup(xdg_wm_base_get_xdg_surface(c->wm, s), w.xs, p);
            }
            want = &xdg_wm_base_interface; code = XDG_WM_BASE_ERROR_INVALID_POSITIONER; break;
        }
        case 11: {
            ok = !win_map(c, &w, "popups", 120, 90);
            if (ok) {
                struct xdg_positioner *p = positioner(c);
                struct wl_surface *s1 = wl_compositor_create_surface(c->comp), *s2 = wl_compositor_create_surface(c->comp);
                struct xdg_surface *x1 = xdg_wm_base_get_xdg_surface(c->wm, s1);
                pop_xs_conf = 0;
                xdg_surface_add_listener(x1, &pop_xs_l, NULL);
                struct xdg_popup *p1 = xdg_surface_get_popup(x1, w.xs, p);
                wl_surface_commit(s1);
                cx_wait(c, &pop_xs_conf, 2000);
                xdg_surface_ack_configure(x1, pop_serial);
                wl_surface_attach(s1, shm_buffer(c, 50, 40, 0xffffffff), 0, 0);
                wl_surface_commit(s1);
                struct xdg_surface *x2 = xdg_wm_base_get_xdg_surface(c->wm, s2);
                pop_xs_conf = 0;
                xdg_surface_add_listener(x2, &pop_xs_l, NULL);
                xdg_surface_get_popup(x2, x1, p);
                wl_surface_commit(s2);
                cx_wait(c, &pop_xs_conf, 2000);
                xdg_surface_ack_configure(x2, pop_serial);
                wl_surface_attach(s2, shm_buffer(c, 50, 40, 0xffffffff), 0, 0);
                wl_surface_commit(s2);
                cx_sync(c);
                xdg_popup_destroy(p1);
            }
            want = &xdg_wm_base_interface; code = XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP; break;
        }
        case 12: ok = !win_role(c, &w, "parent"); if (ok) xdg_toplevel_set_parent(w.top, w.top); want = &xdg_toplevel_interface; code = XDG_TOPLEVEL_ERROR_INVALID_PARENT; break;
        case 13: ok = !win_role(c, &w, "minsize"); if (ok) xdg_toplevel_set_min_size(w.top, -1, -1); want = &xdg_toplevel_interface; code = XDG_TOPLEVEL_ERROR_INVALID_SIZE; break;
        case 14: ok = !win_role(c, &w, "maxsize"); if (ok) { xdg_toplevel_set_min_size(w.top, 300, 300); xdg_toplevel_set_max_size(w.top, 100, 100); wl_surface_commit(w.s); } want = &xdg_toplevel_interface; code = XDG_TOPLEVEL_ERROR_INVALID_SIZE; break;
        case 15: ok = !win_role(c, &w, "edge"); if (ok) xdg_toplevel_resize(w.top, c->seat, c->kb_serial, 99); want = &xdg_toplevel_interface; code = XDG_TOPLEVEL_ERROR_INVALID_RESIZE_EDGE; break;
        }
        if (!ok) skip(item, errs[i].id, errs[i].hint, "couldn't set up: %s", cx_errtext(c));
        else {
            int got = gone_id ? cx_expect_id(c, want, gone_id, code) : cx_expect(c, want, code);
            check(item, errs[i].id, got, errs[i].hint, "%s: %s", errs[i].what, cx_errtext(c));
        }
        cx_close(c);
    }
}
#endif

#ifdef CONF_HAVE_XDG_DECORATION_UNSTABLE_V1
static int deco_conf;
static uint32_t deco_mode;
static void deco_configure(void *d, struct zxdg_toplevel_decoration_v1 *x, uint32_t mode) { (void)d; (void)x; deco_mode = mode; deco_conf++; }
static const struct zxdg_toplevel_decoration_v1_listener deco_l = { deco_configure };
static void f_decoration(void) {
    ITEM("zxdg_decoration_manager_v1");
    {
        OPEN_OR_SKIP("decoration.configure");
        struct zxdg_decoration_manager_v1 *m = bindv(c, &zxdg_decoration_manager_v1_interface, 0);
        if (!m) { skip(item, "decoration.configure", item, "not advertised"); cx_close(c); return; }
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(c->wm, s);
        struct xdg_toplevel *t = xdg_surface_get_toplevel(xs);
        struct zxdg_toplevel_decoration_v1 *dd = zxdg_decoration_manager_v1_get_toplevel_decoration(m, t);
        deco_conf = 0;
        zxdg_toplevel_decoration_v1_add_listener(dd, &deco_l, NULL);
        zxdg_toplevel_decoration_v1_set_mode(dd, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        wl_surface_commit(s);
        cx_wait(c, &deco_conf, 2000);
        check(item, "decoration.configure", deco_conf >= 1 && deco_mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE, "zxdg_toplevel_decoration_v1.configure", "set_mode(server_side) is answered with configure(server_side): %d, mode %u", deco_conf, deco_mode);
        int before = deco_conf;
        zxdg_toplevel_decoration_v1_set_mode(dd, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
        wl_surface_commit(s);
        double end = now_ms() + 2000;
        while (deco_conf == before && now_ms() < end) cx_wait(c, NULL, 50);
        check(item, "decoration.client_side", deco_conf > before && deco_mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE, "zxdg_toplevel_decoration_v1.set_mode", "set_mode(client_side) honoured: mode %u", deco_mode);
        zxdg_toplevel_decoration_v1_unset_mode(dd);
        zxdg_toplevel_decoration_v1_destroy(dd);
        check(item, "decoration.destroy", cx_ok(c), "zxdg_toplevel_decoration_v1.destroy", "unset_mode and destroy: %s", cx_errtext(c));
        cx_close(c);
    }
    for (int k = 0; k < 3; k++) {
        static const char *ids[] = { "decoration.already_constructed", "decoration.unconfigured_buffer", "decoration.orphaned" };
        static const char *hints[] = { "zxdg_toplevel_decoration_v1.error.already_constructed", "zxdg_toplevel_decoration_v1.error.unconfigured_buffer", "zxdg_toplevel_decoration_v1.error.orphaned" };
        OPEN_OR_SKIP(ids[k]);
        struct zxdg_decoration_manager_v1 *m = bindv(c, &zxdg_decoration_manager_v1_interface, 0);
        Win w;
        int ok;
        if (k == 1) ok = !win_map(c, &w, "deco-buffer", 100, 80);
        else ok = !win_role(c, &w, "deco");
        if (!ok) { skip(item, ids[k], hints[k], "no toplevel"); cx_close(c); continue; }
        uint32_t code;
        if (k == 0) {
            zxdg_decoration_manager_v1_get_toplevel_decoration(m, w.top);
            zxdg_decoration_manager_v1_get_toplevel_decoration(m, w.top);
            code = ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED;
        } else if (k == 1) {
            zxdg_decoration_manager_v1_get_toplevel_decoration(m, w.top);
            code = ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER;
        } else {
            zxdg_decoration_manager_v1_get_toplevel_decoration(m, w.top);
            xdg_toplevel_destroy(w.top);
            code = ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED;
        }
        int got = k < 2 ? cx_expect2(c, &zxdg_toplevel_decoration_v1_interface, &zxdg_decoration_manager_v1_interface, code) : cx_expect(c, &zxdg_toplevel_decoration_v1_interface, code);
        check(item, ids[k], got, hints[k], "%s: %s", k == 0 ? "two decorations for one toplevel" : k == 1 ? "a decoration for a toplevel that already has a buffer" : "the toplevel destroyed before its decoration", cx_errtext(c));
        cx_close(c);
    }
}
#endif

#ifdef CONF_HAVE_VIEWPORTER
static void f_viewporter(void) {
    ITEM("wp_viewporter");
    {
        OPEN_OR_SKIP("viewport.flow");
        struct wp_viewporter *vp = bindv(c, &wp_viewporter_interface, 0);
        if (!vp) { skip(item, "viewport.flow", item, "not advertised"); cx_close(c); return; }
#ifdef CONF_HAVE_XDG_SHELL
        Win w;
        if (win_map(c, &w, "viewport", 64, 64)) { skip(item, "viewport.flow", "wp_viewport.set_destination", "no toplevel"); cx_close(c); return; }
        struct wp_viewport *v = wp_viewporter_get_viewport(vp, w.s);
        wp_viewport_set_source(v, wl_fixed_from_int(0), wl_fixed_from_int(0), wl_fixed_from_int(32), wl_fixed_from_int(32));
        wp_viewport_set_destination(v, 300, 200);
        wl_surface_commit(w.s);
        wp_viewport_set_source(v, wl_fixed_from_int(-1), wl_fixed_from_int(-1), wl_fixed_from_int(-1), wl_fixed_from_int(-1));
        wp_viewport_set_destination(v, -1, -1);
        wl_surface_commit(w.s);
        wp_viewport_destroy(v);
        int ok = cx_ok(c);
        check(item, "viewport.flow", ok, "wp_viewport.set_destination", ok ? "source, destination, unset (-1) and destroy" : "%s", cx_errtext(c));
#endif
        cx_close(c);
    }
    struct { const char *id, *hint, *what; uint32_t code; } errs[] = {
        { "viewport.viewport_exists", "wp_viewporter.error.viewport_exists", "two viewports for one surface", WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS },
        { "viewport.bad_value_dest", "wp_viewport.error.bad_value", "set_destination(0, 5)", WP_VIEWPORT_ERROR_BAD_VALUE },
        { "viewport.bad_value_src", "wp_viewport.error.bad_value", "set_source with a negative x", WP_VIEWPORT_ERROR_BAD_VALUE },
        { "viewport.no_surface", "wp_viewport.error.no_surface", "set_destination after the wl_surface is gone", WP_VIEWPORT_ERROR_NO_SURFACE },
        { "viewport.out_of_buffer", "wp_viewport.error.out_of_buffer", "a source rectangle outside the buffer", WP_VIEWPORT_ERROR_OUT_OF_BUFFER },
        { "viewport.bad_size", "wp_viewport.error.bad_size", "a fractional source size and no destination", WP_VIEWPORT_ERROR_BAD_SIZE },
    };
    for (size_t i = 0; i < sizeof errs / sizeof errs[0]; i++) {
        OPEN_OR_SKIP(errs[i].id);
        struct wp_viewporter *vp = bindv(c, &wp_viewporter_interface, 0);
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        struct wp_viewport *v = wp_viewporter_get_viewport(vp, s);
        const struct wl_interface *want = &wp_viewport_interface;
        switch (i) {
        case 0: wp_viewporter_get_viewport(vp, s); want = &wp_viewporter_interface; break;
        case 1: wp_viewport_set_destination(v, 0, 5); break;
        case 2: wp_viewport_set_source(v, wl_fixed_from_int(-5), 0, wl_fixed_from_int(5), wl_fixed_from_int(5)); break;
        case 3: wl_surface_destroy(s); wp_viewport_set_destination(v, 10, 10); break;
        case 4: wp_viewport_set_source(v, 0, 0, wl_fixed_from_int(64), wl_fixed_from_int(64)); wl_surface_attach(s, shm_buffer(c, 16, 16, 0), 0, 0); wl_surface_commit(s); break;
        case 5: wp_viewport_set_source(v, 0, 0, wl_fixed_from_double(7.5), wl_fixed_from_double(7.5)); wl_surface_attach(s, shm_buffer(c, 16, 16, 0), 0, 0); wl_surface_commit(s); break;
        }
        int got = cx_expect(c, want, errs[i].code);
        check(item, errs[i].id, got, errs[i].hint, "%s: %s", errs[i].what, cx_errtext(c));
        cx_close(c);
    }
}
#endif

#ifdef CONF_HAVE_FRACTIONAL_SCALE_V1
static int frac_n;
static uint32_t frac_v;
static void frac_pref(void *d, struct wp_fractional_scale_v1 *f, uint32_t s) { (void)d; (void)f; frac_v = s; frac_n++; }
static const struct wp_fractional_scale_v1_listener frac_l = { frac_pref };
static void f_fractional(void) {
    ITEM("wp_fractional_scale_manager_v1");
    {
        OPEN_OR_SKIP("fractional.preferred_scale");
        struct wp_fractional_scale_manager_v1 *m = bindv(c, &wp_fractional_scale_manager_v1_interface, 0);
        if (!m) { skip(item, "fractional.preferred_scale", item, "not advertised"); cx_close(c); return; }
#ifdef CONF_HAVE_XDG_SHELL
        Win w;
        memset(&w, 0, sizeof w);
        frac_n = 0;
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        struct wp_fractional_scale_v1 *f = wp_fractional_scale_manager_v1_get_fractional_scale(m, s);
        wp_fractional_scale_v1_add_listener(f, &frac_l, NULL);
        w.s = s;
        w.xs = xdg_wm_base_get_xdg_surface(c->wm, s);
        xdg_surface_add_listener(w.xs, &xs_l, &w);
        w.top = xdg_surface_get_toplevel(w.xs);
        xdg_toplevel_add_listener(w.top, &top_l, &w);
        wl_surface_commit(s);
        cx_wait(c, &w.configured, 2000);
        win_ack_commit(c, &w, 100, 80);
        cx_wait(c, &frac_n, 2000);
        check(item, "fractional.preferred_scale", frac_n >= 1 && frac_v >= 120, "wp_fractional_scale_v1.preferred_scale", "preferred_scale for a mapped surface: %d (%u/120)", frac_n, frac_v);
        wp_fractional_scale_v1_destroy(f);
        check(item, "fractional.destroy", cx_ok(c), "wp_fractional_scale_v1.destroy", "destroy: %s", cx_errtext(c));
#endif
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("fractional.exists");
        struct wp_fractional_scale_manager_v1 *m = bindv(c, &wp_fractional_scale_manager_v1_interface, 0);
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        wp_fractional_scale_manager_v1_get_fractional_scale(m, s);
        wp_fractional_scale_manager_v1_get_fractional_scale(m, s);
        int ok = cx_expect(c, &wp_fractional_scale_manager_v1_interface, WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS);
        check(item, "fractional.exists", ok, "wp_fractional_scale_manager_v1.error.fractional_scale_exists", "two for one surface: %s", cx_errtext(c));
        cx_close(c);
    }
}
#endif

#ifdef CONF_HAVE_SINGLE_PIXEL_BUFFER_V1
static void f_spb(void) {
    ITEM("wp_single_pixel_buffer_manager_v1");
    OPEN_OR_SKIP("spb.buffer");
    struct wp_single_pixel_buffer_manager_v1 *m = bindv(c, &wp_single_pixel_buffer_manager_v1_interface, 0);
    if (!m) { skip(item, "spb.buffer", item, "not advertised"); cx_close(c); return; }
#if defined(CONF_HAVE_XDG_SHELL) && defined(CONF_HAVE_VIEWPORTER)
    Win w;
    struct wp_viewporter *vp = bindv(c, &wp_viewporter_interface, 0);
    if (!vp || win_role(c, &w, "single-pixel")) { skip(item, "spb.buffer", "wp_single_pixel_buffer_manager_v1.create_u32_rgba_buffer", "no toplevel or viewporter"); cx_close(c); return; }
    struct wl_buffer *b = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(m, 0x80000000u, 0, 0, 0x80000000u);
    struct wp_viewport *v = wp_viewporter_get_viewport(vp, w.s);
    wp_viewport_set_destination(v, 200, 100);
    wl_surface_attach(w.s, b, 0, 0);
    wl_surface_damage_buffer(w.s, 0, 0, 1, 1);
    wl_surface_commit(w.s);
    int ok = cx_ok(c);
    check(item, "spb.buffer", ok, "wp_single_pixel_buffer_manager_v1.create_u32_rgba_buffer", ok ? "a 1×1 buffer stretched to 200×100 maps" : "%s", cx_errtext(c));
    wl_buffer_destroy(b);
    wp_single_pixel_buffer_manager_v1_destroy(m);
    check(item, "spb.destroy", cx_ok(c), "wp_single_pixel_buffer_manager_v1.destroy", "destroy: %s", cx_errtext(c));
#endif
    cx_close(c);
}
#endif

#ifdef CONF_HAVE_ALPHA_MODIFIER_V1
static void f_alpha(void) {
    ITEM("wp_alpha_modifier_v1");
    {
        OPEN_OR_SKIP("alpha.flow");
        struct wp_alpha_modifier_v1 *m = bindv(c, &wp_alpha_modifier_v1_interface, 0);
        if (!m) { skip(item, "alpha.flow", item, "not advertised"); cx_close(c); return; }
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        struct wp_alpha_modifier_surface_v1 *a = wp_alpha_modifier_v1_get_surface(m, s);
        wp_alpha_modifier_surface_v1_set_multiplier(a, 0x7fffffffu);
        wl_surface_commit(s);
        wp_alpha_modifier_surface_v1_destroy(a);
        check(item, "alpha.flow", cx_ok(c), "wp_alpha_modifier_surface_v1.set_multiplier", "get_surface, set_multiplier, destroy: %s", cx_errtext(c));
        cx_close(c);
    }
    for (int k = 0; k < 2; k++) {
        OPEN_OR_SKIP(k ? "alpha.no_surface" : "alpha.already_constructed");
        struct wp_alpha_modifier_v1 *m = bindv(c, &wp_alpha_modifier_v1_interface, 0);
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        struct wp_alpha_modifier_surface_v1 *a = wp_alpha_modifier_v1_get_surface(m, s);
        int got;
        if (!k) { wp_alpha_modifier_v1_get_surface(m, s); got = cx_expect(c, &wp_alpha_modifier_v1_interface, WP_ALPHA_MODIFIER_V1_ERROR_ALREADY_CONSTRUCTED); }
        else { wl_surface_destroy(s); wp_alpha_modifier_surface_v1_set_multiplier(a, 5); got = cx_expect(c, &wp_alpha_modifier_surface_v1_interface, WP_ALPHA_MODIFIER_SURFACE_V1_ERROR_NO_SURFACE); }
        check(item, k ? "alpha.no_surface" : "alpha.already_constructed", got, k ? "wp_alpha_modifier_surface_v1.error.no_surface" : "wp_alpha_modifier_v1.error.already_constructed",
              "%s: %s", k ? "set_multiplier after the surface is gone" : "two for one surface", cx_errtext(c));
        cx_close(c);
    }
}
#endif

#ifdef CONF_HAVE_PRESENTATION_TIME
static int pres_clock, pres_res;
static void pres_clockf(void *d, struct wp_presentation *p, uint32_t clk) { (void)d; (void)p; (void)clk; pres_clock++; }
static const struct wp_presentation_listener pres_l = { pres_clockf };
static void pf_sync(void *d, struct wp_presentation_feedback *f, struct wl_output *o) { (void)d; (void)f; (void)o; }
static void pf_presented(void *d, struct wp_presentation_feedback *f, uint32_t a, uint32_t b, uint32_t e, uint32_t r, uint32_t g, uint32_t h, uint32_t fl) {
    (void)d; (void)a; (void)b; (void)e; (void)r; (void)g; (void)h; (void)fl; pres_res = 1; wp_presentation_feedback_destroy(f);
}
static void pf_discarded(void *d, struct wp_presentation_feedback *f) { (void)d; pres_res = 2; wp_presentation_feedback_destroy(f); }
static const struct wp_presentation_feedback_listener pf_l = { pf_sync, pf_presented, pf_discarded };
static void f_presentation(void) {
    ITEM("wp_presentation");
    OPEN_OR_SKIP("presentation.clock_id");
    struct wp_presentation *p = bindv(c, &wp_presentation_interface, 0);
    if (!p) { skip(item, "presentation.clock_id", item, "not advertised"); cx_close(c); return; }
    pres_clock = pres_res = 0;
    wp_presentation_add_listener(p, &pres_l, NULL);
    cx_sync(c);
    check(item, "presentation.clock_id", pres_clock == 1, "wp_presentation.clock_id", "clock_id on bind: %d", pres_clock);
#ifdef CONF_HAVE_XDG_SHELL
    Win w;
    if (!win_map(c, &w, "presentation", 100, 80)) {
        struct wp_presentation_feedback *f = wp_presentation_feedback(p, w.s);
        wp_presentation_feedback_add_listener(f, &pf_l, NULL);
        wl_surface_attach(w.s, shm_buffer(c, w.cw > 0 ? w.cw : 100, w.ch > 0 ? w.ch : 80, 0xff00ff00), 0, 0);
        wl_surface_damage(w.s, 0, 0, 10, 10);
        wl_surface_commit(w.s);
        cx_wait(c, &pres_res, 2000);
        check(item, "presentation.feedback", pres_res != 0, "wp_presentation_feedback.presented", "feedback for a commit is presented or discarded: %s", pres_res == 1 ? "presented" : pres_res == 2 ? "discarded" : "neither in 2 s");
    } else skip(item, "presentation.feedback", "wp_presentation_feedback.presented", "no toplevel");
#endif
    cx_close(c);
}
#endif

#ifdef CONF_HAVE_LINUX_DMABUF_V1
static int fb_done, fb_table, fb_main, fb_tranche_done, fb_formats, dm_mods;
static void fb_donef(void *d, struct zwp_linux_dmabuf_feedback_v1 *f) { (void)d; (void)f; fb_done++; }
static void fb_tablef(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, int32_t fd, uint32_t size) { (void)d; (void)f; close(fd); if (size > 0 && size % 16 == 0) fb_table++; }
static void fb_mainf(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, struct wl_array *dev) { (void)d; (void)f; if (dev->size == sizeof(dev_t)) fb_main++; }
static void fb_tdone(void *d, struct zwp_linux_dmabuf_feedback_v1 *f) { (void)d; (void)f; fb_tranche_done++; }
static void fb_tdev(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, struct wl_array *dev) { (void)d; (void)f; (void)dev; }
static void fb_tfmt(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, struct wl_array *idx) { (void)d; (void)f; if (idx->size) fb_formats++; }
static void fb_tflags(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, uint32_t fl) { (void)d; (void)f; (void)fl; }
static const struct zwp_linux_dmabuf_feedback_v1_listener fb_l = { fb_donef, fb_tablef, fb_mainf, fb_tdone, fb_tdev, fb_tfmt, fb_tflags };
static void dm_format(void *d, struct zwp_linux_dmabuf_v1 *m, uint32_t f) { (void)d; (void)m; (void)f; }
static void dm_modifier(void *d, struct zwp_linux_dmabuf_v1 *m, uint32_t f, uint32_t hi, uint32_t lo) { (void)d; (void)m; (void)f; (void)hi; (void)lo; dm_mods++; }
static const struct zwp_linux_dmabuf_v1_listener dm_l = { dm_format, dm_modifier };
static void f_dmabuf(void) {
    ITEM("zwp_linux_dmabuf_v1");
    {
        OPEN_OR_SKIP("dmabuf.default_feedback");
        struct zwp_linux_dmabuf_v1 *m = bindv(c, &zwp_linux_dmabuf_v1_interface, 0);
        if (!m) { skip(item, "dmabuf.default_feedback", item, "not advertised"); cx_close(c); return; }
        uint32_t v = wl_proxy_get_version((struct wl_proxy *)m);
        if (v >= 4) {
            fb_done = fb_table = fb_main = fb_tranche_done = fb_formats = 0;
            struct zwp_linux_dmabuf_feedback_v1 *f = zwp_linux_dmabuf_v1_get_default_feedback(m);
            zwp_linux_dmabuf_feedback_v1_add_listener(f, &fb_l, NULL);
            cx_sync(c);
            int ok = fb_done == 1 && fb_table == 1 && fb_main == 1 && fb_tranche_done >= 1 && fb_formats >= 1;
            check(item, "dmabuf.default_feedback", ok, "zwp_linux_dmabuf_feedback_v1.done", "format_table %d, main_device %d, tranches %d (formats %d), done %d", fb_table, fb_main, fb_tranche_done, fb_formats, fb_done);
            zwp_linux_dmabuf_feedback_v1_destroy(f);
            fb_done = fb_table = fb_main = fb_tranche_done = fb_formats = 0;
            struct wl_surface *s = wl_compositor_create_surface(c->comp);
            f = zwp_linux_dmabuf_v1_get_surface_feedback(m, s);
            zwp_linux_dmabuf_feedback_v1_add_listener(f, &fb_l, NULL);
            cx_sync(c);
            ok = fb_done == 1 && fb_table == 1 && fb_main == 1 && fb_tranche_done >= 1;
            check(item, "dmabuf.surface_feedback", ok, "zwp_linux_dmabuf_v1.get_surface_feedback", "format_table %d, main_device %d, tranches %d, done %d", fb_table, fb_main, fb_tranche_done, fb_done);
        }
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("dmabuf.v3_modifiers");
        struct zwp_linux_dmabuf_v1 *m = bindv(c, &zwp_linux_dmabuf_v1_interface, 3);
        dm_mods = 0;
        zwp_linux_dmabuf_v1_add_listener(m, &dm_l, NULL);
        cx_sync(c);
        check(item, "dmabuf.v3_modifiers", dm_mods > 0, "zwp_linux_dmabuf_v1.modifier", "a v3 client gets modifier events: %d", dm_mods);
        cx_close(c);
    }
    struct { const char *id, *hint, *what; uint32_t code; } errs[] = {
        { "dmabuf.plane_idx", "zwp_linux_buffer_params_v1.error.plane_idx", "add with plane index 7", ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX },
        { "dmabuf.plane_set", "zwp_linux_buffer_params_v1.error.plane_set", "plane 0 added twice", ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET },
        { "dmabuf.incomplete", "zwp_linux_buffer_params_v1.error.incomplete", "create_immed with no planes", ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE },
        { "dmabuf.already_used", "zwp_linux_buffer_params_v1.error.already_used", "create twice on one params", ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED },
        { "dmabuf.invalid_dimensions", "zwp_linux_buffer_params_v1.error.invalid_dimensions", "create_immed 0×0", ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS },
    };
    for (size_t i = 0; i < sizeof errs / sizeof errs[0]; i++) {
        OPEN_OR_SKIP(errs[i].id);
        struct zwp_linux_dmabuf_v1 *m = bindv(c, &zwp_linux_dmabuf_v1_interface, 0);
        struct zwp_linux_buffer_params_v1 *p = zwp_linux_dmabuf_v1_create_params(m);
        int fd = shm_fd(64 * 64 * 4);
        switch (i) {
        case 0: zwp_linux_buffer_params_v1_add(p, fd, 7, 0, 256, 0, 0); break;
        case 1: zwp_linux_buffer_params_v1_add(p, fd, 0, 0, 256, 0, 0); zwp_linux_buffer_params_v1_add(p, fd, 0, 0, 256, 0, 0); break;
        case 2: zwp_linux_buffer_params_v1_create_immed(p, 64, 64, 0x34325241 /* AR24 */, 0); break;
        case 3: zwp_linux_buffer_params_v1_add(p, fd, 0, 0, 256, 0, 0); zwp_linux_buffer_params_v1_create(p, 64, 64, 0x34325241, 0); zwp_linux_buffer_params_v1_create(p, 64, 64, 0x34325241, 0); break;
        case 4: zwp_linux_buffer_params_v1_add(p, fd, 0, 0, 256, 0, 0); zwp_linux_buffer_params_v1_create_immed(p, 0, 0, 0x34325241, 0); break;
        }
        close(fd);
        int got = cx_expect(c, &zwp_linux_buffer_params_v1_interface, errs[i].code);
        check(item, errs[i].id, got, errs[i].hint, "%s: %s", errs[i].what, cx_errtext(c));
        cx_close(c);
    }
}
#endif

#ifdef CONF_HAVE_CURSOR_SHAPE_V1
static void f_cursor_shape(void) {
    ITEM("wp_cursor_shape_manager_v1");
    {
        OPEN_OR_SKIP("cursor_shape.set_shape");
        struct wp_cursor_shape_manager_v1 *m = bindv(c, &wp_cursor_shape_manager_v1_interface, 0);
        if (!m || !c->ptr) { skip(item, "cursor_shape.set_shape", item, "not advertised or no pointer"); cx_close(c); return; }
        struct wp_cursor_shape_device_v1 *d = wp_cursor_shape_manager_v1_get_pointer(m, c->ptr);
        wp_cursor_shape_device_v1_set_shape(d, c->ptr_serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
        uint32_t v = wl_proxy_get_version((struct wl_proxy *)m);
        if (v >= 2) wp_cursor_shape_device_v1_set_shape(d, c->ptr_serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE);
        wp_cursor_shape_device_v1_destroy(d);
        check(item, "cursor_shape.set_shape", cx_ok(c), "wp_cursor_shape_device_v1.set_shape", "get_pointer, set_shape (text%s), destroy: %s", v >= 2 ? ", all_resize" : "", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("cursor_shape.invalid_shape");
        struct wp_cursor_shape_manager_v1 *m = bindv(c, &wp_cursor_shape_manager_v1_interface, 0);
        struct wp_cursor_shape_device_v1 *d = wp_cursor_shape_manager_v1_get_pointer(m, c->ptr);
        wp_cursor_shape_device_v1_set_shape(d, 0, 9999);
        int ok = cx_expect(c, &wp_cursor_shape_device_v1_interface, WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE);
        check(item, "cursor_shape.invalid_shape", ok, "wp_cursor_shape_device_v1.error.invalid_shape", "set_shape(9999): %s", cx_errtext(c));
        cx_close(c);
    }
}
#endif

#ifdef CONF_HAVE_POINTER_CONSTRAINTS_UNSTABLE_V1
static void f_constraints(void) {
    ITEM("zwp_pointer_constraints_v1");
    {
        OPEN_OR_SKIP("constraints.lock_confine");
        struct zwp_pointer_constraints_v1 *m = bindv(c, &zwp_pointer_constraints_v1_interface, 0);
        if (!m || !c->ptr) { skip(item, "constraints.lock_confine", item, "not advertised or no pointer"); cx_close(c); return; }
        struct wl_surface *s = wl_compositor_create_surface(c->comp), *s2 = wl_compositor_create_surface(c->comp);
        struct zwp_locked_pointer_v1 *l = zwp_pointer_constraints_v1_lock_pointer(m, s, c->ptr, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        zwp_locked_pointer_v1_set_cursor_position_hint(l, wl_fixed_from_int(5), wl_fixed_from_int(5));
        zwp_locked_pointer_v1_set_region(l, NULL);
        struct zwp_confined_pointer_v1 *cf = zwp_pointer_constraints_v1_confine_pointer(m, s2, c->ptr, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
        zwp_confined_pointer_v1_set_region(cf, NULL);
        wl_surface_commit(s);
        zwp_locked_pointer_v1_destroy(l);
        zwp_confined_pointer_v1_destroy(cf);
        check(item, "constraints.lock_confine", cx_ok(c), "zwp_pointer_constraints_v1.lock_pointer", "lock, hint, region, confine, destroy: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("constraints.already_constrained");
        struct zwp_pointer_constraints_v1 *m = bindv(c, &zwp_pointer_constraints_v1_interface, 0);
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        zwp_pointer_constraints_v1_lock_pointer(m, s, c->ptr, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        zwp_pointer_constraints_v1_confine_pointer(m, s, c->ptr, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        int ok = cx_expect(c, &zwp_pointer_constraints_v1_interface, ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED);
        check(item, "constraints.already_constrained", ok, "zwp_pointer_constraints_v1.error.already_constrained", "a second constraint on the same surface and pointer: %s", cx_errtext(c));
        cx_close(c);
    }
}
#endif

#ifdef CONF_HAVE_RELATIVE_POINTER_UNSTABLE_V1
static void f_relative(void) {
    ITEM("zwp_relative_pointer_manager_v1");
    OPEN_OR_SKIP("relative.get");
    struct zwp_relative_pointer_manager_v1 *m = bindv(c, &zwp_relative_pointer_manager_v1_interface, 0);
    if (!m || !c->ptr) { skip(item, "relative.get", item, "not advertised or no pointer"); cx_close(c); return; }
    struct zwp_relative_pointer_v1 *r = zwp_relative_pointer_manager_v1_get_relative_pointer(m, c->ptr);
    zwp_relative_pointer_v1_destroy(r);
    zwp_relative_pointer_manager_v1_destroy(m);
    check(item, "relative.get", cx_ok(c), "zwp_relative_pointer_manager_v1.get_relative_pointer", "get_relative_pointer and destroy: %s", cx_errtext(c));
    cx_close(c);
}
#endif

#ifdef CONF_HAVE_POINTER_GESTURES_UNSTABLE_V1
static void f_gestures(void) {
    ITEM("zwp_pointer_gestures_v1");
    OPEN_OR_SKIP("gestures.get");
    struct zwp_pointer_gestures_v1 *m = bindv(c, &zwp_pointer_gestures_v1_interface, 0);
    if (!m || !c->ptr) { skip(item, "gestures.get", item, "not advertised or no pointer"); cx_close(c); return; }
    uint32_t v = wl_proxy_get_version((struct wl_proxy *)m);
    zwp_pointer_gesture_swipe_v1_destroy(zwp_pointer_gestures_v1_get_swipe_gesture(m, c->ptr));
    zwp_pointer_gesture_pinch_v1_destroy(zwp_pointer_gestures_v1_get_pinch_gesture(m, c->ptr));
    if (v >= 3) zwp_pointer_gesture_hold_v1_destroy(zwp_pointer_gestures_v1_get_hold_gesture(m, c->ptr));
    if (v >= 2) zwp_pointer_gestures_v1_release(m);
    check(item, "gestures.get", cx_ok(c), "zwp_pointer_gestures_v1.get_swipe_gesture", "swipe, pinch%s and their destroy: %s", v >= 3 ? ", hold" : "", cx_errtext(c));
    cx_close(c);
}
#endif

#ifdef CONF_HAVE_IDLE_INHIBIT_UNSTABLE_V1
static void f_idle_inhibit(void) {
    ITEM("zwp_idle_inhibit_manager_v1");
    OPEN_OR_SKIP("idle_inhibit.create");
    struct zwp_idle_inhibit_manager_v1 *m = bindv(c, &zwp_idle_inhibit_manager_v1_interface, 0);
    if (!m) { skip(item, "idle_inhibit.create", item, "not advertised"); cx_close(c); return; }
    struct wl_surface *s = wl_compositor_create_surface(c->comp);
    struct zwp_idle_inhibitor_v1 *i = zwp_idle_inhibit_manager_v1_create_inhibitor(m, s);
    cx_sync(c);
    zwp_idle_inhibitor_v1_destroy(i);
    zwp_idle_inhibit_manager_v1_destroy(m);
    check(item, "idle_inhibit.create", cx_ok(c), "zwp_idle_inhibit_manager_v1.create_inhibitor", "create_inhibitor and destroy: %s", cx_errtext(c));
    cx_close(c);
}
#endif

#ifdef CONF_HAVE_KEYBOARD_SHORTCUTS_INHIBIT_UNSTABLE_V1
static void f_ksi(void) {
    ITEM("zwp_keyboard_shortcuts_inhibit_manager_v1");
    {
        OPEN_OR_SKIP("ksi.inhibit");
        struct zwp_keyboard_shortcuts_inhibit_manager_v1 *m = bindv(c, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 0);
        if (!m) { skip(item, "ksi.inhibit", item, "not advertised"); cx_close(c); return; }
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        struct zwp_keyboard_shortcuts_inhibitor_v1 *i = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(m, s, c->seat);
        cx_sync(c);
        zwp_keyboard_shortcuts_inhibitor_v1_destroy(i);
        check(item, "ksi.inhibit", cx_ok(c), "zwp_keyboard_shortcuts_inhibit_manager_v1.inhibit_shortcuts", "inhibit_shortcuts and destroy: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("ksi.already_inhibited");
        struct zwp_keyboard_shortcuts_inhibit_manager_v1 *m = bindv(c, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 0);
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(m, s, c->seat);
        zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(m, s, c->seat);
        int ok = cx_expect(c, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, ZWP_KEYBOARD_SHORTCUTS_INHIBIT_MANAGER_V1_ERROR_ALREADY_INHIBITED);
        check(item, "ksi.already_inhibited", ok, "zwp_keyboard_shortcuts_inhibit_manager_v1.error.already_inhibited", "twice for one surface and seat: %s", cx_errtext(c));
        cx_close(c);
    }
}
#endif

#ifdef CONF_HAVE_XDG_ACTIVATION_V1
static int tok_done;
static char tok_text[256];
static void tok_donef(void *d, struct xdg_activation_token_v1 *t, const char *tok) { (void)d; (void)t; snprintf(tok_text, sizeof tok_text, "%s", tok); tok_done++; }
static const struct xdg_activation_token_v1_listener tok_l = { tok_donef };
static void f_activation(void) {
    ITEM("xdg_activation_v1");
    {
        OPEN_OR_SKIP("activation.token");
        struct xdg_activation_v1 *a = bindv(c, &xdg_activation_v1_interface, 0);
        if (!a) { skip(item, "activation.token", item, "not advertised"); cx_close(c); return; }
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        struct xdg_activation_token_v1 *t = xdg_activation_v1_get_activation_token(a);
        tok_done = 0;
        xdg_activation_token_v1_add_listener(t, &tok_l, NULL);
        xdg_activation_token_v1_set_app_id(t, "org.lordos.wlconformance");
        xdg_activation_token_v1_set_surface(t, s);
        xdg_activation_token_v1_set_serial(t, c->kb_serial, c->seat);
        xdg_activation_token_v1_commit(t);
        cx_wait(c, &tok_done, 2000);
        check(item, "activation.token", tok_done == 1 && tok_text[0], "xdg_activation_token_v1.done", "commit is answered with done(token): %s", tok_done ? "yes" : "no");
        xdg_activation_v1_activate(a, "not-a-token-we-were-given", s);
        xdg_activation_v1_activate(a, tok_text, s);
        xdg_activation_token_v1_destroy(t);
        check(item, "activation.activate", cx_ok(c), "xdg_activation_v1.activate", "activate with a bogus and a real token (no error, maybe no effect): %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("activation.already_used");
        struct xdg_activation_v1 *a = bindv(c, &xdg_activation_v1_interface, 0);
        struct xdg_activation_token_v1 *t = xdg_activation_v1_get_activation_token(a);
        xdg_activation_token_v1_commit(t);
        xdg_activation_token_v1_set_app_id(t, "x");
        int ok = cx_expect(c, &xdg_activation_token_v1_interface, XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED);
        check(item, "activation.already_used", ok, "xdg_activation_token_v1.error.already_used", "a request on a committed token: %s", cx_errtext(c));
        cx_close(c);
    }
}
#endif

#if defined(CONF_HAVE_XDG_FOREIGN_UNSTABLE_V2) && defined(CONF_HAVE_XDG_SHELL)
static int exp_handle, imp_destroyed;
static char handle[256];
static void exp_h(void *d, struct zxdg_exported_v2 *e, const char *h) { (void)d; (void)e; snprintf(handle, sizeof handle, "%s", h); exp_handle++; }
static const struct zxdg_exported_v2_listener exp_l = { exp_h };
static void imp_d(void *d, struct zxdg_imported_v2 *i) { (void)d; (void)i; imp_destroyed++; }
static const struct zxdg_imported_v2_listener imp_l = { imp_d };
static void f_foreign(void) {
    ITEM("zxdg_exporter_v2");
    static const char *const imp = "zxdg_importer_v2";
    {
        OPEN_OR_SKIP("foreign.export_import");
        struct zxdg_exporter_v2 *ex = bindv(c, &zxdg_exporter_v2_interface, 0);
        struct zxdg_importer_v2 *im = bindv(c, &zxdg_importer_v2_interface, 0);
        Win w, d;
        if (!ex || !im || win_map(c, &w, "exported", 120, 90) || win_role(c, &d, "dialog")) { skip(item, "foreign.export_import", item, "not advertised or no toplevel"); cx_close(c); return; }
        exp_handle = imp_destroyed = 0;
        struct zxdg_exported_v2 *e = zxdg_exporter_v2_export_toplevel(ex, w.s);
        zxdg_exported_v2_add_listener(e, &exp_l, NULL);
        cx_wait(c, &exp_handle, 2000);
        check(item, "foreign.handle", exp_handle == 1 && handle[0], "zxdg_exported_v2.handle", "export_toplevel gives a handle: %s", exp_handle ? "yes" : "no");
        struct zxdg_imported_v2 *i = zxdg_importer_v2_import_toplevel(im, handle);
        zxdg_imported_v2_add_listener(i, &imp_l, NULL);
        zxdg_imported_v2_set_parent_of(i, d.s);
        cx_sync(c);
        check(imp, "foreign.set_parent_of", cx_ok(c) && !imp_destroyed, "zxdg_imported_v2.set_parent_of", "import and set_parent_of a toplevel: %s", cx_errtext(c));
        struct zxdg_imported_v2 *bad = zxdg_importer_v2_import_toplevel(im, "no-such-handle");
        int before = imp_destroyed;
        zxdg_imported_v2_add_listener(bad, &imp_l, NULL);
        cx_wait(c, NULL, 300);
        check(imp, "foreign.bad_handle", imp_destroyed > before, "zxdg_imported_v2.destroyed", "importing an unknown handle is answered with destroyed: %s", imp_destroyed > before ? "yes" : "no");
        zxdg_imported_v2_destroy(i);
        zxdg_exported_v2_destroy(e);
        check(item, "foreign.destroy", cx_ok(c), "zxdg_exported_v2.destroy", "destroy: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("foreign.exporter_invalid_surface");
        struct zxdg_exporter_v2 *ex = bindv(c, &zxdg_exporter_v2_interface, 0);
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        zxdg_exporter_v2_export_toplevel(ex, s);
        int ok = cx_expect(c, &zxdg_exporter_v2_interface, ZXDG_EXPORTER_V2_ERROR_INVALID_SURFACE);
        check(item, "foreign.exporter_invalid_surface", ok, "zxdg_exporter_v2.error.invalid_surface", "exporting a surface that isn't a toplevel: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("foreign.imported_invalid_surface");
        (void)item;
        struct zxdg_exporter_v2 *ex = bindv(c, &zxdg_exporter_v2_interface, 0);
        struct zxdg_importer_v2 *im = bindv(c, &zxdg_importer_v2_interface, 0);
        Win w;
        if (win_map(c, &w, "exported2", 120, 90)) { skip(item, "foreign.imported_invalid_surface", "zxdg_imported_v2.error.invalid_surface", "no toplevel"); cx_close(c); return; }
        exp_handle = 0;
        struct zxdg_exported_v2 *e = zxdg_exporter_v2_export_toplevel(ex, w.s);
        zxdg_exported_v2_add_listener(e, &exp_l, NULL);
        cx_wait(c, &exp_handle, 2000);
        struct zxdg_imported_v2 *i = zxdg_importer_v2_import_toplevel(im, handle);
        zxdg_imported_v2_set_parent_of(i, wl_compositor_create_surface(c->comp));
        int ok = cx_expect(c, &zxdg_imported_v2_interface, ZXDG_IMPORTED_V2_ERROR_INVALID_SURFACE);
        check(imp, "foreign.imported_invalid_surface", ok, "zxdg_imported_v2.error.invalid_surface", "set_parent_of a surface that isn't a toplevel: %s", cx_errtext(c));
        cx_close(c);
    }
}
#endif

#if defined(CONF_HAVE_XDG_DIALOG_V1) && defined(CONF_HAVE_XDG_SHELL)
static void f_dialog(void) {
    ITEM("xdg_wm_dialog_v1");
    {
        OPEN_OR_SKIP("dialog.modal");
        struct xdg_wm_dialog_v1 *m = bindv(c, &xdg_wm_dialog_v1_interface, 0);
        Win w;
        if (!m || win_role(c, &w, "dialog")) { skip(item, "dialog.modal", item, "not advertised or no toplevel"); cx_close(c); return; }
        struct xdg_dialog_v1 *d = xdg_wm_dialog_v1_get_xdg_dialog(m, w.top);
        xdg_dialog_v1_set_modal(d);
        xdg_dialog_v1_unset_modal(d);
        xdg_dialog_v1_destroy(d);
        check(item, "dialog.modal", cx_ok(c), "xdg_dialog_v1.set_modal", "get_xdg_dialog, set_modal, unset_modal, destroy: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("dialog.already_used");
        struct xdg_wm_dialog_v1 *m = bindv(c, &xdg_wm_dialog_v1_interface, 0);
        Win w;
        if (win_role(c, &w, "dialog2")) { skip(item, "dialog.already_used", "xdg_wm_dialog_v1.error.already_used", "no toplevel"); cx_close(c); return; }
        xdg_wm_dialog_v1_get_xdg_dialog(m, w.top);
        xdg_wm_dialog_v1_get_xdg_dialog(m, w.top);
        int ok = cx_expect(c, &xdg_wm_dialog_v1_interface, XDG_WM_DIALOG_V1_ERROR_ALREADY_USED);
        check(item, "dialog.already_used", ok, "xdg_wm_dialog_v1.error.already_used", "two dialog objects for one toplevel: %s", cx_errtext(c));
        cx_close(c);
    }
}
#endif

#if defined(CONF_HAVE_XDG_TOPLEVEL_ICON_V1) && defined(CONF_HAVE_XDG_SHELL)
static int icon_sizes, icon_done;
static void ic_size(void *d, struct xdg_toplevel_icon_manager_v1 *m, int32_t s) { (void)d; (void)m; if (s > 0) icon_sizes++; }
static void ic_done(void *d, struct xdg_toplevel_icon_manager_v1 *m) { (void)d; (void)m; icon_done++; }
static const struct xdg_toplevel_icon_manager_v1_listener ic_l = { ic_size, ic_done };
static void f_icon(void) {
    ITEM("xdg_toplevel_icon_manager_v1");
    {
        OPEN_OR_SKIP("icon.sizes");
        icon_sizes = icon_done = 0;
        struct xdg_toplevel_icon_manager_v1 *m = bindv(c, &xdg_toplevel_icon_manager_v1_interface, 0);
        if (!m) { skip(item, "icon.sizes", item, "not advertised"); cx_close(c); return; }
        xdg_toplevel_icon_manager_v1_add_listener(m, &ic_l, NULL);
        cx_sync(c);
        check(item, "icon.sizes", icon_done == 1, "xdg_toplevel_icon_manager_v1.done", "icon_size (%d) and one done on bind (%d)", icon_sizes, icon_done);
        Win w;
        if (win_map(c, &w, "icon", 100, 80)) { skip(item, "icon.set", "xdg_toplevel_icon_manager_v1.set_icon", "no toplevel"); cx_close(c); return; }
        struct xdg_toplevel_icon_v1 *i = xdg_toplevel_icon_manager_v1_create_icon(m);
        xdg_toplevel_icon_v1_set_name(i, "utilities-terminal");
        xdg_toplevel_icon_v1_add_buffer(i, shm_buffer(c, 48, 48, 0xffff0000), 1);
        xdg_toplevel_icon_manager_v1_set_icon(m, w.top, i);
        xdg_toplevel_icon_manager_v1_set_icon(m, w.top, NULL);
        xdg_toplevel_icon_v1_destroy(i);
        check(item, "icon.set", cx_ok(c), "xdg_toplevel_icon_manager_v1.set_icon", "create_icon, set_name, add_buffer, set_icon, unset, destroy: %s", cx_errtext(c));
        cx_close(c);
    }
    for (int k = 0; k < 2; k++) {
        OPEN_OR_SKIP(k ? "icon.immutable" : "icon.invalid_buffer");
        struct xdg_toplevel_icon_manager_v1 *m = bindv(c, &xdg_toplevel_icon_manager_v1_interface, 0);
        struct xdg_toplevel_icon_v1 *i = xdg_toplevel_icon_manager_v1_create_icon(m);
        uint32_t code;
        if (!k) { xdg_toplevel_icon_v1_add_buffer(i, shm_buffer(c, 48, 32, 0xffff0000), 1); code = XDG_TOPLEVEL_ICON_V1_ERROR_INVALID_BUFFER; }
        else {
            Win w;
            if (win_role(c, &w, "icon-immutable")) { skip(item, "icon.immutable", "xdg_toplevel_icon_v1.error.immutable", "no toplevel"); cx_close(c); continue; }
            xdg_toplevel_icon_v1_add_buffer(i, shm_buffer(c, 32, 32, 0xffff0000), 1);
            xdg_toplevel_icon_manager_v1_set_icon(m, w.top, i);
            xdg_toplevel_icon_v1_set_name(i, "x");
            code = XDG_TOPLEVEL_ICON_V1_ERROR_IMMUTABLE;
        }
        int ok = cx_expect(c, &xdg_toplevel_icon_v1_interface, code);
        check(item, k ? "icon.immutable" : "icon.invalid_buffer", ok, k ? "xdg_toplevel_icon_v1.error.immutable" : "xdg_toplevel_icon_v1.error.invalid_buffer",
              "%s: %s", k ? "changing an icon after set_icon" : "a 48×32 (not square) buffer", cx_errtext(c));
        cx_close(c);
    }
}
#endif

#ifdef CONF_HAVE_EXT_IDLE_NOTIFY_V1
static void f_idle_notify(void) {
    ITEM("ext_idle_notifier_v1");
    OPEN_OR_SKIP("idle_notify.get");
    struct ext_idle_notifier_v1 *m = bindv(c, &ext_idle_notifier_v1_interface, 0);
    if (!m) { skip(item, "idle_notify.get", item, "not advertised"); cx_close(c); return; }
    struct ext_idle_notification_v1 *n = ext_idle_notifier_v1_get_idle_notification(m, 5000, c->seat);
    cx_sync(c);
    ext_idle_notification_v1_destroy(n);
    ext_idle_notifier_v1_destroy(m);
    check(item, "idle_notify.get", cx_ok(c), "ext_idle_notifier_v1.get_idle_notification", "get_idle_notification and destroy: %s", cx_errtext(c));
    cx_close(c);
}
#endif

#if defined(CONF_HAVE_TEXT_INPUT_UNSTABLE_V3) && defined(CONF_HAVE_XDG_SHELL)
static int ti_enter;
static void ti_e(void *d, struct zwp_text_input_v3 *t, struct wl_surface *s) { (void)d; (void)t; (void)s; ti_enter++; }
static void ti_l(void *d, struct zwp_text_input_v3 *t, struct wl_surface *s) { (void)d; (void)t; (void)s; }
static void ti_pre(void *d, struct zwp_text_input_v3 *t, const char *x, int32_t a, int32_t b) { (void)d; (void)t; (void)x; (void)a; (void)b; }
static void ti_commit(void *d, struct zwp_text_input_v3 *t, const char *x) { (void)d; (void)t; (void)x; }
static void ti_del(void *d, struct zwp_text_input_v3 *t, uint32_t a, uint32_t b) { (void)d; (void)t; (void)a; (void)b; }
static void ti_done(void *d, struct zwp_text_input_v3 *t, uint32_t s) { (void)d; (void)t; (void)s; }
static const struct zwp_text_input_v3_listener ti_listener = { ti_e, ti_l, ti_pre, ti_commit, ti_del, ti_done };
static void f_text_input(void) {
    ITEM("zwp_text_input_manager_v3");
    OPEN_OR_SKIP("text_input.flow");
    struct zwp_text_input_manager_v3 *m = bindv(c, &zwp_text_input_manager_v3_interface, 0);
    if (!m) { skip(item, "text_input.flow", item, "not advertised"); cx_close(c); return; }
    ti_enter = 0;
    struct zwp_text_input_v3 *t = zwp_text_input_manager_v3_get_text_input(m, c->seat);
    zwp_text_input_v3_add_listener(t, &ti_listener, NULL);
    Win w;
    if (win_map(c, &w, "text-input", 100, 80)) { skip(item, "text_input.flow", "zwp_text_input_v3.enable", "no toplevel"); cx_close(c); return; }
    cx_wait(c, &ti_enter, 2000);
    cx_wait(c, &c->kb_enters, 1000);
    check(item, "text_input.enter", ti_enter >= 1, "zwp_text_input_v3.enter", "enter when the surface gets the keyboard: %d (keyboard enters: %d)", ti_enter, c->kb_enters);
    zwp_text_input_v3_enable(t);
    zwp_text_input_v3_set_surrounding_text(t, "hello", 5, 5);
    zwp_text_input_v3_set_text_change_cause(t, ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_OTHER);
    zwp_text_input_v3_set_content_type(t, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
    zwp_text_input_v3_set_cursor_rectangle(t, 10, 10, 1, 16);
    zwp_text_input_v3_commit(t);
    zwp_text_input_v3_disable(t);
    zwp_text_input_v3_commit(t);
    zwp_text_input_v3_destroy(t);
    check(item, "text_input.flow", cx_ok(c), "zwp_text_input_v3.commit", "enable, surrounding text, content type, cursor rectangle, commit, disable: %s", cx_errtext(c));
    cx_close(c);
}
#endif

#ifdef CONF_HAVE_SECURITY_CONTEXT_V1
static void f_security_context(void) {
    ITEM("wp_security_context_manager_v1");
    char path[96];
    snprintf(path, sizeof path, "%.60s/wlconf-secctx-%d", getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/tmp", (int)getpid());
    {
        OPEN_OR_SKIP("secctx.create");
        struct wp_security_context_manager_v1 *m = bindv(c, &wp_security_context_manager_v1_interface, 0);
        if (!m) { skip(item, "secctx.create", item, "not advertised"); cx_close(c); return; }
        int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
        unlink(path);
        if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof a) || listen(lfd, 4)) { skip(item, "secctx.create", item, "no socket"); cx_close(c); return; }
        int cp[2];
        if (pipe2(cp, O_CLOEXEC)) { skip(item, "secctx.create", item, "no pipe"); cx_close(c); return; }
        struct wp_security_context_v1 *s = wp_security_context_manager_v1_create_listener(m, lfd, cp[1]);
        wp_security_context_v1_set_sandbox_engine(s, "org.lordos.wlconformance");
        wp_security_context_v1_set_app_id(s, "org.lordos.wlconformance.sandboxed");
        wp_security_context_v1_set_instance_id(s, "1");
        wp_security_context_v1_commit(s);
        close(lfd);
        int ok = cx_ok(c);
        check(item, "secctx.create", ok, "wp_security_context_manager_v1.create_listener", ok ? "create_listener, engine, app id, instance id, commit" : "%s", cx_errtext(c));
        if (ok) {
            /* a client of that socket: connects, and doesn't get the manager (or can't nest) */
            int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            int con = fd >= 0 && !connect(fd, (struct sockaddr *)&a, sizeof a);
            struct wl_display *sd = con ? wl_display_connect_to_fd(fd) : NULL;
            int seen_mgr = -1, nglobals = 0;
            if (sd) {
                Conn *sc = calloc(1, sizeof *sc);
                sc->d = sd;
                sc->reg = wl_display_get_registry(sd);
                wl_registry_add_listener(sc->reg, &reg_l, sc);
                double end = now_ms() + 2000;
                while (wl_display_roundtrip(sd) >= 0 && !sc->ng && now_ms() < end) {}
                nglobals = sc->ng;
                seen_mgr = adv(sc, "wp_security_context_manager_v1") ? 1 : 0;
                wl_display_disconnect(sd);
                free(sc);
            }
            check(item, "secctx.client_connects", sd && nglobals > 0, "wp_security_context_v1.commit", "a client on the context's socket gets globals: %d", nglobals);
            check(item, "secctx.no_nesting", seen_mgr == 0, "wp_security_context_manager_v1.error.nested", "the manager is hidden from a sandboxed client: %s", seen_mgr == 0 ? "yes" : seen_mgr < 0 ? "no connection" : "it is advertised");
        }
        wp_security_context_v1_destroy(s);
        close(cp[0]);
        close(cp[1]);
        unlink(path);
        cx_close(c);
    }
    for (int k = 0; k < 3; k++) {
        static const char *ids[] = { "secctx.invalid_listen_fd", "secctx.already_set", "secctx.already_used" };
        OPEN_OR_SKIP(ids[k]);
        struct wp_security_context_manager_v1 *m = bindv(c, &wp_security_context_manager_v1_interface, 0);
        int lfd = k ? socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0) : shm_fd(16);
        if (k) {
            struct sockaddr_un a = { .sun_family = AF_UNIX };
            snprintf(a.sun_path, sizeof a.sun_path, "%s.%d", path, k);
            unlink(a.sun_path);
            if (bind(lfd, (struct sockaddr *)&a, sizeof a) == 0) listen(lfd, 4);
            unlink(a.sun_path);
        }
        int cp[2];
        if (pipe2(cp, O_CLOEXEC)) { cx_close(c); continue; }
        struct wp_security_context_v1 *s = wp_security_context_manager_v1_create_listener(m, lfd, cp[1]);
        const struct wl_interface *want = &wp_security_context_v1_interface;
        uint32_t code = 0;
        if (k == 0) { want = &wp_security_context_manager_v1_interface; code = WP_SECURITY_CONTEXT_MANAGER_V1_ERROR_INVALID_LISTEN_FD; }
        else if (k == 1) { wp_security_context_v1_set_app_id(s, "a"); wp_security_context_v1_set_app_id(s, "b"); code = WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_SET; }
        else { wp_security_context_v1_commit(s); wp_security_context_v1_set_app_id(s, "late"); code = WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_USED; }
        int ok = cx_expect(c, want, code);
        check(item, ids[k], ok, k == 0 ? "wp_security_context_manager_v1.error.invalid_listen_fd" : k == 1 ? "wp_security_context_v1.error.already_set" : "wp_security_context_v1.error.already_used",
              "%s: %s", k == 0 ? "a memfd as the listening socket" : k == 1 ? "set_app_id twice" : "a request after commit", cx_errtext(c));
        close(lfd);
        close(cp[0]);
        close(cp[1]);
        cx_close(c);
    }
}
#endif

/* org_kde_kwin_server_decoration_manager: no XML on the system; the tables written by hand as in wl.c */
static const struct wl_interface kde_deco_iface;
static const struct wl_interface *kde_types[] = { NULL, &kde_deco_iface, &wl_surface_interface };
static const struct wl_message kde_mgr_req[] = { { "create", "no", kde_types + 1 } };
static const struct wl_message kde_mgr_ev[] = { { "default_mode", "u", kde_types } };
static const struct wl_interface kde_mgr_iface = { "org_kde_kwin_server_decoration_manager", 1, 1, kde_mgr_req, 1, kde_mgr_ev };
static const struct wl_message kde_deco_req[] = { { "release", "", kde_types }, { "request_mode", "u", kde_types } };
static const struct wl_message kde_deco_ev[] = { { "mode", "u", kde_types } };
static const struct wl_interface kde_deco_iface = { "org_kde_kwin_server_decoration", 1, 2, kde_deco_req, 1, kde_deco_ev };
static int kde_default_n, kde_mode_n;
static uint32_t kde_mode;
static void kde_default(void *d, struct wl_proxy *p, uint32_t m) { (void)d; (void)p; (void)m; kde_default_n++; }
static void kde_modef(void *d, struct wl_proxy *p, uint32_t m) { (void)d; (void)p; kde_mode = m; kde_mode_n++; }
static void (*const kde_mgr_l[])(void *, struct wl_proxy *, uint32_t) = { kde_default };
static void (*const kde_deco_l[])(void *, struct wl_proxy *, uint32_t) = { kde_modef };
static void f_kde(void) {
    ITEM("org_kde_kwin_server_decoration_manager");
    OPEN_OR_SKIP("kde.default_mode");
    struct wl_proxy *m = bindv(c, &kde_mgr_iface, 0);
    if (!m) { skip(item, "kde.default_mode", item, "not advertised"); cx_close(c); return; }
    kde_default_n = kde_mode_n = 0;
    wl_proxy_add_listener(m, (void (**)(void))kde_mgr_l, NULL);
    cx_sync(c);
    check(item, "kde.default_mode", kde_default_n == 1, "org_kde_kwin_server_decoration_manager.default_mode", "default_mode on bind: %d", kde_default_n);
    struct wl_surface *s = wl_compositor_create_surface(c->comp);
    struct wl_proxy *d = wl_proxy_marshal_flags(m, 0, &kde_deco_iface, 1, 0, NULL, s);
    wl_proxy_add_listener(d, (void (**)(void))kde_deco_l, NULL);
    cx_sync(c);
    check(item, "kde.mode_on_create", kde_mode_n >= 1, "org_kde_kwin_server_decoration.mode", "mode when created: %d", kde_mode_n);
    int before = kde_mode_n;
    wl_proxy_marshal_flags(d, 1, NULL, 1, 0, 1u /* client */);
    cx_sync(c);
    check(item, "kde.request_mode", kde_mode_n > before && kde_mode == 1, "org_kde_kwin_server_decoration.request_mode", "request_mode(client) is answered with mode(client): mode %u", kde_mode);
    wl_proxy_marshal_flags(d, 0, NULL, 1, WL_MARSHAL_FLAG_DESTROY);
    check(item, "kde.release", cx_ok(c), "org_kde_kwin_server_decoration.release", "release: %s", cx_errtext(c));
    cx_close(c);
}

/* ---------------------------------------------------------------- selections for the X11 checks */
/* clip-read <mime> <bytes>: maps a window, waits for the keyboard and for a selection offering <mime>, reads it and
 * prints "CLIP <n> <sum>" (sum: a simple 32-bit FNV-1a); exit 0 if it read <bytes> bytes */
static uint32_t fnv(const unsigned char *p, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; } return h; }
static int clip_read(const char *mime, long want, int primary) {
#ifdef CONF_HAVE_XDG_SHELL
    Conn *c = cx_open();
    if (!c) return 2;
    struct wl_data_device_manager *m = bindv(c, &wl_data_device_manager_interface, 0);
    Win w;
    if (win_map(c, &w, "clip-read", 120, 90)) return 2;
    cx_wait(c, &c->kb_enters, 3000);
    char *buf = malloc((size_t)want + 65536);
    long n = -1;
    if (!primary) {
        struct wl_data_device *dd = wl_data_device_manager_get_data_device(m, c->seat);
        sel_n = 0;
        sel_offer = NULL;
        wl_data_device_add_listener(dd, &dd_l, NULL);
        double end = now_ms() + 4000;
        while (!sel_offer && now_ms() < end) cx_wait(c, NULL, 50);
        if (sel_offer) n = read_offer(c, sel_offer, mime, buf, (size_t)want + 65536, 8000);
    }
#ifdef CONF_HAVE_PRIMARY_SELECTION_UNSTABLE_V1
    else {
        (void)m;
        struct zwp_primary_selection_device_manager_v1 *pm = bindv(c, &zwp_primary_selection_device_manager_v1_interface, 0);
        struct zwp_primary_selection_device_v1 *dv = zwp_primary_selection_device_manager_v1_get_device(pm, c->seat);
        psel_offer = NULL;
        zwp_primary_selection_device_v1_add_listener(dv, &ps_l, NULL);
        double end = now_ms() + 4000;
        while (!psel_offer && now_ms() < end) cx_wait(c, NULL, 50);
        if (psel_offer) n = read_from(c, recv_primary, psel_offer, mime, buf, (size_t)want + 65536, 8000);
    }
#endif
    printf("CLIP %ld %08x focus=%d\n", n, n > 0 ? fnv((unsigned char *)buf, (size_t)n) : 0, c->kb_focus == w.s);
    fflush(stdout);
    cx_close(c);
    return n == want ? 0 : 1;
#else
    (void)mime; (void)want; (void)primary;
    return 2;
#endif
}

/* clip-own <bytes>: maps a window, takes the keyboard, owns the clipboard with <bytes> bytes of a known pattern as
 * text/plain;charset=utf-8 and serves it for 15 s (or until cancelled); prints "OWNED <sum>" */
static char *own_data;
static size_t own_len;
static int own_cancel, own_served;
static void own_send(void *d, struct wl_data_source *s, const char *mime, int fd) {
    (void)d; (void)s; (void)mime;
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    size_t off = 0;
    while (off < own_len) { ssize_t w = write(fd, own_data + off, own_len - off); if (w <= 0) break; off += (size_t)w; }
    close(fd);
    own_served++;
}
static void own_cancelled(void *d, struct wl_data_source *s) { (void)d; (void)s; own_cancel = 1; }
static const struct wl_data_source_listener own_l = { src_target, own_send, own_cancelled, NULL, NULL, NULL };
static int clip_own(long bytes) {
#ifdef CONF_HAVE_XDG_SHELL
    Conn *c = cx_open();
    if (!c) return 2;
    struct wl_data_device_manager *m = bindv(c, &wl_data_device_manager_interface, 0);
    Win w;
    if (win_map(c, &w, "clip-own", 120, 90)) return 2;
    if (cx_wait(c, &c->kb_enters, 3000) != 1) { printf("OWNED none (no keyboard)\n"); fflush(stdout); return 1; }
    own_len = (size_t)bytes;
    own_data = malloc(own_len);
    for (size_t i = 0; i < own_len; i++) own_data[i] = (char)('a' + i % 26);
    struct wl_data_device *dd = wl_data_device_manager_get_data_device(m, c->seat);
    struct wl_data_source *s = wl_data_device_manager_create_data_source(m);
    wl_data_source_add_listener(s, &own_l, NULL);
    wl_data_source_offer(s, "text/plain;charset=utf-8");
    wl_data_source_offer(s, "UTF8_STRING");
    wl_data_device_set_selection(dd, s, c->kb_serial);
    cx_sync(c);
    printf("OWNED %08x\n", fnv((unsigned char *)own_data, own_len));
    fflush(stdout);
    double end = now_ms() + 15000;
    while (!own_cancel && now_ms() < end) if (cx_wait(c, &own_cancel, 200) < 0) break;
    printf("SERVED %d\n", own_served);
    cx_close(c);
    return 0;
#else
    (void)bytes;
    return 2;
#endif
}

/* ---------------------------------------------------------------- main */
static const struct { const char *global; void (*fn)(void); } flows[] = {
    { "wl_compositor", f_compositor },
#ifdef CONF_HAVE_XDG_SHELL
    { "wl_compositor", f_surface_events },
#endif
    { "wl_subcompositor", f_subcompositor },
    { "wl_shm", f_shm },
    { "wl_output", f_output },
#ifdef CONF_HAVE_XDG_OUTPUT_UNSTABLE_V1
    { "zxdg_output_manager_v1", f_xdg_output },
#endif
    { "wl_seat", f_seat },
    { "wl_data_device_manager", f_data_device },
#ifdef CONF_HAVE_PRIMARY_SELECTION_UNSTABLE_V1
    { "zwp_primary_selection_device_manager_v1", f_primary },
#endif
#ifdef CONF_HAVE_XDG_SHELL
    { "xdg_wm_base", f_xdg_shell },
#endif
#ifdef CONF_HAVE_XDG_DECORATION_UNSTABLE_V1
    { "zxdg_decoration_manager_v1", f_decoration },
#endif
#ifdef CONF_HAVE_VIEWPORTER
    { "wp_viewporter", f_viewporter },
#endif
#ifdef CONF_HAVE_FRACTIONAL_SCALE_V1
    { "wp_fractional_scale_manager_v1", f_fractional },
#endif
#ifdef CONF_HAVE_SINGLE_PIXEL_BUFFER_V1
    { "wp_single_pixel_buffer_manager_v1", f_spb },
#endif
#ifdef CONF_HAVE_ALPHA_MODIFIER_V1
    { "wp_alpha_modifier_v1", f_alpha },
#endif
#ifdef CONF_HAVE_PRESENTATION_TIME
    { "wp_presentation", f_presentation },
#endif
#ifdef CONF_HAVE_LINUX_DMABUF_V1
    { "zwp_linux_dmabuf_v1", f_dmabuf },
#endif
#ifdef CONF_HAVE_CURSOR_SHAPE_V1
    { "wp_cursor_shape_manager_v1", f_cursor_shape },
#endif
#ifdef CONF_HAVE_POINTER_CONSTRAINTS_UNSTABLE_V1
    { "zwp_pointer_constraints_v1", f_constraints },
#endif
#ifdef CONF_HAVE_RELATIVE_POINTER_UNSTABLE_V1
    { "zwp_relative_pointer_manager_v1", f_relative },
#endif
#ifdef CONF_HAVE_POINTER_GESTURES_UNSTABLE_V1
    { "zwp_pointer_gestures_v1", f_gestures },
#endif
#ifdef CONF_HAVE_IDLE_INHIBIT_UNSTABLE_V1
    { "zwp_idle_inhibit_manager_v1", f_idle_inhibit },
#endif
#ifdef CONF_HAVE_KEYBOARD_SHORTCUTS_INHIBIT_UNSTABLE_V1
    { "zwp_keyboard_shortcuts_inhibit_manager_v1", f_ksi },
#endif
#ifdef CONF_HAVE_XDG_ACTIVATION_V1
    { "xdg_activation_v1", f_activation },
#endif
#if defined(CONF_HAVE_XDG_FOREIGN_UNSTABLE_V2) && defined(CONF_HAVE_XDG_SHELL)
    { "zxdg_exporter_v2", f_foreign },
#endif
#if defined(CONF_HAVE_XDG_DIALOG_V1) && defined(CONF_HAVE_XDG_SHELL)
    { "xdg_wm_dialog_v1", f_dialog },
#endif
#if defined(CONF_HAVE_XDG_TOPLEVEL_ICON_V1) && defined(CONF_HAVE_XDG_SHELL)
    { "xdg_toplevel_icon_manager_v1", f_icon },
#endif
#ifdef CONF_HAVE_EXT_IDLE_NOTIFY_V1
    { "ext_idle_notifier_v1", f_idle_notify },
#endif
#if defined(CONF_HAVE_TEXT_INPUT_UNSTABLE_V3) && defined(CONF_HAVE_XDG_SHELL)
    { "zwp_text_input_manager_v3", f_text_input },
#endif
#ifdef CONF_HAVE_SECURITY_CONTEXT_V1
    { "wp_security_context_manager_v1", f_security_context },
#endif
    { "org_kde_kwin_server_decoration_manager", f_kde },
};

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 2 && !strcmp(argv[1], "generic")) { generic(argc - 2, argv + 2); return 0; }
    if (argc >= 4 && !strcmp(argv[1], "clip-read")) return clip_read(argv[2], atol(argv[3]), argc > 4 && !strcmp(argv[4], "primary"));
    if (argc >= 3 && !strcmp(argv[1], "clip-own")) return clip_own(atol(argv[2]));
    if (argc >= 2 && !strcmp(argv[1], "list")) {
        for (size_t i = 0; i < sizeof flows / sizeof flows[0]; i++) printf("%s\n", flows[i].global);
        return 0;
    }
    if (argc < 2 || strcmp(argv[1], "flows")) {
        fprintf(stderr, "usage: wl-conformance-client generic <iface:ver:destroy-opcode:since>... | flows [global...] | list | clip-read <mime> <bytes> | clip-own <bytes>\n");
        return 2;
    }
    for (int i = 2; i < argc && nonly < 64; i++) only[nonly++] = argv[i];
    for (size_t i = 0; i < sizeof flows / sizeof flows[0]; i++) {
        int run = !nonly;
        for (int k = 0; k < nonly; k++) if (!strcmp(only[k], flows[i].global)) run = 1;
        if (run) flows[i].fn();
    }
    return 0;
}
