/* tp_route.c — where each thing the touchpad produces goes:
 *
 * - motion and buttons: to the usual pointer (main.c), so they go through dispatch_pointer like a
 *   mouse (drags, menus, Wayland apps);
 * - scroll, pinch and swipe: to the Wayland app that has the pointer when the gesture starts
 *   (zwp_pointer_gestures, axis finger) or, if there is none, to the UI node under the pointer
 *   (__lordGesture → the SDK's onWheel / onPinch / onSwipe). The target stays fixed until the
 *   gesture ends;
 * - 3-finger swipe (or 4, depending on the settings) belongs to the desktop: it always goes to JS, which decides
 *   the theme's action;
 * - scroll inertia is only for the UI (Wayland apps do their own);
 * - touchscreen touches: to the Wayland app underneath (wl_touch) or an emulated pointer. */
#include "tp_route.h"
#include "tp.h"
#include "../wl/wl_input.h"
#include "../ui/js.h"
#include "../ui/ui.h"

#include <stddef.h>

static TpHost host;

static const char *phase_name(int ph) {
    switch (ph) {
    case TP_BEGIN: return "begin";
    case TP_UPDATE: return "update";
    case TP_END: return "end";
    case TP_CANCEL: return "cancel";
    case TP_MOMENTUM: return "momentum";
    case TP_MOMENTUM_END: return "momentum-end";
    }
    return "update";
}

static void where(double *x, double *y, int *w, int *h) {
    *x = *y = 0;
    *w = (int)ui.width;
    *h = (int)ui.height;
    if (host.position) host.position(x, y, w, h);
}

static int node_at(double x, double y) {
    Node *n = ui.root ? hit_test(ui.root, (float)x, (float)y) : NULL;
    return n ? n->id : 0;
}

/* fixed target of each gesture: 1 = Wayland app, 0 = UI (with its node) */
typedef struct { int active, wayland, node; } Latch;
static Latch l_scroll, l_pinch, l_swipe;

static void latch(Latch *l, double x, double y) {
    l->active = 1;
    l->wayland = wl_input_has_focus();
    l->node = l->wayland ? 0 : node_at(x, y);
}

void tp_route_scroll(int phase, double dx, double dy, int source) {
    double x, y;
    int w, h;
    where(&x, &y, &w, &h);
    if (phase == TP_MOMENTUM || phase == TP_MOMENTUM_END) {
        if (l_scroll.wayland) return;                   /* the app already got axis_stop: it does its own inertia */
        js_gesture("scroll", phase_name(phase), l_scroll.node, (float)x, (float)y, dx, dy, source, 0);
        return;
    }
    if (phase == TP_BEGIN || !l_scroll.active) latch(&l_scroll, x, y);
    if (l_scroll.wayland)
        wl_input_axis(source == TPR_FINGER ? WLI_FINGER : source == TPR_WHEEL ? WLI_WHEEL : WLI_CONTINUOUS, dx, dy,
                      phase == TP_END || phase == TP_CANCEL, tp_config()->natural);
    else js_gesture("scroll", phase_name(phase), l_scroll.node, (float)x, (float)y, dx, dy, source, 0);
    if (phase == TP_END || phase == TP_CANCEL) l_scroll.active = 0;
}

void tp_route_pinch(int phase, int fingers, double dx, double dy, double scale, double rotation) {
    static double last_rot;
    double x, y;
    int w, h;
    where(&x, &y, &w, &h);
    if (phase == TP_BEGIN || !l_pinch.active) { latch(&l_pinch, x, y); last_rot = 0; }
    if (l_pinch.wayland) {
        wl_input_pinch(phase == TP_BEGIN ? TP_BEGIN : phase, fingers, dx, dy, scale, rotation - last_rot);
        last_rot = rotation;
    } else js_gesture("pinch", phase_name(phase), l_pinch.node, (float)x, (float)y, scale, rotation, dx, dy);
    if (phase == TP_END || phase == TP_CANCEL) l_pinch.active = 0;
}

