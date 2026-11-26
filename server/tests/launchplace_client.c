/* launchplace_client.c — a Wayland app that behaves like Google Chrome with several profiles, for Workspaces' launch
 * placement (tests/launchplace_headless.sh, docs/WORKSPACES.md "Where launched windows go"). Launched through a .desktop
 * entry by lord.apps.launch, as the real browser is:
 *
 *   1. it shows its "profile picker" (app_id APP, 560×420, blue) and keeps it PICK_MS (the person choosing);
 *   2. order "after": the picker goes away, then the browser window (app_id APP, 1000×700, green) shows;
 *      order "before": the browser window shows first and the picker goes 300 ms later (both orders are real);
 *   3. with "dialog": 400 ms later a dialog (app_id APP, 420×260, orange) shows and only after its first frame
 *      names the browser window as its parent (xdg_toplevel.set_parent, as an X11 app's late WM_TRANSIENT_FOR);
 *   4. it keeps everything up HOLD_MS and leaves.
 *
 *   launchplace-client APP PICK_MS after|before [dialog] [HOLD_MS]
 * Prints what it did on stderr; exit 2 when there is no Wayland display. Test only, never installed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wmbase;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) wmbase = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };
static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };

typedef struct { struct wl_surface *surf; struct xdg_surface *xs; struct xdg_toplevel *top; int w, h; uint32_t color; } Win;

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) { (void)d; xdg_surface_ack_configure(xs, serial); }
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {
    (void)t; (void)st;
    Win *win = d;
    if (w > 0 && h > 0) { win->w = w; win->h = h; }        /* the size the compositor asks (a tile) */
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_bounds, top_caps };

static void draw(Win *w) {
    int stride = w->w * 4, size = stride * w->h;
    int fd = memfd_create("launchplace-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { perror("memfd"); exit(2); }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); exit(2); }
    for (int i = 0; i < w->w * w->h; i++) px[i] = w->color;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w->w, w->h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    munmap(px, (size_t)size);
    wl_surface_attach(w->surf, b, 0, 0);
    wl_surface_damage(w->surf, 0, 0, w->w, w->h);
    wl_surface_commit(w->surf);
    wl_display_roundtrip(dpy);
}

static Win *show(const char *app, const char *title, int W, int H, uint32_t color) {
    Win *w = calloc(1, sizeof *w);
    if (!w) exit(2);
    w->w = W; w->h = H; w->color = color;
    w->surf = wl_compositor_create_surface(comp);
    w->xs = xdg_wm_base_get_xdg_surface(wmbase, w->surf);
    xdg_surface_add_listener(w->xs, &xs_l, w);
    w->top = xdg_surface_get_toplevel(w->xs);
    xdg_toplevel_add_listener(w->top, &top_l, w);
    xdg_toplevel_set_title(w->top, title);
    xdg_toplevel_set_app_id(w->top, app);
    wl_surface_commit(w->surf);
    wl_display_roundtrip(dpy);
    draw(w);
    fprintf(stderr, "launchplace-client: shown %s\n", title);
    return w;
}
static void hide(Win *w, const char *what) {
    xdg_toplevel_destroy(w->top);
    xdg_surface_destroy(w->xs);
    wl_surface_destroy(w->surf);
    wl_display_roundtrip(dpy);
    free(w);
    fprintf(stderr, "launchplace-client: closed %s\n", what);
}
/* keeps answering (pings, configures: a tile's size is drawn at once) for ms milliseconds */
static void run_for(int ms, Win **wins, int n) {
    for (int t = 0; t < ms; t += 50) {
        struct timespec s = { 0, 50 * 1000000L };
        nanosleep(&s, NULL);
        wl_display_roundtrip(dpy);
        for (int i = 0; i < n; i++) if (wins[i]) draw(wins[i]);
    }
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: launchplace-client APP PICK_MS after|before [dialog] [HOLD_MS]\n"); return 1; }
    const char *app = argv[1];
    int pick = atoi(argv[2]), before = !strcmp(argv[3], "before"), dialog = argc > 4 && !strcmp(argv[4], "dialog");
    int hold = argc > 5 ? atoi(argv[5]) : argc > 4 && !dialog ? atoi(argv[4]) : 20000;
    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "launchplace-client: no Wayland display\n"); return 2; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wmbase) return 2;
    xdg_wm_base_add_listener(wmbase, &wm_l, NULL);

    Win *picker = show(app, "Who's using Chrome?", 560, 420, 0xff2a6fd6u);
    Win *wins[3] = { picker, NULL, NULL };
    run_for(pick, wins, 1);
    Win *browser = NULL;
    if (before) {
        browser = show(app, "New Tab - Google Chrome", 1000, 700, 0xff2e9d4au);
        wins[1] = browser;
        run_for(300, wins, 2);
        hide(picker, "the picker");
    } else {
        hide(picker, "the picker");
        run_for(400, NULL, 0);
        browser = show(app, "New Tab - Google Chrome", 1000, 700, 0xff2e9d4au);
    }
    wins[0] = browser; wins[1] = NULL;
    run_for(400, wins, 1);
    if (dialog) {
        Win *d = show(app, "Save File", 420, 260, 0xffe08a1eu);
        run_for(150, NULL, 0);
        xdg_toplevel_set_parent(d->top, browser->top);   /* told late, after its first frame */
        wl_surface_commit(d->surf);
        wl_display_roundtrip(dpy);
        fprintf(stderr, "launchplace-client: the dialog named its parent\n");
        wins[1] = d;
    }
    run_for(hold, wins, 1);
    wl_display_disconnect(dpy);
    return 0;
}
