/* moddrag_client.c — ❖ + drag anywhere in a Linux window moves it, ❖ + right drag resizes it, and the app never gets
 * those presses (GitHub #219; docs/WINDOWS.md, "Modifier drag"; server/src/main.c, sdk/moddrag.js). Against a headless
 * build/lord-server-test with --test-hooks only (tests/moddrag_headless.sh): the hooks' socket moves the pointer, presses
 * the buttons and holds the modifiers (testhooks.h "key"). Checks, as the app sees them:
 *   - a plain click reaches the app;
 *   - ❖ + left drag: no press and no release reach it, and the window moved by the drag (the pointer at the new place is
 *     over the same point of the window);
 *   - Alt + click, with ❖ chosen: the app's;
 *   - ❖ + right drag near the bottom right corner: no press, and the app is configured that much bigger;
 *   - while the app has the pointer locked (a game): ❖ + click is the app's;
 *   - while the app holds the keyboard shortcuts (a virtual machine, a remote desktop): ❖ + click is the app's.
 * Prints ok/FAIL per check; exit 1 on a failure, 77 without the test hooks. */
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"
#include "client/pointer-constraints-unstable-v1-client-protocol.h"
#include "client/keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"

#define KEY_META 125
#define KEY_ALT 56

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wmbase;
static struct wl_seat *seat;
static struct zwp_pointer_constraints_v1 *cons;
static struct zwp_keyboard_shortcuts_inhibit_manager_v1 *ksim;
static int fails, checks;

static void check(const char *what, int ok, const char *fmt, ...) {
    checks++;
    char detail[200] = "";
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    if (ok) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: %s\n", what, detail); }
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---- the test hooks (src/testhooks.h): one command per connection, "ok" or "err …" ---- */
static int th(const char *fmt, ...) {
    char cmd[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    const char *rt = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (!rt || (size_t)snprintf(a.sun_path, sizeof a.sun_path, "%s/lordos-test-hooks.sock", rt) >= sizeof a.sun_path) return -1;
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    char buf[64];
    struct pollfd p = { s, POLLIN, 0 };
    ssize_t n = -1;
    if (!connect(s, (struct sockaddr *)&a, sizeof a) && send(s, cmd, strlen(cmd), MSG_NOSIGNAL) >= 0 && poll(&p, 1, 3000) == 1)
        n = recv(s, buf, sizeof buf - 1, 0);
    close(s);
    if (n <= 0) return -1;
    buf[n] = 0;
    return strncmp(buf, "ok", 2) ? -1 : 0;
}

/* ---- the window ---- */
static struct wl_surface *surf;
static struct xdg_surface *xsurf;
static struct xdg_toplevel *top;
static int cw = 400, ch = 300;          /* the size it was last configured to (0: its own) */
static int width = 400, height = 300;   /* the size it is drawn at */
static int configured, pending_w = -1, pending_h = -1;

static struct wl_buffer *buffer(int W, int H) {
    int stride = W * 4, size = stride * H;
    int fd = memfd_create("moddrag-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { perror("memfd"); exit(2); }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); exit(2); }
    for (int i = 0; i < W * H; i++) px[i] = 0xff3070c0u;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    munmap(px, (size_t)size);
    return b;
}
static void draw(void) {
    wl_surface_attach(surf, buffer(width, height), 0, 0);
    wl_surface_damage(surf, 0, 0, width, height);
    wl_surface_commit(surf);
}
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
    (void)d;
    xdg_surface_ack_configure(xs, serial);
    if (pending_w > 0 && pending_h > 0) { width = pending_w; height = pending_h; cw = pending_w; ch = pending_h; }
    configured++;
    draw();
}
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { (void)d; (void)t; (void)st; pending_w = w; pending_h = h; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_bounds, top_caps };
static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };

/* ---- the pointer and the keyboard, as the app sees them ---- */
static int ptr_in, kb_in, presses, releases;
static double lx, ly;                   /* where the pointer is on the window, in its own coordinates */
static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)serial;
    ptr_in = s == surf;
    lx = wl_fixed_to_double(x);
    ly = wl_fixed_to_double(y);
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) { (void)d; (void)p; (void)serial; (void)s; ptr_in = 0; }
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)t;
    lx = wl_fixed_to_double(x);
    ly = wl_fixed_to_double(y);
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t state) {
    (void)d; (void)p; (void)serial; (void)t; (void)b;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) presses++; else releases++;
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d; (void)p; (void)t; (void)a; (void)v; }
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void p_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener ptr_l = { .enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button, .axis = p_axis,
    .frame = p_frame, .axis_source = p_source, .axis_stop = p_stop, .axis_discrete = p_discrete };
