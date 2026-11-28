/* focusback_client.c — two compositor races the toolkit audit found on a real desktop (docs/TOOLKIT-AUDIT.md):
 *
 * 1. The keyboard comes back at once. A window opens a second window, which takes the keyboard and closes; the first
 *    window then opens a menu. The keyboard used to come back only when the theme decided, a frame or more later, and
 *    Qt closed the menu (or tooltip) it had just opened when the keyboard arrived. Now lord-server gives it back to the
 *    window that had it before, in the same loop iteration (wl.c focus_back): the wl_keyboard.enter must be there by
 *    the next roundtrip, and the menu, a grabbing xdg_popup opened right after the close, must not be dismissed.
 * 2. A late read of an older selection never overwrites a newer one. The app copies twice: the first source answers
 *    its send late, the second at once. The first text used to land in LordOS's clipboard after the second, so what
 *    LordOS offered afterwards (and the X CLIPBOARD, taken back from an X app that had just copied) was the older text.
 *    After the second source is gone, a new window's offer (LordOS's copy) must be the second text.
 *
 *   tests/popup_headless.sh focusback-client     (run by tests/run_suites.sh; or WAYLAND_DISPLAY=... build/focusback-client)
 * Prints ok/FAIL per check, exit 1 on a failure. */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wmbase;
static struct wl_seat *seat;
static struct wl_data_device_manager *ddm;
static int fails, checks;

static void check(const char *what, int ok, const char *why) {
    checks++;
    if (ok) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: %s\n", what, why); }
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) wmbase = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
    else if (!strcmp(iface, wl_seat_interface.name)) seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5);
    else if (!strcmp(iface, wl_data_device_manager_interface.name)) ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, 3);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };
static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };

/* the keyboard: which of our surfaces has it */
static struct wl_surface *kb_on;
static uint32_t last_serial;
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int fd, uint32_t s) { (void)d; (void)k; (void)f; (void)s; close(fd); }
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys) { (void)d; (void)k; (void)keys; kb_on = s; last_serial = serial; }
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) { (void)d; (void)k; (void)serial; if (kb_on == s) kb_on = NULL; }
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t st) { (void)d; (void)k; (void)serial; (void)t; (void)key; (void)st; }
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t a, uint32_t b, uint32_t c, uint32_t g) { (void)d; (void)k; (void)serial; (void)a; (void)b; (void)c; (void)g; }
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t dl) { (void)d; (void)k; (void)r; (void)dl; }
static const struct wl_keyboard_listener kb_l = { kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat };

/* the selection offered to us */
static struct wl_data_offer *sel_offer;
static int sel_events;
static void offer_offer(void *d, struct wl_data_offer *o, const char *mime) { (void)d; (void)o; (void)mime; }
static void offer_actions(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static void offer_action(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static const struct wl_data_offer_listener offer_l = { offer_offer, offer_actions, offer_action };
static void dd_offer(void *d, struct wl_data_device *dd, struct wl_data_offer *o) { (void)d; (void)dd; wl_data_offer_add_listener(o, &offer_l, NULL); }
static void dd_enter(void *d, struct wl_data_device *dd, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o) { (void)d; (void)dd; (void)s; (void)sf; (void)x; (void)y; (void)o; }
static void dd_leave(void *d, struct wl_data_device *dd) { (void)d; (void)dd; }
static void dd_motion(void *d, struct wl_data_device *dd, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)dd; (void)t; (void)x; (void)y; }
static void dd_drop(void *d, struct wl_data_device *dd) { (void)d; (void)dd; }
static void dd_selection(void *d, struct wl_data_device *dd, struct wl_data_offer *o) {
    (void)d; (void)dd;
    if (sel_offer && sel_offer != o) wl_data_offer_destroy(sel_offer);
    sel_offer = o;
    sel_events++;
}
static const struct wl_data_device_listener dd_l = { dd_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_selection };

/* a source that answers send at once (late == 0) or keeps the fd for later */
typedef struct { const char *text; int late, fd, cancelled; } Src;
static void src_target(void *d, struct wl_data_source *s, const char *m) { (void)d; (void)s; (void)m; }
static void src_send(void *d, struct wl_data_source *s, const char *mime, int fd) {
    (void)s; (void)mime;
    Src *x = d;
    if (x->late) { if (x->fd >= 0) close(x->fd); x->fd = fd; return; }
    ssize_t w = write(fd, x->text, strlen(x->text));
    (void)w;
    close(fd);
}
static void src_cancelled(void *d, struct wl_data_source *s) { (void)s; ((Src *)d)->cancelled = 1; }
static void src_dnd_drop(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void src_dnd_finished(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void src_action(void *d, struct wl_data_source *s, uint32_t a) { (void)d; (void)s; (void)a; }
static const struct wl_data_source_listener src_l = { src_target, src_send, src_cancelled, src_dnd_drop, src_dnd_finished, src_action };

typedef struct { int surface_conf, popup_conf, done; uint32_t serial; } Seen;
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) { Seen *s = d; s->surface_conf++; s->serial = serial; (void)xs; }
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { (void)d; (void)t; (void)w; (void)h; (void)st; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_bounds, top_caps };
static void pop_configure(void *d, struct xdg_popup *p, int32_t x, int32_t y, int32_t w, int32_t h) { (void)p; (void)x; (void)y; (void)w; (void)h; ((Seen *)d)->popup_conf++; }
static void pop_done(void *d, struct xdg_popup *p) { (void)p; ((Seen *)d)->done++; }
static void pop_repositioned(void *d, struct xdg_popup *p, uint32_t t) { (void)d; (void)p; (void)t; }
static const struct xdg_popup_listener pop_l = { pop_configure, pop_done, pop_repositioned };

static struct wl_buffer *buffer(int W, int H, uint32_t color) {
    int stride = W * 4, size = stride * H;
    int fd = memfd_create("focusback-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { perror("memfd"); exit(2); }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); exit(2); }
    for (int i = 0; i < W * H; i++) px[i] = color;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    munmap(px, (size_t)size);
    return b;
}

typedef struct { struct wl_surface *surf; struct xdg_surface *xs; struct xdg_toplevel *top; Seen seen; } Win;

static void win_open(Win *w, const char *app_id, int W, int H) {
    memset(w, 0, sizeof *w);
    w->surf = wl_compositor_create_surface(comp);
    w->xs = xdg_wm_base_get_xdg_surface(wmbase, w->surf);
    xdg_surface_add_listener(w->xs, &xs_l, &w->seen);
    w->top = xdg_surface_get_toplevel(w->xs);
    xdg_toplevel_add_listener(w->top, &top_l, &w->seen);
    xdg_toplevel_set_app_id(w->top, app_id);
    wl_surface_commit(w->surf);
    wl_display_roundtrip(dpy);
    if (w->seen.surface_conf) xdg_surface_ack_configure(w->xs, w->seen.serial);
    wl_surface_attach(w->surf, buffer(W, H, 0xffd0d0d0u), 0, 0);
    wl_surface_damage(w->surf, 0, 0, W, H);
    wl_surface_commit(w->surf);
    wl_display_roundtrip(dpy);
}
static void win_close(Win *w) {
    xdg_toplevel_destroy(w->top);
    xdg_surface_destroy(w->xs);
    wl_surface_destroy(w->surf);
}

/* dispatches until cond or ms */
static int wait_for(int (*cond)(void *), void *arg, int ms) {
    double end = now_ms() + ms;
    while (!cond(arg)) {
        double left = end - now_ms();
        if (left <= 0) return 0;
        wl_display_flush(dpy);
        struct pollfd p = { wl_display_get_fd(dpy), POLLIN, 0 };
        if (wl_display_prepare_read(dpy) != 0) { wl_display_dispatch_pending(dpy); continue; }
        if (poll(&p, 1, (int)left + 1) > 0) wl_display_read_events(dpy); else wl_display_cancel_read(dpy);
        wl_display_dispatch_pending(dpy);
    }
    return 1;
}
static int kb_is(void *s) { return kb_on == s; }

static char *read_offer(struct wl_data_offer *o) {
    int p[2];
    if (!o || pipe(p)) return NULL;
    wl_data_offer_receive(o, "text/plain;charset=utf-8", p[1]);
    close(p[1]);
    wl_display_flush(dpy);
    static char buf[256];
    size_t n = 0;
    double end = now_ms() + 2000;
    for (;;) {
        struct pollfd pf = { p[0], POLLIN, 0 };
        wl_display_roundtrip(dpy);
        if (poll(&pf, 1, 50) > 0) {
            ssize_t r = read(p[0], buf + n, sizeof buf - 1 - n);
            if (r <= 0) break;
            n += (size_t)r;
        }
        if (now_ms() > end) break;
    }
    close(p[0]);
    buf[n] = 0;
    return buf;
}

int main(void) {
    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "focusback-client: no Wayland display\n"); return 2; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wmbase || !seat || !ddm) { fprintf(stderr, "focusback-client: a global is missing\n"); return 2; }
    xdg_wm_base_add_listener(wmbase, &wm_l, NULL);
    struct wl_keyboard *kb = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(kb, &kb_l, NULL);
    struct wl_data_device *dd = wl_data_device_manager_get_data_device(ddm, seat);
    wl_data_device_add_listener(dd, &dd_l, NULL);

    /* 1. the keyboard comes back at once, and a menu opened in that moment stays */
    Win a, b;
    win_open(&a, "focusback-a", 400, 300);
    check("the first window gets the keyboard", wait_for(kb_is, a.surf, 3000), "no wl_keyboard.enter");
    win_open(&b, "focusback-b", 200, 120);
    check("the second window gets the keyboard", wait_for(kb_is, b.surf, 3000), "no wl_keyboard.enter");
    wait_for(kb_is, NULL, 300);                         /* settle */
    win_close(&b);
    /* the menu, opened right after the close, as Qt does when the step after the second window runs */
    Seen ps = { 0 };
    struct wl_surface *pop_surf = wl_compositor_create_surface(comp);
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(wmbase);
    xdg_positioner_set_size(pos, 160, 80);
    xdg_positioner_set_anchor_rect(pos, 20, 20, 100, 24);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM);
    struct xdg_surface *pxs = xdg_wm_base_get_xdg_surface(wmbase, pop_surf);
    xdg_surface_add_listener(pxs, &xs_l, &ps);
    struct xdg_popup *pop = xdg_surface_get_popup(pxs, a.xs, pos);
    xdg_popup_add_listener(pop, &pop_l, &ps);
    xdg_positioner_destroy(pos);
    xdg_popup_grab(pop, seat, last_serial);
    wl_surface_commit(pop_surf);
    double t0 = now_ms();
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);
    double back = now_ms() - t0;
    char why[320];
    snprintf(why, sizeof why, "not back after two roundtrips (%.0f ms); it came %s", back,
             wait_for(kb_is, a.surf, 2000) ? "later, from the theme" : "never");
    check("the keyboard is back on the first window by the next roundtrip", kb_on == a.surf, why);
    if (ps.surface_conf) {
        xdg_surface_ack_configure(pxs, ps.serial);
        wl_surface_attach(pop_surf, buffer(160, 80, 0xff2a6fd6u), 0, 0);
        wl_surface_damage(pop_surf, 0, 0, 160, 80);
        wl_surface_commit(pop_surf);
    }
    wait_for(kb_is, NULL, 500);                         /* half a second for a late keyboard change or popup_done */
    check("the menu was configured", ps.popup_conf > 0, "no xdg_popup.configure");
    check("the menu is still open", ps.done == 0, "xdg_popup.popup_done");
    check("the keyboard stayed on the first window", kb_on == a.surf, "it moved");
    xdg_popup_destroy(pop);
    xdg_surface_destroy(pxs);
    wl_surface_destroy(pop_surf);
    wl_display_roundtrip(dpy);

    /* 2. a late read of an older copy doesn't overwrite a newer one */
    Src s1 = { "focusback-older", 1, -1, 0 }, s2 = { "focusback-newer", 0, -1, 0 };
    struct wl_data_source *ds1 = wl_data_device_manager_create_data_source(ddm);
    wl_data_source_add_listener(ds1, &src_l, &s1);
    wl_data_source_offer(ds1, "text/plain;charset=utf-8");
    wl_data_device_set_selection(dd, ds1, last_serial);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);                          /* lord-server asked ds1 for its text: s1 keeps the fd */
    struct wl_data_source *ds2 = wl_data_device_manager_create_data_source(ddm);
    wl_data_source_add_listener(ds2, &src_l, &s2);
    wl_data_source_offer(ds2, "text/plain;charset=utf-8");
    wl_data_device_set_selection(dd, ds2, last_serial);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);
    check("the first source was cancelled", s1.cancelled, "no cancelled");
    check("lord-server read the first source", s1.fd >= 0, "no send");
    if (s1.fd >= 0) {                                   /* the older text arrives now, after the newer */
        ssize_t w = write(s1.fd, s1.text, strlen(s1.text));
        (void)w;
        close(s1.fd);
        s1.fd = -1;
    }
    struct timespec ts = { 0, 200 * 1000000L };
    nanosleep(&ts, NULL);
    wl_display_roundtrip(dpy);
    wl_data_source_destroy(ds2);                        /* the app's copy goes: LordOS's own copy is what is offered */
    wl_data_source_destroy(ds1);
    wl_display_roundtrip(dpy);
    int before = sel_events;
    Win c;
    win_open(&c, "focusback-c", 200, 120);              /* a new window with the keyboard gets the selection */
    wait_for(kb_is, c.surf, 3000);
    struct timespec ts2 = { 0, 100 * 1000000L };
    nanosleep(&ts2, NULL);
    wl_display_roundtrip(dpy);
    char *got = sel_events > before ? read_offer(sel_offer) : NULL;
    snprintf(why, sizeof why, "got \"%s\"", got ? got : "(no offer)");
    check("LordOS kept the newer copy", got && !strcmp(got, "focusback-newer"), why);
    win_close(&c);
    win_close(&a);
    wl_display_roundtrip(dpy);
    check("still up at the end", wl_display_get_error(dpy) == 0, "a protocol error");
    printf("%d checks, %d failed\n", checks, fails);
    wl_display_disconnect(dpy);
    return fails ? 1 : 0;
}
