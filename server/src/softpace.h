/* softpace.h — the software vblank: frames paced by lord-server itself when no flip paces them (headless, or a driver
 * whose page flips complete at once: vmwgfx, some virtual cards; main.c). Pure, header only: tests/softpace_test.c.
 *
 * Frames start on a fixed grid of `period` ms. Waits are in whole milliseconds (poll), so a frame starts up to about
 * a millisecond after its slot; keeping the grid then never adds up (60 Hz stays 60 frames a second, not 57).
 * A frame that starts later than that (nothing wanted a frame at the slot, or lord-server was busy) starts a new grid
 * from now: kept on the old one, the next slot would come less than a period after it, and a frame a client sees for
 * a few milliseconds is a refresh it never got (fifo-v1: an update shown for 3 ms instead of one refresh, before
 * the fix after 0.8.9.5). So two frames always start at least period - softpace_tol(period) apart.
 * A frame is shown when it is painted (no vblank to wait for), so a frame that took long to paint (lord-server busy or
 * preempted on a loaded machine) is shown late, and the next one, on time, came right after it (1 ms apart seen with a
 * busy loop on every core). The next frame then waits for at least half a refresh after it was shown (softpace_shown):
 * a frame painted in under half a refresh never moves the grid (a VM painting 6 ms frames keeps 60 a second), a heavier
 * or preempted one is paced as a missed vblank would pace it. Two frames are always shown at least half a refresh apart,
 * and at least period - softpace_tol(period) apart while they paint in about the same time.
 * Times are ms (main.c's now_ms). */
#ifndef LORD_SOFTPACE_H
#define LORD_SOFTPACE_H

typedef struct { double next; } SoftPace;   /* the next frame's slot, 0: none yet */

/* How late a frame may start and stay on the grid: a wait's rounding to whole ms and a little wake-up latency. */
static inline double softpace_tol(double period) { return period / 8 > 2.0 ? period / 8 : 2.0; }

/* ms until the next frame may start (<= 0: now). Idle or late past the tolerance: the grid starts again now. */
static inline double softpace_wait(SoftPace *p, double now, double period) {
    if (!p->next || now - p->next > softpace_tol(period)) p->next = now;
    return p->next - now;
}

/* A frame started (in the slot softpace_wait gave): the next slot. */
static inline void softpace_started(SoftPace *p, double period) { p->next += period; }

/* The frame that started was shown (painted) at `shown`: the next slot is not before half a refresh after it. */
static inline void softpace_shown(SoftPace *p, double shown, double period) {
    double min = shown + period / 2;
    if (p->next < min) p->next = min;
}

#endif
