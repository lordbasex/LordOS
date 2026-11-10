/* dnd_client.c — Wayland drag and drop (docs/WAYLAND.md, "Drag and drop"; wl.c dev_drag and the dnd_* code):
 * run inside a LordOS session. Without input it checks what a client can get wrong; with two presses in its
 * window (a person, or the VM's end-to-end driver) it drags inside its own window, as an app that is source and
 * target at once.
 *
 *   No press needed: start_drag with a serial that names no press is cancelled; an icon that has another role,
 *   a source used for a drag then given as the selection, and an invalid action mask are protocol errors; the
 *   compositor stays up after each one.
 *   Press 1 (hold it a moment, then release): a made-up serial is cancelled; the real one starts the drag and the
 *   window gets enter with an offer of the source's types and actions; a second start_drag while it runs is
 *   cancelled; the icon surface is destroyed mid-drag; accept and set_actions reach the source (target, action);
 *   text/uri-list is received through the offer; the release drops: drop, dnd_drop_performed, and after finish,
 *   dnd_finished.
 *   Press 2: a drag whose source is destroyed mid-drag: the window gets leave and the compositor stays up.
 *   Press 3 (drag out of the window onto the desktop, not onto a Files folder, and release there): over LordOS
 *   the source is told text/uri-list; nobody claims the drop at its release, so it is cancelled at once and the
 *   list is never asked for.
 *   Press 4 (drag onto a folder of a Files window and release there): Files claims it and the list is asked
 *   for, but the source never writes nor closes it: after DNDG_READ_MS (5 s) it is cancelled, never finished.
 *
 *   make -C server build/dnd-client
 *   WAYLAND_DISPLAY=wayland-0 build/dnd-client            (prints ok/FAIL per check, exit 1 on a failure)
 *   WAYLAND_DISPLAY=wayland-0 build/dnd-client nopress    (only the checks that need no press) */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"

#define URI "file:///tmp/dnd%20client%20test\r\n"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_subcompositor *subcomp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_seat *seat;
static struct wl_data_device_manager *ddm;
static int fails, checks;

static void check(const char *what, long got, long want) {
    checks++;
    if (got == want) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); }
}

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d; (void)ver;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_subcompositor_interface.name)) subcomp = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name)) seat = wl_registry_bind(r, name, &wl_seat_interface, 5);
    else if (!strcmp(iface, wl_data_device_manager_interface.name)) ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, 3);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };
static int configured;
static void xs_configure(void *d, struct xdg_surface *s, uint32_t serial) { (void)d; xdg_surface_ack_configure(s, serial); configured = 1; }
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { (void)d; (void)t; (void)w; (void)h; (void)st; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener top_l = { .configure = top_configure, .close = top_close };

/* the pointer: the serial of the last press on the window, and the release */
static uint32_t press_serial;
static int presses, releases;
static void p_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)s; (void)sf; (void)x; (void)y; }
static void p_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf) { (void)d; (void)p; (void)s; (void)sf; }
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)t; (void)x; (void)y; }
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t st) {
    (void)d; (void)p; (void)t; (void)b;
    if (st == WL_POINTER_BUTTON_STATE_PRESSED) { press_serial = serial; presses++; }
    else releases++;
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d; (void)p; (void)t; (void)a; (void)v; }
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener ptr_l = { p_enter, p_leave, p_motion, p_button, p_axis, p_frame, p_axis_source, p_axis_stop, p_axis_discrete, NULL, NULL };

