/* wl_input.c — what the touchpad and the touchscreen give Wayland apps:
 *
 * - zwp_pointer_gestures_v1 (v3): pinch (Chromium, GTK, Firefox zoom), swipe and hold
 *   (GTK stops its inertia when the fingers rest);
 * - fine scrolling: axis with axis_source finger and axis_stop when the fingers lift (the app does its
 *   own inertia), axis_relative_direction with natural scrolling, and axis_value120 for
 *   the wheel (wl_seat 8+);
 * - wl_touch: direct touches to the surface underneath.
 *
 * Gestures go to the app that had the pointer when they started (even if the pointer leaves it). */
#include "wl_input.h"
#include "../tp/tp.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "pointer-gestures-unstable-v1-protocol.h"

static struct wl_list swipes, pinches, holds, touches, seats;
static int ready;

static uint32_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static uint32_t serial(void) { return wl_display_next_serial(wl__display()); }

#define FOR_CLIENT(list, client, r) wl_resource_for_each(r, list) if (wl_resource_get_client(r) == (client))

static void link_gone(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }
static void res_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }

int wl_input_has_focus(void) { return ready && wl__pointer_focus() != NULL; }

static void pointer_frames(struct wl_client *c) {
    struct wl_resource *p;
    FOR_CLIENT(wl__pointers(), c, p) if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
}

/* ---- scroll ---- */

