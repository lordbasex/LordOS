/* wltiming.h — when a Linux app's held content update may apply: fifo-v1 and commit-timing-v1 (timing.c keeps the
 * protocol objects, wl.c holds and applies the updates; docs/WAYLAND.md, "Frame timing"). Pure, header only:
 * tests/wltiming_test.c checks it.
 *
 * A commit is one content update. One that may not apply yet (it waits for the fifo barrier, its target time, or with
 * explicit sync its acquire point) is held, and every later commit of that surface queues behind it in the surface's
 * one queue (wl.c, syncobj.h's SobjQueue): updates always apply in the order they came.
 * Each latch (the start of a frame lord-server is about to paint for the surface's display) and each frame done
 * applies the queue's head while it is ready.
 *
 *   fifo barrier   set when an update with set_barrier applies; cleared right after the next frame of the surface's
 *                  own display has been latched (wl_frame_done names it), so the update stays at least one refresh
 *                  cycle. A surface nobody can see clears it with its own once-a-second tick (framegate.h), and a
 *                  screen that is off clears every one: a client is never left waiting for good.
 *   target time    CLOCK_MONOTONIC ns, the wp_presentation clock: the update applies at the latch of the first frame
 *                  predicted to be presented at or after it, less a slack of 1/16 of the refresh period (vblank
 *                  timestamps jitter a little; a frame early by that much is the closest one to the target).
 *
 * Times are CLOCK_MONOTONIC nanoseconds. */
#ifndef LORD_WL_WLTIMING_H
#define LORD_WL_WLTIMING_H

#include <stdint.h>

#define WLT_REFRESH_NS 16666667u        /* a display that never told its rate: 60 Hz */

/* What a commit carries for these protocols (all of it double-buffered, see wl_surface.commit). hint and ctype are the
 * tearing-control and content-type values this update makes current. */
typedef struct {
    int set_barrier, wait_barrier;      /* fifo-v1 */
    int has_target;                     /* commit-timing-v1 */
    uint64_t target;
    int hint, ctype;
} WltUpdate;

/* A display's vblank grid, from its last presented frame (wp_presentation's time) and its refresh period. */
typedef struct { uint64_t last; uint32_t refresh; } WltClock;

static inline uint64_t wlt_period(const WltClock *c) { return c && c->refresh ? c->refresh : WLT_REFRESH_NS; }
static inline uint64_t wlt_slack(const WltClock *c) { return wlt_period(c) / 16; }

/* The first vblank after now: when a frame latched now is presented (now + a period when nothing was presented yet). */
static inline uint64_t wlt_next_vblank(const WltClock *c, uint64_t now) {
    uint64_t r = wlt_period(c);
    if (!c || !c->last) return now + r;
    if (c->last > now) return c->last + r;          /* (a flip time just past `now` taken a moment earlier) */
    return c->last + ((now - c->last) / r + 1) * r;
}

/* May update u apply at a latch whose frame is presented at `present`? barrier: the surface's fifo barrier is up.
 * ignore_wait: the surface is a subsurface in synchronized mode (the spec says its wait_barrier is ignored). */
static inline int wlt_ready(const WltUpdate *u, int barrier, int ignore_wait, uint64_t present, uint64_t slack) {
    if (u->wait_barrier && barrier && !ignore_wait) return 0;
    if (u->has_target && u->target > present + slack) return 0;
    return 1;
}

/* ns from now until a latch would take an update whose only wait is its target: the vblank before the first one at
 * or after target - slack (0: the next latch). */
static inline uint64_t wlt_due_in(const WltClock *c, uint64_t target, uint64_t now) {
    uint64_t r = wlt_period(c), s = wlt_slack(c), nv = wlt_next_vblank(c, now);
    if (target <= nv + s) return 0;
    uint64_t base = nv, want = target - s;
    uint64_t v = base + (want - base + r - 1) / r * r;      /* first vblank on the grid at or after want */
    return v - r > now ? v - r - now : 0;
}

#endif
