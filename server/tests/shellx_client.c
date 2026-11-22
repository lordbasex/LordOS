/* shellx_client.c — a plain Wayland client for the shell extras (src/wl/wlshell.h, docs/WAYLAND.md "Shell extras"),
 * run by tests/shellx_headless.sh against a headless lord-server. One mode per run:
 *
 *   globals         the five globals and their versions
 *   tag             a window tagged "main window" (description "Main window") before its first frame, then "preferences"
 *   bell            ten rings on its window in a burst, then one with no window
 *   session-new     a new session (created + its id printed), a window "main" added before its first commit; stays 3 s
 *   session ID NAME restores ID (restored event), restores window NAME before its first commit (toplevel restored event
 *                   before the first configure), stays 3 s; then the same id again from this client: in_use
 *   session-late ID restore_toplevel after the first commit: already_mapped
 *   blur            the capabilities, a blur region (10, 10, 100 × 50 plus 200, 0, 20 × 20), a second effect object for the
 *                   same surface: background_effect_exists
 *   drag ID         tab tear-off: its window restored by session ID ("drag-main"); on the first press in it a drag starts
 *                   with a new window "torn" attached at (20, 10); prints what the drag saw (enter on which window) and
 *                   how it ended. The pointer is driven by the script (tests/shellx_inject.py, as root)
 *   drag-errors     invalid_source (a toplevel drag's source used as a selection) and toplevel_attached
 *
 *   WAYLAND_DISPLAY=wayland-1 build/shellx-client MODE [ARG]   (exit 0, or 1 on a failed check) */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"
#include "client/xdg-toplevel-drag-v1-client-protocol.h"
#include "client/xdg-toplevel-tag-v1-client-protocol.h"
#include "client/xdg-system-bell-v1-client-protocol.h"
#include "client/xdg-session-management-v1-client-protocol.h"
#include "client/ext-background-effect-v1-client-protocol.h"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wmbase;
static struct wl_seat *seat;
static struct wl_data_device_manager *ddm;
static struct xdg_toplevel_drag_manager_v1 *dragm;
static struct xdg_toplevel_tag_manager_v1 *tagm;
static struct xdg_system_bell_v1 *bell;
static struct xdg_session_manager_v1 *sessm;
static struct ext_background_effect_manager_v1 *fxm;
static uint32_t v_drag, v_tag, v_bell, v_sess, v_fx, fx_caps = 99;
static int fails;
static char app_id[64] = "shellx-client";

static void check(const char *what, long got, long want) {
    if (got == want) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); }
    fflush(stdout);
}

static void fx_caps_ev(void *d, struct ext_background_effect_manager_v1 *m, uint32_t c) { (void)d; (void)m; fx_caps = c; }
static const struct ext_background_effect_manager_v1_listener fx_l = { fx_caps_ev };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) wmbase = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name)) seat = wl_registry_bind(r, name, &wl_seat_interface, 5);
    else if (!strcmp(iface, wl_data_device_manager_interface.name)) ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, 3);
    else if (!strcmp(iface, xdg_toplevel_drag_manager_v1_interface.name)) { v_drag = ver; dragm = wl_registry_bind(r, name, &xdg_toplevel_drag_manager_v1_interface, 1); }
    else if (!strcmp(iface, xdg_toplevel_tag_manager_v1_interface.name)) { v_tag = ver; tagm = wl_registry_bind(r, name, &xdg_toplevel_tag_manager_v1_interface, 1); }
    else if (!strcmp(iface, xdg_system_bell_v1_interface.name)) { v_bell = ver; bell = wl_registry_bind(r, name, &xdg_system_bell_v1_interface, 1); }
    else if (!strcmp(iface, xdg_session_manager_v1_interface.name)) { v_sess = ver; sessm = wl_registry_bind(r, name, &xdg_session_manager_v1_interface, 1); }
    else if (!strcmp(iface, ext_background_effect_manager_v1_interface.name)) {
        v_fx = ver;
        fxm = wl_registry_bind(r, name, &ext_background_effect_manager_v1_interface, 1);
        ext_background_effect_manager_v1_add_listener(fxm, &fx_l, NULL);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };

/* ---- windows ---- */

