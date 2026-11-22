/* cycle_client.c — surfaces made children of themselves or of their own descendants. wayland.xml forbids it
 * (wl_subcompositor.bad_parent); lord-server used to accept it, and every walk of the surface tree (damage_node,
 * origin, paint_tree, surface_at) then looped or recursed until the stack ran out. Each case runs on a connection
 * of its own: the request must end it with the right protocol error, and lord-server must still answer afterwards.
 *
 *   WAYLAND_DISPLAY=wayland-0 build/cycle-client      (against `lord-server --headless`: tests/popup_headless.sh
 *                                                       cycle-client; prints ok/FAIL per check, exit 1 on a failure) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "client/xdg-shell-client-protocol.h"

typedef struct {
    struct wl_display *dpy;
    struct wl_compositor *comp;
    struct wl_subcompositor *subc;
    struct wl_shm *shm;
    struct xdg_wm_base *wm;
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
    else if (!strcmp(iface, wl_subcompositor_interface.name)) c->subc = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
    else if (!strcmp(iface, wl_shm_interface.name)) c->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) c->wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 5 ? ver : 5);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };
static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { ping };
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) { (void)d; xdg_surface_ack_configure(xs, serial); }
static const struct xdg_surface_listener xs_l = { xs_configure };

static int connect_to(Conn *c) {
    memset(c, 0, sizeof *c);
    c->dpy = wl_display_connect(NULL);
    if (!c->dpy) return -1;
    struct wl_registry *reg = wl_display_get_registry(c->dpy);
    wl_registry_add_listener(reg, &reg_l, c);
    wl_display_roundtrip(c->dpy);
    if (!c->comp || !c->subc || !c->shm || !c->wm) return -1;
    xdg_wm_base_add_listener(c->wm, &wm_l, NULL);
    return 0;
}

static struct wl_buffer *buffer(Conn *c, int W, int H, uint32_t color) {
    int stride = W * 4, size = stride * H;
    int fd = memfd_create("cycle-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { perror("memfd"); exit(2); }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); exit(2); }
    for (int i = 0; i < W * H; i++) px[i] = color;
    struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, W, H, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    munmap(px, (size_t)size);
    return b;
}

/* a drawn surface (commits that damage it walk its tree) */
static void draw(Conn *c, struct wl_surface *s, uint32_t color) {
    wl_surface_attach(s, buffer(c, 64, 48, color), 0, 0);
    wl_surface_damage(s, 0, 0, 64, 48);
    wl_surface_commit(s);
}

