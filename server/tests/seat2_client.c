/* seat2_client.c — one client that binds wl_seat twice, as Firefox does (GTK 3 binds it for itself and Firefox's own
 * WaylandDisplay binds it again: docs/WAYLAND.md, "Two wl_seat bindings"). Run inside a LordOS session; it needs a
 * person or the VM's end-to-end driver (tools/vm/e2e/ui.py) to click and type in its window.
 *
 * Seat A is bound at version 5 and seat B at the newest the compositor has (up to 9), each with its own wl_pointer and
 * wl_keyboard. The spec: every wl_pointer of the client with the pointer gets the event once, so the event stream of
 * each resource is checked on its own:
 *   Press 1 (move into the window, click once): pointer A and pointer B each get exactly one enter for the window,
 *   one press and one release of BTN_LEFT, the same time stamp on both, each event closed by its own frame (version 5+)
 *   and no event twice; a press's serial is new on each resource.
 *   Then a third wl_pointer (pointer C, from seat A) is made while the pointer is over the window: it must be told
 *   with an enter at once, as a new wl_keyboard is (or its buttons come with no enter and a toolkit drops them).
 *   Press 2 (click once more): pointers A, B and C each get exactly one press and one release.
 *   A key (type one letter): keyboards A and B each get one enter, one press and one release of that key.
 *
 *   make -C server build/seat2-client
 *   WAYLAND_DISPLAY=wayland-0 build/seat2-client     (prints ok/FAIL per check, exit 1 on a failure; 60 s at most) */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_seat *seat_a, *seat_b;
static uint32_t seat_name, seat_ver;
static struct wl_surface *surf;
static int fails;

static void check(const char *what, int ok) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

typedef struct {
    const char *name;
    int enter, leave, press, release, frames, open, twice, bad_surface;
    uint32_t last_serial, press_time[8], release_time[8];
} Ptr;
static Ptr pa = { .name = "A" }, pb = { .name = "B" }, pc = { .name = "C" };

static void p_event(Ptr *p) { if (p->open) p->twice++; p->open = 1; }   /* an event group with no frame between */
static void p_enter(void *d, struct wl_pointer *wp, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
    (void)wp; (void)x; (void)y;
    Ptr *p = d;
    p->enter++;
    p->last_serial = serial;
    if (s != surf) p->bad_surface++;
}
static void p_leave(void *d, struct wl_pointer *wp, uint32_t serial, struct wl_surface *s) {
    (void)wp; (void)serial; (void)s;
    ((Ptr *)d)->leave++;
}
static void p_motion(void *d, struct wl_pointer *wp, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)wp; (void)t; (void)x; (void)y; }
static void p_button(void *d, struct wl_pointer *wp, uint32_t serial, uint32_t t, uint32_t b, uint32_t st) {
    (void)wp;
    Ptr *p = d;
    if (b != BTN_LEFT) return;
    p_event(p);
    if (serial == p->last_serial) p->twice++;
    p->last_serial = serial;
    if (st == WL_POINTER_BUTTON_STATE_PRESSED) { if (p->press < 8) p->press_time[p->press] = t; p->press++; }
    else { if (p->release < 8) p->release_time[p->release] = t; p->release++; }
}
static void p_axis(void *d, struct wl_pointer *wp, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d; (void)wp; (void)t; (void)a; (void)v; }
static void p_frame(void *d, struct wl_pointer *wp) { (void)wp; Ptr *p = d; p->frames++; p->open = 0; }
static void p_asrc(void *d, struct wl_pointer *wp, uint32_t s) { (void)d; (void)wp; (void)s; }
static void p_astop(void *d, struct wl_pointer *wp, uint32_t t, uint32_t a) { (void)d; (void)wp; (void)t; (void)a; }
static void p_adisc(void *d, struct wl_pointer *wp, uint32_t a, int32_t v) { (void)d; (void)wp; (void)a; (void)v; }
static void p_a120(void *d, struct wl_pointer *wp, uint32_t a, int32_t v) { (void)d; (void)wp; (void)a; (void)v; }
static void p_adir(void *d, struct wl_pointer *wp, uint32_t a, uint32_t v) { (void)d; (void)wp; (void)a; (void)v; }
static const struct wl_pointer_listener ptr_l = { p_enter, p_leave, p_motion, p_button, p_axis, p_frame, p_asrc, p_astop, p_adisc, p_a120, p_adir };