typedef struct {
    struct wl_surface *surf;
    struct xdg_surface *xs;
    struct xdg_toplevel *top;
    int w, h, cw, ch, configured, configures;
    uint32_t color;
    int session_restored_before;        /* the toplevel session's restored event came before the first configure */
} Win;

static struct wl_buffer *buffer(int w, int h, uint32_t color) {
    int stride = w * 4, size = stride * h;
    char name[] = "/shellx-XXXXXX";
    int fd = memfd_create("shellx", MFD_CLOEXEC);
    (void)name;
    if (fd < 0 || ftruncate(fd, size)) { perror("memfd"); exit(1); }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); exit(1); }
    for (int i = 0; i < w * h; i++) px[i] = color;
    munmap(px, (size_t)size);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}
static void draw(Win *v) {
    wl_surface_attach(v->surf, buffer(v->w, v->h, v->color), 0, 0);
    wl_surface_damage(v->surf, 0, 0, v->w, v->h);
    wl_surface_commit(v->surf);
}
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
    Win *v = d;
    xdg_surface_ack_configure(xs, serial);
    v->configures++;
    int resize = v->cw > 0 && v->ch > 0 && (v->cw != v->w || v->ch != v->h);
    if (resize) { v->w = v->cw; v->h = v->ch; }
    if (!v->configured || resize) { v->configured = 1; draw(v); }
}
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { (void)t; (void)st; Win *v = d; v->cw = w; v->ch = h; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_bounds, top_caps };

/* the window's objects; nothing committed yet */
static void win_new(Win *v, const char *title, int w, int h, uint32_t color) {
    memset(v, 0, sizeof *v);
    v->w = w; v->h = h; v->color = color;
    v->surf = wl_compositor_create_surface(comp);
    v->xs = xdg_wm_base_get_xdg_surface(wmbase, v->surf);
    xdg_surface_add_listener(v->xs, &xs_l, v);
    v->top = xdg_surface_get_toplevel(v->xs);
    xdg_toplevel_add_listener(v->top, &top_l, v);
    xdg_toplevel_set_title(v->top, title);
    xdg_toplevel_set_app_id(v->top, app_id);   /* one per mode: a remembered place of another mode never applies */
}
static void win_show(Win *v) {
    wl_surface_commit(v->surf);         /* the first commit: no buffer, the configure comes */
    while (!v->configured && wl_display_dispatch(dpy) >= 0) {}
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}
/* dispatches for s seconds (or until the connection fails) */
static int run_for(double s) {
    double end = now_s() + s;
    while (now_s() < end) {
        if (wl_display_roundtrip(dpy) < 0) return -1;
        usleep(20000);
    }
    return 0;
}
/* the protocol error the connection died with, as "interface code" */
static int proto_error(const char *iface, uint32_t code) {
    const struct wl_interface *i = NULL;
    uint32_t id = 0;
    int err = wl_display_get_error(dpy);
    uint32_t c = err == EPROTO ? wl_display_get_protocol_error(dpy, &i, &id) : 0;
    printf("     connection error %d: %s %u\n", err, i ? i->name : "-", c);
    return err == EPROTO && i && !strcmp(i->name, iface) && c == code;
}

/* ---- sessions ---- */

static char got_id[128];
static int sess_created, sess_restored, sess_replaced, ts_restored;
static void s_created(void *d, struct xdg_session_v1 *s, const char *id) { (void)d; (void)s; sess_created = 1; snprintf(got_id, sizeof got_id, "%s", id); }
static void s_restored(void *d, struct xdg_session_v1 *s) { (void)d; (void)s; sess_restored = 1; }
static void s_replaced(void *d, struct xdg_session_v1 *s) { (void)d; (void)s; sess_replaced = 1; }
static const struct xdg_session_v1_listener s_l = { s_created, s_restored, s_replaced };
static void ts_restored_ev(void *d, struct xdg_toplevel_session_v1 *t) {
    (void)t;
    Win *v = d;
    ts_restored++;
    if (v && !v->configures) v->session_restored_before = 1;
}
static const struct xdg_toplevel_session_v1_listener ts_l = { ts_restored_ev };

static struct xdg_session_v1 *session(const char *id, uint32_t reason) {
    sess_created = sess_restored = 0;
    struct xdg_session_v1 *s = xdg_session_manager_v1_get_session(sessm, reason, id);
    xdg_session_v1_add_listener(s, &s_l, NULL);
    wl_display_roundtrip(dpy);
    return s;
}

