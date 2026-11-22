/* wlfixes_client.c — wl_fixes (src/wl/wlfixes.c) as a client sees it. tests/wlperf_headless.sh runs it against
 * a headless lord-server ("fixes"); by hand inside a session: WAYLAND_DISPLAY=wayland-0 build/wlfixes-client.
 *
 *   1. wl_fixes is offered at version 1 and binds.
 *   2. A second registry gets every global; wl_fixes.destroy_registry destroys it. lord-server's wl_display.delete_id
 *      for its id is the proof the registry is gone there: without it libwayland-client keeps the id as a zombie
 *      and never hands it out again. So the id must come back for one of the next registries, and that registry
 *      (a new object at the old id) gets every global exactly once: nothing is left of the old one, and no later
 *      global reaches it.
 *   3. destroy_registry naming an object that isn't a registry is a display error (libwayland-server's own type
 *      check: invalid_method, "invalid arguments", which libwayland-client reports as EINVAL); lord-server lives on.
 *   4. wl_fixes.destroy, then a roundtrip with no error.
 * Exit 0 when all hold, 1 on a failure, 2 without a display. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>

#ifdef LORD_WLFIXES_VENDORED
#include "wl-fixes-client-protocol.h"
#endif

typedef struct { int n; uint32_t fixes_name, fixes_version; } Count;

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
    (void)r;
    Count *c = d;
    c->n++;
    if (!strcmp(iface, "wl_fixes")) { c->fixes_name = name; c->fixes_version = version; }
}
static void global_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener listener = { global, global_remove };

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

#define NEXT 6                          /* registries made after the destroyed one: its id must be among them */

int main(void) {
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "wlfixes-client: no Wayland display\n"); return 2; }

    /* 1. the global */
    Count c1 = { 0 };
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &listener, &c1);
    wl_display_roundtrip(dpy);
    CHECK(c1.fixes_name, "wl_fixes is not offered");
    if (!c1.fixes_name) return 1;
    CHECK(c1.fixes_version == 1, "wl_fixes is offered at version %u, not 1", c1.fixes_version);
    struct wl_fixes *fixes = wl_registry_bind(reg, c1.fixes_name, &wl_fixes_interface, 1);

    /* 2. a registry destroyed */
    Count c2 = { 0 };
    struct wl_registry *gone = wl_display_get_registry(dpy);
    wl_registry_add_listener(gone, &listener, &c2);
    wl_display_roundtrip(dpy);
    CHECK(c2.n == c1.n, "the second registry got %d globals, the first %d", c2.n, c1.n);
    uint32_t gone_id = wl_proxy_get_id((struct wl_proxy *)gone);
    wl_fixes_destroy_registry(fixes, gone);
    wl_registry_destroy(gone);
    CHECK(wl_display_roundtrip(dpy) >= 0, "destroy_registry: error %d", wl_display_get_error(dpy));
    Count cn[NEXT] = { 0 };
    struct wl_registry *next[NEXT];
    int reused = -1;
    for (int i = 0; i < NEXT; i++) {
        next[i] = wl_display_get_registry(dpy);
        wl_registry_add_listener(next[i], &listener, &cn[i]);
        if (wl_proxy_get_id((struct wl_proxy *)next[i]) == gone_id) reused = i;
    }
    CHECK(wl_display_roundtrip(dpy) >= 0, "registries after destroy_registry: error %d", wl_display_get_error(dpy));
    CHECK(reused >= 0, "the destroyed registry's id %u never came back: lord-server didn't delete it", gone_id);
    for (int i = 0; i < NEXT; i++)
        CHECK(cn[i].n == c1.n, "registry %d (id %u) got %d globals, not %d", i, wl_proxy_get_id((struct wl_proxy *)next[i]), cn[i].n, c1.n);
    printf("fixes: wl_fixes v%u; registry %u destroyed, its id back for registry %d of %d, %d globals each\n",
           c1.fixes_version, gone_id, reused, NEXT, reused >= 0 ? cn[reused].n : -1);
    for (int i = 0; i < NEXT; i++) wl_fixes_destroy_registry(fixes, next[i]), wl_registry_destroy(next[i]);

    /* 4. destroy */
    wl_fixes_destroy(fixes);
    CHECK(wl_display_roundtrip(dpy) >= 0, "wl_fixes.destroy: error %d", wl_display_get_error(dpy));
    wl_registry_destroy(reg);
    wl_display_disconnect(dpy);

    /* 3. not a registry: on a connection of its own, since the error ends it */
    dpy = wl_display_connect(NULL);
    if (!dpy) { printf("FAIL no second connection\n"); return 1; }
    Count c3 = { 0 };
    reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &listener, &c3);
    wl_display_roundtrip(dpy);
    fixes = wl_registry_bind(reg, c3.fixes_name, &wl_fixes_interface, 1);
    wl_fixes_destroy_registry(fixes, (struct wl_registry *)fixes);
    wl_display_roundtrip(dpy);
    /* a wl_display error (invalid_object, invalid_method) reaches the client as EINVAL; any other as EPROTO */
    int err = wl_display_get_error(dpy);
    CHECK(err == EINVAL, "destroy_registry on a wl_fixes: error %d (%s), not EINVAL", err, strerror(err));
    printf("fixes: destroy_registry on a wl_fixes is a display error (%s)\n", strerror(err));
    wl_display_disconnect(dpy);
    return fails ? 1 : 0;
}
