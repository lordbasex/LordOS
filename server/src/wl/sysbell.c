/* sysbell.c — xdg-system-bell-v1: an app rings the bell (docs/WAYLAND.md, "The bell").
 *
 * ring(surface) becomes a "bell" event for the shell, on the window the surface belongs to (its toplevel), or on
 * window 0 when the app names none. sdk/wayland.js plays LordOS's own alert sound (lord.sound.play, synthesized by
 * lord-server: it goes through PipeWire as an event sound, so the system volume and mute apply) unless Do Not
 * Disturb is on, and marks a window that isn't in front as wanting attention (the visual bell: the Dock and the
 * taskbar show it, as for xdg-activation). How often an app may ring is limited here (bellgate.h): a ring held back
 * is dropped. */
#include "wlshell.h"
#include "bellgate.h"
#include "xdg-system-bell-v1-protocol.h"

#include <time.h>
#include <wayland-server-core.h>

static BellGate gate;

static uint32_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static void bell_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void bell_ring(struct wl_client *c, struct wl_resource *r, struct wl_resource *surface) {
    (void)r;
    if (!bell_pass(&gate, c, now_ms())) return;
    int win = surface ? wl__window_of(wl__surface_id(surface)) : 0;
    wl__emit(win, "bell", "{}");
}
static const struct xdg_system_bell_v1_interface bell_impl = { .destroy = bell_destroy, .ring = bell_ring };
static void bind_bell(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &xdg_system_bell_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &bell_impl, NULL, NULL);
}

void sysbell_globals(struct wl_display *dpy) {
    wl_global_create(dpy, &xdg_system_bell_v1_interface, 1, NULL, bind_bell);
}