static int system_swipe(int fingers) {
    const TpConfig *c = tp_config();
    if (fingers == 3) return c->swipe3 && !c->three_drag;
    if (fingers == 4) return c->swipe4 || (c->three_drag && c->swipe3);   /* with three-finger drag, it moves to 4 */
    return 0;
}

static void route_swipe(int phase, int fingers, double dx, double dy) {
    static int sys;
    double x, y;
    int w, h;
    where(&x, &y, &w, &h);
    if (phase == TP_BEGIN || !l_swipe.active) { latch(&l_swipe, x, y); sys = system_swipe(fingers); }
    if (sys || !l_swipe.wayland)
        js_gesture("swipe", phase_name(phase), sys ? node_at(x, y) : l_swipe.node, (float)x, (float)y, dx, dy, fingers, sys);
    else wl_input_swipe(phase, fingers, dx, dy);
    if (phase == TP_END || phase == TP_CANCEL) l_swipe.active = 0;
}

/* ---- high-resolution and horizontal mouse wheels ---- */

static int acc_v, acc_h;
void tp_route_wheel120(int v120, int h120) { acc_v += v120; acc_h += h120; }

void tp_route_wheel_flush(void) {
    if (!acc_v && !acc_h) return;
    int v = acc_v, h = acc_h;
    acc_v = acc_h = 0;
    double x, y;
    int w, hh;
    where(&x, &y, &w, &hh);
    if (wl_input_has_focus()) { wl_input_wheel120(v, h); return; }
    /* a lone event: the target is the current node (no phase, like a wheel) */
    l_scroll.active = 0;
    js_gesture("scroll", "update", node_at(x, y), (float)x, (float)y, h * 48.0 / 120, -v * 48.0 / 120, TPR_WHEEL, 0);
}

/* ---- the driver's sink ---- */

static void s_motion(void *ud, double dx, double dy, double rx, double ry) { (void)ud; if (host.move) host.move(dx, dy, rx, ry); }
static void s_button(void *ud, int b, int down) { (void)ud; if (host.button) host.button(b, down); }
static void s_scroll(void *ud, int ph, double dx, double dy) { (void)ud; tp_route_scroll(ph, dx, dy, TPR_FINGER); }
static void s_pinch(void *ud, int ph, int n, double dx, double dy, double sc, double rot) { (void)ud; tp_route_pinch(ph, n, dx, dy, sc, rot); }
static void s_swipe(void *ud, int ph, int n, double dx, double dy) { (void)ud; route_swipe(ph, n, dx, dy); }
static void s_hold(void *ud, int ph, int n) { (void)ud; if (ph == TP_BEGIN ? wl_input_has_focus() : 1) wl_input_hold(ph, n); }
static int s_touch(void *ud, int ph, int id, double x, double y) {
    (void)ud;
    double px, py;
    int w, h;
    where(&px, &py, &w, &h);
    return wl_input_touch(ph, id, x * w, y * h);
}
static void s_touch_frame(void *ud) { (void)ud; wl_input_touch_frame(); }
static void s_touch_orient(void *ud, int id, double deg) { (void)ud; wl_input_touch_orientation(id, deg); }
static void s_warp(void *ud, double x, double y) {
    (void)ud;
    double px, py;
    int w, h;
    where(&px, &py, &w, &h);
    if (host.warp) host.warp(x * (w - 1), y * (h - 1));
}
static void s_frame(void *ud) { (void)ud; if (host.flush) host.flush(); }

static const TpSink sink = {
    .motion = s_motion, .button = s_button, .scroll = s_scroll, .pinch = s_pinch, .swipe = s_swipe, .hold = s_hold,
    .touch = s_touch, .touch_frame = s_touch_frame, .touch_orient = s_touch_orient, .warp = s_warp, .frame = s_frame,
};

void tp_route_init(const TpHost *h) {
    host = *h;
    tp_set_sink(&sink, NULL);
}
