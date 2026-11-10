/* kbinhibit_client.c — keyboard shortcuts inhibitors as a client sees them (docs/WAYLAND.md, "Keyboard shortcuts
 * inhibit"): run inside a LordOS session.
 *
 *   One inhibitor per surface: a second is the already_inhibited protocol error, and the compositor stays up.
 *   Destroyed and asked for again on the same surface: no error. A wl_surface destroyed before its inhibitor, or
 *   with it still there: the inhibitor's destroy and the compositor are fine, and a new window may ask again.
 *
 *   WAYLAND_DISPLAY=wayland-0 build/kbinhibit-client          (prints ok/FAIL per check, exit 1 on a failure)
 *   WAYLAND_DISPLAY=wayland-0 build/kbinhibit-client hold 30  (keeps a window with an inhibitor for 30 s and prints
 *                                                              active/inactive and the keys it gets: the first time
 *                                                              LordOS asks; with the window in front ❖Tab arrives
 *                                                              here, and ❖Esc turns it off and back on) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"
#include "client/keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"

typedef struct {
    struct wl_display *dpy;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct xdg_wm_base *wmbase;
    struct zwp_keyboard_shortcuts_inhibit_manager_v1 *ksim;
} Conn;

static int fails, checks;

static void check(const char *what, long got, long want) {
    checks++;
    if (got == want) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); }
}

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    Conn *c = d;
    if (!strcmp(iface, wl_compositor_interface.name)) c->comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) c->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name)) c->seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) c->wmbase = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
    else if (!strcmp(iface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name))
        c->ksim = wl_registry_bind(r, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };

static int connect_to(Conn *c) {
    memset(c, 0, sizeof *c);
    c->dpy = wl_display_connect(NULL);
    if (!c->dpy) return -1;
    struct wl_registry *reg = wl_display_get_registry(c->dpy);
    wl_registry_add_listener(reg, &reg_l, c);
    wl_display_roundtrip(c->dpy);
    if (!c->comp || !c->shm || !c->wmbase || !c->seat) return -1;
    xdg_wm_base_add_listener(c->wmbase, &wm_l, NULL);
    return 0;
}

typedef struct {
    struct wl_surface *surf;
    struct xdg_surface *xs;
    struct xdg_toplevel *top;
} Win;

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) { (void)d; xdg_surface_ack_configure(xs, serial); }
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { (void)d; (void)t; (void)w; (void)h; (void)st; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_bounds, top_caps };

static Win *new_window(Conn *c, const char *title, uint32_t color) {
    Win *w = calloc(1, sizeof *w);
    w->surf = wl_compositor_create_surface(c->comp);
    w->xs = xdg_wm_base_get_xdg_surface(c->wmbase, w->surf);
    xdg_surface_add_listener(w->xs, &xs_l, w);
    w->top = xdg_surface_get_toplevel(w->xs);
    xdg_toplevel_add_listener(w->top, &top_l, w);
    xdg_toplevel_set_title(w->top, title);
    xdg_toplevel_set_app_id(w->top, "kbinhibit-client");
    wl_surface_commit(w->surf);
    wl_display_roundtrip(c->dpy);
    int W = 360, H = 220, stride = W * 4, size = stride * H;
    int fd = memfd_create("kbinhibit-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { perror("memfd"); exit(2); }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); exit(2); }
    for (int i = 0; i < W * H; i++) px[i] = color;
    struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    munmap(px, (size_t)size);
    wl_surface_attach(w->surf, b, 0, 0);
    wl_surface_damage(w->surf, 0, 0, W, H);
    wl_surface_commit(w->surf);
    wl_display_roundtrip(c->dpy);
    return w;
}

static void got_active(void *d, struct zwp_keyboard_shortcuts_inhibitor_v1 *i) { (void)i; ((int *)d)[0]++; printf("     active\n"); fflush(stdout); }
static void got_inactive(void *d, struct zwp_keyboard_shortcuts_inhibitor_v1 *i) { (void)i; ((int *)d)[1]++; printf("     inactive\n"); fflush(stdout); }
static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener inh_l = { got_active, got_inactive };

/* hold: the keys it gets, to see ❖Tab and ⌥Tab arrive */
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t s) { (void)d; (void)k; (void)f; (void)s; close(fd); }
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *w, struct wl_array *a) { (void)d; (void)k; (void)s; (void)w; (void)a; }
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *w) { (void)d; (void)k; (void)s; (void)w; }
static void kb_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t st) {
    (void)d; (void)k; (void)s; (void)t;
    printf("     key %u %s\n", key, st ? "down" : "up");
    fflush(stdout);
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t g) {
    (void)d; (void)k; (void)s; (void)lat; (void)lock; (void)g;
    printf("     modifiers 0x%x\n", dep);
    fflush(stdout);
}
static const struct wl_keyboard_listener kb_l = { kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, NULL };

static void sleep_ms(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }

