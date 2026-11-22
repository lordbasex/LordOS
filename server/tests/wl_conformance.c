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
 * file. Run it only against the headless lord-server the driver starts (never a live session); with its --test-hooks
 * (src/testhooks.h) the flows that need a pointer over a window, a graphics tablet, a VR headset or Xwayland's
 * connection get them from lord-server itself, and are skipped without it. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>
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
#ifdef CONF_HAVE_WL_FIXES
#include "wl-fixes-client-protocol.h"
#endif
#ifdef CONF_HAVE_EXT_BACKGROUND_EFFECT_V1
#include "ext-background-effect-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_FIFO_V1
#include "fifo-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_COMMIT_TIMING_V1
#include "commit-timing-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_TEARING_CONTROL_V1
#include "tearing-control-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_CONTENT_TYPE_V1
#include "content-type-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_POINTER_WARP_V1
#include "pointer-warp-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_XDG_SYSTEM_BELL_V1
#include "xdg-system-bell-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_XDG_TOPLEVEL_DRAG_V1
#include "xdg-toplevel-drag-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_XDG_TOPLEVEL_TAG_V1
#include "xdg-toplevel-tag-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_XDG_SESSION_MANAGEMENT_V1
#include "xdg-session-management-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_TABLET_V2
#include "tablet-v2-client-protocol.h"
#endif
#ifdef CONF_HAVE_COLOR_MANAGEMENT_V1
#include "color-management-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_COLOR_REPRESENTATION_V1
#include "color-representation-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_LINUX_DRM_SYNCOBJ_V1
#include <drm/drm.h>
#include <linux/udmabuf.h>
#include "linux-drm-syncobj-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_DRM_LEASE_V1
#include "drm-lease-v1-client-protocol.h"
#endif
#ifdef CONF_HAVE_XWAYLAND_SHELL_V1
#include "xwayland-shell-v1-client-protocol.h"
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
    struct wl_surface *kb_focus, *ptr_focus;
    int kb_enters, ptr_enters, motions, buttons;
    double mx, my;                  /* the last wl_pointer.motion, surface-local */
    uint32_t btn_serial;
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

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
    (void)p;
    Conn *c = d;
    c->ptr_serial = serial;
    c->ptr_enters++;
    c->ptr_focus = s;
    c->mx = wl_fixed_to_double(x);
    c->my = wl_fixed_to_double(y);
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) { (void)p; (void)serial; Conn *c = d; if (c->ptr_focus == s) c->ptr_focus = NULL; }
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)p; (void)t; Conn *c = d; c->mx = wl_fixed_to_double(x); c->my = wl_fixed_to_double(y); c->motions++; }
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t st) {
    (void)p; (void)t; (void)b;
    Conn *c = d;
    if (st == WL_POINTER_BUTTON_STATE_PRESSED) { c->btn_serial = serial; c->buttons++; }
}
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

static int ptr_over(Conn *c, struct wl_surface *s);     /* the test hooks (below) */

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
        else if (cx_wait(c, &c->ptr_enters, 300) != 1 && !ptr_over(c, w.s))
            skip(item, "pointer.role", "wl_pointer.error.role", "the pointer never entered the window (headless without --test-hooks): set_cursor without the enter serial is ignored by the protocol");
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

static double dabs(double v) { return v < 0 ? -v : v; }

/* ---------------------------------------------------------------- the test hooks (lord-server --headless --test-hooks)
 * src/testhooks.h: one command per connection to $XDG_RUNTIME_DIR/lordos-test-hooks.sock, the answer "ok" (with an fd
 * for "xwl") or "err ...". The pointer, the buttons, a fake graphics tablet, a connection taken as Xwayland's and a fake
 * card for drm-lease-v1; without the socket (another server) the checks that need them are skipped. */
static int th_cmd_fd(const char *cmd, int *fd_out) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (fd_out) *fd_out = -1;
    if (!rt) return -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if ((size_t)snprintf(a.sun_path, sizeof a.sun_path, "%s/lordos-test-hooks.sock", rt) >= sizeof a.sun_path) return -1;
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    if (connect(s, (struct sockaddr *)&a, sizeof a) || send(s, cmd, strlen(cmd), MSG_NOSIGNAL) < 0) { close(s); return -1; }
    char buf[128];
    union { char b[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } cbuf;
    struct iovec iov = { buf, sizeof buf - 1 };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf.b, .msg_controllen = sizeof cbuf.b };
    struct pollfd p = { s, POLLIN, 0 };
    ssize_t n = poll(&p, 1, 3000) == 1 ? recvmsg(s, &m, MSG_CMSG_CLOEXEC) : -1;
    close(s);
    if (n <= 0) return -1;
    buf[n] = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int f;
            memcpy(&f, CMSG_DATA(c), sizeof f);
            if (fd_out) *fd_out = f; else close(f);
        }
    return strncmp(buf, "ok", 2) ? -1 : 0;
}
static int th(const char *fmt, ...) {
    char b[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return th_cmd_fd(b, NULL);
}
static int th_have(void) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char p[300];
    snprintf(p, sizeof p, "%s/lordos-test-hooks.sock", rt ? rt : "/nonexistent");
    return access(p, F_OK) == 0;
}
/* the pointer over surface s (a big window the theme placed in the middle): moved off it and back to a few points
 * near the middle of the screen until wl_pointer.enter names s; 1 when it did */
static int ptr_over(Conn *c, struct wl_surface *s) {
    static const int pts[][2] = { { 640, 400 }, { 600, 360 }, { 700, 450 }, { 520, 300 }, { 760, 520 } };
    if (!c->ptr || !th_have()) return 0;
    for (size_t i = 0; i < sizeof pts / sizeof pts[0]; i++) {
        th("ptr 3 3");
        cx_wait(c, NULL, 60);
        th("ptr %d %d", pts[i][0], pts[i][1]);
        double end = now_ms() + 700;
        while (c->ptr_focus != s && now_ms() < end) cx_wait(c, NULL, 40);
        if (c->ptr_focus == s) return 1;
    }
    return 0;
}
/* a second connection, made from an fd (the test hooks' "xwl") */
static Conn *cx_open_fd(int fd) {
    Conn *c = calloc(1, sizeof *c);
    c->d = wl_display_connect_to_fd(fd);
    if (!c->d) { close(fd); free(c); return NULL; }
    c->reg = wl_display_get_registry(c->d);
    wl_registry_add_listener(c->reg, &reg_l, c);
    wl_display_roundtrip(c->d);
    c->comp = bindv(c, &wl_compositor_interface, 0);
    c->shm = bindv(c, &wl_shm_interface, 1);
#ifdef CONF_HAVE_XDG_SHELL
    c->wm = bindv(c, &xdg_wm_base_interface, 0);
    if (c->wm) xdg_wm_base_add_listener(c->wm, &wm_l, c);
#endif
    wl_display_roundtrip(c->d);
    return c;
}
static void close_win(Conn *c, Win *w) {
    if (!w->s) return;
    xdg_toplevel_destroy(w->top);
    xdg_surface_destroy(w->xs);
    wl_surface_destroy(w->s);
    w->s = NULL;
    cx_sync(c);
}

