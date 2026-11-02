/* deco_client.c — who draws a window's frame (docs/WAYLAND.md, "Frame and colors"): run inside a
 * LordOS session, it checks that axia-server honors the mode an app asks for.
 *
 *   xdg-decoration: the first configure prefers SERVER_SIDE; set_mode(CLIENT_SIDE) is answered
 *   CLIENT_SIDE (also after the window is mapped, followed by an xdg_surface.configure);
 *   unset_mode and set_mode(SERVER_SIDE) give SERVER_SIDE back.
 *   org_kde_kwin_server_decoration (GTK 3): default «server», request_mode(client) → client.
 *
 *   WAYLAND_DISPLAY=wayland-0 build/deco-client            (prints ok/FAIL per check, exit 1 on a failure)
 *   WAYLAND_DISPLAY=wayland-0 build/deco-client hold 6     (then keeps a mapped window and switches
 *                                                           its mode every 1.5 s, to watch the theme) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"
#include "client/xdg-decoration-unstable-v1-client-protocol.h"

/* org_kde_kwin_server_decoration: the same hand-written tables as server/src/wl/wl.c */
static const struct wl_interface kde_deco_interface;
static const struct wl_interface *kde_types[] = { NULL, &kde_deco_interface, &wl_surface_interface };
static const struct wl_message kde_mgr_requests[] = { { "create", "no", kde_types + 1 } };
static const struct wl_message kde_mgr_events[] = { { "default_mode", "u", kde_types } };
static const struct wl_interface kde_mgr_interface = { "org_kde_kwin_server_decoration_manager", 1, 1, kde_mgr_requests, 1, kde_mgr_events };
static const struct wl_message kde_deco_requests[] = { { "release", "", kde_types }, { "request_mode", "u", kde_types } };
static const struct wl_message kde_deco_events[] = { { "mode", "u", kde_types } };
static const struct wl_interface kde_deco_interface = { "org_kde_kwin_server_decoration", 1, 2, kde_deco_requests, 1, kde_deco_events };
enum { KDE_NONE = 0, KDE_CLIENT = 1, KDE_SERVER = 2 };

static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wmbase;
static struct zxdg_decoration_manager_v1 *decm;
static struct wl_proxy *kdem;
static int fails, checks;

static void check(const char *what, long got, long want) {
    checks++;
    if (got == want) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); }
}

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) wmbase = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
    else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) decm = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
    else if (!strcmp(iface, kde_mgr_interface.name)) kdem = wl_registry_bind(r, name, &kde_mgr_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };

typedef struct {
    struct wl_surface *surf;
    struct xdg_surface *xs;
    struct xdg_toplevel *top;
    struct zxdg_toplevel_decoration_v1 *deco;
    int mode, mode_events, xs_configures, w, h, closed;
    uint32_t serial;
} Win;

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
    Win *w = d;
    w->xs_configures++;
    w->serial = serial;
    xdg_surface_ack_configure(xs, serial);
}
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {
    (void)t; (void)st;
    Win *win = d;
    if (w > 0) win->w = w;
    if (h > 0) win->h = h;
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)t; ((Win *)d)->closed = 1; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_bounds, top_caps };
static void deco_configure(void *d, struct zxdg_toplevel_decoration_v1 *z, uint32_t mode) {
    (void)z;
    Win *w = d;
    w->mode = (int)mode;
    w->mode_events++;
}
static const struct zxdg_toplevel_decoration_v1_listener deco_l = { deco_configure };

static int kde_mode = -1, kde_default = -1;
static void kde_mode_ev(void *d, struct wl_proxy *p, uint32_t m) { (void)d; (void)p; kde_mode = (int)m; }
static void kde_default_ev(void *d, struct wl_proxy *p, uint32_t m) { (void)d; (void)p; kde_default = (int)m; }
static void (*kde_deco_listener[])(void *, struct wl_proxy *, uint32_t) = { kde_mode_ev };
static void (*kde_mgr_listener[])(void *, struct wl_proxy *, uint32_t) = { kde_default_ev };