/* ---- drag ---- */

static struct wl_pointer *ptr;
static uint32_t press_serial;
static int pressed;
static struct wl_surface *enter_surf;
static int enters_main, enters_torn, drag_cancelled, drag_dropped;
static Win mainw, torn;
static void p_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)s; (void)sf; (void)x; (void)y; }
static void p_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf) { (void)d; (void)p; (void)s; (void)sf; }
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)t; (void)x; (void)y; }
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t st) {
    (void)d; (void)p; (void)t; (void)b;
    if (st == WL_POINTER_BUTTON_STATE_PRESSED && !pressed) { pressed = 1; press_serial = serial; }
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d; (void)p; (void)t; (void)a; (void)v; }
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_src(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void p_disc(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void p_v120(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void p_rdir(void *d, struct wl_pointer *p, uint32_t a, uint32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener p_l = { p_enter, p_leave, p_motion, p_button, p_axis, p_frame, p_src, p_stop, p_disc, p_v120, p_rdir };

static void dd_offer(void *d, struct wl_data_device *dd, struct wl_data_offer *o) { (void)d; (void)dd; (void)o; }
static void dd_enter(void *d, struct wl_data_device *dd, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o) {
    (void)d; (void)dd; (void)s; (void)x; (void)y; (void)o;
    enter_surf = sf;
    if (sf == mainw.surf) enters_main++;
    if (sf == torn.surf) enters_torn++;
}
static void dd_leave(void *d, struct wl_data_device *dd) { (void)d; (void)dd; enter_surf = NULL; }
static void dd_motion(void *d, struct wl_data_device *dd, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)dd; (void)t; (void)x; (void)y; }
static void dd_drop(void *d, struct wl_data_device *dd) { (void)d; (void)dd; }
static void dd_sel(void *d, struct wl_data_device *dd, struct wl_data_offer *o) { (void)d; (void)dd; (void)o; }
static const struct wl_data_device_listener dd_l = { dd_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_sel };

static void src_target(void *d, struct wl_data_source *s, const char *m) { (void)d; (void)s; (void)m; }
static void src_send(void *d, struct wl_data_source *s, const char *m, int32_t fd) { (void)d; (void)s; (void)m; close(fd); }
static void src_cancelled(void *d, struct wl_data_source *s) { (void)d; (void)s; drag_cancelled = 1; }
static void src_dnd_drop(void *d, struct wl_data_source *s) { (void)d; (void)s; drag_dropped = 1; }
static void src_dnd_fin(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void src_action(void *d, struct wl_data_source *s, uint32_t a) { (void)d; (void)s; (void)a; }
static const struct wl_data_source_listener src_l = { src_target, src_send, src_cancelled, src_dnd_drop, src_dnd_fin, src_action };

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: shellx-client MODE [ARG]\n"); return 2; }
    const char *mode = argv[1];
    snprintf(app_id, sizeof app_id, "shellx-%s", mode);
    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "no Wayland display\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wmbase) { fprintf(stderr, "missing core globals\n"); return 1; }
    xdg_wm_base_add_listener(wmbase, &wm_l, NULL);

    if (!strcmp(mode, "globals")) {
        printf("xdg_toplevel_drag_manager_v1 %u\nxdg_toplevel_tag_manager_v1 %u\nxdg_system_bell_v1 %u\n"
               "xdg_session_manager_v1 %u\next_background_effect_manager_v1 %u caps %u\n", v_drag, v_tag, v_bell, v_sess, v_fx, fx_caps);
        check("toplevel drag v1", v_drag, 1);
        check("toplevel tag v1", v_tag, 1);
        check("system bell v1", v_bell, 1);
        check("session management v1", v_sess, 1);
        check("background effect v1", v_fx, 1);
        return fails ? 1 : 0;
    }
    if (!strcmp(mode, "tag")) {
        Win v;
        win_new(&v, "tagged", 320, 200, 0xff3070c0);
        xdg_toplevel_tag_manager_v1_set_toplevel_tag(tagm, v.top, "main window");
        xdg_toplevel_tag_manager_v1_set_toplevel_description(tagm, v.top, "Main window");
        win_show(&v);
        run_for(1.0);
        xdg_toplevel_tag_manager_v1_set_toplevel_tag(tagm, v.top, "preferences\x01");   /* the control character goes */
        run_for(1.0);
        return wl_display_get_error(dpy) ? 1 : 0;
    }
    if (!strcmp(mode, "bell")) {
        Win v;
        win_new(&v, "bell", 200, 120, 0xffc03030);
        win_show(&v);
        run_for(0.5);
        for (int i = 0; i < 10; i++) xdg_system_bell_v1_ring(bell, v.surf);
        wl_display_roundtrip(dpy);
        usleep(400000);
        xdg_system_bell_v1_ring(bell, NULL);
        run_for(0.5);
        return wl_display_get_error(dpy) ? 1 : 0;
    }
    if (!strcmp(mode, "session-new")) {
        struct xdg_session_v1 *s = session(NULL, XDG_SESSION_MANAGER_V1_REASON_LAUNCH);
        check("a new session is created", sess_created, 1);
        printf("id %s\n", got_id);
        fflush(stdout);
        Win v;
        win_new(&v, "session", 360, 240, 0xff30a050);
        struct xdg_toplevel_session_v1 *t = xdg_session_v1_add_toplevel(s, v.top, "main");
        xdg_toplevel_session_v1_add_listener(t, &ts_l, &v);
        win_show(&v);
        run_for(3.0);
        check("an added window is never restored", ts_restored, 0);
        return fails ? 1 : 0;
    }
    if (!strcmp(mode, "session") && argc > 3) {
        struct xdg_session_v1 *s = session(argv[2], XDG_SESSION_MANAGER_V1_REASON_SESSION_RESTORE);
        printf("created %d restored %d id %s\n", sess_created, sess_restored, sess_created ? got_id : argv[2]);
        fflush(stdout);
        Win v;
        win_new(&v, "restored", 300, 200, 0xff8040a0);
        struct xdg_toplevel_session_v1 *t = xdg_session_v1_restore_toplevel(s, v.top, argv[3]);
        xdg_toplevel_session_v1_add_listener(t, &ts_l, &v);
        win_show(&v);
        run_for(3.0);
        printf("toplevel restored %d (before its first configure: %d), size %dx%d\n", ts_restored, v.session_restored_before, v.w, v.h);
        fflush(stdout);
        if (argc > 4 && !strcmp(argv[4], "inuse")) {
            session(argv[2], XDG_SESSION_MANAGER_V1_REASON_LAUNCH);
            check("the same session twice from one client: in_use", proto_error("xdg_session_manager_v1", XDG_SESSION_MANAGER_V1_ERROR_IN_USE), 1);
        }
        return fails ? 1 : 0;
    }
    if (!strcmp(mode, "session-late") && argc > 2) {
        struct xdg_session_v1 *s = session(argv[2], XDG_SESSION_MANAGER_V1_REASON_LAUNCH);
        Win v;
        win_new(&v, "late", 200, 120, 0xff808080);
        win_show(&v);
        run_for(0.3);
        xdg_session_v1_restore_toplevel(s, v.top, "main");
        wl_display_roundtrip(dpy);
        check("restore_toplevel after the first commit: already_mapped", proto_error("xdg_session_v1", XDG_SESSION_V1_ERROR_ALREADY_MAPPED), 1);
        return fails ? 1 : 0;
    }
    if (!strcmp(mode, "blur")) {
        wl_display_roundtrip(dpy);
        printf("capabilities %u\n", fx_caps);
        check("blur offered on the CPU path", fx_caps, EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR);
        Win v;
        win_new(&v, "blur", 300, 200, 0x80202020);
        struct ext_background_effect_surface_v1 *fx = ext_background_effect_manager_v1_get_background_effect(fxm, v.surf);
        struct wl_region *rg = wl_compositor_create_region(comp);
        wl_region_add(rg, 10, 10, 100, 50);
        wl_region_add(rg, 200, 0, 20, 20);
        ext_background_effect_surface_v1_set_blur_region(fx, rg);
        wl_region_destroy(rg);
        win_show(&v);
        run_for(1.0);
        ext_background_effect_manager_v1_get_background_effect(fxm, v.surf);
        wl_display_roundtrip(dpy);
        check("a second effect object: background_effect_exists",
              proto_error("ext_background_effect_manager_v1", EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS), 1);
        return fails ? 1 : 0;
    }
    if (!strcmp(mode, "drag-errors")) {
        struct wl_data_source *src = wl_data_device_manager_create_data_source(ddm);
        wl_data_source_offer(src, "text/plain");
        xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(dragm, src);
        struct wl_data_device *dd = wl_data_device_manager_get_data_device(ddm, seat);
        wl_data_device_set_selection(dd, src, 0);
        wl_display_roundtrip(dpy);
        check("a toplevel drag's source as the selection: invalid_source",
              proto_error("xdg_toplevel_drag_manager_v1", XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE), 1);
        wl_display_disconnect(dpy);
        dpy = wl_display_connect(NULL);
        comp = NULL;
        reg = wl_display_get_registry(dpy);
        wl_registry_add_listener(reg, &reg_l, NULL);
        wl_display_roundtrip(dpy);
        xdg_wm_base_add_listener(wmbase, &wm_l, NULL);
        Win a, b;
        win_new(&a, "a", 100, 80, 0xff404040);
        win_new(&b, "b", 100, 80, 0xff404040);
        src = wl_data_device_manager_create_data_source(ddm);
        struct xdg_toplevel_drag_v1 *td = xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(dragm, src);
        xdg_toplevel_drag_v1_attach(td, a.top, 0, 0);
        xdg_toplevel_drag_v1_attach(td, b.top, 0, 0);
        wl_display_roundtrip(dpy);
        check("a second attach while one is attached: toplevel_attached",
              proto_error("xdg_toplevel_drag_v1", XDG_TOPLEVEL_DRAG_V1_ERROR_TOPLEVEL_ATTACHED), 1);
        return fails ? 1 : 0;
    }
    if (!strcmp(mode, "drag") && argc > 2) {
        ptr = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(ptr, &p_l, NULL);
        struct wl_data_device *dd = wl_data_device_manager_get_data_device(ddm, seat);
        wl_data_device_add_listener(dd, &dd_l, NULL);
        struct xdg_session_v1 *s = session(argv[2], XDG_SESSION_MANAGER_V1_REASON_LAUNCH);
        win_new(&mainw, "drag main", 400, 300, 0xff2060a0);
        xdg_session_v1_restore_toplevel(s, mainw.top, "drag-main");
        win_show(&mainw);
        printf("ready\n");
        fflush(stdout);
        double end = now_s() + 20;
        while (!pressed && now_s() < end && wl_display_roundtrip(dpy) >= 0) usleep(10000);
        check("the press reached the window", pressed, 1);
        if (!pressed) return 1;
        struct wl_data_source *src = wl_data_device_manager_create_data_source(ddm);
        wl_data_source_add_listener(src, &src_l, NULL);
        wl_data_source_offer(src, "application/x-shellx-tab");
        wl_data_source_set_actions(src, WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
        struct xdg_toplevel_drag_v1 *td = xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(dragm, src);
        wl_data_device_start_drag(dd, src, mainw.surf, NULL, press_serial);
        /* the tab torn off: a new window attached before it is mapped, added to the session to be found after */
        win_new(&torn, "torn", 200, 150, 0xffa06020);
        xdg_session_v1_add_toplevel(s, torn.top, "torn");
        xdg_toplevel_drag_v1_attach(td, torn.top, 20, 10);
        win_show(&torn);
        printf("dragging\n");
        fflush(stdout);
        end = now_s() + 20;
        while (!drag_cancelled && !drag_dropped && now_s() < end && wl_display_roundtrip(dpy) >= 0) usleep(10000);
        printf("enters on main %d, on torn %d; cancelled %d dropped %d\n", enters_main, enters_torn, drag_cancelled, drag_dropped);
        check("the dragged window is never a target", enters_torn, 0);
        check("the window under it is", enters_main > 0, 1);
        check("let go where nothing takes it: dropped there, not cancelled", drag_dropped && !drag_cancelled, 1);
        xdg_toplevel_drag_v1_destroy(td);
        run_for(3.0);                   /* its place goes to the session file */
        return fails ? 1 : 0;
    }
    fprintf(stderr, "unknown mode %s\n", mode);
    return 2;
}