/* a source: what the compositor told it */
/* uri_targets: how many times it was told text/uri-list; hold: keep the send fd open, unwritten (held) */
typedef struct { int cancelled, sends, performed, finished, target_ok, uri_targets, hold, held; uint32_t action; } Src;
static void s_target(void *d, struct wl_data_source *s, const char *mime) {
    (void)s;
    ((Src *)d)->target_ok = mime && !strcmp(mime, "text/uri-list");
    if (((Src *)d)->target_ok) ((Src *)d)->uri_targets++;
}
static void s_send(void *d, struct wl_data_source *s, const char *mime, int32_t fd) {
    (void)s;
    ((Src *)d)->sends++;
    if (((Src *)d)->hold) { if (((Src *)d)->held >= 0) close(((Src *)d)->held); ((Src *)d)->held = fd; return; }
    if (!strcmp(mime, "text/uri-list") && write(fd, URI, strlen(URI)) < 0) perror("write");
    close(fd);
}
static void s_cancelled(void *d, struct wl_data_source *s) { (void)s; ((Src *)d)->cancelled++; }
static void s_performed(void *d, struct wl_data_source *s) { (void)s; ((Src *)d)->performed++; }
static void s_finished(void *d, struct wl_data_source *s) { (void)s; ((Src *)d)->finished++; }
static void s_action(void *d, struct wl_data_source *s, uint32_t a) { (void)s; ((Src *)d)->action = a; }
static const struct wl_data_source_listener src_l = { s_target, s_send, s_cancelled, s_performed, s_finished, s_action };

static struct wl_data_source *new_source(Src *st) {
    memset(st, 0, sizeof *st);
    st->held = -1;
    struct wl_data_source *s = wl_data_device_manager_create_data_source(ddm);
    wl_data_source_add_listener(s, &src_l, st);
    wl_data_source_offer(s, "text/uri-list");
    wl_data_source_offer(s, "text/plain;charset=utf-8");
    wl_data_source_set_actions(s, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    return s;
}

/* the window as a target: the offer it got with enter, its types and actions, and the drop */
static struct wl_data_offer *cur_offer, *new_offer;
static int enters, leaves, drops, offer_mimes;
static uint32_t offer_src_actions, offer_action;
static void o_offer(void *d, struct wl_data_offer *o, const char *m) {
    (void)d; (void)o;
    if (!strcmp(m, "text/uri-list") || !strcmp(m, "text/plain;charset=utf-8")) offer_mimes++;
}
static void o_src_actions(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; offer_src_actions = a; }
static void o_action(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; offer_action = a; }
static const struct wl_data_offer_listener off_l = { o_offer, o_src_actions, o_action };
static void d_offer(void *d, struct wl_data_device *dv, struct wl_data_offer *o) { (void)d; (void)dv; new_offer = o; offer_mimes = 0; wl_data_offer_add_listener(o, &off_l, NULL); }
static void d_enter(void *d, struct wl_data_device *dv, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o) {
    (void)d; (void)dv; (void)s; (void)sf; (void)x; (void)y;
    cur_offer = o;
    enters++;
}
static void d_leave(void *d, struct wl_data_device *dv) { (void)d; (void)dv; leaves++; }
static void d_motion(void *d, struct wl_data_device *dv, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)dv; (void)t; (void)x; (void)y; }
static void d_drop(void *d, struct wl_data_device *dv) { (void)d; (void)dv; drops++; }
static void d_selection(void *d, struct wl_data_device *dv, struct wl_data_offer *o) { (void)d; (void)dv; (void)o; }
static const struct wl_data_device_listener dev_l = { d_offer, d_enter, d_leave, d_motion, d_drop, d_selection };

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec / 1e9; }
static int *wait_var, wait_want;
static int reached(void) { return *wait_var >= wait_want; }
/* dispatches until *v >= want or the timeout */
static int wait_until(int *v, int want, double secs) {
    double end = now() + secs;
    wait_var = v;
    wait_want = want;
    while (!reached() && now() < end) {
        wl_display_flush(dpy);
        struct pollfd p = { wl_display_get_fd(dpy), POLLIN, 0 };
        if (poll(&p, 1, 100) > 0) { if (wl_display_dispatch(dpy) < 0) return 0; }
        else wl_display_dispatch_pending(dpy);
    }
    return reached();
}