typedef struct { const char *name; int enter, press, release; uint32_t key, last_serial, twice; } Kbd;
static Kbd ka = { .name = "A" }, kb = { .name = "B" };
static void k_keymap(void *d, struct wl_keyboard *k, uint32_t f, int fd, uint32_t sz) { (void)d; (void)k; (void)f; (void)sz; close(fd); }
static void k_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys) {
    (void)k; (void)s; (void)keys;
    Kbd *b = d;
    if (s == surf) b->enter++;
    b->last_serial = serial;
}
static void k_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) { (void)d; (void)k; (void)serial; (void)s; }
static void k_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t st) {
    (void)k; (void)t;
    Kbd *b = d;
    if (serial == b->last_serial) b->twice++;
    b->last_serial = serial;
    if (st == WL_KEYBOARD_KEY_STATE_PRESSED) { b->press++; b->key = key; } else b->release++;
}
static void k_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t a, uint32_t b, uint32_t c, uint32_t g) {
    (void)d; (void)k; (void)serial; (void)a; (void)b; (void)c; (void)g;
}
static void k_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t delay) { (void)d; (void)k; (void)r; (void)delay; }
static const struct wl_keyboard_listener kbd_l = { k_keymap, k_enter, k_leave, k_key, k_mods, k_repeat };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name)) { seat_name = name; seat_ver = ver; }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };
static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };
static uint32_t conf_serial;
static int configured;
static void xs_conf(void *d, struct xdg_surface *xs, uint32_t serial) { (void)d; (void)xs; conf_serial = serial; configured = 1; }
static const struct xdg_surface_listener xs_l = { xs_conf };
static void top_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) { (void)d; (void)t; (void)w; (void)h; (void)s; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener top_l = { top_conf, top_close, NULL, NULL };

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec / 1e9; }
/* dispatch until cond() or the deadline */
static int wait_for(int (*cond)(void), double deadline) {
    while (!cond()) {
        if (now_s() > deadline) return 0;
        wl_display_flush(dpy);
        if (wl_display_prepare_read(dpy) != 0) { wl_display_dispatch_pending(dpy); continue; }
        struct pollfd pf = { wl_display_get_fd(dpy), POLLIN, 0 };
        if (poll(&pf, 1, 100) > 0) wl_display_read_events(dpy); else wl_display_cancel_read(dpy);
        if (wl_display_dispatch_pending(dpy) < 0) return 0;
    }
    return 1;
}
static int entered(void) { return pa.enter && pb.enter; }
static int click1(void) { return pa.release >= 1 && pb.release >= 1; }
static int click2(void) { return pa.release >= 2 && pb.release >= 2 && pc.release >= 1; }
static int typed(void) { return ka.release >= 1 && kb.release >= 1; }

static void report_ptr(Ptr *p, int presses, int enters) {
    char w[160];
    snprintf(w, sizeof w, "pointer %s: %d enter (want %d), %d presses and %d releases (want %d each), every event in its own frame, none twice",
             p->name, p->enter, enters, p->press, p->release, presses);
    check(w, p->enter == enters && !p->bad_surface && p->press == presses && p->release == presses && !p->twice && !p->open);
}

int main(void) {
    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "seat2-client: no Wayland display\n"); return 2; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wm || !seat_name) { fprintf(stderr, "seat2-client: no compositor, shm, xdg_wm_base or seat\n"); return 2; }
    xdg_wm_base_add_listener(wm, &wm_l, NULL);
    seat_a = wl_registry_bind(reg, seat_name, &wl_seat_interface, seat_ver < 5 ? seat_ver : 5);
    seat_b = wl_registry_bind(reg, seat_name, &wl_seat_interface, seat_ver < 9 ? seat_ver : 9);
    wl_pointer_add_listener(wl_seat_get_pointer(seat_a), &ptr_l, &pa);
    wl_pointer_add_listener(wl_seat_get_pointer(seat_b), &ptr_l, &pb);
    wl_keyboard_add_listener(wl_seat_get_keyboard(seat_a), &kbd_l, &ka);
    wl_keyboard_add_listener(wl_seat_get_keyboard(seat_b), &kbd_l, &kb);

    int W = 400, H = 300, stride = W * 4;
    int fd = memfd_create("seat2-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, stride * H) < 0) { perror("memfd"); return 2; }
    uint32_t *px = mmap(NULL, (size_t)stride * H, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); return 2; }
    for (int i = 0; i < W * H; i++) px[i] = 0xff2a6f4au;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, stride * H);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, W, H, stride, WL_SHM_FORMAT_XRGB8888);
    surf = wl_compositor_create_surface(comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, surf);
    xdg_surface_add_listener(xs, &xs_l, NULL);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(top, &top_l, NULL);
    xdg_toplevel_set_title(top, "seat2-client: click, click, type");
    xdg_toplevel_set_app_id(top, "seat2-client");
    wl_surface_commit(surf);
    while (!configured && wl_display_dispatch(dpy) >= 0) {}
    xdg_surface_ack_configure(xs, conf_serial);
    wl_surface_attach(surf, buf, 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, W, H);
    wl_surface_commit(surf);
    printf("seat2-client: seat A version %u, seat B version %u; click in the window, click again, then type a letter\n",
           seat_ver < 5 ? seat_ver : 5, seat_ver < 9 ? seat_ver : 9);
    fflush(stdout);

    double end = now_s() + 60;
    if (!wait_for(entered, end) || !wait_for(click1, end)) { printf("FAIL no click in 60 s\n"); return 1; }
    report_ptr(&pa, 1, 1);
    report_ptr(&pb, 1, 1);
    check("pointers A and B: the press and the release have the same time stamp on both",
          pa.press_time[0] == pb.press_time[0] && pa.release_time[0] == pb.release_time[0]);
    /* a pointer made while the pointer is over the window */
    wl_pointer_add_listener(wl_seat_get_pointer(seat_a), &ptr_l, &pc);
    wl_display_roundtrip(dpy);
    check("pointer C, made while over the window: an enter at once", pc.enter == 1 && !pc.bad_surface);
    if (!wait_for(click2, end)) { printf("FAIL no second click in 60 s\n"); return 1; }
    report_ptr(&pa, 2, 1);
    report_ptr(&pb, 2, 1);
    report_ptr(&pc, 1, 1);
    if (!wait_for(typed, end)) { printf("FAIL no key in 60 s\n"); return 1; }
    wl_display_roundtrip(dpy);
    for (Kbd *k = &ka; k; k = k == &ka ? &kb : NULL) {
        char w[160];
        snprintf(w, sizeof w, "keyboard %s: %d enter, %d press and %d release (want 1 each), none twice", k->name, k->enter, k->press, k->release);
        check(w, k->enter == 1 && k->press == 1 && k->release == 1 && !k->twice);
    }
    check("keyboards A and B: the same key", ka.key == kb.key);
    printf("%s\n", fails ? "seat2-client: FAILED" : "seat2-client: all checks passed");
    wl_display_disconnect(dpy);
    return fails ? 1 : 0;
}
