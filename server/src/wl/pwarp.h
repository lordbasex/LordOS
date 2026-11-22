/* pwarp.h — whether a Wayland app may move the pointer (wp_pointer_warp_v1.warp_pointer, wl_warp.c). Pure, so
 * tests/pwarp_test.c checks every rule. See docs/WAYLAND.md, "Pointer warp".
 *
 * Only the app in front of the person, on its own window, from an event it really got:
 *   - the pointer is over one of its surfaces (it has pointer focus) and the request names that surface or another
 *     of the same window;
 *   - the serial is the one of the wl_pointer.enter it got for that focus (not a made-up or an old one);
 *   - it also has the keyboard: a window in the background never takes the pointer, even one under it;
 *   - the point is inside the surface (0 ≤ x < width, 0 ≤ y < height) and a finite number;
 *   - the pointer isn't locked (pointer-constraints: the app asked for it to stay; it can unlock), nor in a drag, a
 *     window move or LordOS's own pointer lock (Machines' capture).
 * A confinement still holds: main.c clamps the new position to it. A refused request is ignored (the protocol has no
 * error for it). */
#ifndef LORD_WL_PWARP_H
#define LORD_WL_PWARP_H

#include <math.h>

typedef struct {
    int focus;                  /* the app has the pointer on one of its surfaces */
    int same_window;            /* the surface it names is that surface or of the same window */
    int serial_ok;              /* the serial is the enter serial of that focus */
    int keyboard;               /* the app has the keyboard */
    int locked;                 /* a pointer lock is active (pointer-constraints) */
    int busy;                   /* a drag, a window move, or LordOS's own pointer lock */
    double x, y, w, h;          /* the point, and the surface's size, in its own coordinates */
} PwarpReq;

enum { PWARP_OK = 0, PWARP_NO_FOCUS, PWARP_OTHER_SURFACE, PWARP_SERIAL, PWARP_NO_KEYBOARD, PWARP_OUTSIDE, PWARP_LOCKED, PWARP_BUSY };

static inline int pwarp_decide(const PwarpReq *r) {
    if (!r->focus) return PWARP_NO_FOCUS;
    if (!r->same_window) return PWARP_OTHER_SURFACE;
    if (!r->serial_ok) return PWARP_SERIAL;
    if (!r->keyboard) return PWARP_NO_KEYBOARD;
    if (!isfinite(r->x) || !isfinite(r->y) || r->x < 0 || r->y < 0 || r->x >= r->w || r->y >= r->h) return PWARP_OUTSIDE;
    if (r->locked) return PWARP_LOCKED;
    if (r->busy) return PWARP_BUSY;
    return PWARP_OK;
}

#endif