/* a solid buffer: blue for "my own frame" (client side), grey for "the theme's frame" */
static void draw(Win *w, int csd) {
    int W = w->w > 0 ? w->w : 360, H = w->h > 0 ? w->h : 220, stride = W * 4, size = stride * H;
    int fd = memfd_create("deco-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { perror("memfd"); exit(2); }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); exit(2); }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) px[y * W + x] = csd ? (y < 36 ? 0xff1e5aa8u : 0xffdfe8f5u) : 0xffe6e6e6u;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    munmap(px, (size_t)size);
    wl_surface_attach(w->surf, b, 0, 0);
    wl_surface_damage(w->surf, 0, 0, W, H);
    wl_surface_commit(w->surf);
}

static Win *new_window(struct wl_display *dpy, const char *title) {
    Win *w = calloc(1, sizeof *w);
    w->mode = -1;
    w->surf = wl_compositor_create_surface(comp);
    w->xs = xdg_wm_base_get_xdg_surface(wmbase, w->surf);
    xdg_surface_add_listener(w->xs, &xs_l, w);
    w->top = xdg_surface_get_toplevel(w->xs);
    xdg_toplevel_add_listener(w->top, &top_l, w);
    xdg_toplevel_set_title(w->top, title);
    xdg_toplevel_set_app_id(w->top, "deco-client");
    if (decm) {
        w->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(decm, w->top);
        zxdg_toplevel_decoration_v1_add_listener(w->deco, &deco_l, w);
    }
    wl_surface_commit(w->surf);
    wl_display_roundtrip(dpy);
    return w;
}

static void sleep_ms(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }

int main(int argc, char **argv) {
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "deco-client: no Wayland display\n"); return 2; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wmbase) { fprintf(stderr, "deco-client: missing core globals\n"); return 2; }
    xdg_wm_base_add_listener(wmbase, &wm_l, NULL);
    check("xdg-decoration manager offered", decm != NULL, 1);
    check("KDE server-decoration manager offered", kdem != NULL, 1);
    if (kdem) { wl_proxy_add_listener(kdem, (void (**)(void))kde_mgr_listener, NULL); wl_display_roundtrip(dpy); }
    check("KDE default mode is server", kde_default, KDE_SERVER);
    if (!decm) return 1;

    /* before mapping */
    Win *a = new_window(dpy, "deco-client A");
    check("first configure prefers server side", a->mode, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    zxdg_toplevel_decoration_v1_set_mode(a->deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    wl_display_roundtrip(dpy);
    check("set_mode(client) answered client", a->mode, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    zxdg_toplevel_decoration_v1_unset_mode(a->deco);
    wl_display_roundtrip(dpy);
    check("unset_mode answered server", a->mode, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    zxdg_toplevel_decoration_v1_set_mode(a->deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    wl_display_roundtrip(dpy);
    check("set_mode(server) answered server", a->mode, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

    /* mapped, then switched live */
    draw(a, 0);
    wl_display_roundtrip(dpy);
    sleep_ms(300);
    wl_display_roundtrip(dpy);
    int before = a->xs_configures;
    zxdg_toplevel_decoration_v1_set_mode(a->deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    wl_display_roundtrip(dpy);
    check("mapped: set_mode(client) answered client", a->mode, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    check("mapped: followed by an xdg_surface.configure", a->xs_configures > before, 1);
    draw(a, 1);
    wl_display_roundtrip(dpy);
    sleep_ms(400);                       /* the theme drops its frame: the content area grows → configure */
    wl_display_roundtrip(dpy);
    printf("info content after client side: %dx%d\n", a->w, a->h);
    zxdg_toplevel_decoration_v1_set_mode(a->deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    wl_display_roundtrip(dpy);
    check("mapped: back to server", a->mode, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    draw(a, 0);
    wl_display_roundtrip(dpy);
    sleep_ms(400);
    wl_display_roundtrip(dpy);
    printf("info content after server side: %dx%d\n", a->w, a->h);

    /* GTK 3's protocol */
    if (kdem) {
        struct wl_surface *s2 = wl_compositor_create_surface(comp);
        struct wl_proxy *kd = wl_proxy_marshal_constructor(kdem, 0, &kde_deco_interface, NULL, s2);
        wl_proxy_add_listener(kd, (void (**)(void))kde_deco_listener, NULL);
        wl_display_roundtrip(dpy);
        check("KDE: new decoration says server", kde_mode, KDE_SERVER);
        wl_proxy_marshal(kd, 1, (uint32_t)KDE_CLIENT);
        wl_display_roundtrip(dpy);
        check("KDE: request_mode(client) answered client", kde_mode, KDE_CLIENT);
        wl_proxy_marshal(kd, 1, (uint32_t)KDE_SERVER);
        wl_display_roundtrip(dpy);
        check("KDE: request_mode(server) answered server", kde_mode, KDE_SERVER);
        wl_proxy_marshal(kd, 0);
        wl_proxy_destroy(kd);
        wl_surface_destroy(s2);
    }

    if (argc >= 3 && !strcmp(argv[1], "hold")) {
        int secs = atoi(argv[2]), csd = 0;
        for (int i = 0; i < secs * 2 / 3 + 1 && !a->closed; i++) {
            sleep_ms(1500);
            csd = !csd;
            zxdg_toplevel_decoration_v1_set_mode(a->deco, csd ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
            wl_display_roundtrip(dpy);
            draw(a, csd);
            wl_display_roundtrip(dpy);
            printf("hold: %s, %dx%d\n", csd ? "client side" : "server side", a->w, a->h);
            fflush(stdout);
        }
    }
    printf("%d checks, %d failed\n", checks, fails);
    wl_display_disconnect(dpy);
    return fails ? 1 : 0;
}