void wl_input_axis(int source, double dx, double dy, int stop, int natural) {
    struct wl_resource *focus = ready ? wl__pointer_focus() : NULL;
    if (!focus) return;
    struct wl_client *c = wl_resource_get_client(focus);
    uint32_t t = now_ms();
    uint32_t src = source == WLI_FINGER ? WL_POINTER_AXIS_SOURCE_FINGER : source == WLI_WHEEL ? WL_POINTER_AXIS_SOURCE_WHEEL
                 : WL_POINTER_AXIS_SOURCE_CONTINUOUS;
    struct wl_resource *p;
    FOR_CLIENT(wl__pointers(), c, p) {
        int v = wl_resource_get_version(p);
        if (v >= 5) wl_pointer_send_axis_source(p, src);
        for (int axis = 0; axis < 2; axis++) {
            double val = axis ? dx : dy;
            uint32_t a = axis ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL;
            if (stop) { if (v >= 5) wl_pointer_send_axis_stop(p, t, a); continue; }
            if (val == 0) continue;
#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
            if (v >= 9) wl_pointer_send_axis_relative_direction(p, a, natural ? WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED
                                                                            : WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
#else
            (void)natural;
#endif
            wl_pointer_send_axis(p, t, a, wl_fixed_from_double(val));
        }
        if (v >= 5) wl_pointer_send_frame(p);
    }
}

void wl_input_wheel(struct wl_resource *p, uint32_t t, int steps) {
    if (!steps) return;
    int v = wl_resource_get_version(p);
    if (v >= 5) wl_pointer_send_axis_source(p, WL_POINTER_AXIS_SOURCE_WHEEL);
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
    if (v >= 8) wl_pointer_send_axis_value120(p, WL_POINTER_AXIS_VERTICAL_SCROLL, -steps * 120);
    else
#endif
    if (v >= 5) wl_pointer_send_axis_discrete(p, WL_POINTER_AXIS_VERTICAL_SCROLL, -steps);
    wl_pointer_send_axis(p, t, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(-steps * 10));
}

void wl_input_wheel120(int v120, int h120) {
    struct wl_resource *focus = ready ? wl__pointer_focus() : NULL;
    if (!focus) return;
    static int rest[2];                 /* old clients (v5–7): whole steps with axis_discrete */
    uint32_t t = now_ms();
    struct wl_resource *p;
    int vals[2] = { -v120, h120 };      /* axis: + = down / right */
    int disc[2];
    for (int a = 0; a < 2; a++) { rest[a] += vals[a]; disc[a] = rest[a] / 120; rest[a] -= disc[a] * 120; }
    FOR_CLIENT(wl__pointers(), wl_resource_get_client(focus), p) {
        int v = wl_resource_get_version(p);
        if (v >= 5) wl_pointer_send_axis_source(p, WL_POINTER_AXIS_SOURCE_WHEEL);
        for (int a = 0; a < 2; a++) {
            if (!vals[a]) continue;
            uint32_t axis = a ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL;
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
            if (v >= 8) wl_pointer_send_axis_value120(p, axis, vals[a]);
            else
#endif
            if (v >= 5 && disc[a]) wl_pointer_send_axis_discrete(p, axis, disc[a]);
            wl_pointer_send_axis(p, t, axis, wl_fixed_from_double(vals[a] / 12.0));
        }
        if (v >= 5) wl_pointer_send_frame(p);
    }
}

/* ---- gestures ---- */

static struct wl_client *g_client[3];      /* app receiving the gesture in progress: swipe, pinch, hold */
static double g_last_rot;

static struct wl_client *gesture_target(int kind, int phase) {
    if (phase == TP_BEGIN) {
        struct wl_resource *f = ready ? wl__pointer_focus() : NULL;
        g_client[kind] = f ? wl_resource_get_client(f) : NULL;
    }
    return g_client[kind];
}

void wl_input_swipe(int phase, int fingers, double dx, double dy) {
    struct wl_client *c = gesture_target(0, phase);
    if (!c) return;
    struct wl_resource *r, *focus = wl__pointer_focus();
    uint32_t t = now_ms();
    FOR_CLIENT(&swipes, c, r) {
        if (phase == TP_BEGIN) { if (focus) zwp_pointer_gesture_swipe_v1_send_begin(r, serial(), t, focus, (uint32_t)fingers); }
        else if (phase == TP_UPDATE) zwp_pointer_gesture_swipe_v1_send_update(r, t, wl_fixed_from_double(dx), wl_fixed_from_double(dy));
        else zwp_pointer_gesture_swipe_v1_send_end(r, serial(), t, phase == TP_CANCEL);
    }
    pointer_frames(c);
    if (phase == TP_END || phase == TP_CANCEL) g_client[0] = NULL;
}

void wl_input_pinch(int phase, int fingers, double dx, double dy, double scale, double rot) {
    struct wl_client *c = gesture_target(1, phase);
    if (!c) return;
    struct wl_resource *r, *focus = wl__pointer_focus();
    uint32_t t = now_ms();
    if (phase == TP_BEGIN) g_last_rot = 0;
    FOR_CLIENT(&pinches, c, r) {
        if (phase == TP_BEGIN) { if (focus) zwp_pointer_gesture_pinch_v1_send_begin(r, serial(), t, focus, (uint32_t)fingers); }
        else if (phase == TP_UPDATE)
            zwp_pointer_gesture_pinch_v1_send_update(r, t, wl_fixed_from_double(dx), wl_fixed_from_double(dy),
                                                     wl_fixed_from_double(scale), wl_fixed_from_double(rot));
        else zwp_pointer_gesture_pinch_v1_send_end(r, serial(), t, phase == TP_CANCEL);
    }
    pointer_frames(c);
    if (phase == TP_END || phase == TP_CANCEL) g_client[1] = NULL;
}

void wl_input_hold(int phase, int fingers) {
    struct wl_client *c = gesture_target(2, phase);
    if (!c) return;
    struct wl_resource *r, *focus = wl__pointer_focus();
    uint32_t t = now_ms();
    FOR_CLIENT(&holds, c, r) {
        if (phase == TP_BEGIN) { if (focus) zwp_pointer_gesture_hold_v1_send_begin(r, serial(), t, focus, (uint32_t)fingers); }
        else zwp_pointer_gesture_hold_v1_send_end(r, serial(), t, phase == TP_CANCEL);
    }
    pointer_frames(c);
    if (phase != TP_BEGIN) g_client[2] = NULL;
}

static const struct zwp_pointer_gesture_swipe_v1_interface swipe_impl = { .destroy = res_destroy };
static const struct zwp_pointer_gesture_pinch_v1_interface pinch_impl = { .destroy = res_destroy };
static const struct zwp_pointer_gesture_hold_v1_interface hold_impl = { .destroy = res_destroy };

static void make(struct wl_client *c, struct wl_resource *mgr, uint32_t id, const struct wl_interface *iface,
                 const void *impl, struct wl_list *list) {
    struct wl_resource *r = wl_resource_create(c, iface, wl_resource_get_version(mgr), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, impl, NULL, link_gone);
    wl_list_insert(list, wl_resource_get_link(r));
}
static void pg_swipe(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *ptr) {
    (void)ptr; make(c, r, id, &zwp_pointer_gesture_swipe_v1_interface, &swipe_impl, &swipes);
}
static void pg_pinch(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *ptr) {
    (void)ptr; make(c, r, id, &zwp_pointer_gesture_pinch_v1_interface, &pinch_impl, &pinches);
}
static void pg_hold(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *ptr) {
    (void)ptr; make(c, r, id, &zwp_pointer_gesture_hold_v1_interface, &hold_impl, &holds);
}
static const struct zwp_pointer_gestures_v1_interface pg_impl = {
    .get_swipe_gesture = pg_swipe, .get_pinch_gesture = pg_pinch, .release = res_destroy, .get_hold_gesture = pg_hold,
};
static void bind_pg(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &zwp_pointer_gestures_v1_interface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &pg_impl, NULL, NULL);
}

