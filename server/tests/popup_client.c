/* popup_client.c — a popup opened twice on the same wl_surface (GitHub #159), as GTK 4 does with a popover: the
 * wl_surface stays, each opening gets a new xdg_surface and xdg_popup. Every opening's first commit must be
 * answered with xdg_popup.configure and xdg_surface.configure; the second one wasn't and a GTK 4 app (a dropdown)
 * froze waiting for it. A toplevel made again on its old wl_surface is checked the same way.
 *
 *   WAYLAND_DISPLAY=wayland-0 build/popup-client      (inside a session, or against `lord-server --headless`;
 *                                                       prints ok/FAIL per check, exit 1 on a failure) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wmbase;
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
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };
static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };

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
    int fd = memfd_create("popup-client", MFD_CLOEXEC);
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

/* a toplevel on surf: configured, acked, drawn */
static struct xdg_toplevel *toplevel(struct wl_surface *surf, struct xdg_surface **xs_out, Seen *seen, const char *what) {
    memset(seen, 0, sizeof *seen);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wmbase, surf);
    xdg_surface_add_listener(xs, &xs_l, seen);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(top, &top_l, seen);
    xdg_toplevel_set_app_id(top, "popup-client");
    wl_surface_commit(surf);
    wl_display_roundtrip(dpy);
    check(what, seen->surface_conf > 0, 1);
    if (seen->surface_conf) xdg_surface_ack_configure(xs, seen->serial);
    wl_surface_attach(surf, buffer(400, 300, 0xffd0d0d0u), 0, 0);
    wl_surface_damage(surf, 0, 0, 400, 300);
    wl_surface_commit(surf);
    wl_display_roundtrip(dpy);
    *xs_out = xs;
    return top;
}

/* one opening of the popup on pop_surf, as GTK 4 does it; closed again afterwards */
static void open_popup(struct wl_surface *pop_surf, struct xdg_surface *parent, int n) {
    Seen seen = { 0 };
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(wmbase);
    xdg_positioner_set_size(pos, 200, 58);
    xdg_positioner_set_anchor_rect(pos, 20, 40, 200, 34);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wmbase, pop_surf);
    xdg_surface_add_listener(xs, &xs_l, &seen);
    struct xdg_popup *pop = xdg_surface_get_popup(xs, parent, pos);
    xdg_popup_add_listener(pop, &pop_l, &seen);
    xdg_positioner_destroy(pos);
    wl_surface_commit(pop_surf);              /* the initial commit, no buffer */
    wl_display_roundtrip(dpy);
    char what[96];
    snprintf(what, sizeof what, "opening %d: xdg_popup.configure", n);
    check(what, seen.popup_conf, 1);
    snprintf(what, sizeof what, "opening %d: xdg_surface.configure", n);
    check(what, seen.surface_conf, 1);
    if (seen.surface_conf) {
        xdg_surface_ack_configure(xs, seen.serial);
        wl_surface_attach(pop_surf, buffer(200, 58, 0xff2a6fd6u), 0, 0);
        wl_surface_damage(pop_surf, 0, 0, 200, 58);
        wl_surface_commit(pop_surf);
        wl_display_roundtrip(dpy);
    }
    /* closed: the role object and the xdg_surface go, the wl_surface stays (its buffer detached first) */
    xdg_popup_destroy(pop);
    xdg_surface_destroy(xs);
    wl_surface_attach(pop_surf, NULL, 0, 0);
    wl_surface_commit(pop_surf);
    wl_display_roundtrip(dpy);
    snprintf(what, sizeof what, "opening %d: the connection is still up", n);
    check(what, wl_display_get_error(dpy), 0);
    const struct wl_interface *iface = NULL;
    uint32_t id = 0, code = wl_display_get_error(dpy) ? wl_display_get_protocol_error(dpy, &iface, &id) : 0;
    if (iface) printf("     protocol error %u on %s@%u\n", code, iface->name, id);
}

int main(void) {
    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "popup-client: no Wayland display\n"); return 2; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wmbase) { fprintf(stderr, "popup-client: no compositor, shm or xdg_wm_base\n"); return 2; }
    xdg_wm_base_add_listener(wmbase, &wm_l, NULL);

    Seen ts;
    struct xdg_surface *main_xs;
    struct wl_surface *main_surf = wl_compositor_create_surface(comp);
    struct xdg_toplevel *top = toplevel(main_surf, &main_xs, &ts, "the window is configured");

    struct wl_surface *pop_surf = wl_compositor_create_surface(comp);   /* one wl_surface for every opening */
    for (int i = 1; i <= 3; i++) open_popup(pop_surf, main_xs, i);

    /* a toplevel given up and made again on the same wl_surface is configured again too */
    xdg_toplevel_destroy(top);
    xdg_surface_destroy(main_xs);
    wl_surface_attach(main_surf, NULL, 0, 0);
    wl_surface_commit(main_surf);
    wl_display_roundtrip(dpy);
    top = toplevel(main_surf, &main_xs, &ts, "the window made again is configured");
    open_popup(pop_surf, main_xs, 4);

    xdg_toplevel_destroy(top);
    xdg_surface_destroy(main_xs);
    wl_surface_destroy(pop_surf);
    wl_surface_destroy(main_surf);
    wl_display_roundtrip(dpy);
    check("still up at the end", wl_display_get_error(dpy), 0);
    printf("%d checks, %d failed\n", checks, fails);
    wl_display_disconnect(dpy);
    return fails ? 1 : 0;
}