static struct wl_buffer *buffer(int w, int h, uint32_t color) {
    int stride = w * 4, size = stride * h;
    int fd = memfd_create("dnd-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { perror("memfd"); exit(2); }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); exit(2); }
    for (int i = 0; i < w * h; i++) px[i] = color;
    munmap(px, (size_t)size);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

/* A request that must be a protocol error, on a connection of its own: 1 if it was, and the main one still answers. */
typedef void (*BadFn)(struct wl_display *d, struct wl_compositor *c, struct wl_subcompositor *sc, struct wl_data_device_manager *m, struct wl_seat *s);
static struct wl_compositor *b_comp;
static struct wl_subcompositor *b_sub;
static struct wl_data_device_manager *b_ddm;
static struct wl_seat *b_seat;
static void b_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d; (void)ver;
    if (!strcmp(iface, wl_compositor_interface.name)) b_comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_subcompositor_interface.name)) b_sub = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
    else if (!strcmp(iface, wl_data_device_manager_interface.name)) b_ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, 3);
    else if (!strcmp(iface, wl_seat_interface.name)) b_seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
}
static const struct wl_registry_listener b_reg = { b_global, reg_remove };
static int refused(BadFn fn) {
    struct wl_display *d = wl_display_connect(NULL);
    if (!d) return 0;
    b_comp = NULL; b_sub = NULL; b_ddm = NULL; b_seat = NULL;
    wl_registry_add_listener(wl_display_get_registry(d), &b_reg, NULL);
    wl_display_roundtrip(d);
    if (!b_comp || !b_sub || !b_ddm || !b_seat) { wl_display_disconnect(d); return 0; }
    fn(d, b_comp, b_sub, b_ddm, b_seat);
    wl_display_roundtrip(d);
    int err = wl_display_get_error(d) != 0;
    wl_display_disconnect(d);
    wl_display_roundtrip(dpy);
    return err && !wl_display_get_error(dpy);
}
static void bad_icon_role(struct wl_display *d, struct wl_compositor *c, struct wl_subcompositor *sc, struct wl_data_device_manager *m, struct wl_seat *s) {
    (void)d;
    struct wl_surface *parent = wl_compositor_create_surface(c), *icon = wl_compositor_create_surface(c), *origin = wl_compositor_create_surface(c);
    wl_subcompositor_get_subsurface(sc, icon, parent);        /* the icon is a subsurface already */
    struct wl_data_device *dev = wl_data_device_manager_get_data_device(m, s);
    wl_data_device_start_drag(dev, wl_data_device_manager_create_data_source(m), origin, icon, 1);
}
static void bad_used_source(struct wl_display *d, struct wl_compositor *c, struct wl_subcompositor *sc, struct wl_data_device_manager *m, struct wl_seat *s) {
    (void)d; (void)sc;
    struct wl_surface *origin = wl_compositor_create_surface(c);
    struct wl_data_device *dev = wl_data_device_manager_get_data_device(m, s);
    struct wl_data_source *src = wl_data_device_manager_create_data_source(m);
    wl_data_device_start_drag(dev, src, origin, NULL, 1);     /* refused (no press), but the source is spent */
    wl_data_device_set_selection(dev, src, 1);
}
static void bad_actions(struct wl_display *d, struct wl_compositor *c, struct wl_subcompositor *sc, struct wl_data_device_manager *m, struct wl_seat *s) {
    (void)d; (void)c; (void)sc; (void)s;
    wl_data_source_set_actions(wl_data_device_manager_create_data_source(m), 0x80);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    int press = !(argc > 1 && !strcmp(argv[1], "nopress"));
    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "dnd-client: no Wayland display\n"); return 2; }
    wl_registry_add_listener(wl_display_get_registry(dpy), &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !subcomp || !shm || !wm || !seat || !ddm) { fprintf(stderr, "dnd-client: a global is missing\n"); return 2; }
    xdg_wm_base_add_listener(wm, &wm_l, NULL);
    struct wl_pointer *ptr = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(ptr, &ptr_l, NULL);
    struct wl_data_device *dev = wl_data_device_manager_get_data_device(ddm, seat);
    wl_data_device_add_listener(dev, &dev_l, NULL);

    struct wl_surface *win = wl_compositor_create_surface(comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, win);
    xdg_surface_add_listener(xs, &xs_l, NULL);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(top, &top_l, NULL);
    xdg_toplevel_set_title(top, "dnd-client: press here, hold a moment, release (follow the prompts)");
    xdg_toplevel_set_app_id(top, "dnd-client");
    wl_surface_commit(win);
    while (!configured && wl_display_dispatch(dpy) >= 0) {}
    wl_surface_attach(win, buffer(420, 260, 0xffc04040u), 0, 0);
    wl_surface_damage(win, 0, 0, 420, 260);
    wl_surface_commit(win);
    wl_display_roundtrip(dpy);

    /* no press: a serial that names none is cancelled, and nothing enters */
    Src st0;
    struct wl_data_source *s0 = new_source(&st0);
    wl_data_device_start_drag(dev, s0, win, NULL, 12345);
    wl_display_roundtrip(dpy);
    check("start_drag without a press is cancelled", st0.cancelled, 1);
    check("and nothing enters", enters, 0);
    wl_data_source_destroy(s0);
    check("an icon with another role is a protocol error", refused(bad_icon_role), 1);
    check("a drag's source given as the selection is a protocol error", refused(bad_used_source), 1);
    check("an invalid action mask is a protocol error", refused(bad_actions), 1);
    if (!press) { printf("%d checks, %d failed\n", checks, fails); return fails ? 1 : 0; }

    /* press 1: a drag inside the window, dropped on it */
    printf("press in the red window, hold a moment, then release\n");
    if (!wait_until(&presses, 1, 60)) { printf("FAIL no press in 60 s\n"); return 1; }
    Src bad, st, second;
    struct wl_data_source *sb = new_source(&bad);
    wl_data_device_start_drag(dev, sb, win, NULL, press_serial + 1000);
    wl_display_roundtrip(dpy);
    check("a made-up serial is cancelled", bad.cancelled, 1);
    struct wl_data_source *src = new_source(&st);
    struct wl_surface *icon = wl_compositor_create_surface(comp);
    wl_data_device_start_drag(dev, src, win, icon, press_serial);
    wl_surface_attach(icon, buffer(32, 32, 0xc02a6fd6u), 0, 0);
    wl_surface_commit(icon);
    wl_display_roundtrip(dpy);
    check("the press's serial starts the drag", st.cancelled, 0);
    check("the window gets enter", enters, 1);
    check("with an offer", cur_offer != NULL && cur_offer == new_offer, 1);
    check("of the source's two types", offer_mimes, 2);
    check("and its actions", (long)offer_src_actions, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    struct wl_data_source *s2 = new_source(&second);
    wl_data_device_start_drag(dev, s2, win, NULL, press_serial);
    wl_display_roundtrip(dpy);
    check("a second start_drag while one runs is cancelled", second.cancelled, 1);
    check("the first one goes on", st.cancelled, 0);
    wl_surface_destroy(icon);
    wl_display_roundtrip(dpy);
    check("the icon destroyed mid-drag: still up", wl_display_get_error(dpy), 0);
    if (cur_offer) {
        wl_data_offer_accept(cur_offer, 0, "text/uri-list");
        wl_data_offer_set_actions(cur_offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE,
                                  WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        wl_display_roundtrip(dpy);
        check("accept reaches the source", st.target_ok, 1);
        check("the preferred action is agreed (offer)", (long)offer_action, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        check("the preferred action is agreed (source)", (long)st.action, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        int p[2];
        char got[256] = "";
        if (!pipe(p)) {
            wl_data_offer_receive(cur_offer, "text/uri-list", p[1]);
            close(p[1]);
            wait_until(&st.sends, 1, 2);
            ssize_t n = 0;
            struct pollfd pf = { p[0], POLLIN, 0 };
            if (poll(&pf, 1, 2000) > 0) n = read(p[0], got, sizeof got - 1);
            got[n > 0 ? n : 0] = 0;
            close(p[0]);
        }
        check("text/uri-list is received through the offer", !strcmp(got, URI), 1);
        int p2[2];
        if (!pipe(p2)) {                        /* a type the source never offered: the fd is only closed */
            int before = st.sends;
            wl_data_offer_receive(cur_offer, "image/png", p2[1]);
            close(p2[1]);
            wl_display_roundtrip(dpy);
            check("a type it did not offer is not asked of the source", st.sends, before);
            close(p2[0]);
        }
    }
    printf("now release\n");
    wait_until(&drops, 1, 30);
    check("the release drops on the window", drops, 1);
    wait_until(&st.performed, 1, 2);
    check("the source is told dnd_drop_performed", st.performed, 1);
    if (cur_offer) { wl_data_offer_finish(cur_offer); wl_data_offer_destroy(cur_offer); cur_offer = NULL; }
    wait_until(&st.finished, 1, 2);
    check("after finish, dnd_finished", st.finished, 1);
    check("and never cancelled", st.cancelled, 0);
    wl_data_source_destroy(src);
    wl_data_source_destroy(sb);
    wl_data_source_destroy(s2);

    /* press 2: the source destroyed mid-drag */
    printf("press in the red window again, hold a moment, then release\n");
    if (!wait_until(&presses, 2, 60)) { printf("FAIL no second press in 60 s\n"); return 1; }
    Src st3;
    int leaves0 = leaves, enters0 = enters;
    struct wl_data_source *s3 = new_source(&st3);
    wl_data_device_start_drag(dev, s3, win, NULL, press_serial);
    wl_display_roundtrip(dpy);
    check("the second drag enters", enters, enters0 + 1);
    wl_data_source_destroy(s3);
    wl_display_roundtrip(dpy);
    check("its source destroyed: the window gets leave", leaves, leaves0 + 1);
    check("and the compositor is still up", wl_display_get_error(dpy), 0);
    if (cur_offer) { wl_data_offer_destroy(cur_offer); cur_offer = NULL; }

    /* press 3: dropped on LordOS where nothing takes files: nobody claims it at the release */
    printf("press in the red window again, drag out of it onto the desktop (not a Files folder), release there\n");
    if (!wait_until(&presses, 3, 60)) { printf("FAIL no third press in 60 s\n"); return 1; }
    Src st4;
    struct wl_data_source *s4 = new_source(&st4);
    wl_data_device_start_drag(dev, s4, win, NULL, press_serial);
    wl_display_roundtrip(dpy);
    check("the third drag starts", st4.cancelled, 0);
    wait_until(&st4.uri_targets, 1, 60);
    check("over LordOS the source is told text/uri-list", st4.uri_targets >= 1, 1);
    wait_until(&st4.cancelled, 1, 60);
    check("an unclaimed drop on LordOS is cancelled", st4.cancelled, 1);
    wl_display_roundtrip(dpy);
    check("and its list is never asked for", st4.sends, 0);
    check("nor is it told dnd_drop_performed", st4.performed, 0);
    wl_data_source_destroy(s4);
    if (cur_offer) { wl_data_offer_destroy(cur_offer); cur_offer = NULL; }

    /* press 4: claimed by a Files folder, but the list never ends: cancelled after DNDG_READ_MS */
    printf("press in the red window again, drag onto a folder of a Files window, release there\n");
    if (!wait_until(&presses, 4, 60)) { printf("FAIL no fourth press in 60 s\n"); return 1; }
    Src st5;
    struct wl_data_source *s5 = new_source(&st5);
    st5.hold = 1;
    wl_data_device_start_drag(dev, s5, win, NULL, press_serial);
    wl_display_roundtrip(dpy);
    wait_until(&st5.sends, 1, 60);
    check("Files claims the drop: the list is asked for", st5.sends, 1);
    double t0 = now();
    wait_until(&st5.cancelled, 1, 10);
    check("a list that never ends is cancelled", st5.cancelled, 1);
    check("after about 5 s", now() - t0 > 3.5 && now() - t0 < 8, 1);
    check("and never finished", st5.finished, 0);
    if (st5.held >= 0) close(st5.held);
    wl_data_source_destroy(s5);
    if (cur_offer) { wl_data_offer_destroy(cur_offer); cur_offer = NULL; }
    printf("%d checks, %d failed\n", checks, fails);
    wl_display_disconnect(dpy);
    return fails ? 1 : 0;
}