/* ---- wl_touch ---- */

#define MAX_TOUCH 10
static struct { int on; struct wl_client *client; double ox, oy; } tch[MAX_TOUCH];

static const struct wl_touch_interface touch_impl = { .release = res_destroy };

void wl_input_get_touch(struct wl_client *c, struct wl_resource *seat, uint32_t id) {
    struct wl_resource *t = wl_resource_create(c, &wl_touch_interface, wl_resource_get_version(seat), id);
    if (!t) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(t, &touch_impl, NULL, link_gone);
    wl_list_insert(&touches, wl_resource_get_link(t));
}

static int client_has_touch(struct wl_client *c) {
    struct wl_resource *t;
    FOR_CLIENT(&touches, c, t) return 1;
    return 0;
}

static struct wl_client *frame_clients[MAX_TOUCH];
static int nframe;
static void mark_frame(struct wl_client *c) {
    for (int i = 0; i < nframe; i++) if (frame_clients[i] == c) return;
    if (nframe < MAX_TOUCH) frame_clients[nframe++] = c;
}

int wl_input_touch(int phase, int id, double x, double y) {
    if (!ready || id < 0 || id >= MAX_TOUCH) return 0;
    uint32_t t = now_ms();
    struct wl_resource *r;
    if (phase == TP_BEGIN) {
        float lx, ly;
        struct wl_resource *s = wl__surface_at((float)x, (float)y, &lx, &ly);
        if (!s) return 0;
        struct wl_client *c = wl_resource_get_client(s);
        if (!client_has_touch(c)) return 0;
        tch[id].on = 1;
        tch[id].client = c;
        tch[id].ox = x - lx;
        tch[id].oy = y - ly;
        float k = wl__client_scale(c);          /* an X11 app's surface is in X pixels */
        FOR_CLIENT(&touches, c, r) wl_touch_send_down(r, serial(), t, s, id, wl_fixed_from_double(lx * k), wl_fixed_from_double(ly * k));
        mark_frame(c);
        return 1;
    }
    if (!tch[id].on) return 0;
    struct wl_client *c = tch[id].client;
    if (phase == TP_UPDATE) {
        double k = wl__client_scale(c);
        FOR_CLIENT(&touches, c, r) wl_touch_send_motion(r, t, id, wl_fixed_from_double((x - tch[id].ox) * k), wl_fixed_from_double((y - tch[id].oy) * k));
    } else {
        FOR_CLIENT(&touches, c, r) {
            if (phase == TP_CANCEL) wl_touch_send_cancel(r);
            else wl_touch_send_up(r, serial(), t, id);
        }
        tch[id].on = 0;
    }
    mark_frame(c);
    return 1;
}

void wl_input_touch_frame(void) {
    struct wl_resource *r;
    for (int i = 0; i < nframe; i++) FOR_CLIENT(&touches, frame_clients[i], r) wl_touch_send_frame(r);
    nframe = 0;
}

/* ---- wl_seat: capabilities (touch, if there is a touchscreen) ---- */

typedef struct { struct wl_listener l; struct wl_resource *seat; struct wl_list link; } SeatRef;

static void seat_gone(struct wl_listener *l, void *data) {
    (void)data;
    SeatRef *s = wl_container_of(l, s, l);
    wl_list_remove(&s->link);
    free(s);
}

static uint32_t caps(void) {
    return WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD | (tp_count(TP_KIND_TOUCHSCREEN) ? WL_SEAT_CAPABILITY_TOUCH : 0);
}

void wl_input_seat_caps(struct wl_resource *seat) {
    wl_seat_send_capabilities(seat, caps());
    SeatRef *s = calloc(1, sizeof *s);
    if (!s) return;
    s->seat = seat;
    s->l.notify = seat_gone;
    wl_resource_add_destroy_listener(seat, &s->l);
    wl_list_insert(&seats, &s->link);
}

/* another monitor with another input was lent: already open apps learn whether there is a touchscreen */
void wl_input_devices_changed(void) {
    if (!ready) return;
    SeatRef *s;
    wl_list_for_each(s, &seats, link) wl_seat_send_capabilities(s->seat, caps());
}

void wl_input_globals(struct wl_display *dpy) {
    wl_list_init(&swipes);
    wl_list_init(&pinches);
    wl_list_init(&holds);
    wl_list_init(&touches);
    wl_list_init(&seats);
    wl_global_create(dpy, &zwp_pointer_gestures_v1_interface, 3, NULL, bind_pg);
    ready = 1;
}
