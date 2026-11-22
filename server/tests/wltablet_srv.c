/* wltablet_srv.c — the compositor side of tests/wltablet_test.c: a Wayland display with wl_compositor, wl_seat (a
 * pointer only), src/wl/wl_tablet.c and src/wl/wl_warp.c as lord-server has them, and wl.c's few accessors stood in for
 * by a scene: one surface of the test's client at (100, 50), 400 × 300 on screen. Only the server headers here; the
 * client is in wltablet_test.c (the two sets of headers don't mix in one file). */
#include "wltablet_srv.h"
#include "../src/wl/wl_tablet.h"
#include "../src/wl/wl_warp.h"
#include "../src/wl/wl_input.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

static struct wl_display *dpy;
static struct wl_event_loop *loop;
static struct wl_resource *surfaces[4];
static int nsurfaces;
static struct wl_client *clients[2];
SrvScene scene;

/* ---- wl.c, as the scene says ---- */
struct wl_resource *wl__surface_at(float x, float y, float *lx, float *ly) {
    struct wl_resource *s = scene.under ? surfaces[scene.under - 1] : NULL;
    if (!s || x < 100 || y < 50 || x >= 500 || y >= 350) return NULL;
    *lx = x - 100;
    *ly = y - 50;
    return s;
}
int wl__surface_box(struct wl_resource *surface, float *x, float *y, float *w, float *h) {
    for (int i = 0; i < nsurfaces; i++)
        if (surfaces[i] == surface) { *x = 100; *y = 50; *w = 400; *h = 300; return 1; }
    return 0;
}
int wl__surface_held(struct wl_resource *surface) { (void)surface; return scene.held; }
int wl__same_window(struct wl_resource *a, struct wl_resource *b) { return a && a == b; }
struct wl_resource *wl__key_focus(void) { return scene.key ? surfaces[scene.key - 1] : NULL; }
struct wl_resource *wl__pointer_focus(void) { return scene.pointer ? surfaces[scene.pointer - 1] : NULL; }
uint32_t wl__pointer_enter_serial(void) { return scene.enter_serial; }
int wl__dragging(void) { return scene.dragging; }
int wl_pointer_locked(void) { return scene.locked; }
float wl__client_scale(struct wl_client *c) { (void)c; return scene.scale > 0 ? scene.scale : 1; }
void wl__tool_press(struct wl_resource *surface, float x, float y, int down, uint32_t serial) {
    (void)x; (void)y; (void)serial;
    if (down && surface) scene.presses++;
    if (!down) scene.releases++;
}
void wl__tool_cursor(struct wl_resource *surface, int32_t x, int32_t y) { scene.cursor_set++; scene.cursor_hx = x; scene.cursor_hy = y; (void)surface; }
void wl__pointer_leave(void) { scene.pointer_leaves++; }

static int warp_host(float x, float y) { scene.warps++; scene.warp_x = x; scene.warp_y = y; return 1; }

/* ---- wl_compositor and wl_seat, as little as the client uses ---- */
static void surf_gone(struct wl_resource *r) {
    for (int i = 0; i < nsurfaces; i++) if (surfaces[i] == r) surfaces[i] = NULL;
}
static void surf_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_surface_interface surface_impl = { .destroy = surf_destroy };
static void comp_surface(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *s = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s, &surface_impl, NULL, surf_gone);
    if (nsurfaces < 4) surfaces[nsurfaces++] = s;
}
static const struct wl_compositor_interface comp_impl = { .create_surface = comp_surface };
static void bind_comp(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    (void)d;
    struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, (int)v, id);
    wl_resource_set_implementation(r, &comp_impl, NULL, NULL);
}
static void res_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_pointer_interface pointer_impl = { .release = res_release };
static void seat_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *p = wl_resource_create(c, &wl_pointer_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(p, &pointer_impl, NULL, NULL);
}
static const struct wl_seat_interface seat_impl = { .get_pointer = seat_pointer, .release = res_release };
static void bind_seat(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    (void)d;
    struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, (int)v, id);
    wl_resource_set_implementation(r, &seat_impl, NULL, NULL);
    wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER);
}

int srv_start(const int *fds, int n) {
    dpy = wl_display_create();
    if (!dpy) return -1;
    loop = wl_display_get_event_loop(dpy);
    wl_tablet_init();
    wl_global_create(dpy, &wl_compositor_interface, 4, NULL, bind_comp);
    wl_global_create(dpy, &wl_seat_interface, 5, NULL, bind_seat);
    wl_tablet_globals(dpy);
    wl_warp_globals(dpy);
    wl_warp_set_host(warp_host);
    for (int i = 0; i < n && i < 2; i++) clients[i] = wl_client_create(dpy, fds[i]);
    return 0;
}
void srv_dispatch(void) {
    wl_event_loop_dispatch(loop, 0);
    wl_display_flush_clients(dpy);
}
void srv_flush(void) { wl_display_flush_clients(dpy); }
int srv_surfaces(void) { return nsurfaces; }
void srv_key_focus(int i) { wl_tablet_key_focus(i ? surfaces[i - 1] : NULL); }
void srv_stop(void) { wl_display_destroy_clients(dpy); wl_display_destroy(dpy); }
