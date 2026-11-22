/* wl_warp.c — wp_pointer_warp_v1: see wl_warp.h, and pwarp.h for who may move the pointer. */
#include "wl_warp.h"
#include "pwarp.h"
#include "wl.h"
#include "wl_input.h"
#include "wl_tablet.h"

#include <stdio.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include "pointer-warp-v1-protocol.h"

static int (*host_move)(float x, float y);
void wl_warp_set_host(int (*move)(float x, float y)) { host_move = move; }

static void warp_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }

static void warp_pointer(struct wl_client *c, struct wl_resource *r, struct wl_resource *surface, struct wl_resource *pointer,
                         wl_fixed_t fx, wl_fixed_t fy, uint32_t serial) {
    (void)r; (void)pointer;
    struct wl_resource *focus = wl__pointer_focus(), *keys = wl__key_focus();
    PwarpReq q = { 0 };
    q.focus = focus && wl_resource_get_client(focus) == c;
    q.same_window = q.focus && (surface == focus || wl__same_window(surface, focus));
    q.serial_ok = serial != 0 && serial == wl__pointer_enter_serial();
    q.keyboard = keys && wl_resource_get_client(keys) == c;
    float ox = 0, oy = 0, w = 0, h = 0;
    wl__surface_box(surface, &ox, &oy, &w, &h);       /* not shown: 0 × 0, so outside */
    float k = wl__client_scale(c);                     /* a scaled Xwayland speaks X pixels */
    q.x = wl_fixed_to_double(fx) / k;
    q.y = wl_fixed_to_double(fy) / k;
    q.w = w;
    q.h = h;
    q.locked = wl_pointer_locked();
    q.busy = wl__dragging();
    int why = pwarp_decide(&q);
    if (why == PWARP_OK && (!host_move || !host_move(ox + (float)q.x, oy + (float)q.y))) why = PWARP_BUSY;
    if (getenv("LORD_WL_DEBUG"))
        fprintf(stderr, "wayland: pointer warp to %.1f,%.1f: %s (%d)\n", q.x, q.y, why == PWARP_OK ? "done" : "refused", why);
}

static const struct wp_pointer_warp_v1_interface warp_impl = { .destroy = warp_destroy, .warp_pointer = warp_pointer };
static void bind_warp(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wp_pointer_warp_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &warp_impl, NULL, NULL);
}

void wl_warp_globals(struct wl_display *dpy) { wl_global_create(dpy, &wp_pointer_warp_v1_interface, 1, NULL, bind_warp); }