/* a mapped window: the root the subsurfaces hang from */
static struct wl_surface *window(Conn *c) {
    struct wl_surface *s = wl_compositor_create_surface(c->comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(c->wm, s);
    xdg_surface_add_listener(xs, &xs_l, NULL);
    struct xdg_toplevel *t = xdg_surface_get_toplevel(xs);
    xdg_toplevel_set_app_id(t, "cycle-client");
    wl_surface_commit(s);
    wl_display_roundtrip(c->dpy);
    draw(c, s, 0xffd0d0d0u);
    wl_display_roundtrip(c->dpy);
    return s;
}

/* the connection ended with code on an object of iface */
static void expect_error(Conn *c, const char *what, const struct wl_interface *want_iface, uint32_t want_code) {
    wl_display_roundtrip(c->dpy);
    const struct wl_interface *iface = NULL;
    uint32_t id = 0, code = wl_display_get_error(c->dpy) ? wl_display_get_protocol_error(c->dpy, &iface, &id) : 0;
    char w[160];
    snprintf(w, sizeof w, "%s: protocol error on %s", what, want_iface->name);
    check(w, iface == want_iface, 1);
    snprintf(w, sizeof w, "%s: error code", what);
    check(w, (long)code, (long)want_code);
    wl_display_disconnect(c->dpy);
}

/* lord-server still answers: a new connection gets a window configured */
static void alive(const char *after) {
    Conn c;
    char w[160];
    snprintf(w, sizeof w, "lord-server answers after %s", after);
    if (connect_to(&c)) { check(w, 0, 1); return; }
    window(&c);
    check(w, wl_display_get_error(c.dpy), 0);
    wl_display_disconnect(c.dpy);
}

int main(void) {
    Conn c;
    if (connect_to(&c)) { fprintf(stderr, "cycle-client: no Wayland display with wl_subcompositor, shm and xdg_wm_base\n"); return 2; }
    wl_display_disconnect(c.dpy);

    /* 1. a subsurface of itself */
    connect_to(&c);
    {
        struct wl_surface *s = wl_compositor_create_surface(c.comp);
        wl_subcompositor_get_subsurface(c.subc, s, s);
        expect_error(&c, "a subsurface of itself", &wl_subcompositor_interface, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT);
    }
    alive("a subsurface of itself");

    /* 2. a window's subsurface a made the child of its own child b (its own subsurface object destroyed first, so
     *    it has no parent at that moment): a → b → a */
    connect_to(&c);
    {
        struct wl_surface *w = window(&c), *a = wl_compositor_create_surface(c.comp), *b = wl_compositor_create_surface(c.comp);
        struct wl_subsurface *sa = wl_subcompositor_get_subsurface(c.subc, a, w);
        wl_subcompositor_get_subsurface(c.subc, b, a);
        draw(&c, b, 0xff2a6fd6u);
        draw(&c, a, 0xffd62a6fu);
        wl_surface_commit(w);
        wl_display_roundtrip(c.dpy);
        wl_subsurface_destroy(sa);
        wl_subcompositor_get_subsurface(c.subc, a, b);
        draw(&c, a, 0xff6fd62au);
        draw(&c, b, 0xff2a6fd6u);
        expect_error(&c, "a subsurface under its own child", &wl_subcompositor_interface, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT);
    }
    alive("a subsurface under its own child");

    /* 3. three levels: a → b → c, then a under c */
    connect_to(&c);
    {
        struct wl_surface *a = wl_compositor_create_surface(c.comp), *b = wl_compositor_create_surface(c.comp);
        struct wl_surface *d = wl_compositor_create_surface(c.comp);
        wl_subcompositor_get_subsurface(c.subc, b, a);
        wl_subcompositor_get_subsurface(c.subc, d, b);
        wl_subcompositor_get_subsurface(c.subc, a, d);
        draw(&c, a, 0xff6fd62au);
        expect_error(&c, "a subsurface under its grandchild", &wl_subcompositor_interface, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT);
    }
    alive("a subsurface under its grandchild");

    /* 4. a popup whose parent is its own xdg_surface */
    connect_to(&c);
    {
        struct wl_surface *s = wl_compositor_create_surface(c.comp);
        struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(c.wm, s);
        struct xdg_positioner *pos = xdg_wm_base_create_positioner(c.wm);
        xdg_positioner_set_size(pos, 100, 40);
        xdg_positioner_set_anchor_rect(pos, 0, 0, 10, 10);
        xdg_surface_get_popup(xs, xs, pos);
        wl_surface_commit(s);
        expect_error(&c, "a popup of itself", &xdg_surface_interface, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT);
    }
    alive("a popup of itself");

    /* 5. two popups, each the other's parent */
    connect_to(&c);
    {
        struct wl_surface *s1 = wl_compositor_create_surface(c.comp), *s2 = wl_compositor_create_surface(c.comp);
        struct xdg_surface *x1 = xdg_wm_base_get_xdg_surface(c.wm, s1), *x2 = xdg_wm_base_get_xdg_surface(c.wm, s2);
        struct xdg_positioner *pos = xdg_wm_base_create_positioner(c.wm);
        xdg_positioner_set_size(pos, 100, 40);
        xdg_positioner_set_anchor_rect(pos, 0, 0, 10, 10);
        xdg_surface_get_popup(x2, x1, pos);
        xdg_surface_get_popup(x1, x2, pos);
        wl_surface_commit(s1);
        wl_surface_commit(s2);
        expect_error(&c, "two popups, each the other's parent", &xdg_surface_interface, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT);
    }
    alive("two popups, each the other's parent");

    /* 6. what stays allowed: a window with a subsurface that has a subsurface, all drawn */
    connect_to(&c);
    {
        struct wl_surface *w = window(&c), *a = wl_compositor_create_surface(c.comp), *b = wl_compositor_create_surface(c.comp);
        struct wl_subsurface *sa = wl_subcompositor_get_subsurface(c.subc, a, w), *sb = wl_subcompositor_get_subsurface(c.subc, b, a);
        wl_subsurface_set_position(sa, 10, 10);
        wl_subsurface_set_position(sb, 5, 5);
        wl_subsurface_set_desync(sb);
        draw(&c, b, 0xff2a6fd6u);
        draw(&c, a, 0xffd62a6fu);
        wl_surface_commit(w);
        wl_display_roundtrip(c.dpy);
        check("a subsurface of a subsurface is still fine", wl_display_get_error(c.dpy), 0);
        wl_display_disconnect(c.dpy);
    }
    alive("the last case");

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