int main(int argc, char **argv) {
    Conn a;
    if (connect_to(&a)) { fprintf(stderr, "kbinhibit-client: no Wayland display\n"); return 2; }
    check("zwp_keyboard_shortcuts_inhibit_manager_v1 offered", a.ksim != NULL, 1);
    if (!a.ksim) { printf("%d checks, %d failed\n", checks, fails); return 1; }

    /* one inhibitor, destroyed, and another on the same window */
    Win *w = new_window(&a, "kbinhibit-client", 0xff3a7a4au);
    int seen[2] = { 0, 0 };
    struct zwp_keyboard_shortcuts_inhibitor_v1 *i1 = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(a.ksim, w->surf, a.seat);
    zwp_keyboard_shortcuts_inhibitor_v1_add_listener(i1, &inh_l, seen);
    wl_display_roundtrip(a.dpy);
    check("an inhibitor is accepted", wl_display_get_error(a.dpy), 0);
    check("no inactive before any active", seen[1] <= seen[0], 1);
    zwp_keyboard_shortcuts_inhibitor_v1_destroy(i1);
    wl_display_roundtrip(a.dpy);
    struct zwp_keyboard_shortcuts_inhibitor_v1 *i2 = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(a.ksim, w->surf, a.seat);
    zwp_keyboard_shortcuts_inhibitor_v1_add_listener(i2, &inh_l, seen);
    wl_display_roundtrip(a.dpy);
    check("after its destroy the same surface may ask again", wl_display_get_error(a.dpy), 0);

    if (argc >= 3 && !strcmp(argv[1], "hold")) {
        struct wl_keyboard *kb = wl_seat_get_keyboard(a.seat);
        wl_keyboard_add_listener(kb, &kb_l, NULL);
        printf("     holding: click the green window; LordOS asks the first time\n");
        fflush(stdout);
        for (int i = 0, n = atoi(argv[2]) * 4; i < n; i++) {
            sleep_ms(250);
            wl_display_roundtrip(a.dpy);
        }
        wl_keyboard_destroy(kb);
    }

    /* a second inhibitor for the same surface is a protocol error, on its own connection */
    {
        Conn d;
        if (!connect_to(&d) && d.ksim) {
            Win *dw = new_window(&d, "kbinhibit-client: twice", 0xff606060u);
            zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(d.ksim, dw->surf, d.seat);
            zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(d.ksim, dw->surf, d.seat);
            wl_display_roundtrip(d.dpy);
            const struct wl_interface *iface = NULL;
            uint32_t id = 0;
            int err = wl_display_get_error(d.dpy);
            uint32_t code = err ? wl_display_get_protocol_error(d.dpy, &iface, &id) : 99;
            check("a second inhibitor is refused", err != 0, 1);
            check("with already_inhibited", code, ZWP_KEYBOARD_SHORTCUTS_INHIBIT_MANAGER_V1_ERROR_ALREADY_INHIBITED);
            check("from the manager", iface == &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
            wl_display_disconnect(d.dpy);
        }
        wl_display_roundtrip(a.dpy);
        check("the compositor is still up after the refusal", wl_display_get_error(a.dpy), 0);
    }

    /* the wl_surface goes first: the inhibitor no longer points at it, its destroy is fine, and a client that
     * disconnects with an inhibitor on a gone surface leaves the compositor up */
    {
        Conn e;
        if (!connect_to(&e) && e.ksim) {
            Win *ew = new_window(&e, "kbinhibit-client: gone first", 0xff804040u);
            struct zwp_keyboard_shortcuts_inhibitor_v1 *gi = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(e.ksim, ew->surf, e.seat);
            wl_display_roundtrip(e.dpy);
            xdg_toplevel_destroy(ew->top);
            xdg_surface_destroy(ew->xs);
            wl_surface_destroy(ew->surf);
            wl_display_roundtrip(e.dpy);
            check("a surface destroyed before its inhibitor", wl_display_get_error(e.dpy), 0);
            zwp_keyboard_shortcuts_inhibitor_v1_destroy(gi);
            wl_display_roundtrip(e.dpy);
            check("then the inhibitor's destroy", wl_display_get_error(e.dpy), 0);
            Win *nw = new_window(&e, "kbinhibit-client: next", 0xff404080u);
            struct zwp_keyboard_shortcuts_inhibitor_v1 *ni = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(e.ksim, nw->surf, e.seat);
            wl_display_roundtrip(e.dpy);
            check("a new window asks again", wl_display_get_error(e.dpy), 0);
            (void)ni;
            wl_surface_destroy(nw->surf);           /* leaves with the inhibitor alive on a gone surface */
            wl_display_disconnect(e.dpy);
        }
        wl_display_roundtrip(a.dpy);
        check("the compositor is still up after that client left", wl_display_get_error(a.dpy), 0);
    }

    zwp_keyboard_shortcuts_inhibitor_v1_destroy(i2);
    wl_display_roundtrip(a.dpy);
    check("still up after the cleanup", wl_display_get_error(a.dpy), 0);
    printf("%d checks, %d failed\n", checks, fails);
    wl_display_disconnect(a.dpy);
    return fails ? 1 : 0;
}