/* ---------------------------------------------------------------- wl_fixes */
#ifdef CONF_HAVE_WL_FIXES
static int fx_globals;
static void fx_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) { (void)d; (void)r; (void)name; (void)iface; (void)ver; fx_globals++; }
static void fx_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener fx_reg_l = { fx_global, fx_remove };
static void f_fixes(void) {
    ITEM("wl_fixes");
    {
        OPEN_OR_SKIP("fixes.destroy_registry");
        struct wl_fixes *f = bindv(c, &wl_fixes_interface, 0);
        if (!f) { skip(item, "fixes.destroy_registry", item, "not advertised"); cx_close(c); return; }
        struct wl_registry *gone = wl_display_get_registry(c->d);
        fx_globals = 0;
        wl_registry_add_listener(gone, &fx_reg_l, NULL);
        cx_sync(c);
        int before = fx_globals;
        uint32_t gone_id = wl_proxy_get_id((struct wl_proxy *)gone);
        wl_fixes_destroy_registry(f, gone);
        wl_registry_destroy(gone);
        int ok = cx_ok(c);
        check(item, "fixes.destroy_registry", ok && before == c->ng, "wl_fixes.destroy_registry", "a registry (%d globals) destroyed on the server side: %s", before, cx_errtext(c));
        /* its id was deleted by the server (wl_display.delete_id): a new registry may get it back */
        int reused = 0;
        struct wl_registry *next[6];
        for (int i = 0; i < 6; i++) { next[i] = wl_display_get_registry(c->d); if (wl_proxy_get_id((struct wl_proxy *)next[i]) == gone_id) reused = 1; }
        cx_sync(c);
        check(item, "fixes.id_reused", reused && cx_ok(c), "wl_fixes.destroy_registry", "the destroyed registry's id %u came back for a new one: %s", gone_id, reused ? "yes" : "no");
        for (int i = 0; i < 6; i++) { wl_fixes_destroy_registry(f, next[i]); wl_registry_destroy(next[i]); }
        wl_fixes_destroy(f);
        check(item, "fixes.destroy", cx_ok(c), "wl_fixes.destroy", "destroy: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("fixes.not_a_registry");
        struct wl_fixes *f = bindv(c, &wl_fixes_interface, 0);
        wl_fixes_destroy_registry(f, (struct wl_registry *)f);
        wl_display_roundtrip(c->d);
        int e = wl_display_get_error(c->d);
        check(item, "fixes.not_a_registry", e == EINVAL, "wl_fixes.destroy_registry", "destroy_registry naming a wl_fixes: libwayland's invalid_object (%s)", e ? strerror(e) : "no error");
        cx_close(c);
    }
}
#endif

/* ---------------------------------------------------------------- ext-background-effect-v1 */
#ifdef CONF_HAVE_EXT_BACKGROUND_EFFECT_V1
static int bg_caps_n;
static uint32_t bg_caps;
static void bg_capsf(void *d, struct ext_background_effect_manager_v1 *m, uint32_t f) { (void)d; (void)m; bg_caps = f; bg_caps_n++; }
static const struct ext_background_effect_manager_v1_listener bg_l = { bg_capsf };
static void f_bgeffect(void) {
    ITEM("ext_background_effect_manager_v1");
    {
        OPEN_OR_SKIP("blur.capabilities");
        struct ext_background_effect_manager_v1 *m = bindv(c, &ext_background_effect_manager_v1_interface, 0);
        if (!m) { skip(item, "blur.capabilities", item, "not advertised"); cx_close(c); return; }
        bg_caps_n = 0;
        ext_background_effect_manager_v1_add_listener(m, &bg_l, NULL);
        cx_sync(c);
        check(item, "blur.capabilities", bg_caps_n == 1 && bg_caps == EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR, "ext_background_effect_manager_v1.capabilities",
              "capabilities once on bind: %d, blur %s (the CPU path draws it)", bg_caps_n, bg_caps & EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR ? "offered" : "not offered");
        Win w;
        if (win_map(c, &w, "wlconf-blur", 400, 300)) { skip(item, "blur.set_region", "ext_background_effect_surface_v1.set_blur_region", "no window"); cx_close(c); return; }
        struct ext_background_effect_surface_v1 *e = ext_background_effect_manager_v1_get_background_effect(m, w.s);
        struct wl_region *r = wl_compositor_create_region(c->comp);
        wl_region_add(r, 10, 10, 120, 80);
        wl_region_add(r, 200, 150, 60, 40);
        ext_background_effect_surface_v1_set_blur_region(e, r);
        wl_region_destroy(r);
        wl_surface_commit(w.s);
        check(item, "blur.set_region", cx_ok(c), "ext_background_effect_manager_v1.get_background_effect,ext_background_effect_surface_v1.set_blur_region",
              "get_background_effect and a two-rectangle region, committed on a window: %s", cx_errtext(c));
        cx_wait(c, NULL, 300);              /* the shell is told ("blur", checked by the driver) */
        ext_background_effect_surface_v1_set_blur_region(e, NULL);
        wl_surface_commit(w.s);
        check(item, "blur.unset", cx_ok(c), "ext_background_effect_surface_v1.set_blur_region", "a null region (no blur), committed: %s", cx_errtext(c));
        ext_background_effect_surface_v1_destroy(e);
        wl_surface_commit(w.s);
        e = ext_background_effect_manager_v1_get_background_effect(m, w.s);   /* the surface may get a new one once the old is gone */
        ext_background_effect_surface_v1_destroy(e);
        ext_background_effect_manager_v1_destroy(m);
        check(item, "blur.destroy", cx_ok(c), "ext_background_effect_surface_v1.destroy,ext_background_effect_manager_v1.destroy",
              "the effect destroyed (blur removed at the next commit), a new one for the same surface, the manager destroyed: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("blur.exists");
        struct ext_background_effect_manager_v1 *m = bindv(c, &ext_background_effect_manager_v1_interface, 0);
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        ext_background_effect_manager_v1_get_background_effect(m, s);
        ext_background_effect_manager_v1_get_background_effect(m, s);
        int ok = cx_expect(c, &ext_background_effect_manager_v1_interface, EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS);
        check(item, "blur.exists", ok, "ext_background_effect_manager_v1.error.background_effect_exists", "a second effect object for a surface: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("blur.surface_destroyed");
        struct ext_background_effect_manager_v1 *m = bindv(c, &ext_background_effect_manager_v1_interface, 0);
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        struct ext_background_effect_surface_v1 *e = ext_background_effect_manager_v1_get_background_effect(m, s);
        wl_surface_destroy(s);
        ext_background_effect_surface_v1_set_blur_region(e, NULL);
        int ok = cx_expect(c, &ext_background_effect_surface_v1_interface, EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED);
        check(item, "blur.surface_destroyed", ok, "ext_background_effect_surface_v1.error.surface_destroyed", "set_blur_region after the wl_surface is gone: %s", cx_errtext(c));
        cx_close(c);
    }
}
#endif

/* ---------------------------------------------------------------- frame timing: fifo, commit timing, tearing, content type */
static double cbt[4];
static int cbn[4];
static void cbt_fire(void *d, struct wl_callback *cb, uint32_t t) { (void)t; int i = (int)(intptr_t)d; cbt[i] = now_ms(); cbn[i]++; wl_callback_destroy(cb); }
static const struct wl_callback_listener cbt_l = { cbt_fire };
static void frame_at(struct wl_surface *s, int i) { cbn[i] = 0; cbt[i] = 0; wl_callback_add_listener(wl_surface_frame(s), &cbt_l, (void *)(intptr_t)i); }
static void commit_buf(Conn *c, Win *w, uint32_t color) {
    int bw = w->cw > 0 ? w->cw : 200, bh = w->ch > 0 ? w->ch : 150;
    struct wl_buffer *b = shm_buffer(c, bw, bh, color);
    wl_surface_attach(w->s, b, 0, 0);
    wl_surface_damage_buffer(w->s, 0, 0, bw, bh);
    wl_surface_commit(w->s);
}

#ifdef CONF_HAVE_FIFO_V1
static void f_fifo(void) {
    ITEM("wp_fifo_manager_v1");
    {
        OPEN_OR_SKIP("fifo.barrier");
        struct wp_fifo_manager_v1 *m = bindv(c, &wp_fifo_manager_v1_interface, 0);
        Win w;
        if (!m || win_map(c, &w, "wlconf-fifo", 200, 150)) { skip(item, "fifo.barrier", item, "not advertised or no window"); cx_close(c); return; }
        cx_wait(c, NULL, 300);
        struct wp_fifo_v1 *f = wp_fifo_manager_v1_get_fifo(m, w.s);
        /* one commit sets the barrier, the next waits for it: the second can't apply in the same refresh as the first */
        int okn = 0;
        double worst = 1e9;
        for (int k = 0; k < 5; k++) {
            wp_fifo_v1_set_barrier(f);
            frame_at(w.s, 0);
            commit_buf(c, &w, 0xff204060);
            wp_fifo_v1_wait_barrier(f);
            frame_at(w.s, 1);
            commit_buf(c, &w, 0xff406020);
            double end = now_ms() + 2000;
            while (!cbn[1] && now_ms() < end) cx_wait(c, NULL, 20);
            if (cbn[0] && cbn[1]) { double dt = cbt[1] - cbt[0]; if (dt < worst) worst = dt; if (dt >= 8) okn++; }
        }
        check(item, "fifo.barrier", okn == 5, "wp_fifo_manager_v1.get_fifo,wp_fifo_v1.set_barrier,wp_fifo_v1.wait_barrier",
              "a commit waiting on the barrier the one before it set applied a refresh later: %d of 5 (closest %.1f ms; 60 Hz headless)", okn, worst);
        /* a wait with no barrier up doesn't hold */
        wp_fifo_v1_wait_barrier(f);
        frame_at(w.s, 2);
        commit_buf(c, &w, 0xff602040);
        double t0 = now_ms(), end = t0 + 2000;
        while (!cbn[2] && now_ms() < end) cx_wait(c, NULL, 20);
        check(item, "fifo.no_barrier", cbn[2] > 0, "wp_fifo_v1.wait_barrier", "wait_barrier with no barrier up applies at once: %s (%.0f ms)", cbn[2] ? "yes" : "never", cbt[2] - t0);
        wp_fifo_v1_destroy(f);
        wp_fifo_manager_v1_destroy(m);
        check(item, "fifo.destroy", cx_ok(c), "wp_fifo_v1.destroy,wp_fifo_manager_v1.destroy", "destroy: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("fifo.already_exists");
        struct wp_fifo_manager_v1 *m = bindv(c, &wp_fifo_manager_v1_interface, 0);
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        wp_fifo_manager_v1_get_fifo(m, s);
        wp_fifo_manager_v1_get_fifo(m, s);
        int ok = cx_expect(c, &wp_fifo_manager_v1_interface, WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS);
        check(item, "fifo.already_exists", ok, "wp_fifo_manager_v1.error.already_exists", "a second fifo object for a surface: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("fifo.surface_destroyed");
        struct wp_fifo_manager_v1 *m = bindv(c, &wp_fifo_manager_v1_interface, 0);
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        struct wp_fifo_v1 *f = wp_fifo_manager_v1_get_fifo(m, s);
        wl_surface_destroy(s);
        wp_fifo_v1_set_barrier(f);
        int ok = cx_expect(c, &wp_fifo_v1_interface, WP_FIFO_V1_ERROR_SURFACE_DESTROYED);
        check(item, "fifo.surface_destroyed", ok, "wp_fifo_v1.error.surface_destroyed", "set_barrier after the wl_surface is gone: %s", cx_errtext(c));
        cx_close(c);
    }
}
#endif

#ifdef CONF_HAVE_COMMIT_TIMING_V1
static void timer_target(struct wp_commit_timer_v1 *t, double ms_ahead, double *target_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec + (uint64_t)(ms_ahead * 1e6);
    uint64_t sec = ns / 1000000000ull;
    wp_commit_timer_v1_set_timestamp(t, (uint32_t)(sec >> 32), (uint32_t)sec, (uint32_t)(ns % 1000000000ull));
    *target_ms = (double)ns / 1e6;
}
static void f_commit_timing(void) {
    ITEM("wp_commit_timing_manager_v1");
    {
        OPEN_OR_SKIP("timing.target");
        struct wp_commit_timing_manager_v1 *m = bindv(c, &wp_commit_timing_manager_v1_interface, 0);
        Win w;
        if (!m || win_map(c, &w, "wlconf-timing", 200, 150)) { skip(item, "timing.target", item, "not advertised or no window"); cx_close(c); return; }
        cx_wait(c, NULL, 300);
        struct wp_commit_timer_v1 *t = wp_commit_timing_manager_v1_get_timer(m, w.s);
        static const double ahead[] = { 100, 250, 400 };
        int okn = 0;
        double late = 0, early = 0;
        for (int k = 0; k < 3; k++) {
            double target;
            timer_target(t, ahead[k], &target);
            frame_at(w.s, 0);
            commit_buf(c, &w, 0xff000000u | (uint32_t)(k * 40));
            double end = now_ms() + ahead[k] + 2000;
            while (!cbn[0] && now_ms() < end) cx_wait(c, NULL, 10);
            /* not before its target (less 1/16 of a 60 Hz refresh), and within two refreshes after it */
            double d = cbt[0] - target;
            if (cbn[0] && d >= -1.1 && d <= 40) okn++;
            if (d < early) early = d;
            if (d > late) late = d;
        }
        check(item, "timing.target", okn == 3, "wp_commit_timing_manager_v1.get_timer,wp_commit_timer_v1.set_timestamp",
              "commits with a target 100, 250 and 400 ms ahead applied at or after it: %d of 3 (from %.1f to %+.1f ms of their targets)", okn, early, late);
        /* a timestamp is one commit's: the next commit with none applies at once */
        frame_at(w.s, 1);
        commit_buf(c, &w, 0xff336699);
        double t0 = now_ms(), end = t0 + 1000;
        while (!cbn[1] && now_ms() < end) cx_wait(c, NULL, 10);
        check(item, "timing.one_commit", cbn[1] && cbt[1] - t0 < 100, "wp_commit_timer_v1.set_timestamp", "the next commit, with no timestamp, is not held: %.0f ms", cbt[1] - t0);
        wp_commit_timer_v1_destroy(t);
        wp_commit_timing_manager_v1_destroy(m);
        check(item, "timing.destroy", cx_ok(c), "wp_commit_timer_v1.destroy,wp_commit_timing_manager_v1.destroy", "destroy: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("timing.timer_exists");
        struct wp_commit_timing_manager_v1 *m = bindv(c, &wp_commit_timing_manager_v1_interface, 0);
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        wp_commit_timing_manager_v1_get_timer(m, s);
        wp_commit_timing_manager_v1_get_timer(m, s);
        int ok = cx_expect(c, &wp_commit_timing_manager_v1_interface, WP_COMMIT_TIMING_MANAGER_V1_ERROR_COMMIT_TIMER_EXISTS);
        check(item, "timing.timer_exists", ok, "wp_commit_timing_manager_v1.error.commit_timer_exists", "a second timer for a surface: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("timing.invalid_timestamp");
        struct wp_commit_timing_manager_v1 *m = bindv(c, &wp_commit_timing_manager_v1_interface, 0);
        struct wp_commit_timer_v1 *t = wp_commit_timing_manager_v1_get_timer(m, wl_compositor_create_surface(c->comp));
        wp_commit_timer_v1_set_timestamp(t, 0, 1, 1000000000u);
        int ok = cx_expect(c, &wp_commit_timer_v1_interface, WP_COMMIT_TIMER_V1_ERROR_INVALID_TIMESTAMP);
        check(item, "timing.invalid_timestamp", ok, "wp_commit_timer_v1.error.invalid_timestamp", "tv_nsec = 1 000 000 000: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("timing.timestamp_exists");
        struct wp_commit_timing_manager_v1 *m = bindv(c, &wp_commit_timing_manager_v1_interface, 0);
        struct wp_commit_timer_v1 *t = wp_commit_timing_manager_v1_get_timer(m, wl_compositor_create_surface(c->comp));
        wp_commit_timer_v1_set_timestamp(t, 0, 1, 0);
        wp_commit_timer_v1_set_timestamp(t, 0, 2, 0);
        int ok = cx_expect(c, &wp_commit_timer_v1_interface, WP_COMMIT_TIMER_V1_ERROR_TIMESTAMP_EXISTS);
        check(item, "timing.timestamp_exists", ok, "wp_commit_timer_v1.error.timestamp_exists", "two timestamps before one commit: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("timing.surface_destroyed");
        struct wp_commit_timing_manager_v1 *m = bindv(c, &wp_commit_timing_manager_v1_interface, 0);
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        struct wp_commit_timer_v1 *t = wp_commit_timing_manager_v1_get_timer(m, s);
        wl_surface_destroy(s);
        wp_commit_timer_v1_set_timestamp(t, 0, 1, 0);
        int ok = cx_expect(c, &wp_commit_timer_v1_interface, WP_COMMIT_TIMER_V1_ERROR_SURFACE_DESTROYED);
        check(item, "timing.surface_destroyed", ok, "wp_commit_timer_v1.error.surface_destroyed", "set_timestamp after the wl_surface is gone: %s", cx_errtext(c));
        cx_close(c);
    }
}
#endif

#ifdef CONF_HAVE_TEARING_CONTROL_V1
static void f_tearing(void) {
    ITEM("wp_tearing_control_manager_v1");
    {
        OPEN_OR_SKIP("tearing.hint");
        struct wp_tearing_control_manager_v1 *m = bindv(c, &wp_tearing_control_manager_v1_interface, 0);
        Win w;
        if (!m || win_map(c, &w, "wlconf-tearing", 200, 150)) { skip(item, "tearing.hint", item, "not advertised or no window"); cx_close(c); return; }
        struct wp_tearing_control_v1 *t = wp_tearing_control_manager_v1_get_tearing_control(m, w.s);
        wp_tearing_control_v1_set_presentation_hint(t, WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
        frame_at(w.s, 0);
        commit_buf(c, &w, 0xff112233);
        cx_wait(c, &cbn[0], 2000);
        check(item, "tearing.async", cbn[0] && cx_ok(c), "wp_tearing_control_manager_v1.get_tearing_control,wp_tearing_control_v1.set_presentation_hint",
              "the async hint on a window, committed, and its frame shown: %s", cbn[0] ? cx_errtext(c) : "no frame");
        wp_tearing_control_v1_set_presentation_hint(t, WP_TEARING_CONTROL_V1_PRESENTATION_HINT_VSYNC);
        frame_at(w.s, 1);
        commit_buf(c, &w, 0xff332211);
        cx_wait(c, &cbn[1], 2000);
        check(item, "tearing.vsync", cbn[1] && cx_ok(c), "wp_tearing_control_v1.set_presentation_hint", "back to vsync: %s", cbn[1] ? cx_errtext(c) : "no frame");
        wp_tearing_control_v1_destroy(t);
        wl_surface_commit(w.s);
        /* destroyed: the surface may get a new one */
        t = wp_tearing_control_manager_v1_get_tearing_control(m, w.s);
        wp_tearing_control_v1_destroy(t);
        wp_tearing_control_manager_v1_destroy(m);
        check(item, "tearing.destroy", cx_ok(c), "wp_tearing_control_v1.destroy,wp_tearing_control_manager_v1.destroy", "destroyed (back to vsync), a new one for the surface, the manager destroyed: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("tearing.exists");
        struct wp_tearing_control_manager_v1 *m = bindv(c, &wp_tearing_control_manager_v1_interface, 0);
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        wp_tearing_control_manager_v1_get_tearing_control(m, s);
        wp_tearing_control_manager_v1_get_tearing_control(m, s);
        int ok = cx_expect(c, &wp_tearing_control_manager_v1_interface, WP_TEARING_CONTROL_MANAGER_V1_ERROR_TEARING_CONTROL_EXISTS);
        check(item, "tearing.exists", ok, "wp_tearing_control_manager_v1.error.tearing_control_exists", "a second tearing control for a surface: %s", cx_errtext(c));
        cx_close(c);
    }
}
#endif

#ifdef CONF_HAVE_CONTENT_TYPE_V1
static void f_content_type(void) {
    ITEM("wp_content_type_manager_v1");
    {
        OPEN_OR_SKIP("content_type.set");
        struct wp_content_type_manager_v1 *m = bindv(c, &wp_content_type_manager_v1_interface, 0);
        Win w;
        if (!m || win_map(c, &w, "wlconf-ctype", 200, 150)) { skip(item, "content_type.set", item, "not advertised or no window"); cx_close(c); return; }
        struct wp_content_type_v1 *t = wp_content_type_manager_v1_get_surface_content_type(m, w.s);
        static const uint32_t types[] = { WP_CONTENT_TYPE_V1_TYPE_PHOTO, WP_CONTENT_TYPE_V1_TYPE_VIDEO, WP_CONTENT_TYPE_V1_TYPE_GAME, WP_CONTENT_TYPE_V1_TYPE_NONE };
        for (int k = 0; k < 4; k++) { wp_content_type_v1_set_content_type(t, types[k]); wl_surface_commit(w.s); }
        check(item, "content_type.set", cx_ok(c), "wp_content_type_manager_v1.get_surface_content_type,wp_content_type_v1.set_content_type",
              "photo, video, game and none, each committed: %s", cx_errtext(c));
        wp_content_type_v1_destroy(t);
        wl_surface_commit(w.s);
        t = wp_content_type_manager_v1_get_surface_content_type(m, w.s);
        wp_content_type_v1_destroy(t);
        wp_content_type_manager_v1_destroy(m);
        check(item, "content_type.destroy", cx_ok(c), "wp_content_type_v1.destroy,wp_content_type_manager_v1.destroy", "destroyed (none again), a new one, the manager destroyed: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("content_type.already_constructed");
        struct wp_content_type_manager_v1 *m = bindv(c, &wp_content_type_manager_v1_interface, 0);
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        wp_content_type_manager_v1_get_surface_content_type(m, s);
        wp_content_type_manager_v1_get_surface_content_type(m, s);
        int ok = cx_expect(c, &wp_content_type_manager_v1_interface, WP_CONTENT_TYPE_MANAGER_V1_ERROR_ALREADY_CONSTRUCTED);
        check(item, "content_type.already_constructed", ok, "wp_content_type_manager_v1.error.already_constructed", "a second content type object for a surface: %s", cx_errtext(c));
        cx_close(c);
    }
}
#endif

/* ---------------------------------------------------------------- pointer-warp-v1 */
#if defined(CONF_HAVE_POINTER_WARP_V1) && defined(CONF_HAVE_XDG_SHELL)
static void f_pointer_warp(void) {
    ITEM("wp_pointer_warp_v1");
    OPEN_OR_SKIP("warp.moves");
    struct wp_pointer_warp_v1 *pw = bindv(c, &wp_pointer_warp_v1_interface, 0);
    Win w;
    if (!pw || !c->ptr || win_map(c, &w, "wlconf-warp", 800, 600)) { skip(item, "warp.moves", item, "not advertised, no pointer or no window"); cx_close(c); return; }
    cx_wait(c, &c->kb_enters, 2000);
    if (!ptr_over(c, w.s)) { skip(item, "warp.moves", "wp_pointer_warp_v1.warp_pointer", "the pointer could not be put over the window (needs --test-hooks)"); cx_close(c); return; }
    uint32_t enter = c->ptr_serial;
    c->motions = 0;
    wp_pointer_warp_v1_warp_pointer(pw, w.s, c->ptr, wl_fixed_from_int(30), wl_fixed_from_int(40), enter);
    double end = now_ms() + 1500;
    while (!(c->motions && dabs(c->mx - 30) < 1.01 && dabs(c->my - 40) < 1.01) && now_ms() < end) cx_wait(c, NULL, 30);
    int moved = c->motions && dabs(c->mx - 30) < 1.01 && dabs(c->my - 40) < 1.01;
    check(item, "warp.moves", moved, "wp_pointer_warp_v1.warp_pointer", "warp to (30, 40) with the enter's serial: the pointer moved there (%s, motion to %.1f, %.1f)", moved ? "yes" : "no", c->mx, c->my);
    c->motions = 0;
    wp_pointer_warp_v1_warp_pointer(pw, w.s, c->ptr, wl_fixed_from_int(60), wl_fixed_from_int(70), enter + 1000);
    cx_wait(c, NULL, 400);
    check(item, "warp.stale_serial", c->motions == 0 && cx_ok(c), "wp_pointer_warp_v1.warp_pointer", "a serial that is not the enter's is ignored: %d motion(s)", c->motions);
    c->motions = 0;
    wp_pointer_warp_v1_warp_pointer(pw, w.s, c->ptr, wl_fixed_from_int(-5), wl_fixed_from_int(9000), enter);
    cx_wait(c, NULL, 400);
    check(item, "warp.outside", c->motions == 0 && cx_ok(c), "wp_pointer_warp_v1.warp_pointer", "a point outside the surface is ignored: %d motion(s)", c->motions);
    wp_pointer_warp_v1_destroy(pw);
    check(item, "warp.destroy", cx_ok(c), "wp_pointer_warp_v1.destroy", "destroy: %s", cx_errtext(c));
    cx_close(c);
}
#endif

/* ---------------------------------------------------------------- xdg-system-bell-v1 */
#if defined(CONF_HAVE_XDG_SYSTEM_BELL_V1) && defined(CONF_HAVE_XDG_SHELL)
static void f_bell(void) {
    ITEM("xdg_system_bell_v1");
    OPEN_OR_SKIP("bell.ring");
    struct xdg_system_bell_v1 *b = bindv(c, &xdg_system_bell_v1_interface, 0);
    Win w;
    if (!b || win_map(c, &w, "wlconf-bell", 200, 150)) { skip(item, "bell.ring", item, "not advertised or no window"); cx_close(c); return; }
    cx_wait(c, NULL, 300);
    xdg_system_bell_v1_ring(b, w.s);
    check(item, "bell.ring_surface", cx_ok(c), "xdg_system_bell_v1.ring", "ring on a window's surface (the shell is told \"bell\" on that window: checked by the driver): %s", cx_errtext(c));
    cx_wait(c, NULL, 300);
    xdg_system_bell_v1_ring(b, NULL);
    check(item, "bell.ring_null", cx_ok(c), "xdg_system_bell_v1.ring", "ring with no surface: %s", cx_errtext(c));
    for (int i = 0; i < 30; i++) xdg_system_bell_v1_ring(b, w.s);  /* held back past the rate limit, never an error */
    check(item, "bell.flood", cx_ok(c), "xdg_system_bell_v1.ring", "30 rings at once (the rate limit drops them, no error): %s", cx_errtext(c));
    xdg_system_bell_v1_destroy(b);
    check(item, "bell.destroy", cx_ok(c), "xdg_system_bell_v1.destroy", "destroy: %s", cx_errtext(c));
    cx_wait(c, NULL, 300);
    cx_close(c);
}
#endif

/* ---------------------------------------------------------------- xdg-toplevel-tag-v1 */
#if defined(CONF_HAVE_XDG_TOPLEVEL_TAG_V1) && defined(CONF_HAVE_XDG_SHELL)
static void f_tag(void) {
    ITEM("xdg_toplevel_tag_manager_v1");
    OPEN_OR_SKIP("tag.before_map");
    struct xdg_toplevel_tag_manager_v1 *m = bindv(c, &xdg_toplevel_tag_manager_v1_interface, 0);
    if (!m) { skip(item, "tag.before_map", item, "not advertised"); cx_close(c); return; }
    Win w;
    if (win_role(c, &w, "wlconf-tag")) { skip(item, "tag.before_map", item, "no window"); cx_close(c); return; }
    xdg_toplevel_tag_manager_v1_set_toplevel_tag(m, w.top, "conformance window");
    xdg_toplevel_tag_manager_v1_set_toplevel_description(m, w.top, "Conformance window");
    commit_buf(c, &w, 0xff446688);
    check(item, "tag.before_map", cx_ok(c), "xdg_toplevel_tag_manager_v1.set_toplevel_tag,xdg_toplevel_tag_manager_v1.set_toplevel_description",
          "tag and description set before the first frame (they reach the shell with the window: checked by the driver): %s", cx_errtext(c));
    cx_wait(c, NULL, 400);
    xdg_toplevel_tag_manager_v1_set_toplevel_tag(m, w.top, "conformance window, changed");
    xdg_toplevel_tag_manager_v1_set_toplevel_description(m, w.top, "Ventana \xc3\xb1 \x01 cut");
    check(item, "tag.changed", cx_ok(c), "xdg_toplevel_tag_manager_v1.set_toplevel_tag,xdg_toplevel_tag_manager_v1.set_toplevel_description",
          "changed while mapped (a \"tag\" event to the shell), with a control character in the description: %s", cx_errtext(c));
    char longtag[400];
    memset(longtag, 'x', sizeof longtag - 1);
    longtag[sizeof longtag - 1] = 0;
    xdg_toplevel_tag_manager_v1_set_toplevel_tag(m, w.top, longtag);
    check(item, "tag.long", cx_ok(c), "xdg_toplevel_tag_manager_v1.set_toplevel_tag", "a 399-byte tag is cut, never an error: %s", cx_errtext(c));
    cx_wait(c, NULL, 300);
    xdg_toplevel_tag_manager_v1_destroy(m);
    check(item, "tag.destroy", cx_ok(c), "xdg_toplevel_tag_manager_v1.destroy", "destroy: %s", cx_errtext(c));
    cx_close(c);
}
#endif

/* ---------------------------------------------------------------- xdg-toplevel-drag-v1 */
#if defined(CONF_HAVE_XDG_TOPLEVEL_DRAG_V1) && defined(CONF_HAVE_XDG_SHELL)
static int td_cancelled, td_dropped, td_finished;
static void td_target(void *d, struct wl_data_source *s, const char *m) { (void)d; (void)s; (void)m; }
static void td_send(void *d, struct wl_data_source *s, const char *m, int fd) { (void)d; (void)s; (void)m; close(fd); }
static void td_cancel(void *d, struct wl_data_source *s) { (void)d; (void)s; td_cancelled++; }
static void td_drop(void *d, struct wl_data_source *s) { (void)d; (void)s; td_dropped++; }
static void td_finish(void *d, struct wl_data_source *s) { (void)d; (void)s; td_finished++; }
static void td_action(void *d, struct wl_data_source *s, uint32_t a) { (void)d; (void)s; (void)a; }
static const struct wl_data_source_listener td_src_l = { td_target, td_send, td_cancel, td_drop, td_finish, td_action };
static void f_toplevel_drag(void) {
    ITEM("xdg_toplevel_drag_manager_v1");
    {
        OPEN_OR_SKIP("tldrag.follow");
        struct xdg_toplevel_drag_manager_v1 *m = bindv(c, &xdg_toplevel_drag_manager_v1_interface, 0);
        struct wl_data_device_manager *dm = bindv(c, &wl_data_device_manager_interface, 0);
        Win w, t;
        if (!m || !dm || !c->ptr || win_map(c, &w, "wlconf-tldrag-origin", 800, 600)) { skip(item, "tldrag.follow", item, "not advertised, no pointer or no window"); cx_close(c); return; }
        cx_wait(c, &c->kb_enters, 2000);
        if (!ptr_over(c, w.s)) {
            skip(item, "tldrag.follow", "xdg_toplevel_drag_v1.attach", "the pointer could not be put over the window (needs --test-hooks)");
            skip(item, "tldrag.ongoing_drag", "xdg_toplevel_drag_v1.error.ongoing_drag", "no pointer over the window");
            cx_close(c);
            return;
        }
        struct wl_data_device *dd = wl_data_device_manager_get_data_device(dm, c->seat);
        c->buttons = 0;
        th("btn 0 1");
        double end = now_ms() + 1500;
        while (!c->buttons && now_ms() < end) cx_wait(c, NULL, 30);
        struct wl_data_source *src = wl_data_device_manager_create_data_source(dm);
        td_cancelled = td_dropped = td_finished = 0;
        wl_data_source_add_listener(src, &td_src_l, NULL);
        wl_data_source_offer(src, "application/x-wlconf-tab");
        if (wl_proxy_get_version((struct wl_proxy *)src) >= 3) wl_data_source_set_actions(src, WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
        struct xdg_toplevel_drag_v1 *td = xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(m, src);
        /* the torn-off tab's window: attached before it is mapped, with the point under the pointer */
        win_role(c, &t, "wlconf-tldrag");
        xdg_toplevel_drag_v1_attach(td, t.top, 20, 10);
        wl_data_device_start_drag(dd, src, w.s, NULL, c->btn_serial);
        commit_buf(c, &t, 0xff884422);
        int ok = cx_ok(c);
        check(item, "tldrag.follow", ok && c->buttons, "xdg_toplevel_drag_manager_v1.get_xdg_toplevel_drag,xdg_toplevel_drag_v1.attach",
              "a tab torn off: a drag started with the press's serial, a new window attached at (20, 10) and mapped (it follows the pointer: \"tldrag\" to the shell, checked by the driver): %s",
              c->buttons ? cx_errtext(c) : "the press never reached the window");
        th("ptr 660 420");
        th("ptr 680 440");
        cx_wait(c, NULL, 300);
        uint32_t tdid = wl_proxy_get_id((struct wl_proxy *)td);
        xdg_toplevel_drag_v1_destroy(td);
        int e = cx_expect_id(c, &xdg_toplevel_drag_v1_interface, tdid, XDG_TOPLEVEL_DRAG_V1_ERROR_ONGOING_DRAG);
        check(item, "tldrag.ongoing_drag", e, "xdg_toplevel_drag_v1.error.ongoing_drag", "destroyed while the drag runs: %s", cx_errtext(c));
        th("btn 0 0");
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("tldrag.end");
        struct xdg_toplevel_drag_manager_v1 *m = bindv(c, &xdg_toplevel_drag_manager_v1_interface, 0);
        struct wl_data_device_manager *dm = bindv(c, &wl_data_device_manager_interface, 0);
        Win w;
        if (!c->ptr || win_map(c, &w, "wlconf-tldrag-origin2", 800, 600) || (cx_wait(c, &c->kb_enters, 2000), !ptr_over(c, w.s))) {
            skip(item, "tldrag.end", "xdg_toplevel_drag_v1.destroy", "no pointer over the window");
            cx_close(c);
        } else {
            struct wl_data_device *dd = wl_data_device_manager_get_data_device(dm, c->seat);
            c->buttons = 0;
            th("btn 0 1");
            double end = now_ms() + 1500;
            while (!c->buttons && now_ms() < end) cx_wait(c, NULL, 30);
            struct wl_data_source *src = wl_data_device_manager_create_data_source(dm);
            td_cancelled = td_dropped = td_finished = 0;
            wl_data_source_add_listener(src, &td_src_l, NULL);
            wl_data_source_offer(src, "application/x-wlconf-tab");
            if (wl_proxy_get_version((struct wl_proxy *)src) >= 3) wl_data_source_set_actions(src, WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
            struct xdg_toplevel_drag_v1 *td = xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(m, src);
            xdg_toplevel_drag_v1_attach(td, w.top, 100, 12);       /* the whole window, dragged by its title bar */
            wl_data_device_start_drag(dd, src, w.s, NULL, c->btn_serial);
            cx_sync(c);
            th("ptr 700 450");
            th("btn 0 0");
            end = now_ms() + 2000;
            while (!td_cancelled && !td_dropped && now_ms() < end) cx_wait(c, NULL, 30);
            xdg_toplevel_drag_v1_destroy(td);
            check(item, "tldrag.end", (td_cancelled || td_dropped) && cx_ok(c), "xdg_toplevel_drag_v1.destroy",
                  "the drag ended at the release (%s), then the object destroyed: %s", td_dropped ? "dropped" : td_cancelled ? "cancelled" : "never ended", cx_errtext(c));
            xdg_toplevel_drag_manager_v1_destroy(m);
            check(item, "tldrag.manager_destroy", cx_ok(c), "xdg_toplevel_drag_manager_v1.destroy", "destroy: %s", cx_errtext(c));
            cx_close(c);
        }
    }
    {
        OPEN_OR_SKIP("tldrag.invalid_source");
        struct xdg_toplevel_drag_manager_v1 *m = bindv(c, &xdg_toplevel_drag_manager_v1_interface, 0);
        struct wl_data_device_manager *dm = bindv(c, &wl_data_device_manager_interface, 0);
        struct wl_data_source *src = wl_data_device_manager_create_data_source(dm);
        xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(m, src);
        xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(m, src);
        int ok = cx_expect(c, &xdg_toplevel_drag_manager_v1_interface, XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE);
        check(item, "tldrag.invalid_source", ok, "xdg_toplevel_drag_manager_v1.error.invalid_source", "a second toplevel drag for one source: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("tldrag.selection_source");
        struct xdg_toplevel_drag_manager_v1 *m = bindv(c, &xdg_toplevel_drag_manager_v1_interface, 0);
        struct wl_data_device_manager *dm = bindv(c, &wl_data_device_manager_interface, 0);
        Win w;
        if (!c->kb || win_map(c, &w, "wlconf-tldrag-sel", 200, 150) || cx_wait(c, &c->kb_enters, 2000) != 1) {
            skip(item, "tldrag.selection_source", "xdg_toplevel_drag_manager_v1.error.invalid_source", "no keyboard focus");
        } else {
            struct wl_data_device *dd = wl_data_device_manager_get_data_device(dm, c->seat);
            struct wl_data_source *src = wl_data_device_manager_create_data_source(dm);
            wl_data_source_offer(src, "text/plain");
            wl_data_device_set_selection(dd, src, c->kb_serial);
            xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(m, src);
            int ok = cx_expect(c, &xdg_toplevel_drag_manager_v1_interface, XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE);
            check(item, "tldrag.selection_source", ok, "xdg_toplevel_drag_manager_v1.error.invalid_source", "a source already used as the selection: %s", cx_errtext(c));
        }
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("tldrag.toplevel_attached");
        struct xdg_toplevel_drag_manager_v1 *m = bindv(c, &xdg_toplevel_drag_manager_v1_interface, 0);
        struct wl_data_device_manager *dm = bindv(c, &wl_data_device_manager_interface, 0);
        Win a, b;
        if (win_role(c, &a, "wlconf-tldrag-a") || win_role(c, &b, "wlconf-tldrag-b")) { skip(item, "tldrag.toplevel_attached", "xdg_toplevel_drag_v1.error.toplevel_attached", "no toplevels"); cx_close(c); return; }
        struct xdg_toplevel_drag_v1 *td = xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(m, wl_data_device_manager_create_data_source(dm));
        xdg_toplevel_drag_v1_attach(td, a.top, 0, 0);
        xdg_toplevel_drag_v1_attach(td, b.top, 0, 0);
        int ok = cx_expect(c, &xdg_toplevel_drag_v1_interface, XDG_TOPLEVEL_DRAG_V1_ERROR_TOPLEVEL_ATTACHED);
        check(item, "tldrag.toplevel_attached", ok, "xdg_toplevel_drag_v1.error.toplevel_attached", "attach while another toplevel with its role is attached: %s", cx_errtext(c));
        cx_close(c);
    }
}
#endif

/* ---------------------------------------------------------------- xdg-session-management-v1 */
#if defined(CONF_HAVE_XDG_SESSION_MANAGEMENT_V1) && defined(CONF_HAVE_XDG_SHELL)
typedef struct { int created, restored, replaced; char id[128]; } SessEv;
static void se_created(void *d, struct xdg_session_v1 *s, const char *id) { (void)s; SessEv *e = d; e->created++; snprintf(e->id, sizeof e->id, "%s", id); }
static void se_restored(void *d, struct xdg_session_v1 *s) { (void)s; ((SessEv *)d)->restored++; }
static void se_replaced(void *d, struct xdg_session_v1 *s) { (void)s; ((SessEv *)d)->replaced++; }
static const struct xdg_session_v1_listener se_l = { se_created, se_restored, se_replaced };
static int tse_restored;
static void tse_restoredf(void *d, struct xdg_toplevel_session_v1 *t) { (void)d; (void)t; tse_restored++; }
static const struct xdg_toplevel_session_v1_listener tse_l = { tse_restoredf };
/* a toplevel role with no commit yet (restore_toplevel must come before the first one) */
static void win_uncommitted(Conn *c, Win *w, const char *title) {
    memset(w, 0, sizeof *w);
    w->s = wl_compositor_create_surface(c->comp);
    w->xs = xdg_wm_base_get_xdg_surface(c->wm, w->s);
    xdg_surface_add_listener(w->xs, &xs_l, w);
    w->top = xdg_surface_get_toplevel(w->xs);
    xdg_toplevel_add_listener(w->top, &top_l, w);
    xdg_toplevel_set_title(w->top, title);
    xdg_toplevel_set_app_id(w->top, "org.lordos.wlconformance");
}
static void win_first_commit_map(Conn *c, Win *w, int width, int height) {
    wl_surface_commit(w->s);
    if (cx_wait(c, &w->configured, 3000) != 1) return;
    xdg_surface_ack_configure(w->xs, w->serial);
    w->acked = w->serial;
    w->has_acked = 1;
    commit_buf(c, w, 0xff557799);
    (void)width; (void)height;
}
static void f_session(void) {
    ITEM("xdg_session_manager_v1");
    char sid[128] = "";
    {
        OPEN_OR_SKIP("session.created");
        struct xdg_session_manager_v1 *m = bindv(c, &xdg_session_manager_v1_interface, 0);
        if (!m) { skip(item, "session.created", item, "not advertised"); cx_close(c); return; }
        SessEv e = { 0 };
        struct xdg_session_v1 *s = xdg_session_manager_v1_get_session(m, XDG_SESSION_MANAGER_V1_REASON_LAUNCH, NULL);
        xdg_session_v1_add_listener(s, &se_l, &e);
        cx_sync(c);
        check(item, "session.created", e.created == 1 && strlen(e.id) >= 16, "xdg_session_manager_v1.get_session,xdg_session_v1.created", "a new session: created with an id of %zu characters", strlen(e.id));
        snprintf(sid, sizeof sid, "%s", e.id);
        Win w;
        win_uncommitted(c, &w, "wlconf-session");
        struct xdg_toplevel_session_v1 *ts = xdg_session_v1_add_toplevel(s, w.top, "main");
        win_first_commit_map(c, &w, 300, 200);
        cx_wait(c, NULL, 300);
        xdg_toplevel_session_v1_rename(ts, "primary");
        xdg_session_v1_remove_toplevel(s, "nothing-by-that-name");
        check(item, "session.add_rename", cx_ok(c), "xdg_session_v1.add_toplevel,xdg_toplevel_session_v1.rename,xdg_session_v1.remove_toplevel",
              "a window added as \"main\", renamed \"primary\", an unknown name removed: %s", cx_errtext(c));
        cx_wait(c, NULL, 2300);                 /* its place is kept (every 2 s, and when it closes) */
        close_win(c, &w);
        xdg_toplevel_session_v1_destroy(ts);
        xdg_session_v1_destroy(s);
        check(item, "session.destroy", cx_ok(c), "xdg_toplevel_session_v1.destroy,xdg_session_v1.destroy", "the window closed and both objects destroyed (the session is kept): %s", cx_errtext(c));
        cx_close(c);
    }
    if (!sid[0]) return;
    {
        OPEN_OR_SKIP("session.restored");
        Conn *c2 = cx_open();
        struct xdg_session_manager_v1 *m = bindv(c, &xdg_session_manager_v1_interface, 0);
        SessEv e = { 0 }, e2 = { 0 };
        struct xdg_session_v1 *s = xdg_session_manager_v1_get_session(m, XDG_SESSION_MANAGER_V1_REASON_RECOVER, sid);
        xdg_session_v1_add_listener(s, &se_l, &e);
        cx_sync(c);
        check(item, "session.restored", e.restored == 1 && !e.created, "xdg_session_v1.restored", "the same id from a new connection: restored (%d), not created (%d)", e.restored, e.created);
        Win w;
        win_uncommitted(c, &w, "wlconf-session-restored");
        tse_restored = 0;
        struct xdg_toplevel_session_v1 *ts = xdg_session_v1_restore_toplevel(s, w.top, "primary");
        xdg_toplevel_session_v1_add_listener(ts, &tse_l, NULL);
        cx_sync(c);
        check(item, "session.toplevel_restored", tse_restored == 1, "xdg_session_v1.restore_toplevel,xdg_toplevel_session_v1.restored",
              "restore_toplevel(\"primary\") before the first commit: restored %d time(s)", tse_restored);
        win_first_commit_map(c, &w, 300, 200);
        check(item, "session.restored_maps", cx_ok(c) && w.configured, "xdg_session_v1.restore_toplevel", "the restored window maps where it was: %s", cx_errtext(c));
        /* another client takes the session: this one is told it was replaced */
        if (c2) {
            struct xdg_session_manager_v1 *m2 = bindv(c2, &xdg_session_manager_v1_interface, 0);
            struct xdg_session_v1 *s2 = xdg_session_manager_v1_get_session(m2, XDG_SESSION_MANAGER_V1_REASON_SESSION_RESTORE, sid);
            xdg_session_v1_add_listener(s2, &se_l, &e2);
            cx_sync(c2);
            cx_wait(c, &e.replaced, 1000);
            check(item, "session.replaced", e.replaced == 1 && e2.restored == 1, "xdg_session_v1.replaced", "the session taken by another connection: replaced here (%d), restored there (%d)", e.replaced, e2.restored);
            /* that one removes it: the id finds nothing after */
            xdg_session_v1_remove(s2);
            check(item, "session.remove", cx_ok(c2), "xdg_session_v1.remove", "remove: %s", cx_errtext(c2));
            SessEv e3 = { 0 };
            struct xdg_session_v1 *s3 = xdg_session_manager_v1_get_session(m2, XDG_SESSION_MANAGER_V1_REASON_RECOVER, sid);
            xdg_session_v1_add_listener(s3, &se_l, &e3);
            cx_sync(c2);
            check(item, "session.removed_gone", e3.created == 1 && strcmp(e3.id, sid), "xdg_session_v1.remove", "the removed id asked for again: a new session (created %d, another id: %s)", e3.created, strcmp(e3.id, sid) ? "yes" : "no");
            xdg_session_v1_destroy(s3);
            xdg_session_manager_v1_destroy(m2);
            check(item, "session.manager_destroy", cx_ok(c2), "xdg_session_manager_v1.destroy", "destroy: %s", cx_errtext(c2));
            cx_close(c2);
        } else skip(item, "session.replaced", "xdg_session_v1.replaced", "no second connection");
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("session.in_use");
        struct xdg_session_manager_v1 *m = bindv(c, &xdg_session_manager_v1_interface, 0);
        SessEv e = { 0 };
        struct xdg_session_v1 *s = xdg_session_manager_v1_get_session(m, XDG_SESSION_MANAGER_V1_REASON_LAUNCH, NULL);
        xdg_session_v1_add_listener(s, &se_l, &e);
        cx_sync(c);
        xdg_session_manager_v1_get_session(m, XDG_SESSION_MANAGER_V1_REASON_RECOVER, e.id);
        int ok = cx_expect(c, &xdg_session_manager_v1_interface, XDG_SESSION_MANAGER_V1_ERROR_IN_USE);
        check(item, "session.in_use", ok, "xdg_session_manager_v1.error.in_use", "a session this client already holds asked for again: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("session.invalid_reason");
        struct xdg_session_manager_v1 *m = bindv(c, &xdg_session_manager_v1_interface, 0);
        xdg_session_manager_v1_get_session(m, 99, NULL);
        int ok = cx_expect(c, &xdg_session_manager_v1_interface, XDG_SESSION_MANAGER_V1_ERROR_INVALID_REASON);
        check(item, "session.invalid_reason", ok, "xdg_session_manager_v1.error.invalid_reason", "reason 99: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("session.invalid_session_id");
        struct xdg_session_manager_v1 *m = bindv(c, &xdg_session_manager_v1_interface, 0);
        xdg_session_manager_v1_get_session(m, XDG_SESSION_MANAGER_V1_REASON_RECOVER, "\xff\xfe not UTF-8");
        int ok = cx_expect(c, &xdg_session_manager_v1_interface, XDG_SESSION_MANAGER_V1_ERROR_INVALID_SESSION_ID);
        check(item, "session.invalid_session_id", ok, "xdg_session_manager_v1.error.invalid_session_id", "an id that is not UTF-8: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("session.name_in_use");
        struct xdg_session_manager_v1 *m = bindv(c, &xdg_session_manager_v1_interface, 0);
        struct xdg_session_v1 *s = xdg_session_manager_v1_get_session(m, XDG_SESSION_MANAGER_V1_REASON_LAUNCH, NULL);
        Win a, b;
        win_uncommitted(c, &a, "wlconf-session-a");
        win_uncommitted(c, &b, "wlconf-session-b");
        xdg_session_v1_add_toplevel(s, a.top, "same");
        xdg_session_v1_add_toplevel(s, b.top, "same");
        int ok = cx_expect(c, &xdg_session_v1_interface, XDG_SESSION_V1_ERROR_NAME_IN_USE);
        check(item, "session.name_in_use", ok, "xdg_session_v1.error.name_in_use", "two windows of a session under one name: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("session.already_added");
        struct xdg_session_manager_v1 *m = bindv(c, &xdg_session_manager_v1_interface, 0);
        struct xdg_session_v1 *s = xdg_session_manager_v1_get_session(m, XDG_SESSION_MANAGER_V1_REASON_LAUNCH, NULL);
        Win a;
        win_uncommitted(c, &a, "wlconf-session-a");
        xdg_session_v1_add_toplevel(s, a.top, "one");
        xdg_session_v1_add_toplevel(s, a.top, "two");
        int ok = cx_expect(c, &xdg_session_v1_interface, XDG_SESSION_V1_ERROR_ALREADY_ADDED);
        check(item, "session.already_added", ok, "xdg_session_v1.error.already_added", "one window added twice: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("session.already_mapped");
        struct xdg_session_manager_v1 *m = bindv(c, &xdg_session_manager_v1_interface, 0);
        struct xdg_session_v1 *s = xdg_session_manager_v1_get_session(m, XDG_SESSION_MANAGER_V1_REASON_LAUNCH, NULL);
        Win a;
        win_role(c, &a, "wlconf-session-committed");
        xdg_session_v1_restore_toplevel(s, a.top, "late");
        int ok = cx_expect(c, &xdg_session_v1_interface, XDG_SESSION_V1_ERROR_ALREADY_MAPPED);
        check(item, "session.already_mapped", ok, "xdg_session_v1.error.already_mapped", "restore_toplevel after the window's first commit: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("session.invalid_name");
        struct xdg_session_manager_v1 *m = bindv(c, &xdg_session_manager_v1_interface, 0);
        struct xdg_session_v1 *s = xdg_session_manager_v1_get_session(m, XDG_SESSION_MANAGER_V1_REASON_LAUNCH, NULL);
        Win a;
        win_uncommitted(c, &a, "wlconf-session-a");
        xdg_session_v1_add_toplevel(s, a.top, "bad \xff name");
        int ok = cx_expect(c, &xdg_session_v1_interface, XDG_SESSION_V1_ERROR_INVALID_NAME);
        check(item, "session.invalid_name", ok, "xdg_session_v1.error.invalid_name", "a window name that is not UTF-8: %s", cx_errtext(c));
        cx_close(c);
    }
}
#endif

/* ---------------------------------------------------------------- tablet-v2, with the test hooks' fake tablet */
#if defined(CONF_HAVE_TABLET_V2) && defined(CONF_HAVE_XDG_SHELL)
typedef struct {
    int tablets, tools, pads, tab_name, tab_id, tab_path, tab_done, tab_bus, tab_removed;
    int tool_type, tool_serial, tool_hwid, tool_caps, tool_done, tool_removed, prox_in, prox_out, down, up, motion, pressure,
        distance, tilt, rotation, slider, wheel, button, frame;
    int pad_group, pad_path, pad_buttons, pad_done, pad_button, pad_enter, pad_leave, pad_removed;
    int grp_buttons, grp_ring, grp_strip, grp_done, grp_mode_switch, grp_dial, grp_modes;
    int ring_source, ring_angle, ring_stop, ring_frame, strip_source, strip_position, strip_stop, strip_frame, dial_delta, dial_frame;
    uint32_t prox_serial, enter_serial, mode_serial;
    struct zwp_tablet_tool_v2 *tool[8];
    struct zwp_tablet_v2 *tab;
    struct zwp_tablet_pad_v2 *pad;
    struct zwp_tablet_pad_ring_v2 *ring;
    struct zwp_tablet_pad_strip_v2 *strip;
    struct zwp_tablet_pad_dial_v2 *dial;
    struct zwp_tablet_pad_group_v2 *group;
    struct wl_surface *prox_surface;
    struct zwp_tablet_tool_v2 *prox_tool;
} TabEv;
static TabEv te;
static void tb_name(void *d, struct zwp_tablet_v2 *t, const char *n) { (void)d; (void)t; (void)n; te.tab_name++; }
static void tb_id(void *d, struct zwp_tablet_v2 *t, uint32_t v, uint32_t p) { (void)d; (void)t; (void)v; (void)p; te.tab_id++; }
static void tb_path(void *d, struct zwp_tablet_v2 *t, const char *p) { (void)d; (void)t; (void)p; te.tab_path++; }
static void tb_done(void *d, struct zwp_tablet_v2 *t) { (void)d; (void)t; te.tab_done++; }
static void tb_removed(void *d, struct zwp_tablet_v2 *t) { (void)d; (void)t; te.tab_removed++; }
static void tb_bus(void *d, struct zwp_tablet_v2 *t, uint32_t b) { (void)d; (void)t; (void)b; te.tab_bus++; }
static const struct zwp_tablet_v2_listener tb_l = { tb_name, tb_id, tb_path, tb_done, tb_removed, tb_bus };
static void tt_type(void *d, struct zwp_tablet_tool_v2 *t, uint32_t x) { (void)d; (void)t; (void)x; te.tool_type++; }
static void tt_serial(void *d, struct zwp_tablet_tool_v2 *t, uint32_t a, uint32_t b) { (void)d; (void)t; (void)a; (void)b; te.tool_serial++; }
static void tt_hwid(void *d, struct zwp_tablet_tool_v2 *t, uint32_t a, uint32_t b) { (void)d; (void)t; (void)a; (void)b; te.tool_hwid++; }
static void tt_cap(void *d, struct zwp_tablet_tool_v2 *t, uint32_t x) { (void)d; (void)t; (void)x; te.tool_caps++; }
static void tt_done(void *d, struct zwp_tablet_tool_v2 *t) { (void)d; (void)t; te.tool_done++; }
static void tt_removed(void *d, struct zwp_tablet_tool_v2 *t) { (void)d; (void)t; te.tool_removed++; }
static void tt_prox_in(void *d, struct zwp_tablet_tool_v2 *t, uint32_t serial, struct zwp_tablet_v2 *tab, struct wl_surface *s) { (void)d; (void)t; (void)tab; te.prox_in++; te.prox_serial = serial; te.prox_surface = s; te.prox_tool = t; }
static void tt_prox_out(void *d, struct zwp_tablet_tool_v2 *t) { (void)d; (void)t; te.prox_out++; }
static void tt_down(void *d, struct zwp_tablet_tool_v2 *t, uint32_t serial) { (void)d; (void)t; (void)serial; te.down++; }
static void tt_up(void *d, struct zwp_tablet_tool_v2 *t) { (void)d; (void)t; te.up++; }
static void tt_motion(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)t; (void)x; (void)y; te.motion++; }
static void tt_pressure(void *d, struct zwp_tablet_tool_v2 *t, uint32_t p) { (void)d; (void)t; (void)p; te.pressure++; }
static void tt_distance(void *d, struct zwp_tablet_tool_v2 *t, uint32_t p) { (void)d; (void)t; (void)p; te.distance++; }
static void tt_tilt(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t a, wl_fixed_t b) { (void)d; (void)t; (void)a; (void)b; te.tilt++; }
static void tt_rotation(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t a) { (void)d; (void)t; (void)a; te.rotation++; }
static void tt_slider(void *d, struct zwp_tablet_tool_v2 *t, int32_t a) { (void)d; (void)t; (void)a; te.slider++; }
static void tt_wheel(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t a, int32_t b) { (void)d; (void)t; (void)a; (void)b; te.wheel++; }
static void tt_button(void *d, struct zwp_tablet_tool_v2 *t, uint32_t serial, uint32_t b, uint32_t st) { (void)d; (void)t; (void)serial; (void)b; (void)st; te.button++; }
static void tt_frame(void *d, struct zwp_tablet_tool_v2 *t, uint32_t time) { (void)d; (void)t; (void)time; te.frame++; }
static const struct zwp_tablet_tool_v2_listener tt_l = { tt_type, tt_serial, tt_hwid, tt_cap, tt_done, tt_removed, tt_prox_in, tt_prox_out, tt_down, tt_up,
                                                         tt_motion, tt_pressure, tt_distance, tt_tilt, tt_rotation, tt_slider, tt_wheel, tt_button, tt_frame };
static void rg_source(void *d, struct zwp_tablet_pad_ring_v2 *r, uint32_t s) { (void)d; (void)r; (void)s; te.ring_source++; }
static void rg_angle(void *d, struct zwp_tablet_pad_ring_v2 *r, wl_fixed_t a) { (void)d; (void)r; (void)a; te.ring_angle++; }
static void rg_stop(void *d, struct zwp_tablet_pad_ring_v2 *r) { (void)d; (void)r; te.ring_stop++; }
static void rg_frame(void *d, struct zwp_tablet_pad_ring_v2 *r, uint32_t t) { (void)d; (void)r; (void)t; te.ring_frame++; }
static const struct zwp_tablet_pad_ring_v2_listener rg_l = { rg_source, rg_angle, rg_stop, rg_frame };
static void st_source(void *d, struct zwp_tablet_pad_strip_v2 *r, uint32_t s) { (void)d; (void)r; (void)s; te.strip_source++; }
static void st_position(void *d, struct zwp_tablet_pad_strip_v2 *r, uint32_t p) { (void)d; (void)r; (void)p; te.strip_position++; }
static void st_stop(void *d, struct zwp_tablet_pad_strip_v2 *r) { (void)d; (void)r; te.strip_stop++; }
static void st_frame(void *d, struct zwp_tablet_pad_strip_v2 *r, uint32_t t) { (void)d; (void)r; (void)t; te.strip_frame++; }
static const struct zwp_tablet_pad_strip_v2_listener st_l = { st_source, st_position, st_stop, st_frame };
static void dl_delta(void *d, struct zwp_tablet_pad_dial_v2 *r, int32_t v) { (void)d; (void)r; (void)v; te.dial_delta++; }
static void dl_frame(void *d, struct zwp_tablet_pad_dial_v2 *r, uint32_t t) { (void)d; (void)r; (void)t; te.dial_frame++; }
static const struct zwp_tablet_pad_dial_v2_listener dl_l = { dl_delta, dl_frame };
static void gp_buttons(void *d, struct zwp_tablet_pad_group_v2 *g, struct wl_array *b) { (void)d; (void)g; (void)b; te.grp_buttons++; }
static void gp_ring(void *d, struct zwp_tablet_pad_group_v2 *g, struct zwp_tablet_pad_ring_v2 *r) { (void)d; (void)g; te.grp_ring++; te.ring = r; zwp_tablet_pad_ring_v2_add_listener(r, &rg_l, NULL); }
static void gp_strip(void *d, struct zwp_tablet_pad_group_v2 *g, struct zwp_tablet_pad_strip_v2 *s) { (void)d; (void)g; te.grp_strip++; te.strip = s; zwp_tablet_pad_strip_v2_add_listener(s, &st_l, NULL); }
static void gp_modes(void *d, struct zwp_tablet_pad_group_v2 *g, uint32_t n) { (void)d; (void)g; (void)n; te.grp_modes++; }
static void gp_done(void *d, struct zwp_tablet_pad_group_v2 *g) { (void)d; (void)g; te.grp_done++; }
static void gp_mode(void *d, struct zwp_tablet_pad_group_v2 *g, uint32_t t, uint32_t serial, uint32_t m) { (void)d; (void)g; (void)t; (void)m; te.grp_mode_switch++; te.mode_serial = serial; }
static void gp_dial(void *d, struct zwp_tablet_pad_group_v2 *g, struct zwp_tablet_pad_dial_v2 *x) { (void)d; (void)g; te.grp_dial++; te.dial = x; zwp_tablet_pad_dial_v2_add_listener(x, &dl_l, NULL); }
static const struct zwp_tablet_pad_group_v2_listener gp_l = { gp_buttons, gp_ring, gp_strip, gp_modes, gp_done, gp_mode, gp_dial };
static void pd_group(void *d, struct zwp_tablet_pad_v2 *p, struct zwp_tablet_pad_group_v2 *g) { (void)d; (void)p; te.pad_group++; te.group = g; zwp_tablet_pad_group_v2_add_listener(g, &gp_l, NULL); }
static void pd_path(void *d, struct zwp_tablet_pad_v2 *p, const char *x) { (void)d; (void)p; (void)x; te.pad_path++; }
static void pd_buttons(void *d, struct zwp_tablet_pad_v2 *p, uint32_t n) { (void)d; (void)p; (void)n; te.pad_buttons++; }
static void pd_done(void *d, struct zwp_tablet_pad_v2 *p) { (void)d; (void)p; te.pad_done++; }
static void pd_button(void *d, struct zwp_tablet_pad_v2 *p, uint32_t t, uint32_t b, uint32_t s) { (void)d; (void)p; (void)t; (void)b; (void)s; te.pad_button++; }
static void pd_enter(void *d, struct zwp_tablet_pad_v2 *p, uint32_t serial, struct zwp_tablet_v2 *t, struct wl_surface *s) { (void)d; (void)p; (void)t; (void)s; te.pad_enter++; te.enter_serial = serial; }
static void pd_leave(void *d, struct zwp_tablet_pad_v2 *p, uint32_t serial, struct wl_surface *s) { (void)d; (void)p; (void)serial; (void)s; te.pad_leave++; }
static void pd_removed(void *d, struct zwp_tablet_pad_v2 *p) { (void)d; (void)p; te.pad_removed++; }
static const struct zwp_tablet_pad_v2_listener pd_l = { pd_group, pd_path, pd_buttons, pd_done, pd_button, pd_enter, pd_leave, pd_removed };
static void ts_tablet(void *d, struct zwp_tablet_seat_v2 *s, struct zwp_tablet_v2 *t) { (void)d; (void)s; te.tablets++; te.tab = t; zwp_tablet_v2_add_listener(t, &tb_l, NULL); }
static void ts_tool(void *d, struct zwp_tablet_seat_v2 *s, struct zwp_tablet_tool_v2 *t) { (void)d; (void)s; if (te.tools < 8) te.tool[te.tools] = t; te.tools++; zwp_tablet_tool_v2_add_listener(t, &tt_l, NULL); }
static void ts_pad(void *d, struct zwp_tablet_seat_v2 *s, struct zwp_tablet_pad_v2 *p) { (void)d; (void)s; te.pads++; te.pad = p; zwp_tablet_pad_v2_add_listener(p, &pd_l, NULL); }
static const struct zwp_tablet_seat_v2_listener ts_l = { ts_tablet, ts_tool, ts_pad };

/* evdev events of the fake pen or pad, each frame ended with SYN_REPORT */
static void pen(unsigned type, unsigned code, int value) { th("pen %u %u %d", type, code, value); }
static void pen_syn(void) { th("pen %u %u 0", (unsigned)EV_SYN, 0u); }
static void pad(unsigned type, unsigned code, int value) { th("pad %u %u %d", type, code, value); }
static void pad_syn(void) { th("pad %u %u 0", (unsigned)EV_SYN, 0u); }

static void f_tablet(void) {
    ITEM("zwp_tablet_manager_v2");
    if (!th_have()) { skip(item, "tablet.added", "zwp_tablet_seat_v2.tablet_added", "needs lord-server --test-hooks (a fake tablet)"); return; }
    {
        OPEN_OR_SKIP("tablet.added");
        struct zwp_tablet_manager_v2 *m = bindv(c, &zwp_tablet_manager_v2_interface, 0);
        Win w;
        if (!m || win_map(c, &w, "wlconf-tablet", 800, 600)) { skip(item, "tablet.added", item, "not advertised or no window"); cx_close(c); return; }
        cx_wait(c, &c->kb_enters, 2000);
        memset(&te, 0, sizeof te);
        th("tablet add");
        struct zwp_tablet_seat_v2 *seat = zwp_tablet_manager_v2_get_tablet_seat(m, c->seat);
        zwp_tablet_seat_v2_add_listener(seat, &ts_l, NULL);
        cx_sync(c);
        uint32_t tv = wl_proxy_get_version((struct wl_proxy *)m);
        check(item, "tablet.added", te.tablets == 1 && te.tab_name && te.tab_id && te.tab_path && te.tab_done && (tv < 2 || te.tab_bus),
              "zwp_tablet_manager_v2.get_tablet_seat,zwp_tablet_seat_v2.tablet_added,zwp_tablet_v2.name,zwp_tablet_v2.id,zwp_tablet_v2.path,zwp_tablet_v2.bustype,zwp_tablet_v2.done",
              "a tablet plugged in: tablet_added %d, name %d, id %d, path %d, bustype %d, done %d", te.tablets, te.tab_name, te.tab_id, te.tab_path, te.tab_bus, te.tab_done);
        check(item, "tablet.pad_added", te.pads == 1 && te.pad_group == 1 && te.pad_path && te.pad_buttons && te.pad_done && te.grp_buttons && te.grp_ring && te.grp_strip &&
              (tv < 2 || te.grp_dial) && te.grp_done && te.grp_modes == 0,
              "zwp_tablet_seat_v2.pad_added,zwp_tablet_pad_v2.group,zwp_tablet_pad_v2.path,zwp_tablet_pad_v2.buttons,zwp_tablet_pad_v2.done,zwp_tablet_pad_group_v2.buttons,zwp_tablet_pad_group_v2.ring,zwp_tablet_pad_group_v2.strip,zwp_tablet_pad_group_v2.dial,zwp_tablet_pad_group_v2.done",
              "its pad: pad_added %d, a group with buttons, a ring, a strip, a dial and no modes event for its one mode (%d %d %d %d, modes %d), path, buttons, done", te.pads,
              te.grp_buttons, te.grp_ring, te.grp_strip, te.grp_dial, te.grp_modes);
        /* a Wacom mouse first (no serial: it belongs to this tablet), its wheel turned one click */
        pen(EV_KEY, BTN_TOOL_MOUSE, 1);
        pen(EV_ABS, ABS_X, 5000);
        pen(EV_ABS, ABS_Y, 5000);
        pen_syn();
        pen(EV_REL, REL_WHEEL, -1);
        pen_syn();
        double end = now_ms() + 1500;
        while (!te.wheel && now_ms() < end) cx_wait(c, NULL, 30);
        check(item, "tablet.wheel", te.wheel >= 1, "zwp_tablet_tool_v2.wheel", "a mouse tool's wheel turned one click: wheel %d", te.wheel);
        pen(EV_KEY, BTN_TOOL_MOUSE, 0);
        pen_syn();
        end = now_ms() + 1500;
        while (!te.prox_out && now_ms() < end) cx_wait(c, NULL, 30);
        check(item, "tablet.proximity_out", te.prox_out >= 1, "zwp_tablet_tool_v2.proximity_out", "the mouse lifted away: proximity_out %d", te.prox_out);
        /* the pen comes in over the window: a tool with a serial appears, then proximity, down, the axes, a button, up */
        int tools0 = te.tools, prox0 = te.prox_in;
        te.tool_type = te.tool_serial = te.tool_hwid = te.tool_caps = te.tool_done = 0;
        pen(EV_KEY, BTN_TOOL_PEN, 1);
        pen(EV_MSC, MSC_SERIAL, 0x1234);
        pen(EV_ABS, ABS_MISC, 0x822);
        pen(EV_ABS, ABS_X, 5000);
        pen(EV_ABS, ABS_Y, 5000);
        pen(EV_ABS, ABS_DISTANCE, 20);
        pen_syn();
        end = now_ms() + 1500;
        while (te.prox_in == prox0 && now_ms() < end) cx_wait(c, NULL, 30);
        check(item, "tablet.tool_added", te.tools == tools0 + 1 && te.tool_type && te.tool_serial && te.tool_hwid && te.tool_caps >= 4 && te.tool_done,
              "zwp_tablet_seat_v2.tool_added,zwp_tablet_tool_v2.type,zwp_tablet_tool_v2.hardware_serial,zwp_tablet_tool_v2.hardware_id_wacom,zwp_tablet_tool_v2.capability,zwp_tablet_tool_v2.done",
              "a pen with a serial and a Wacom id: tool_added %d, type %d, hardware_serial %d, hardware_id_wacom %d, %d capabilities (pressure, distance, tilt, rotation), done %d",
              te.tools - tools0, te.tool_type, te.tool_serial, te.tool_hwid, te.tool_caps, te.tool_done);
        check(item, "tablet.proximity_in", te.prox_in > prox0 && te.prox_surface == w.s, "zwp_tablet_tool_v2.proximity_in,zwp_tablet_tool_v2.frame",
              "the pen over the window: proximity_in on its surface (%d)", te.prox_in - prox0);
        pen(EV_KEY, BTN_TOUCH, 1);
        pen(EV_ABS, ABS_PRESSURE, 600);
        pen(EV_ABS, ABS_DISTANCE, 0);
        pen(EV_ABS, ABS_TILT_X, 20);
        pen(EV_ABS, ABS_TILT_Y, -10);
        pen(EV_ABS, ABS_Z, 90);
        pen(EV_ABS, ABS_X, 5100);
        pen_syn();
        pen(EV_KEY, BTN_STYLUS, 1);
        pen_syn();
        pen(EV_KEY, BTN_STYLUS, 0);
        pen(EV_KEY, BTN_TOUCH, 0);
        pen(EV_ABS, ABS_PRESSURE, 0);
        pen_syn();
        end = now_ms() + 1500;
        while (!te.up && now_ms() < end) cx_wait(c, NULL, 30);
        /* a cursor for the pen, with its proximity serial */
        struct wl_surface *cur = wl_compositor_create_surface(c->comp);
        wl_surface_attach(cur, shm_buffer(c, 16, 16, 0xff000000), 0, 0);
        wl_surface_commit(cur);
        if (te.prox_tool) zwp_tablet_tool_v2_set_cursor(te.prox_tool, te.prox_serial, cur, 1, 1);
        check(item, "tablet.set_cursor", te.prox_tool && cx_ok(c), "zwp_tablet_tool_v2.set_cursor", "set_cursor with the proximity_in serial: %s", cx_errtext(c));
        pen(EV_KEY, BTN_TOOL_PEN, 0);
        pen_syn();
        /* an airbrush: its finger wheel is the slider */
        pen(EV_KEY, BTN_TOOL_AIRBRUSH, 1);
        pen(EV_ABS, ABS_X, 5000);
        pen(EV_ABS, ABS_Y, 5000);
        pen(EV_ABS, ABS_WHEEL, 50);
        pen_syn();
        pen(EV_ABS, ABS_WHEEL, 80);
        pen_syn();
        end = now_ms() + 1500;
        while (!te.slider && now_ms() < end) cx_wait(c, NULL, 30);
        pen(EV_KEY, BTN_TOOL_AIRBRUSH, 0);
        pen_syn();
        check(item, "tablet.axes", te.down && te.motion && te.pressure && te.distance && te.tilt && te.rotation && te.slider && te.button >= 2 && te.up && te.frame >= 3,
              "zwp_tablet_tool_v2.down,zwp_tablet_tool_v2.motion,zwp_tablet_tool_v2.pressure,zwp_tablet_tool_v2.distance,zwp_tablet_tool_v2.tilt,zwp_tablet_tool_v2.rotation,zwp_tablet_tool_v2.slider,zwp_tablet_tool_v2.button,zwp_tablet_tool_v2.up",
              "down %d, motion %d, pressure %d, distance %d, tilt %d, rotation %d, slider (airbrush) %d, button %d, up %d, frame %d", te.down, te.motion, te.pressure, te.distance, te.tilt,
              te.rotation, te.slider, te.button, te.up, te.frame);
        /* the pad: a button, the ring, the strip, the dial, and the finger lifted */
        pad(EV_KEY, BTN_0, 1);
        pad_syn();
        end = now_ms() + 1500;
        while (!te.pad_enter && now_ms() < end) cx_wait(c, NULL, 30);
        check(item, "tablet.pad_enter", te.pad_enter == 1 && te.grp_mode_switch >= 1, "zwp_tablet_pad_v2.enter,zwp_tablet_pad_group_v2.mode_switch",
              "the pad's first event: enter on the window with the keyboard, then mode_switch(0): enter %d, mode_switch %d", te.pad_enter, te.grp_mode_switch);
        pad(EV_KEY, BTN_0, 0);
        pad_syn();
        pad(EV_ABS, ABS_WHEEL, 20);
        pad(EV_ABS, ABS_RX, 64);
        pad(EV_REL, REL_WHEEL, 1);
        pad_syn();
        pad(EV_ABS, ABS_WHEEL, 0);
        pad(EV_ABS, ABS_RX, 0);
        pad_syn();
        end = now_ms() + 1500;
        while (!(te.ring_stop && te.strip_stop) && now_ms() < end) cx_wait(c, NULL, 30);
        check(item, "tablet.pad_events", te.pad_button >= 2 && te.ring_source && te.ring_angle && te.ring_stop && te.ring_frame && te.strip_source && te.strip_position &&
              te.strip_stop && te.strip_frame && (tv < 2 || (te.dial_delta && te.dial_frame)),
              "zwp_tablet_pad_v2.button,zwp_tablet_pad_ring_v2.source,zwp_tablet_pad_ring_v2.angle,zwp_tablet_pad_ring_v2.stop,zwp_tablet_pad_ring_v2.frame,zwp_tablet_pad_strip_v2.source,zwp_tablet_pad_strip_v2.position,zwp_tablet_pad_strip_v2.stop,zwp_tablet_pad_strip_v2.frame,zwp_tablet_pad_dial_v2.delta,zwp_tablet_pad_dial_v2.frame",
              "button %d; ring source %d angle %d stop %d frame %d; strip source %d position %d stop %d frame %d; dial delta %d frame %d", te.pad_button, te.ring_source,
              te.ring_angle, te.ring_stop, te.ring_frame, te.strip_source, te.strip_position, te.strip_stop, te.strip_frame, te.dial_delta, te.dial_frame);
        if (te.pad) zwp_tablet_pad_v2_set_feedback(te.pad, 0, "Undo", te.mode_serial);
        if (te.ring) zwp_tablet_pad_ring_v2_set_feedback(te.ring, "Zoom", te.mode_serial);
        if (te.strip) zwp_tablet_pad_strip_v2_set_feedback(te.strip, "Brush size", te.mode_serial);
        if (te.dial) zwp_tablet_pad_dial_v2_set_feedback(te.dial, "Scroll", te.mode_serial);
        check(item, "tablet.feedback", te.pad && te.ring && te.strip && cx_ok(c), "zwp_tablet_pad_v2.set_feedback,zwp_tablet_pad_ring_v2.set_feedback,zwp_tablet_pad_strip_v2.set_feedback,zwp_tablet_pad_dial_v2.set_feedback",
              "set_feedback on the pad's button, ring, strip and dial with the mode_switch serial: %s", cx_errtext(c));
        /* the window loses the keyboard: the pad leaves it */
        Win w2;
        win_map(c, &w2, "wlconf-tablet-2", 300, 200);
        end = now_ms() + 1500;
        while (!te.pad_leave && now_ms() < end) cx_wait(c, NULL, 30);
        check(item, "tablet.pad_leave", te.pad_leave >= 1, "zwp_tablet_pad_v2.leave", "another window takes the keyboard: the pad leaves the first (leave %d, enter %d)", te.pad_leave, te.pad_enter);
        /* unplugged: removed, for the tablet, its pad and its tools */
        th("tablet remove");
        end = now_ms() + 1500;
        while (!(te.tab_removed && te.pad_removed && te.tool_removed) && now_ms() < end) cx_wait(c, NULL, 30);
        check(item, "tablet.removed", te.tab_removed == 1 && te.pad_removed == 1 && te.tool_removed >= 1 && te.tool_removed < te.tools, "zwp_tablet_v2.removed,zwp_tablet_pad_v2.removed,zwp_tablet_tool_v2.removed",
              "the tablet unplugged: tablet removed %d, pad removed %d, %d of its %d tools removed (those with no serial; a tool with one may come to another tablet)",
              te.tab_removed, te.pad_removed, te.tool_removed, te.tools);
        for (int i = 0; i < te.tools && i < 8; i++) zwp_tablet_tool_v2_destroy(te.tool[i]);
        if (te.ring) zwp_tablet_pad_ring_v2_destroy(te.ring);
        if (te.strip) zwp_tablet_pad_strip_v2_destroy(te.strip);
        if (te.dial) zwp_tablet_pad_dial_v2_destroy(te.dial);
        if (te.group) zwp_tablet_pad_group_v2_destroy(te.group);
        if (te.pad) zwp_tablet_pad_v2_destroy(te.pad);
        if (te.tab) zwp_tablet_v2_destroy(te.tab);
        zwp_tablet_seat_v2_destroy(seat);
        zwp_tablet_manager_v2_destroy(m);
        check(item, "tablet.destroy", cx_ok(c), "zwp_tablet_tool_v2.destroy,zwp_tablet_pad_ring_v2.destroy,zwp_tablet_pad_strip_v2.destroy,zwp_tablet_pad_dial_v2.destroy,zwp_tablet_pad_group_v2.destroy,zwp_tablet_pad_v2.destroy,zwp_tablet_v2.destroy,zwp_tablet_seat_v2.destroy,zwp_tablet_manager_v2.destroy",
              "every object destroyed: %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("tablet.cursor_role");
        struct zwp_tablet_manager_v2 *m = bindv(c, &zwp_tablet_manager_v2_interface, 0);
        Win w;
        if (!m || win_map(c, &w, "wlconf-tablet-role", 800, 600)) { skip(item, "tablet.cursor_role", "zwp_tablet_tool_v2.error.role", "no window"); cx_close(c); return; }
        memset(&te, 0, sizeof te);
        th("tablet add");
        struct zwp_tablet_seat_v2 *seat = zwp_tablet_manager_v2_get_tablet_seat(m, c->seat);
        zwp_tablet_seat_v2_add_listener(seat, &ts_l, NULL);
        cx_sync(c);
        pen(EV_KEY, BTN_TOOL_PEN, 1);
        pen(EV_ABS, ABS_X, 5000);
        pen(EV_ABS, ABS_Y, 5000);
        pen_syn();
        double end = now_ms() + 1500;
        while (!te.prox_in && now_ms() < end) cx_wait(c, NULL, 30);
        if (!te.prox_in || !te.prox_tool) skip(item, "tablet.cursor_role", "zwp_tablet_tool_v2.error.role", "the pen never came over the window");
        else {
            zwp_tablet_tool_v2_set_cursor(te.prox_tool, te.prox_serial, w.s, 0, 0);
            int ok = cx_expect(c, &zwp_tablet_tool_v2_interface, ZWP_TABLET_TOOL_V2_ERROR_ROLE);
            check(item, "tablet.cursor_role", ok, "zwp_tablet_tool_v2.error.role", "a toplevel's surface as the tool's cursor: %s", cx_errtext(c));
        }
        cx_close(c);
        th("pen %u %u 0", (unsigned)EV_KEY, (unsigned)BTN_TOOL_PEN);
        pen_syn();
        th("tablet remove");
    }
}
#endif

/* ---------------------------------------------------------------- color-management-v1 and color-representation-v1 */
#ifdef CONF_HAVE_COLOR_MANAGEMENT_V1
static uint32_t cm_intents, cm_features, cm_tfs, cm_prims;
static int cm_done;
static void cm_intent(void *d, struct wp_color_manager_v1 *m, uint32_t v) { (void)d; (void)m; if (v < 32) cm_intents |= 1u << v; }
static void cm_feature(void *d, struct wp_color_manager_v1 *m, uint32_t v) { (void)d; (void)m; if (v < 32) cm_features |= 1u << v; }
static void cm_tf(void *d, struct wp_color_manager_v1 *m, uint32_t v) { (void)d; (void)m; if (v < 32) cm_tfs |= 1u << v; }
static void cm_prim(void *d, struct wp_color_manager_v1 *m, uint32_t v) { (void)d; (void)m; if (v < 32) cm_prims |= 1u << v; }
static void cm_donef(void *d, struct wp_color_manager_v1 *m) { (void)d; (void)m; cm_done++; }
static const struct wp_color_manager_v1_listener cm_l = { cm_intent, cm_feature, cm_tf, cm_prim, cm_donef };
typedef struct { int ready, failed; uint32_t id, cause; } Img;
static void img_failed(void *d, struct wp_image_description_v1 *i, uint32_t cause, const char *msg) { (void)i; (void)msg; Img *m = d; m->failed = 1; m->cause = cause; }
static void img_ready(void *d, struct wp_image_description_v1 *i, uint32_t id) { (void)i; Img *m = d; m->ready = 1; m->id = id; }
static const struct wp_image_description_v1_listener img_l = { img_failed, img_ready };
typedef struct { int done, prim, prim_named, tf_named, tf_power, lum, tprim, tlum, tcll, tfall; } Info;
static void in_done(void *d, struct wp_image_description_info_v1 *i) { (void)i; ((Info *)d)->done++; }
static void in_icc(void *d, struct wp_image_description_info_v1 *i, int32_t fd, uint32_t n) { (void)d; (void)i; (void)n; close(fd); }
static void in_prim(void *d, struct wp_image_description_info_v1 *i, int32_t a, int32_t b, int32_t c, int32_t e, int32_t f, int32_t g, int32_t h, int32_t k) {
    (void)i; (void)a; (void)b; (void)c; (void)e; (void)f; (void)g; (void)h; (void)k; ((Info *)d)->prim++;
}
static void in_prim_named(void *d, struct wp_image_description_info_v1 *i, uint32_t v) { (void)i; (void)v; ((Info *)d)->prim_named++; }
static void in_tf_power(void *d, struct wp_image_description_info_v1 *i, uint32_t v) { (void)i; (void)v; ((Info *)d)->tf_power++; }
static void in_tf_named(void *d, struct wp_image_description_info_v1 *i, uint32_t v) { (void)i; (void)v; ((Info *)d)->tf_named++; }
static void in_lum(void *d, struct wp_image_description_info_v1 *i, uint32_t a, uint32_t b, uint32_t c) { (void)i; (void)a; (void)b; (void)c; ((Info *)d)->lum++; }
static void in_tprim(void *d, struct wp_image_description_info_v1 *i, int32_t a, int32_t b, int32_t c, int32_t e, int32_t f, int32_t g, int32_t h, int32_t k) {
    (void)i; (void)a; (void)b; (void)c; (void)e; (void)f; (void)g; (void)h; (void)k; ((Info *)d)->tprim++;
}
static void in_tlum(void *d, struct wp_image_description_info_v1 *i, uint32_t a, uint32_t b) { (void)i; (void)a; (void)b; ((Info *)d)->tlum++; }
static void in_tcll(void *d, struct wp_image_description_info_v1 *i, uint32_t a) { (void)i; (void)a; ((Info *)d)->tcll++; }
static void in_tfall(void *d, struct wp_image_description_info_v1 *i, uint32_t a) { (void)i; (void)a; ((Info *)d)->tfall++; }
static const struct wp_image_description_info_v1_listener in_l = { in_done, in_icc, in_prim, in_prim_named, in_tf_power, in_tf_named, in_lum, in_tprim, in_tlum, in_tcll, in_tfall };
static int out_changed, pref_changed;
static void cmo_changed(void *d, struct wp_color_management_output_v1 *o) { (void)d; (void)o; out_changed++; }
static const struct wp_color_management_output_v1_listener cmo_l = { cmo_changed };
static void fb_changed(void *d, struct wp_color_management_surface_feedback_v1 *f, uint32_t id) { (void)d; (void)f; (void)id; pref_changed++; }
static const struct wp_color_management_surface_feedback_v1_listener cmfb_l = { fb_changed };

static struct wp_image_description_v1 *img_watch(Conn *c, struct wp_image_description_v1 *d, Img *m) {
    memset(m, 0, sizeof *m);
    wp_image_description_v1_add_listener(d, &img_l, m);
    double end = now_ms() + 2000;
    while (!m->ready && !m->failed && now_ms() < end) if (cx_wait(c, NULL, 30) < 0) break;
    return d;
}
static struct wp_image_description_v1 *img_params(Conn *c, struct wp_color_manager_v1 *cm, uint32_t tf, uint32_t prim, Img *m) {
    struct wp_image_description_creator_params_v1 *p = wp_color_manager_v1_create_parametric_creator(cm);
    wp_image_description_creator_params_v1_set_tf_named(p, tf);
    wp_image_description_creator_params_v1_set_primaries_named(p, prim);
    return img_watch(c, wp_image_description_creator_params_v1_create(p), m);
}
static Conn *cm_open(struct wp_color_manager_v1 **cm) {
    Conn *c = cx_open();
    *cm = c ? bindv(c, &wp_color_manager_v1_interface, 0) : NULL;
    return c;
}
#define CM_ERR(id, hint, what, iface, code, body) do { \
        struct wp_color_manager_v1 *cm; Conn *c = cm_open(&cm); \
        if (!c || !cm) { skip(item, id, hint, "not advertised"); if (c) cx_close(c); break; } \
        body; \
        int ok_ = cx_expect(c, iface, code); \
        check(item, id, ok_, hint, what ": %s", cx_errtext(c)); \
        cx_close(c); } while (0)
/* the same, for an error on an object the request itself destroyed (create is a destructor) */
#define CM_ERR_GONE(id, hint, what, iface, code, body) do { \
        struct wp_color_manager_v1 *cm; Conn *c = cm_open(&cm); \
        if (!c || !cm) { skip(item, id, hint, "not advertised"); if (c) cx_close(c); break; } \
        body; \
        int ok_ = cx_expect_id(c, iface, 0, code); \
        check(item, id, ok_, hint, what ": %s", cx_errtext(c)); \
        cx_close(c); } while (0)

static void f_color(void) {
    ITEM("wp_color_manager_v1");
    {
        struct wp_color_manager_v1 *cm;
        Conn *c = cm_open(&cm);
        if (!c || !cm) { skip(item, "color.supported", item, "not advertised"); if (c) cx_close(c); return; }
        cm_intents = cm_features = cm_tfs = cm_prims = 0;
        cm_done = 0;
        wp_color_manager_v1_add_listener(cm, &cm_l, NULL);
        cx_sync(c);
        check(item, "color.supported", cm_done == 1 && (cm_intents & 3) == 3 && (cm_features & (1u << WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC)) && cm_tfs && cm_prims,
              "wp_color_manager_v1.supported_intent,wp_color_manager_v1.supported_feature,wp_color_manager_v1.supported_tf_named,wp_color_manager_v1.supported_primaries_named,wp_color_manager_v1.done",
              "on bind: intents %#x, features %#x, transfer functions %#x, primaries %#x, done %d", cm_intents, cm_features, cm_tfs, cm_prims, cm_done);
        /* the display's description, and everything get_information tells of it */
        struct wp_color_management_output_v1 *o = wp_color_manager_v1_get_output(cm, c->out);
        out_changed = 0;
        wp_color_management_output_v1_add_listener(o, &cmo_l, NULL);
        Img od;
        struct wp_image_description_v1 *odesc = img_watch(c, wp_color_management_output_v1_get_image_description(o), &od);
        check(item, "color.output", od.ready && od.id, "wp_color_manager_v1.get_output,wp_color_management_output_v1.get_image_description,wp_image_description_v1.ready",
              "the display's image description: ready (identity %u)", od.id);
        Info in = { 0 };
        struct wp_image_description_info_v1 *inf = wp_image_description_v1_get_information(odesc);
        wp_image_description_info_v1_add_listener(inf, &in_l, &in);
        cx_sync(c);
        check(item, "color.information", in.done == 1 && in.prim == 1 && in.prim_named && (in.tf_named || in.tf_power) && in.lum && in.tprim && in.tlum,
              "wp_image_description_v1.get_information,wp_image_description_info_v1.primaries,wp_image_description_info_v1.primaries_named,wp_image_description_info_v1.tf_named,wp_image_description_info_v1.luminances,wp_image_description_info_v1.target_primaries,wp_image_description_info_v1.target_luminance,wp_image_description_info_v1.done",
              "get_information: primaries %d, named %d, tf named %d / power %d, luminances %d, target primaries %d, target luminance %d, done %d",
              in.prim, in.prim_named, in.tf_named, in.tf_power, in.lum, in.tprim, in.tlum, in.done);
        /* a window's preferred description is its display's */
        Win w;
        if (win_map(c, &w, "wlconf-color", 200, 150)) { skip(item, "color.feedback", item, "no window"); cx_close(c); return; }
        cx_wait(c, NULL, 300);
        struct wp_color_management_surface_feedback_v1 *fb = wp_color_manager_v1_get_surface_feedback(cm, w.s);
        pref_changed = 0;
        wp_color_management_surface_feedback_v1_add_listener(fb, &cmfb_l, NULL);
        Img pref, prefp;
        struct wp_image_description_v1 *pd = img_watch(c, wp_color_management_surface_feedback_v1_get_preferred(fb), &pref);
        img_watch(c, wp_color_management_surface_feedback_v1_get_preferred_parametric(fb), &prefp);
        check(item, "color.feedback", pref.ready && pref.id == od.id && prefp.ready && prefp.id == od.id,
              "wp_color_manager_v1.get_surface_feedback,wp_color_management_surface_feedback_v1.get_preferred,wp_color_management_surface_feedback_v1.get_preferred_parametric",
              "preferred and preferred parametric: the display's (%u, %u; display %u)", pref.id, prefp.id, od.id);
        Info pin = { 0 };
        wp_image_description_info_v1_add_listener(wp_image_description_v1_get_information(pd), &in_l, &pin);
        cx_sync(c);
        check(item, "color.preferred_information", pin.done == 1, "wp_image_description_v1.get_information", "the preferred description tells its information too: done %d", pin.done);
        /* parametric descriptions: the same parameters give the same identity, others another; a power curve with
         * light levels; primaries on one line are refused (failed, unsupported) */
        Img a, a2, b, pw, bad;
        struct wp_image_description_v1 *ld = img_params(c, cm, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB, &a);
        img_params(c, cm, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB, &a2);
        img_params(c, cm, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB, WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3, &b);
        check(item, "color.parametric", a.ready && a2.ready && a.id == a2.id && b.ready && b.id != a.id && a.id != od.id,
              "wp_color_manager_v1.create_parametric_creator,wp_image_description_creator_params_v1.set_tf_named,wp_image_description_creator_params_v1.set_primaries_named,wp_image_description_creator_params_v1.create",
              "linear sRGB twice: one identity (%u, %u); sRGB on Display P3: another (%u)", a.id, a2.id, b.id);
        struct wp_image_description_creator_params_v1 *pp = wp_color_manager_v1_create_parametric_creator(cm);
        wp_image_description_creator_params_v1_set_tf_power(pp, 24000);
        wp_image_description_creator_params_v1_set_primaries(pp, 708000, 292000, 170000, 797000, 131000, 46000, 312700, 329000);
        wp_image_description_creator_params_v1_set_max_cll(pp, 50);
        wp_image_description_creator_params_v1_set_max_fall(pp, 40);
        img_watch(c, wp_image_description_creator_params_v1_create(pp), &pw);
        check(item, "color.power_primaries", pw.ready, "wp_image_description_creator_params_v1.set_tf_power,wp_image_description_creator_params_v1.set_primaries,wp_image_description_creator_params_v1.set_max_cll,wp_image_description_creator_params_v1.set_max_fall",
              "power 2.4 on BT.2020's primaries given as numbers, with max_cll and max_fall: ready %d", pw.ready);
        pp = wp_color_manager_v1_create_parametric_creator(cm);
        wp_image_description_creator_params_v1_set_tf_named(pp, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
        wp_image_description_creator_params_v1_set_primaries(pp, 200000, 200000, 400000, 400000, 600000, 600000, 312700, 329000);
        img_watch(c, wp_image_description_creator_params_v1_create(pp), &bad);
        check(item, "color.failed", bad.failed && bad.cause == WP_IMAGE_DESCRIPTION_V1_CAUSE_UNSUPPORTED, "wp_image_description_v1.failed",
              "primaries on one line: failed (%d), cause unsupported (%u)", bad.failed, bad.cause);
        /* set on the window, unset, the display's own, destroyed */
        struct wp_color_management_surface_v1 *cs = wp_color_manager_v1_get_surface(cm, w.s);
        wp_color_management_surface_v1_set_image_description(cs, ld, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
        wp_image_description_v1_destroy(ld);
        commit_buf(c, &w, 0xff804020);
        wp_color_management_surface_v1_unset_image_description(cs);
        wp_color_management_surface_v1_set_image_description(cs, odesc, WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE);
        wl_surface_commit(w.s);
        wp_color_management_surface_v1_destroy(cs);
        wl_surface_commit(w.s);
        check(item, "color.surface", cx_ok(c), "wp_color_manager_v1.get_surface,wp_color_management_surface_v1.set_image_description,wp_color_management_surface_v1.unset_image_description,wp_color_management_surface_v1.destroy,wp_image_description_v1.destroy",
              "linear light set on a window (perceptual), unset, the display's own (relative), destroyed, each committed: %s", cx_errtext(c));
        check(item, "color.no_spurious_changes", out_changed == 0 && pref_changed == 0, "wp_color_management_output_v1.image_description_changed,wp_color_management_surface_feedback_v1.preferred_changed",
              "one display that never changes: no image_description_changed (%d) and no preferred_changed (%d)", out_changed, pref_changed);
        wp_color_management_surface_feedback_v1_destroy(fb);
        wp_color_management_output_v1_destroy(o);
        wp_color_manager_v1_destroy(cm);
        check(item, "color.destroy", cx_ok(c), "wp_color_management_surface_feedback_v1.destroy,wp_color_management_output_v1.destroy,wp_color_manager_v1.destroy", "destroy: %s", cx_errtext(c));
        cx_close(c);
    }
    CM_ERR("color.icc_unsupported", "wp_color_manager_v1.error.unsupported_feature,wp_color_manager_v1.create_icc_creator", "create_icc_creator (ICC is not advertised)", &wp_color_manager_v1_interface,
           WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE, wp_color_manager_v1_create_icc_creator(cm));
    CM_ERR("color.scrgb_unsupported", "wp_color_manager_v1.create_windows_scrgb", "create_windows_scrgb (not advertised)", &wp_color_manager_v1_interface,
           WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE, wp_color_manager_v1_create_windows_scrgb(cm));
    CM_ERR("color.surface_exists", "wp_color_manager_v1.error.surface_exists", "get_surface twice for one wl_surface", &wp_color_manager_v1_interface,
           WP_COLOR_MANAGER_V1_ERROR_SURFACE_EXISTS, { struct wl_surface *s = wl_compositor_create_surface(c->comp); wp_color_manager_v1_get_surface(cm, s); wp_color_manager_v1_get_surface(cm, s); });
    CM_ERR("color.invalid_tf", "wp_image_description_creator_params_v1.error.invalid_tf", "PQ (not advertised)", &wp_image_description_creator_params_v1_interface,
           WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF,
           wp_image_description_creator_params_v1_set_tf_named(wp_color_manager_v1_create_parametric_creator(cm), WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ));
    CM_ERR("color.invalid_tf_power", "wp_image_description_creator_params_v1.error.invalid_tf", "power 0.5 (below 1.0)", &wp_image_description_creator_params_v1_interface,
           WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF, wp_image_description_creator_params_v1_set_tf_power(wp_color_manager_v1_create_parametric_creator(cm), 5000));
    CM_ERR("color.invalid_primaries_named", "wp_image_description_creator_params_v1.error.invalid_primaries_named", "named primaries 99", &wp_image_description_creator_params_v1_interface,
           WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_PRIMARIES_NAMED, wp_image_description_creator_params_v1_set_primaries_named(wp_color_manager_v1_create_parametric_creator(cm), 99));
    CM_ERR("color.already_set", "wp_image_description_creator_params_v1.error.already_set", "a transfer function set twice", &wp_image_description_creator_params_v1_interface,
           WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET, {
               struct wp_image_description_creator_params_v1 *p = wp_color_manager_v1_create_parametric_creator(cm);
               wp_image_description_creator_params_v1_set_tf_named(p, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
               wp_image_description_creator_params_v1_set_tf_named(p, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB); });
    CM_ERR_GONE("color.incomplete_set", "wp_image_description_creator_params_v1.error.incomplete_set", "create with no primaries", &wp_image_description_creator_params_v1_interface,
                WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INCOMPLETE_SET, {
                    struct wp_image_description_creator_params_v1 *p = wp_color_manager_v1_create_parametric_creator(cm);
                    wp_image_description_creator_params_v1_set_tf_named(p, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
                    wp_image_description_creator_params_v1_create(p); });
    CM_ERR("color.unsupported_luminances", "wp_image_description_creator_params_v1.error.unsupported_feature,wp_image_description_creator_params_v1.set_luminances", "set_luminances (not advertised)", &wp_image_description_creator_params_v1_interface,
           WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE, wp_image_description_creator_params_v1_set_luminances(wp_color_manager_v1_create_parametric_creator(cm), 2000, 80, 80));
    CM_ERR("color.unsupported_mastering", "wp_image_description_creator_params_v1.set_mastering_display_primaries", "set_mastering_display_primaries (not advertised)",
           &wp_image_description_creator_params_v1_interface, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE,
           wp_image_description_creator_params_v1_set_mastering_display_primaries(wp_color_manager_v1_create_parametric_creator(cm), 1, 1, 1, 1, 1, 1, 1, 1));
    CM_ERR("color.unsupported_mastering_luminance", "wp_image_description_creator_params_v1.set_mastering_luminance", "set_mastering_luminance (not advertised)",
           &wp_image_description_creator_params_v1_interface, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_UNSUPPORTED_FEATURE,
           wp_image_description_creator_params_v1_set_mastering_luminance(wp_color_manager_v1_create_parametric_creator(cm), 1, 1000));
    CM_ERR_GONE("color.invalid_luminance", "wp_image_description_creator_params_v1.error.invalid_luminance", "max_cll above gamma 2.2's 80 cd/m²", &wp_image_description_creator_params_v1_interface,
                WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE, {
                    struct wp_image_description_creator_params_v1 *p = wp_color_manager_v1_create_parametric_creator(cm);
                    wp_image_description_creator_params_v1_set_tf_named(p, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
                    wp_image_description_creator_params_v1_set_primaries_named(p, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
                    wp_image_description_creator_params_v1_set_max_cll(p, 1000);
                    wp_image_description_creator_params_v1_create(p); });
    CM_ERR("color.no_information", "wp_image_description_v1.error.no_information", "get_information on a client's own description", &wp_image_description_v1_interface,
           WP_IMAGE_DESCRIPTION_V1_ERROR_NO_INFORMATION, {
               Img m;
               wp_image_description_v1_get_information(img_params(c, cm, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020, &m)); });
    CM_ERR("color.not_ready", "wp_image_description_v1.error.not_ready", "get_information on a failed description", &wp_image_description_v1_interface,
           WP_IMAGE_DESCRIPTION_V1_ERROR_NOT_READY, {
               Img m;
               struct wp_image_description_creator_params_v1 *p = wp_color_manager_v1_create_parametric_creator(cm);
               wp_image_description_creator_params_v1_set_tf_named(p, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
               wp_image_description_creator_params_v1_set_primaries(p, 200000, 200000, 400000, 400000, 600000, 600000, 312700, 329000);
               wp_image_description_v1_get_information(img_watch(c, wp_image_description_creator_params_v1_create(p), &m)); });
    CM_ERR("color.image_description", "wp_color_management_surface_v1.error.image_description", "set_image_description with a failed description",
           &wp_color_management_surface_v1_interface, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_IMAGE_DESCRIPTION, {
               Img m;
               struct wp_image_description_creator_params_v1 *p = wp_color_manager_v1_create_parametric_creator(cm);
               wp_image_description_creator_params_v1_set_tf_named(p, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
               wp_image_description_creator_params_v1_set_primaries(p, 200000, 200000, 400000, 400000, 600000, 600000, 312700, 329000);
               struct wp_image_description_v1 *d = img_watch(c, wp_image_description_creator_params_v1_create(p), &m);
               wp_color_management_surface_v1_set_image_description(wp_color_manager_v1_get_surface(cm, wl_compositor_create_surface(c->comp)), d, 0); });
    CM_ERR("color.render_intent", "wp_color_management_surface_v1.error.render_intent", "the absolute intent (not advertised)", &wp_color_management_surface_v1_interface,
           WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_RENDER_INTENT, {
               Img m;
               struct wp_image_description_v1 *d = img_params(c, cm, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB, &m);
               wp_color_management_surface_v1_set_image_description(wp_color_manager_v1_get_surface(cm, wl_compositor_create_surface(c->comp)), d, WP_COLOR_MANAGER_V1_RENDER_INTENT_ABSOLUTE); });
    CM_ERR("color.surface_inert", "wp_color_management_surface_v1.error.inert", "set_image_description after the wl_surface is gone", &wp_color_management_surface_v1_interface,
           WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT, {
               Img m;
               struct wp_image_description_v1 *d = img_params(c, cm, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB, &m);
               struct wl_surface *s = wl_compositor_create_surface(c->comp);
               struct wp_color_management_surface_v1 *cs = wp_color_manager_v1_get_surface(cm, s);
               wl_surface_destroy(s);
               wp_color_management_surface_v1_set_image_description(cs, d, 0); });
    CM_ERR("color.feedback_inert", "wp_color_management_surface_feedback_v1.error.inert", "get_preferred after the wl_surface is gone",
           &wp_color_management_surface_feedback_v1_interface, WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_ERROR_INERT, {
               struct wl_surface *s = wl_compositor_create_surface(c->comp);
               struct wp_color_management_surface_feedback_v1 *fb = wp_color_manager_v1_get_surface_feedback(cm, s);
               wl_surface_destroy(s);
               wp_color_management_surface_feedback_v1_get_preferred(fb); });
}
#endif

#ifdef CONF_HAVE_COLOR_REPRESENTATION_V1
static uint32_t cr_alphas;
static int cr_coefs, cr_done;
static void cr_alpha(void *d, struct wp_color_representation_manager_v1 *m, uint32_t a) { (void)d; (void)m; if (a < 32) cr_alphas |= 1u << a; }
static void cr_coef(void *d, struct wp_color_representation_manager_v1 *m, uint32_t a, uint32_t b) { (void)d; (void)m; (void)a; (void)b; cr_coefs++; }
static void cr_donef(void *d, struct wp_color_representation_manager_v1 *m) { (void)d; (void)m; cr_done++; }
static const struct wp_color_representation_manager_v1_listener cr_l = { cr_alpha, cr_coef, cr_donef };
#define CR_ERR(id, hint, what, code, body) do { \
        Conn *c = cx_open(); \
        struct wp_color_representation_manager_v1 *crm = c ? bindv(c, &wp_color_representation_manager_v1_interface, 0) : NULL; \
        if (!c || !crm) { skip(item, id, hint, "not advertised"); if (c) cx_close(c); break; } \
        struct wl_surface *s = wl_compositor_create_surface(c->comp); \
        body; \
        int ok_ = cx_expect(c, &wp_color_representation_surface_v1_interface, code); \
        check(item, id, ok_, hint, what ": %s", cx_errtext(c)); \
        cx_close(c); } while (0)
static void f_color_repr(void) {
    ITEM("wp_color_representation_manager_v1");
    {
        OPEN_OR_SKIP("repr.supported");
        struct wp_color_representation_manager_v1 *crm = bindv(c, &wp_color_representation_manager_v1_interface, 0);
        if (!crm) { skip(item, "repr.supported", item, "not advertised"); cx_close(c); return; }
        cr_alphas = 0;
        cr_coefs = cr_done = 0;
        wp_color_representation_manager_v1_add_listener(crm, &cr_l, NULL);
        cx_sync(c);
        check(item, "repr.supported", cr_done == 1 && cr_alphas == 7 && cr_coefs >= 1,
              "wp_color_representation_manager_v1.supported_alpha_mode,wp_color_representation_manager_v1.supported_coefficients_and_ranges,wp_color_representation_manager_v1.done",
              "on bind: alpha modes %#x (the three), %d coefficient/range pair(s), done %d", cr_alphas, cr_coefs, cr_done);
        Win w;
        if (win_map(c, &w, "wlconf-repr", 200, 150)) { skip(item, "repr.set", item, "no window"); cx_close(c); return; }
        struct wp_color_representation_surface_v1 *rs = wp_color_representation_manager_v1_get_surface(crm, w.s);
        wp_color_representation_surface_v1_set_alpha_mode(rs, WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_STRAIGHT);
        wp_color_representation_surface_v1_set_coefficients_and_range(rs, WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_IDENTITY, WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL);
        commit_buf(c, &w, 0x80804020);
        wp_color_representation_surface_v1_set_alpha_mode(rs, WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_PREMULTIPLIED_OPTICAL);
        wl_surface_commit(w.s);
        wp_color_representation_surface_v1_set_alpha_mode(rs, WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_PREMULTIPLIED_ELECTRICAL);
        wl_surface_commit(w.s);
        check(item, "repr.set", cx_ok(c), "wp_color_representation_manager_v1.get_surface,wp_color_representation_surface_v1.set_alpha_mode,wp_color_representation_surface_v1.set_coefficients_and_range",
              "straight, optical and electrical premultiplied alpha, identity/full, each committed on a window: %s", cx_errtext(c));
        /* a chroma location is for subsampled YCbCr only: with no buffer it waits for the commit */
        struct wl_surface *s2 = wl_compositor_create_surface(c->comp);
        struct wp_color_representation_surface_v1 *rs2 = wp_color_representation_manager_v1_get_surface(crm, s2);
        wp_color_representation_surface_v1_set_chroma_location(rs2, WP_COLOR_REPRESENTATION_SURFACE_V1_CHROMA_LOCATION_TYPE_0);
        wp_color_representation_surface_v1_set_chroma_location(rs2, 0);
        wl_surface_commit(s2);
        check(item, "repr.chroma_none", cx_ok(c), "wp_color_representation_surface_v1.set_chroma_location", "a chroma location set, then none, committed with no buffer: %s", cx_errtext(c));
        wp_color_representation_surface_v1_destroy(rs2);
        wp_color_representation_surface_v1_destroy(rs);
        wl_surface_commit(w.s);
        wp_color_representation_manager_v1_destroy(crm);
        check(item, "repr.destroy", cx_ok(c), "wp_color_representation_surface_v1.destroy,wp_color_representation_manager_v1.destroy", "destroy (back to the defaults): %s", cx_errtext(c));
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("repr.surface_exists");
        struct wp_color_representation_manager_v1 *crm = bindv(c, &wp_color_representation_manager_v1_interface, 0);
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        wp_color_representation_manager_v1_get_surface(crm, s);
        wp_color_representation_manager_v1_get_surface(crm, s);
        int ok = cx_expect(c, &wp_color_representation_manager_v1_interface, WP_COLOR_REPRESENTATION_MANAGER_V1_ERROR_SURFACE_EXISTS);
        check(item, "repr.surface_exists", ok, "wp_color_representation_manager_v1.error.surface_exists", "get_surface twice: %s", cx_errtext(c));
        cx_close(c);
    }
    CR_ERR("repr.alpha_mode", "wp_color_representation_surface_v1.error.alpha_mode", "alpha mode 3 (none such)", WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_ALPHA_MODE,
           wp_color_representation_surface_v1_set_alpha_mode(wp_color_representation_manager_v1_get_surface(crm, s), 3));
    CR_ERR("repr.coefficients", "wp_color_representation_surface_v1.error.coefficients", "BT.709 limited (no YCbCr import is advertised)", WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_COEFFICIENTS,
           wp_color_representation_surface_v1_set_coefficients_and_range(wp_color_representation_manager_v1_get_surface(crm, s), WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT709,
                                                                         WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED));
    CR_ERR("repr.pixel_format", "wp_color_representation_surface_v1.error.pixel_format", "a chroma location committed with an RGB buffer", WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_PIXEL_FORMAT, {
        struct wp_color_representation_surface_v1 *rs = wp_color_representation_manager_v1_get_surface(crm, s);
        wp_color_representation_surface_v1_set_chroma_location(rs, WP_COLOR_REPRESENTATION_SURFACE_V1_CHROMA_LOCATION_TYPE_0);
        wl_surface_attach(s, shm_buffer(c, 8, 8, 0xff000000u), 0, 0);
        wl_surface_commit(s); });
    CR_ERR("repr.inert", "wp_color_representation_surface_v1.error.inert", "set_alpha_mode after the wl_surface is gone", WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_INERT, {
        struct wp_color_representation_surface_v1 *rs = wp_color_representation_manager_v1_get_surface(crm, s);
        wl_surface_destroy(s);
        wp_color_representation_surface_v1_set_alpha_mode(rs, 0); });
}
#endif

/* ---------------------------------------------------------------- linux-drm-syncobj-v1 (a render node's timelines, udmabufs) */
#if defined(CONF_HAVE_LINUX_DRM_SYNCOBJ_V1) && defined(CONF_HAVE_LINUX_DMABUF_V1) && defined(CONF_HAVE_XDG_SHELL)
static int so_rnode = -1, so_udmabuf = -1;
static int so_open_rnode(void) {
    for (int i = 128; i < 192; i++) {
        char p[64];
        snprintf(p, sizeof p, "/dev/dri/renderD%d", i);
        int fd = open(p, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        struct drm_get_cap cap = { .capability = DRM_CAP_SYNCOBJ_TIMELINE };
        if (!ioctl(fd, DRM_IOCTL_GET_CAP, &cap) && cap.value) return fd;
        close(fd);
    }
    return -1;
}
typedef struct { uint32_t h; struct wp_linux_drm_syncobj_timeline_v1 *t; } Tl;
static Tl tl_new(struct wp_linux_drm_syncobj_manager_v1 *som) {
    Tl t = { 0, NULL };
    struct drm_syncobj_create cr = { 0 };
    if (ioctl(so_rnode, DRM_IOCTL_SYNCOBJ_CREATE, &cr)) return t;
    t.h = cr.handle;
    struct drm_syncobj_handle a = { .handle = cr.handle };
    if (ioctl(so_rnode, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &a)) return t;
    t.t = wp_linux_drm_syncobj_manager_v1_import_timeline(som, a.fd);
    close(a.fd);
    return t;
}
static void tl_signal(Tl t, uint64_t pt) {
    struct drm_syncobj_timeline_array a = { .handles = (uintptr_t)&t.h, .points = (uintptr_t)&pt, .count_handles = 1 };
    if (ioctl(so_rnode, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &a)) {}
}
static int tl_signalled(Tl t, uint64_t pt, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t deadline = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec + (int64_t)ms * 1000000LL;
    struct drm_syncobj_timeline_wait w = { .handles = (uintptr_t)&t.h, .points = (uintptr_t)&pt, .timeout_nsec = deadline, .count_handles = 1,
                                           .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT };
    return ioctl(so_rnode, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &w) == 0;
}
#define SO_W 64
#define SO_H 48
static struct wl_buffer *udma_buffer(Conn *c, struct zwp_linux_dmabuf_v1 *dm, uint32_t color) {
    size_t stride = SO_W * 4, size = (stride * SO_H + 4095) & ~(size_t)4095;
    int memfd = memfd_create("wlconf-udmabuf", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    if (memfd < 0 || ftruncate(memfd, (off_t)size) || fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK)) { if (memfd >= 0) close(memfd); return NULL; }
    uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (px != MAP_FAILED) { for (int i = 0; i < SO_W * SO_H; i++) px[i] = color; munmap(px, size); }
    struct udmabuf_create u = { .memfd = (uint32_t)memfd, .flags = UDMABUF_FLAGS_CLOEXEC, .offset = 0, .size = size };
    int fd = ioctl(so_udmabuf, UDMABUF_CREATE, &u);
    close(memfd);
    if (fd < 0) return NULL;
    struct zwp_linux_buffer_params_v1 *p = zwp_linux_dmabuf_v1_create_params(dm);
    zwp_linux_buffer_params_v1_add(p, fd, 0, 0, (uint32_t)stride, 0, 0);    /* DRM_FORMAT_MOD_LINEAR */
    struct wl_buffer *b = zwp_linux_buffer_params_v1_create_immed(p, SO_W, SO_H, 0x34325258 /* XR24 */, 0);
    zwp_linux_buffer_params_v1_destroy(p);
    close(fd);
    (void)c;
    return b;
}
typedef struct { Conn *c; struct wp_linux_drm_syncobj_manager_v1 *som; struct zwp_linux_dmabuf_v1 *dm; Win w; struct wp_linux_drm_syncobj_surface_v1 *so; } SoCtx;
static int so_setup(SoCtx *x, const char *title, int map_window) {
    memset(x, 0, sizeof *x);
    x->c = cx_open();
    if (!x->c) return -1;
    x->som = bindv(x->c, &wp_linux_drm_syncobj_manager_v1_interface, 0);
    x->dm = bindv(x->c, &zwp_linux_dmabuf_v1_interface, 3);
    if (!x->som || !x->dm) return -1;
    if (win_role(x->c, &x->w, title)) return -1;
    x->so = wp_linux_drm_syncobj_manager_v1_get_surface(x->som, x->w.s);
    (void)map_window;
    return 0;
}
static void so_commit(SoCtx *x, struct wl_buffer *b, Tl acq, uint64_t ap, Tl rel, uint64_t rp) {
    wl_surface_attach(x->w.s, b, 0, 0);
    wl_surface_damage_buffer(x->w.s, 0, 0, SO_W, SO_H);
    wp_linux_drm_syncobj_surface_v1_set_acquire_point(x->so, acq.t, (uint32_t)(ap >> 32), (uint32_t)ap);
    wp_linux_drm_syncobj_surface_v1_set_release_point(x->so, rel.t, (uint32_t)(rp >> 32), (uint32_t)rp);
    wl_surface_commit(x->w.s);
}
#define SO_ERR(id, hint, what, iface, code, body) do { \
        SoCtx x; \
        if (so_setup(&x, "wlconf-syncobj-error", 0)) { skip(item, id, hint, "no connection or globals"); if (x.c) cx_close(x.c); break; } \
        Conn *c = x.c; \
        body; \
        int ok_ = cx_expect(c, iface, code); \
        check(item, id, ok_, hint, what ": %s", cx_errtext(c)); \
        cx_close(c); } while (0)
static void f_syncobj(void) {
    ITEM("wp_linux_drm_syncobj_manager_v1");
    if (so_rnode < 0) so_rnode = so_open_rnode();
    if (so_udmabuf < 0) so_udmabuf = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (so_rnode < 0 || so_udmabuf < 0) {
        skip(item, "syncobj.wait", "wp_linux_drm_syncobj_surface_v1.set_acquire_point", "needs a render node with timeline syncobjs and /dev/udmabuf (%s)", so_rnode < 0 ? "no render node" : strerror(errno));
        return;
    }
    {
        SoCtx x;
        if (so_setup(&x, "wlconf-syncobj", 1)) { skip(item, "syncobj.wait", item, "not advertised"); if (x.c) cx_close(x.c); return; }
        Conn *c = x.c;
        Tl a = tl_new(x.som), r1 = tl_new(x.som), r2 = tl_new(x.som);
        struct wl_buffer *b1 = udma_buffer(c, x.dm, 0xff3060c0), *b2 = udma_buffer(c, x.dm, 0xffc06030);
        check(item, "syncobj.import", a.t && r1.t && r2.t && b1 && b2 && cx_ok(c), "wp_linux_drm_syncobj_manager_v1.get_surface,wp_linux_drm_syncobj_manager_v1.import_timeline",
              "three timelines imported and a syncobj surface made: %s", cx_errtext(c));
        /* the first frame, its point signalled before the commit: applied at once */
        tl_signal(a, 1);
        frame_at(x.w.s, 0);
        so_commit(&x, b1, a, 1, r1, 1);
        cx_wait(c, &cbn[0], 2000);
        check(item, "syncobj.signalled", cbn[0] > 0, "wp_linux_drm_syncobj_surface_v1.set_acquire_point,wp_linux_drm_syncobj_surface_v1.set_release_point",
              "a frame whose acquire point was already signalled is applied: %s", cbn[0] ? "yes" : "no frame callback");
        int copied = tl_signalled(r1, 1, 300);
        /* the second, not signalled yet: held */
        frame_at(x.w.s, 1);
        so_commit(&x, b2, a, 2, r2, 1);
        cx_wait(c, &cbn[1], 500);
        int held = !cbn[1];
        tl_signal(a, 2);
        cx_wait(c, &cbn[1], 2000);
        check(item, "syncobj.wait", held && cbn[1], "wp_linux_drm_syncobj_surface_v1.set_acquire_point",
              "a frame with its acquire point not signalled is held (%s), then applied once it is (%s)", held ? "yes" : "no", cbn[1] ? "yes" : "no");
        int rel = copied || tl_signalled(r1, 1, 0);
        for (int i = 0; i < 40 && !rel; i++) { frame_at(x.w.s, 2); wl_surface_commit(x.w.s); cx_wait(c, &cbn[2], 100); rel = tl_signalled(r1, 1, 0); }
        check(item, "syncobj.release", rel, "wp_linux_drm_syncobj_surface_v1.set_release_point", "the replaced buffer's release point is signalled (%s)", copied ? "copied at once: the CPU path" : "after the next frame");
        /* the syncobj object destroyed: implicit sync again, a plain commit is fine */
        wp_linux_drm_syncobj_surface_v1_destroy(x.so);
        wp_linux_drm_syncobj_timeline_v1_destroy(a.t);
        frame_at(x.w.s, 3);
        wl_surface_attach(x.w.s, b1, 0, 0);
        wl_surface_commit(x.w.s);
        cx_wait(c, &cbn[3], 2000);
        check(item, "syncobj.destroy", cbn[3] && cx_ok(c), "wp_linux_drm_syncobj_surface_v1.destroy,wp_linux_drm_syncobj_timeline_v1.destroy",
              "the surface's syncobj object and a timeline destroyed, then a plain commit: %s", cbn[3] ? cx_errtext(c) : "no frame");
        close_win(c, &x.w);
        check(item, "syncobj.last_release", tl_signalled(r2, 1, 1000), "wp_linux_drm_syncobj_surface_v1.set_release_point", "the last buffer's release point is signalled when its surface goes");
        wp_linux_drm_syncobj_manager_v1_destroy(x.som);
        check(item, "syncobj.manager_destroy", cx_ok(c), "wp_linux_drm_syncobj_manager_v1.destroy", "destroy: %s", cx_errtext(c));
        cx_close(c);
    }
    const struct wl_interface *si = &wp_linux_drm_syncobj_surface_v1_interface, *mi = &wp_linux_drm_syncobj_manager_v1_interface;
    SO_ERR("syncobj.no_acquire_point", "wp_linux_drm_syncobj_surface_v1.error.no_acquire_point", "a buffer with no acquire point", si, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_ACQUIRE_POINT, {
        Tl r = tl_new(x.som);
        wl_surface_attach(x.w.s, udma_buffer(c, x.dm, 0), 0, 0);
        wp_linux_drm_syncobj_surface_v1_set_release_point(x.so, r.t, 0, 1);
        wl_surface_commit(x.w.s); });
    SO_ERR("syncobj.no_release_point", "wp_linux_drm_syncobj_surface_v1.error.no_release_point", "a buffer with no release point", si, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_RELEASE_POINT, {
        Tl a = tl_new(x.som);
        wl_surface_attach(x.w.s, udma_buffer(c, x.dm, 0), 0, 0);
        wp_linux_drm_syncobj_surface_v1_set_acquire_point(x.so, a.t, 0, 1);
        wl_surface_commit(x.w.s); });
    SO_ERR("syncobj.no_buffer", "wp_linux_drm_syncobj_surface_v1.error.no_buffer", "points with no buffer", si, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_BUFFER, {
        Tl a = tl_new(x.som);
        Tl r = tl_new(x.som);
        wp_linux_drm_syncobj_surface_v1_set_acquire_point(x.so, a.t, 0, 1);
        wp_linux_drm_syncobj_surface_v1_set_release_point(x.so, r.t, 0, 1);
        wl_surface_commit(x.w.s); });
    SO_ERR("syncobj.unsupported_buffer", "wp_linux_drm_syncobj_surface_v1.error.unsupported_buffer", "a shared-memory buffer with points", si, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_UNSUPPORTED_BUFFER, {
        Tl a = tl_new(x.som);
        Tl r = tl_new(x.som);
        so_commit(&x, shm_buffer(c, SO_W, SO_H, 0), a, 1, r, 1); });
    SO_ERR("syncobj.conflicting_points", "wp_linux_drm_syncobj_surface_v1.error.conflicting_points", "acquire and release on one timeline, release not above acquire", si,
           WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS, {
        Tl a = tl_new(x.som);
        so_commit(&x, udma_buffer(c, x.dm, 0), a, 7, a, 3); });
    SO_ERR("syncobj.surface_exists", "wp_linux_drm_syncobj_manager_v1.error.surface_exists", "a second syncobj object for a surface", mi, WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_SURFACE_EXISTS,
           wp_linux_drm_syncobj_manager_v1_get_surface(x.som, x.w.s));
    SO_ERR("syncobj.no_surface", "wp_linux_drm_syncobj_surface_v1.error.no_surface", "a point after the wl_surface is gone", si, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_SURFACE, {
        Tl a = tl_new(x.som);
        xdg_toplevel_destroy(x.w.top);
        xdg_surface_destroy(x.w.xs);
        wl_surface_destroy(x.w.s);
        wp_linux_drm_syncobj_surface_v1_set_acquire_point(x.so, a.t, 0, 1); });
    SO_ERR("syncobj.invalid_timeline", "wp_linux_drm_syncobj_manager_v1.error.invalid_timeline", "a timeline fd that is not a syncobj", mi, WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_INVALID_TIMELINE, {
        int fd = memfd_create("not-a-syncobj", MFD_CLOEXEC);
        wp_linux_drm_syncobj_manager_v1_import_timeline(x.som, fd);
        close(fd); });
}
#endif

/* ---------------------------------------------------------------- drm-lease-v1 on the test hooks' fake card */
#ifdef CONF_HAVE_DRM_LEASE_V1
typedef struct {
    struct wp_drm_lease_device_v1 *dev;
    int drm_fd, done, released, nconn;
    struct wp_drm_lease_connector_v1 *conn[8];
    int cname, cdesc, cid, cdone, withdrawn;
    uint32_t cids[8];
} LsDev;
static void lc_name(void *d, struct wp_drm_lease_connector_v1 *k, const char *n) { (void)k; (void)n; ((LsDev *)d)->cname++; }
static void lc_desc(void *d, struct wp_drm_lease_connector_v1 *k, const char *n) { (void)k; (void)n; ((LsDev *)d)->cdesc++; }
static void lc_id(void *d, struct wp_drm_lease_connector_v1 *k, uint32_t id) {
    LsDev *l = d;
    l->cid++;
    for (int i = 0; i < l->nconn && i < 8; i++) if (l->conn[i] == k) l->cids[i] = id;
}
static void lc_done(void *d, struct wp_drm_lease_connector_v1 *k) { (void)k; ((LsDev *)d)->cdone++; }
static void lc_withdrawn(void *d, struct wp_drm_lease_connector_v1 *k) { (void)k; ((LsDev *)d)->withdrawn++; }
static const struct wp_drm_lease_connector_v1_listener lc_l = { lc_name, lc_desc, lc_id, lc_done, lc_withdrawn };
static void ld_fd(void *d, struct wp_drm_lease_device_v1 *v, int32_t fd) { (void)v; LsDev *l = d; if (l->drm_fd >= 0) close(l->drm_fd); l->drm_fd = fd; }
static void ld_conn(void *d, struct wp_drm_lease_device_v1 *v, struct wp_drm_lease_connector_v1 *k) {
    (void)v;
    LsDev *l = d;
    if (l->nconn < 8) l->conn[l->nconn] = k;
    l->nconn++;
    wp_drm_lease_connector_v1_add_listener(k, &lc_l, l);
}
static void ld_done(void *d, struct wp_drm_lease_device_v1 *v) { (void)v; ((LsDev *)d)->done++; }
static void ld_released(void *d, struct wp_drm_lease_device_v1 *v) { ((LsDev *)d)->released++; wp_drm_lease_device_v1_destroy(v); }
static const struct wp_drm_lease_device_v1_listener ld_l = { ld_fd, ld_conn, ld_done, ld_released };
typedef struct { int fd, finished; } LsLease;
static void ll_fd(void *d, struct wp_drm_lease_v1 *l, int32_t fd) { (void)l; ((LsLease *)d)->fd = fd; }
static void ll_finished(void *d, struct wp_drm_lease_v1 *l) { (void)l; ((LsLease *)d)->finished++; }
static const struct wp_drm_lease_v1_listener ll_l = { ll_fd, ll_finished };
static int ls_bind(Conn *c, LsDev *l) {
    memset(l, 0, sizeof *l);
    l->drm_fd = -1;
    l->dev = bindv(c, &wp_drm_lease_device_v1_interface, 0);
    if (!l->dev) return -1;
    wp_drm_lease_device_v1_add_listener(l->dev, &ld_l, l);
    cx_sync(c);
    cx_wait(c, &l->done, 1000);
    return 0;
}
static struct wp_drm_lease_v1 *ls_lease(LsDev *l, LsLease *out) {
    memset(out, 0, sizeof *out);
    out->fd = -1;
    struct wp_drm_lease_request_v1 *q = wp_drm_lease_device_v1_create_lease_request(l->dev);
    wp_drm_lease_request_v1_request_connector(q, l->conn[0]);
    struct wp_drm_lease_v1 *ls = wp_drm_lease_request_v1_submit(q);
    wp_drm_lease_v1_add_listener(ls, &ll_l, out);
    return ls;
}
static void f_drm_lease(void) {
    ITEM("wp_drm_lease_device_v1");
    if (!th_have() || th("lease plug")) { skip(item, "lease.global", "wp_drm_lease_device_v1", "needs lord-server --test-hooks (a fake card)"); return; }
    {
        OPEN_OR_SKIP("lease.global");
        LsDev l;
        int bound = ls_bind(c, &l) == 0;
        check(item, "lease.global", bound, "wp_drm_lease_device_v1", "a non-desktop connector plugged in: the global is offered to a trusted client: %s", bound ? "yes" : "no");
        if (!bound) { cx_close(c); th("lease unplug"); return; }
        int fdok = l.drm_fd >= 0 && fcntl(l.drm_fd, F_GETFD) >= 0;
        check(item, "lease.offer", fdok && l.nconn == 1 && l.cname == 1 && l.cdesc == 1 && l.cid == 1 && l.cdone == 1 && l.done >= 1 && l.cids[0] == 42,
              "wp_drm_lease_device_v1.drm_fd,wp_drm_lease_device_v1.connector,wp_drm_lease_connector_v1.name,wp_drm_lease_connector_v1.description,wp_drm_lease_connector_v1.connector_id,wp_drm_lease_connector_v1.done,wp_drm_lease_device_v1.done",
              "drm_fd (%s), then only the headset (%d connector(s), id %u; never the desktop display), its name, description, id and done, then done", fdok ? "an fd" : "none", l.nconn, l.cids[0]);
        /* the first lease: the person is asked (the driver's theme answers "allow once"), then lease_fd */
        LsLease a;
        struct wp_drm_lease_v1 *la = ls_lease(&l, &a);
        double end = now_ms() + 3000;
        while (a.fd < 0 && !a.finished && now_ms() < end) cx_wait(c, NULL, 30);
        check(item, "lease.granted", a.fd >= 0 && !a.finished, "wp_drm_lease_device_v1.create_lease_request,wp_drm_lease_request_v1.request_connector,wp_drm_lease_request_v1.submit,wp_drm_lease_v1.lease_fd",
              "a lease of the headset, after the system's prompt (answered \"allow once\"): lease_fd %s", a.fd >= 0 ? "received" : a.finished ? "no: finished" : "no answer");
        cx_wait(c, &l.withdrawn, 1000);
        check(item, "lease.withdrawn", l.withdrawn == 1 && l.done >= 2, "wp_drm_lease_connector_v1.withdrawn,wp_drm_lease_device_v1.done",
              "while it is leased the connector is withdrawn from the offers (%d), then done (%d in all)", l.withdrawn, l.done);
        /* unplugged while leased: revoked, finished */
        th("lease unplug");
        end = now_ms() + 2000;
        while (!a.finished && now_ms() < end) cx_wait(c, NULL, 30);
        check(item, "lease.unplugged", a.finished == 1, "wp_drm_lease_v1.finished", "the headset unplugged while leased: the lease finished (%d)", a.finished);
        wp_drm_lease_v1_destroy(la);
        if (a.fd >= 0) close(a.fd);
        th("lease plug");
        cx_wait(c, NULL, 200);
        wp_drm_lease_connector_v1_destroy(l.conn[0]);
        wp_drm_lease_device_v1_release(l.dev);
        end = now_ms() + 1000;
        while (!l.released && now_ms() < end) cx_wait(c, NULL, 30);
        check(item, "lease.release", l.released == 1 && cx_ok(c), "wp_drm_lease_device_v1.release,wp_drm_lease_device_v1.released,wp_drm_lease_connector_v1.destroy,wp_drm_lease_v1.destroy",
              "the lease and connector destroyed, the device released: released %d, %s", l.released, cx_errtext(c));
        if (l.drm_fd >= 0) close(l.drm_fd);
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("lease.denied");
        LsDev l;
        if (ls_bind(c, &l) || !l.nconn) { skip(item, "lease.denied", "wp_drm_lease_v1.finished", "no device or connector"); cx_close(c); goto errors; }
        LsLease a, b;
        ls_lease(&l, &a);
        double end = now_ms() + 3000;
        while (a.fd < 0 && !a.finished && now_ms() < end) cx_wait(c, NULL, 30);
        check(item, "lease.denied", a.finished == 1 && a.fd < 0, "wp_drm_lease_v1.finished", "the second request: the person says \"don't allow\": finished, no lease_fd (%s)", a.fd >= 0 ? "it got one" : "none");
        ls_lease(&l, &b);
        end = now_ms() + 2000;
        while (b.fd < 0 && !b.finished && now_ms() < end) cx_wait(c, NULL, 30);
        check(item, "lease.saved_deny", b.finished == 1 && b.fd < 0, "wp_drm_lease_v1.finished", "after \"don't allow\" is saved: finished at once, no prompt (%d)", b.finished);
        if (l.drm_fd >= 0) close(l.drm_fd);
        cx_close(c);
    }
errors:
    {
        OPEN_OR_SKIP("lease.wrong_device");
        LsDev l1, l2;
        if (ls_bind(c, &l1) || ls_bind(c, &l2) || !l2.nconn) skip(item, "lease.wrong_device", "wp_drm_lease_request_v1.error.wrong_device", "no device or connector");
        else {
            struct wp_drm_lease_request_v1 *q = wp_drm_lease_device_v1_create_lease_request(l1.dev);
            wp_drm_lease_request_v1_request_connector(q, l2.conn[0]);
            int ok = cx_expect(c, &wp_drm_lease_request_v1_interface, WP_DRM_LEASE_REQUEST_V1_ERROR_WRONG_DEVICE);
            check(item, "lease.wrong_device", ok, "wp_drm_lease_request_v1.error.wrong_device", "a connector of another device object: %s", cx_errtext(c));
        }
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("lease.duplicate_connector");
        LsDev l;
        if (ls_bind(c, &l) || !l.nconn) skip(item, "lease.duplicate_connector", "wp_drm_lease_request_v1.error.duplicate_connector", "no device or connector");
        else {
            struct wp_drm_lease_request_v1 *q = wp_drm_lease_device_v1_create_lease_request(l.dev);
            wp_drm_lease_request_v1_request_connector(q, l.conn[0]);
            wp_drm_lease_request_v1_request_connector(q, l.conn[0]);
            int ok = cx_expect(c, &wp_drm_lease_request_v1_interface, WP_DRM_LEASE_REQUEST_V1_ERROR_DUPLICATE_CONNECTOR);
            check(item, "lease.duplicate_connector", ok, "wp_drm_lease_request_v1.error.duplicate_connector", "one connector requested twice: %s", cx_errtext(c));
        }
        cx_close(c);
    }
    {
        OPEN_OR_SKIP("lease.empty_lease");
        LsDev l;
        if (ls_bind(c, &l)) skip(item, "lease.empty_lease", "wp_drm_lease_request_v1.error.empty_lease", "no device");
        else {
            struct wp_drm_lease_request_v1 *q = wp_drm_lease_device_v1_create_lease_request(l.dev);
            uint32_t qid = wl_proxy_get_id((struct wl_proxy *)q);
            wp_drm_lease_request_v1_submit(q);
            int ok = cx_expect_id(c, &wp_drm_lease_request_v1_interface, qid, WP_DRM_LEASE_REQUEST_V1_ERROR_EMPTY_LEASE);
            check(item, "lease.empty_lease", ok, "wp_drm_lease_request_v1.error.empty_lease", "submit with no connector: %s", cx_errtext(c));
        }
        cx_close(c);
    }
    {
        /* unplugged: the global goes away (a new connection doesn't see it) */
        th("lease unplug");
        Conn *c = cx_open();
        int seen = c && adv(c, "wp_drm_lease_device_v1");
        check(item, "lease.global_removed", c && !seen, "wp_drm_lease_device_v1", "the headset unplugged: no global for a new client (%s)", seen ? "still offered" : "gone");
        cx_close(c);
    }
}
#endif

/* ---------------------------------------------------------------- xwayland-shell-v1, on a connection the test hooks make
 * as lord-server makes Xwayland's */
#if defined(CONF_HAVE_XWAYLAND_SHELL_V1) && defined(CONF_HAVE_XDG_SHELL)
/* the previous one may still be going away on the server's side: a few tries */
static Conn *xwl_open(void) {
    for (int i = 0; i < 20; i++) {
        int fd = -1;
        if (!th_cmd_fd("xwl", &fd) && fd >= 0) return cx_open_fd(fd);
        usleep(50000);
    }
    return NULL;
}
static void f_xwayland_shell(void) {
    ITEM("xwayland_shell_v1");
    if (!th_have()) { skip(item, "xwshell.hidden", "xwayland_shell_v1", "needs lord-server --test-hooks"); return; }
    {
        Conn *c = cx_open();
        int seen = c && adv(c, "xwayland_shell_v1");
        check(item, "xwshell.hidden", c && !seen, "xwayland_shell_v1", "an ordinary client doesn't see the global: %s", seen ? "it does" : "hidden");
        cx_close(c);
    }
    {
        Conn *c = xwl_open();
        if (!c) { skip(item, "xwshell.serial", "xwayland_shell_v1.get_xwayland_surface", "no Xwayland-like connection (Xwayland already running?)"); return; }
        struct xwayland_shell_v1 *sh = bindv(c, &xwayland_shell_v1_interface, 0);
        check(item, "xwshell.offered", sh != NULL, "xwayland_shell_v1", "offered to the connection lord-server made itself: %s", sh ? "yes" : "no");
        if (!sh) { cx_close(c); return; }
        struct wl_surface *s = wl_compositor_create_surface(c->comp);
        struct xwayland_surface_v1 *x = xwayland_shell_v1_get_xwayland_surface(sh, s);
        xwayland_surface_v1_set_serial(x, 0x1234, 0x1);
        wl_surface_commit(s);
        check(item, "xwshell.serial", cx_ok(c), "xwayland_shell_v1.get_xwayland_surface,xwayland_surface_v1.set_serial", "the xwayland_surface role and a serial, committed: %s", cx_errtext(c));
        xwayland_surface_v1_destroy(x);
        wl_surface_commit(s);
        xwayland_shell_v1_destroy(sh);
        check(item, "xwshell.destroy", cx_ok(c), "xwayland_surface_v1.destroy,xwayland_shell_v1.destroy", "destroy (the association stays): %s", cx_errtext(c));
        cx_close(c);
    }
#define XW_ERR(id, hint, what, iface, code, body) do { \
        Conn *c = xwl_open(); \
        struct xwayland_shell_v1 *sh = c ? bindv(c, &xwayland_shell_v1_interface, 0) : NULL; \
        if (!sh) { skip(item, id, hint, "no Xwayland-like connection"); cx_close(c); break; } \
        struct wl_surface *s = wl_compositor_create_surface(c->comp); \
        body; \
        int ok_ = cx_expect(c, iface, code); \
        check(item, id, ok_, hint, what ": %s", cx_errtext(c)); \
        cx_close(c); \
        usleep(100000); } while (0)
    XW_ERR("xwshell.role_twice", "xwayland_shell_v1.error.role", "get_xwayland_surface twice for one surface", &xwayland_shell_v1_interface, XWAYLAND_SHELL_V1_ERROR_ROLE, {
        xwayland_shell_v1_get_xwayland_surface(sh, s);
        xwayland_shell_v1_get_xwayland_surface(sh, s); });
    XW_ERR("xwshell.role_other", "xwayland_shell_v1.error.role", "get_xwayland_surface on a surface that is a subsurface", &xwayland_shell_v1_interface, XWAYLAND_SHELL_V1_ERROR_ROLE, {
        struct wl_subcompositor *sc = bindv(c, &wl_subcompositor_interface, 0);
        wl_subcompositor_get_subsurface(sc, s, wl_compositor_create_surface(c->comp));
        xwayland_shell_v1_get_xwayland_surface(sh, s); });
    XW_ERR("xwshell.role_then_xdg", "xdg_wm_base.error.role", "an xwayland_surface given an xdg_surface after", &xdg_wm_base_interface, XDG_WM_BASE_ERROR_ROLE, {
        xwayland_shell_v1_get_xwayland_surface(sh, s);
        xdg_wm_base_get_xdg_surface(c->wm, s); });
    XW_ERR("xwshell.invalid_serial", "xwayland_surface_v1.error.invalid_serial", "set_serial(0)", &xwayland_surface_v1_interface, XWAYLAND_SURFACE_V1_ERROR_INVALID_SERIAL,
           xwayland_surface_v1_set_serial(xwayland_shell_v1_get_xwayland_surface(sh, s), 0, 0));
    XW_ERR("xwshell.already_associated", "xwayland_surface_v1.error.already_associated", "a second serial committed on one surface", &xwayland_surface_v1_interface,
           XWAYLAND_SURFACE_V1_ERROR_ALREADY_ASSOCIATED, {
        struct xwayland_surface_v1 *x = xwayland_shell_v1_get_xwayland_surface(sh, s);
        xwayland_surface_v1_set_serial(x, 7, 0);
        wl_surface_commit(s);
        xwayland_surface_v1_set_serial(x, 8, 0);
        wl_surface_commit(s); });
#undef XW_ERR
}
#endif

/* org_kde_kwin_server_decoration_manager: the tables written by hand as in wl.c (KDE's XML, vendored in
 * third_party/plasma-wayland-protocols, gives the driver its static checks and the generic bind) */
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
    check(item, "kde.mode_on_create", kde_mode_n >= 1, "org_kde_kwin_server_decoration_manager.create,org_kde_kwin_server_decoration.mode", "mode when created: %d", kde_mode_n);
    /* "The server will acknowledge this by another event with the same mode" */
    static const uint32_t modes[] = { 1u /* Client */, 2u /* Server */, 0u /* None */ };
    static const char *const names[] = { "client", "server", "none" };
    for (int k = 0; k < 3; k++) {
        char id[32];
        snprintf(id, sizeof id, "kde.request_mode_%s", names[k]);
        int before = kde_mode_n;
        wl_proxy_marshal_flags(d, 1, NULL, 1, 0, modes[k]);
        cx_sync(c);
        check(item, id, kde_mode_n == before + 1 && kde_mode == modes[k], "org_kde_kwin_server_decoration.request_mode,org_kde_kwin_server_decoration.mode",
              "request_mode(%s) is answered with mode(%s): mode %u", names[k], names[k], kde_mode);
    }
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
#ifdef CONF_HAVE_WL_FIXES
    { "wl_fixes", f_fixes },
#endif
#ifdef CONF_HAVE_EXT_BACKGROUND_EFFECT_V1
    { "ext_background_effect_manager_v1", f_bgeffect },
#endif
#ifdef CONF_HAVE_FIFO_V1
    { "wp_fifo_manager_v1", f_fifo },
#endif
#ifdef CONF_HAVE_COMMIT_TIMING_V1
    { "wp_commit_timing_manager_v1", f_commit_timing },
#endif
#ifdef CONF_HAVE_TEARING_CONTROL_V1
    { "wp_tearing_control_manager_v1", f_tearing },
#endif
#ifdef CONF_HAVE_CONTENT_TYPE_V1
    { "wp_content_type_manager_v1", f_content_type },
#endif
#if defined(CONF_HAVE_POINTER_WARP_V1) && defined(CONF_HAVE_XDG_SHELL)
    { "wp_pointer_warp_v1", f_pointer_warp },
#endif
#if defined(CONF_HAVE_XDG_SYSTEM_BELL_V1) && defined(CONF_HAVE_XDG_SHELL)
    { "xdg_system_bell_v1", f_bell },
#endif
#if defined(CONF_HAVE_XDG_TOPLEVEL_TAG_V1) && defined(CONF_HAVE_XDG_SHELL)
    { "xdg_toplevel_tag_manager_v1", f_tag },
#endif
#if defined(CONF_HAVE_XDG_TOPLEVEL_DRAG_V1) && defined(CONF_HAVE_XDG_SHELL)
    { "xdg_toplevel_drag_manager_v1", f_toplevel_drag },
#endif
#if defined(CONF_HAVE_XDG_SESSION_MANAGEMENT_V1) && defined(CONF_HAVE_XDG_SHELL)
    { "xdg_session_manager_v1", f_session },
#endif
#if defined(CONF_HAVE_TABLET_V2) && defined(CONF_HAVE_XDG_SHELL)
    { "zwp_tablet_manager_v2", f_tablet },
#endif
#ifdef CONF_HAVE_COLOR_MANAGEMENT_V1
    { "wp_color_manager_v1", f_color },
#endif
#ifdef CONF_HAVE_COLOR_REPRESENTATION_V1
    { "wp_color_representation_manager_v1", f_color_repr },
#endif
#if defined(CONF_HAVE_LINUX_DRM_SYNCOBJ_V1) && defined(CONF_HAVE_LINUX_DMABUF_V1) && defined(CONF_HAVE_XDG_SHELL)
    { "wp_linux_drm_syncobj_manager_v1", f_syncobj },
#endif
#ifdef CONF_HAVE_DRM_LEASE_V1
    { "wp_drm_lease_device_v1", f_drm_lease },
#endif
#if defined(CONF_HAVE_XWAYLAND_SHELL_V1) && defined(CONF_HAVE_XDG_SHELL)
    { "xwayland_shell_v1", f_xwayland_shell },
#endif
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
