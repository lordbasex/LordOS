/* xwshell.c — xwayland-shell-v1: which wl_surface shows which X window, without WL_SURFACE_ID's race.
 *
 * Xwayland 23.1 and newer bind xwayland_shell_v1 when it is offered, give each window's surface the xwayland_surface
 * role and set a serial on it (double-buffered: it counts at the surface's commit); the same serial reaches the
 * window manager as a WL_SURFACE_SERIAL client message on the X window (xwm.c). Whichever comes second makes the
 * match. A serial names one surface for good, so a recycled surface id can never be mistaken for another window's.
 *
 * The global is for Xwayland only: hidden from every other client's registry (secctx_set_gate), and a bind by any
 * other client is refused with an error. Errors, as the protocol names them: role (the surface has a role, or isn't
 * Xwayland's), already_associated (a second serial committed on one surface), invalid_serial (0). */
#include "wlshell.h"
#include "xwm.h"
#include "secctx.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "xwayland-shell-v1-protocol.h"

/* One per surface that got the role; it lives as long as the surface (an association outlives its object). */
typedef struct {
    struct wl_resource *res;            /* the xwayland_surface_v1 (NULL once destroyed) */
    struct wl_listener gone;            /* the wl_surface */
    int sid;
    uint64_t pending, serial;           /* set_serial before / after its commit (0: none) */
    struct wl_list link;
} XwSurf;

static struct wl_list surfs;

static void surf_free(XwSurf *x) {
    if (x->res) wl_resource_set_user_data(x->res, NULL);   /* inert from now */
    wl_list_remove(&x->gone.link);
    wl_list_remove(&x->link);
    free(x);
}
static void surface_gone(struct wl_listener *l, void *data) {
    (void)data;
    XwSurf *x = wl_container_of(l, x, gone);
    surf_free(x);
}

static void xs_serial(struct wl_client *c, struct wl_resource *r, uint32_t lo, uint32_t hi) {
    (void)c;
    XwSurf *x = wl_resource_get_user_data(r);
    uint64_t serial = (uint64_t)hi << 32 | lo;
    if (!serial) {
        wl_resource_post_error(r, XWAYLAND_SURFACE_V1_ERROR_INVALID_SERIAL, "the serial must not be 0");
        return;
    }
    if (x) x->pending = serial;
}
static void xs_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void xs_gone(struct wl_resource *r) {
    XwSurf *x = wl_resource_get_user_data(r);
    if (x) x->res = NULL;
}
static const struct xwayland_surface_v1_interface xs_impl = { .set_serial = xs_serial, .destroy = xs_destroy };

static void sh_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface) {
    int sid = wl__surface_id(surface);
    XwSurf *x;
    wl_list_for_each(x, &surfs, link) if (x->sid == sid) break;
    if (&x->link != &surfs || !sid || !wlx_surface_free(surface)) {
        wl_resource_post_error(r, XWAYLAND_SHELL_V1_ERROR_ROLE, "the surface already has a role");
        return;
    }
    struct wl_resource *xr = wl_resource_create(c, &xwayland_surface_v1_interface, wl_resource_get_version(r), id);
    x = xr ? calloc(1, sizeof *x) : NULL;
    if (!x) { if (xr) wl_resource_destroy(xr); wl_client_post_no_memory(c); return; }
    x->res = xr;
    x->sid = sid;
    x->gone.notify = surface_gone;
    wl_resource_add_destroy_listener(surface, &x->gone);
    wl_list_insert(&surfs, &x->link);
    wl_resource_set_implementation(xr, &xs_impl, x, xs_gone);
}
static void sh_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct xwayland_shell_v1_interface sh_impl = { .destroy = sh_destroy, .get_xwayland_surface = sh_get };

static void bind_shell(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &xwayland_shell_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    if (c != wlx_client()) {            /* "an implementation-defined error": only the Xwayland lord-server started */
        wl_resource_post_error(r, WL_DISPLAY_ERROR_INVALID_OBJECT, "xwayland_shell_v1 is only for Xwayland");
        return;
    }
    wl_resource_set_implementation(r, &sh_impl, NULL, NULL);
}

static int gate(struct wl_client *c, const char *iface) {
    return strcmp(iface, xwayland_shell_v1_interface.name) || (c && c == wlx_client());
}

void xwshell_globals(struct wl_display *dpy) {
    wl_list_init(&surfs);
    wl_global_create(dpy, &xwayland_shell_v1_interface, 1, NULL, bind_shell);
    secctx_set_gate(gate);
}

void xwshell_commit(int id) {
    XwSurf *x;
    wl_list_for_each(x, &surfs, link) {
        if (x->sid != id) continue;
        if (!x->pending) return;
        uint64_t serial = x->pending;
        x->pending = 0;
        if (x->serial) {
            if (x->res) wl_resource_post_error(x->res, XWAYLAND_SURFACE_V1_ERROR_ALREADY_ASSOCIATED, "the surface already has a serial");
            return;
        }
        x->serial = serial;
        xwm_surface_serial(id, serial);     /* the window that sent it, if its message came first */
        return;
    }
}

int xwshell_surface_for(uint64_t serial) {
    XwSurf *x;
    if (!serial || !surfs.next) return 0;
    wl_list_for_each(x, &surfs, link) if (x->serial == serial) return x->sid;
    return 0;
}
