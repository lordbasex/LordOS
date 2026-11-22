/* wlproof_client.c — a window that shows, in pixels, what the conformance tool checks by protocol (tools/wl-proof.mjs
 * screenshots it; docs/wayland-proofs/): wl_subsurface.place_above/place_below and wp_viewport's crop and scale.
 *
 *   window   400×300, grey, its right third transparent
 *   red      160×160 at (30, 30), made first           \  the second subsurface is over the first by default;
 *   green    160×160 at (110, 90), made second          /  place_above(red, green) puts red over green
 *   blue     150×150 at (240, 130): place_below(parent): under the window, seen only through its transparent third
 *   viewport a 64×64 buffer in four colours (red, green, blue, white quadrants), cropped to its bottom-right quadrant
 *            (32, 32, 32×32: white) and scaled to 100×100 at (20, 190): one white square, nothing else of the buffer
 *
 *   WAYLAND_DISPLAY=wayland-0 build/wlproof-client [seconds]   (stays up that long, 20 by default; exit 1 on an error) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"
#include "client/viewporter-client-protocol.h"

static struct wl_compositor *comp;
static struct wl_subcompositor *subc;
static struct wl_shm *shm;
static struct xdg_wm_base *wmbase;
static struct wp_viewporter *vporter;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d; (void)ver;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_subcompositor_interface.name)) subc = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) wmbase = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, wp_viewporter_interface.name)) vporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };
static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };

static int configured;
static uint32_t conf_serial;
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) { (void)d; (void)xs; conf_serial = serial; configured = 1; }
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { (void)d; (void)t; (void)w; (void)h; (void)st; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; exit(0); }
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, NULL, NULL };

/* fill(x, y) gives each pixel (ARGB, premultiplied) */
static struct wl_buffer *buffer(int W, int H, uint32_t (*fill)(int, int)) {
    int stride = W * 4, size = stride * H;
    int fd = memfd_create("wlproof", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { perror("memfd"); exit(2); }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); exit(2); }
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) px[y * W + x] = fill(x, y);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    munmap(px, (size_t)size);
    return b;
}
static uint32_t f_window(int x, int y) { (void)y; return x >= 267 ? 0 : 0xff808080u; }
static uint32_t f_red(int x, int y) { (void)x; (void)y; return 0xffe02020u; }
static uint32_t f_green(int x, int y) { (void)x; (void)y; return 0xff20c020u; }
static uint32_t f_blue(int x, int y) { (void)x; (void)y; return 0xff2040e0u; }
static uint32_t f_quads(int x, int y) {
    static const uint32_t q[4] = { 0xffe02020u, 0xff20c020u, 0xff2040e0u, 0xffffffffu };
    return q[(y >= 32) * 2 + (x >= 32)];
}

static struct wl_surface *sub(struct wl_surface *parent, struct wl_subsurface **ss, int x, int y, int w, int h, uint32_t (*fill)(int, int)) {
    struct wl_surface *s = wl_compositor_create_surface(comp);
    *ss = wl_subcompositor_get_subsurface(subc, s, parent);
    wl_subsurface_set_position(*ss, x, y);
    wl_subsurface_set_desync(*ss);
    wl_surface_attach(s, buffer(w, h, fill), 0, 0);
    wl_surface_damage(s, 0, 0, w, h);
    wl_surface_commit(s);
    return s;
}

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 20;
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "no Wayland display\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !subc || !shm || !wmbase || !vporter) { fprintf(stderr, "a global is missing\n"); return 1; }
    xdg_wm_base_add_listener(wmbase, &wm_l, NULL);

    struct wl_surface *win = wl_compositor_create_surface(comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wmbase, win);
    xdg_surface_add_listener(xs, &xs_l, NULL);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(top, &top_l, NULL);
    xdg_toplevel_set_title(top, "wlproof");
    xdg_toplevel_set_app_id(top, "org.lordos.wlproof");
    xdg_toplevel_set_min_size(top, 400, 300);
    xdg_toplevel_set_max_size(top, 400, 300);
    wl_surface_commit(win);
    while (!configured && wl_display_dispatch(dpy) >= 0) {}
    xdg_surface_ack_configure(xs, conf_serial);

    struct wl_subsurface *sr, *sg, *sb, *sv;
    struct wl_surface *red = sub(win, &sr, 30, 30, 160, 160, f_red);
    struct wl_surface *green = sub(win, &sg, 110, 90, 160, 160, f_green);
    sub(win, &sb, 240, 130, 150, 150, f_blue);
    wl_subsurface_place_above(sr, green);       /* red over green */
    wl_subsurface_place_below(sb, win);         /* blue under the window */
    (void)red;
    /* the viewport: the bottom-right quadrant (white) of a four-colour buffer, scaled to 100×100 */
    struct wl_surface *v = wl_compositor_create_surface(comp);
    sv = wl_subcompositor_get_subsurface(subc, v, win);
    wl_subsurface_set_position(sv, 20, 190);
    wl_subsurface_set_desync(sv);
    struct wp_viewport *vp = wp_viewporter_get_viewport(vporter, v);
    wp_viewport_set_source(vp, wl_fixed_from_int(32), wl_fixed_from_int(32), wl_fixed_from_int(32), wl_fixed_from_int(32));
    wp_viewport_set_destination(vp, 100, 100);
    wl_surface_attach(v, buffer(64, 64, f_quads), 0, 0);
    wl_surface_damage(v, 0, 0, 100, 100);
    wl_surface_commit(v);

    wl_surface_attach(win, buffer(400, 300, f_window), 0, 0);
    wl_surface_damage(win, 0, 0, 400, 300);
    wl_surface_commit(win);                     /* the parent's commit applies the order */
    if (wl_display_roundtrip(dpy) < 0) { fprintf(stderr, "protocol error\n"); return 1; }
    printf("wlproof: mapped (red over green, blue under the window, a cropped and scaled viewport)\n");
    fflush(stdout);
    for (int i = 0; i < secs * 10; i++) {
        if (wl_display_roundtrip(dpy) < 0) { fprintf(stderr, "protocol error\n"); return 1; }
        usleep(100000);
    }
    wl_display_disconnect(dpy);
    return 0;
}