static void k_keymap(void *d, struct wl_keyboard *k, uint32_t f, int fd, uint32_t size) { (void)d; (void)k; (void)f; (void)size; close(fd); }
static void k_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys) { (void)d; (void)k; (void)serial; (void)keys; kb_in = s == surf; }
static void k_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) { (void)d; (void)k; (void)serial; (void)s; kb_in = 0; }
static void k_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state) { (void)d; (void)k; (void)serial; (void)t; (void)key; (void)state; }
static void k_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t a, uint32_t b, uint32_t c, uint32_t g) { (void)d; (void)k; (void)serial; (void)a; (void)b; (void)c; (void)g; }
static void k_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t delay) { (void)d; (void)k; (void)r; (void)delay; }
static const struct wl_keyboard_listener kb_l = { k_keymap, k_enter, k_leave, k_key, k_mods, k_repeat };

static int locked;
static void lk_locked(void *d, struct zwp_locked_pointer_v1 *l) { (void)d; (void)l; locked = 1; }
static void lk_unlocked(void *d, struct zwp_locked_pointer_v1 *l) { (void)d; (void)l; locked = 0; }
static const struct zwp_locked_pointer_v1_listener lk_l = { lk_locked, lk_unlocked };
static int inhibiting;
static void ki_active(void *d, struct zwp_keyboard_shortcuts_inhibitor_v1 *k) { (void)d; (void)k; inhibiting = 1; }
static void ki_inactive(void *d, struct zwp_keyboard_shortcuts_inhibitor_v1 *k) { (void)d; (void)k; inhibiting = 0; }
static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener ki_l = { ki_active, ki_inactive };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) wmbase = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
    else if (!strcmp(iface, wl_seat_interface.name)) seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5);
    else if (!strcmp(iface, zwp_pointer_constraints_v1_interface.name)) cons = wl_registry_bind(r, name, &zwp_pointer_constraints_v1_interface, 1);
    else if (!strcmp(iface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name)) ksim = wl_registry_bind(r, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

/* the events that come in `ms` milliseconds */
static void pump(int ms) {
    double end = now_ms() + ms;
    while (now_ms() < end) {
        wl_display_flush(dpy);
        struct pollfd p = { wl_display_get_fd(dpy), POLLIN, 0 };
        int left = (int)(end - now_ms());
        if (left < 1) left = 1;
        if (poll(&p, 1, left > 40 ? 40 : left) > 0 && wl_display_dispatch(dpy) < 0) { fprintf(stderr, "moddrag-client: connection lost\n"); exit(1); }
        else wl_display_dispatch_pending(dpy);
    }
}
/* until *flag is set, at most ms */
static int wait_for(int *flag, int ms) {
    double end = now_ms() + ms;
    while (!*flag && now_ms() < end) pump(30);
    return *flag;
}
static void step(const char *cmd) { if (th("%s", cmd)) { fprintf(stderr, "moddrag-client: the test hooks refused \"%s\"\n", cmd); exit(1); } pump(60); }

/* a click with modifier key (0 none) held */
static void click(int key, int button) {
    char b[32];
    if (key) { snprintf(b, sizeof b, "key %d 1", key); step(b); }
    snprintf(b, sizeof b, "btn %d 1", button); step(b);
    snprintf(b, sizeof b, "btn %d 0", button); step(b);
    if (key) { snprintf(b, sizeof b, "key %d 0", key); step(b); }
    pump(200);
}

int main(int argc, char **argv) {
    /* "off": Settings → Windows → Move and resize with is Off (the default): lord-server must take no press at all */
    int off = argc > 1 && !strcmp(argv[1], "off");
    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "moddrag-client: no Wayland display\n"); return 2; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wmbase || !seat) { fprintf(stderr, "moddrag-client: no compositor, shm, xdg_wm_base or seat\n"); return 2; }
    if (th("ptr 3 3")) { printf("skipped: needs lord-server-test --test-hooks\n"); return 77; }
    xdg_wm_base_add_listener(wmbase, &wm_l, NULL);
    struct wl_pointer *ptr = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(ptr, &ptr_l, NULL);
    struct wl_keyboard *kb = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(kb, &kb_l, NULL);

    surf = wl_compositor_create_surface(comp);
    xsurf = xdg_wm_base_get_xdg_surface(wmbase, surf);
    xdg_surface_add_listener(xsurf, &xs_l, NULL);
    top = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(top, &top_l, NULL);
    xdg_toplevel_set_title(top, "moddrag");
    xdg_toplevel_set_app_id(top, "moddrag-client");
    wl_surface_commit(surf);
    check("the window is mapped", wait_for(&configured, 4000), "never configured");
    wait_for(&kb_in, 3000);
    pump(800);                                  /* the theme placed and drew it */

    /* the pointer over the window, near its middle: a few points around the screen's middle until it enters */
    static const int pts[][2] = { { 640, 420 }, { 600, 380 }, { 700, 460 }, { 560, 340 }, { 740, 500 } };
    int px = -1, py = -1;
    for (size_t i = 0; i < sizeof pts / sizeof *pts && px < 0; i++) {
        th("ptr 3 3");
        pump(100);
        char b[32];
        snprintf(b, sizeof b, "ptr %d %d", pts[i][0], pts[i][1]);
        step(b);
        if (wait_for(&ptr_in, 600) && lx > 40 && ly > 40 && lx < width - 40 && ly < height - 40) { px = pts[i][0]; py = pts[i][1]; }
    }
    check("the pointer is over the window", px >= 0, "it never entered the window's middle");
    if (px < 0) { printf("%d checks, %d failed\n", checks, fails); return 1; }
    double lx0 = lx, ly0 = ly;

    presses = releases = 0;
    click(0, 0);
    check("a plain click reaches the app", presses == 1 && releases == 1, "%d presses, %d releases", presses, releases);

    if (off) {                                  /* the default: ❖ + drag and ❖ + right click are the app's, nothing moves */
        presses = releases = 0;
        step("key 125 1");
        step("btn 0 1");
        char o[48];
        snprintf(o, sizeof o, "ptr %d %d", px + 40, py + 25); step(o);
        snprintf(o, sizeof o, "ptr %d %d", px + 80, py + 50); step(o);
        step("btn 0 0");
        step("key 125 0");
        pump(400);
        check("off: ❖ + drag is the app's (its press and release)", presses == 1 && releases == 1, "%d presses, %d releases", presses, releases);
        check("off: the window did not move", ptr_in && lx > lx0 + 76 && lx < lx0 + 84 && ly > ly0 + 46 && ly < ly0 + 54,
              "the pointer is at (%.0f, %.0f) on the window, want (%.0f, %.0f)", lx, ly, lx0 + 80, ly0 + 50);
        int w0 = width, h0 = height;
        presses = releases = 0;
        click(KEY_META, 2);
        click(KEY_ALT, 0);
        click(KEY_META, 1);
        pump(300);
        check("off: ❖ + right click, Alt + click and ❖ + middle click are the app's", presses == 3 && releases == 3, "%d presses, %d releases", presses, releases);
        check("off: the window kept its size", width == w0 && height == h0, "%d × %d, was %d × %d", width, height, w0, h0);
        wl_display_roundtrip(dpy);
        check("still connected at the end", wl_display_get_error(dpy) == 0, "protocol error %d", wl_display_get_error(dpy));
        printf("%d checks, %d failed\n", checks, fails);
        wl_display_disconnect(dpy);
        return fails ? 1 : 0;
    }

    /* ❖ + left drag by (80, 50): the app gets nothing, the window follows */
    presses = releases = 0;
    step("key 125 1");
    step("btn 0 1");
    char b[48];
    snprintf(b, sizeof b, "ptr %d %d", px + 40, py + 25); step(b);
    snprintf(b, sizeof b, "ptr %d %d", px + 80, py + 50); step(b);
    step("btn 0 0");
    step("key 125 0");
    pump(400);
    check("❖ + drag: the app gets no press and no release", presses == 0 && releases == 0, "%d presses, %d releases", presses, releases);
    snprintf(b, sizeof b, "ptr %d %d", px + 81, py + 51); step(b);
    pump(300);
    check("❖ + drag: the window moved with the pointer", ptr_in && lx > lx0 - 3 && lx < lx0 + 5 && ly > ly0 - 3 && ly < ly0 + 5,
          "at the dragged-to point the pointer is at (%.0f, %.0f) on the window, was (%.0f, %.0f)%s", lx, ly, lx0, ly0, ptr_in ? "" : ", not over it");
    px += 80;
    py += 50;
    /* the window's top left on screen, from where the pointer is on it */
    int ox = px + 1 - (int)lx, oy = py + 1 - (int)ly;

    /* Alt + click while ❖ is the modifier: the app's (GIMP's, Blender's) */
    snprintf(b, sizeof b, "ptr %d %d", px, py); step(b);
    presses = releases = 0;
    click(KEY_ALT, 0);
    check("Alt + click with ❖ chosen: the app's", presses == 1 && releases == 1, "%d presses, %d releases", presses, releases);

    /* ❖ + right drag near the bottom right corner by (60, 40): resized from there */
    int w0 = width, h0 = height;
    snprintf(b, sizeof b, "ptr %d %d", ox + w0 - 25, oy + h0 - 25); step(b);
    presses = releases = 0;
    step("key 125 1");
    step("btn 2 1");
    snprintf(b, sizeof b, "ptr %d %d", ox + w0 + 5, oy + h0 - 5); step(b);
    snprintf(b, sizeof b, "ptr %d %d", ox + w0 + 35, oy + h0 + 15); step(b);
    step("btn 2 0");
    step("key 125 0");
    pump(600);
    check("❖ + right drag: the app gets no press", presses == 0 && releases == 0, "%d presses, %d releases", presses, releases);
    check("❖ + right drag: the app is resized from the bottom right corner by the drag",
          width >= w0 + 56 && width <= w0 + 64 && height >= h0 + 36 && height <= h0 + 44, "%d × %d, was %d × %d (want +60, +40)", width, height, w0, h0);

    /* the pointer locked by the app (a game): ❖ + click is the app's */
    snprintf(b, sizeof b, "ptr %d %d", ox + 100, oy + 100); step(b);
    if (cons) {
        struct zwp_locked_pointer_v1 *lk = zwp_pointer_constraints_v1_lock_pointer(cons, surf, ptr, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        zwp_locked_pointer_v1_add_listener(lk, &lk_l, NULL);
        wl_surface_commit(surf);
        snprintf(b, sizeof b, "ptr %d %d", ox + 101, oy + 100); step(b);
        wait_for(&locked, 1500);
        check("the app locked the pointer", locked, "never locked");
        presses = releases = 0;
        click(KEY_META, 0);
        check("pointer locked: ❖ + click is the app's", presses == 1 && releases == 1, "%d presses, %d releases", presses, releases);
        zwp_locked_pointer_v1_destroy(lk);
        pump(200);
    } else check("pointer constraints are offered", 0, "no zwp_pointer_constraints_v1");

    /* the keyboard shortcuts held by the app (a virtual machine): ❖ + click is the app's */
    if (ksim) {
        struct zwp_keyboard_shortcuts_inhibitor_v1 *ki = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(ksim, surf, seat);
        zwp_keyboard_shortcuts_inhibitor_v1_add_listener(ki, &ki_l, NULL);
        wait_for(&inhibiting, 3000);             /* the theme's hook allows it (tests/moddrag_headless.sh) */
        check("the app holds the shortcuts", inhibiting, "the inhibitor never became active");
        presses = releases = 0;
        click(KEY_META, 0);
        check("shortcuts held: ❖ + click is the app's", presses == 1 && releases == 1, "%d presses, %d releases", presses, releases);
        zwp_keyboard_shortcuts_inhibitor_v1_destroy(ki);
        pump(300);
        presses = releases = 0;
        click(KEY_META, 0);
        check("shortcuts let go: ❖ + click is the window manager's again", presses == 0 && releases == 0, "%d presses, %d releases", presses, releases);
    } else check("keyboard shortcuts inhibitors are offered", 0, "no zwp_keyboard_shortcuts_inhibit_manager_v1");

    wl_display_roundtrip(dpy);
    check("still connected at the end", wl_display_get_error(dpy) == 0, "protocol error %d", wl_display_get_error(dpy));
    printf("%d checks, %d failed\n", checks, fails);
    wl_display_disconnect(dpy);
    return fails ? 1 : 0;
}
