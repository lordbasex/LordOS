/* wlfixes.c — wl_fixes (wlfixes.h). Version 1: destroy and destroy_registry.
 *
 * destroy_registry destroys the client's wl_registry resource on our side. libwayland-server keeps every
 * registry in its display's list until the resource's destroy handler takes it out, and destroying the resource
 * also sends wl_display.delete_id for its id, so the client can reuse the id and no global or global_remove event
 * reaches that registry again. The argument is checked by libwayland itself: an object of another interface is
 * the display's invalid_object error before this code runs, and a client can only name its own objects.
 *
 * libwayland 1.24 ships wl_fixes_interface; on an older one (Debian 13: 1.23) mk/wlfixes.mk generates it from
 * the vendored third_party/wayland/wl-fixes.xml and defines LORD_WLFIXES_VENDORED. Tests: the "fixes" part of
 * tests/wlperf_headless.sh (tests/wlfixes_client.c). */
#include "wlfixes.h"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#ifdef LORD_WLFIXES_VENDORED
#include "wl-fixes-protocol.h"
#endif

#define WLFIXES_VERSION 1

static void fixes_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void fixes_destroy_registry(struct wl_client *c, struct wl_resource *r, struct wl_resource *registry)
{
    (void)c; (void)r;
    wl_resource_destroy(registry);
}

static const struct wl_fixes_interface fixes_impl = {
    .destroy = fixes_destroy,
    .destroy_registry = fixes_destroy_registry,
};

static void bind_fixes(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_fixes_interface, (int)version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &fixes_impl, NULL, NULL);
}

int wlfixes_init(struct wl_display *dpy)
{
    return wl_global_create(dpy, &wl_fixes_interface, WLFIXES_VERSION, NULL, bind_fixes) ? 0 : -1;
}
