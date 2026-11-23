/* softpace_test.c — the software vblank (src/softpace.h): frames on a grid, never two started or shown too close. */
#include "softpace.h"

#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "softpace_test: line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b) ((a) - (b) < 1e-6 && (b) - (a) < 1e-6)

/* the main loop: a frame wanted at `want` (ms), waits until its slot; returns when it started */
static double frame(SoftPace *p, double want, double period) {
    double w = softpace_wait(p, want, period);
    double at = w > 0 ? want + w : want;
    if (w > 0) softpace_wait(p, at, period);        /* woken at the slot */
    softpace_started(p, period);
    return at;
}

/* the same, painted in `ms`: shown when the paint ends (main.c: softpace_shown after render) */
static double paint(SoftPace *p, double want, double period, double ms) {
    double w = softpace_wait(p, want, period);
    double at = w > 0 ? want + w : want;
    if (w > 0) softpace_wait(p, at, period);
    softpace_started(p, period);
    softpace_shown(p, at + ms, period);
    return at;
}

int main(void) {
    const double P = 1000.0 / 60;
    /* the first frame: at once */
    SoftPace p = { 0 };
    CHECK(softpace_wait(&p, 1000, P) == 0);
    softpace_started(&p, P);
    CHECK(NEAR(p.next, 1000 + P));
    /* a wake-up a little late (the wait rounded up to whole ms) keeps the grid: 60 frames a second stay 60 */
    double t = 1000;
    for (int i = 1; i <= 600; i++) t = frame(&p, 1000 + i * P + 0.9, P);
    CHECK(NEAR(p.next, 1000 + 601 * P));
    CHECK(t - (1000 + 600 * P) < 1);
    /* the bug fixed after 0.8.9.5: nothing wanted the slot at 1000+601P; a client commits 12 ms after it. The old grid
     * put the next frame 4.7 ms after this one (its update on screen for 4.7 ms, not a refresh) */
    double late = 1000 + 601 * P + 12;
    double a = frame(&p, late, P);
    CHECK(NEAR(a, late));                           /* at once: never more latency */
    double b = frame(&p, a + 0.1, P);               /* the next update, wanted right away */
    CHECK(b - a >= P - softpace_tol(P));
    CHECK(NEAR(b - a, P));                          /* the grid started again from the late frame */
    /* every lateness in (0, P): two frames in a row never closer than P - tol */
    for (int k = 0; k <= 160; k++) {
        SoftPace q = { 0 };
        double x = 2000;
        frame(&q, x, P);                            /* a grid from 2000 */
        double l = k * P / 160;                     /* the next frame wanted l ms past its slot */
        double f1 = frame(&q, 2000 + P + l, P);
        double f2 = frame(&q, f1, P);
        CHECK(f2 - f1 >= P - softpace_tol(P) - 1e-9);
        CHECK(f1 >= 2000 + P + l - 1e-9);           /* never later than wanted when its slot has passed */
    }
    /* frames shown when painted: one painted slowly (preempted on a loaded machine, 15 ms) is shown late; the next,
     * wanted at once, waits half a refresh after it was shown, not its slot 1.7 ms later (1 ms apart, seen with a
     * busy loop on every core before this) */
    SoftPace h = { 0 };
    double s0 = paint(&h, 3000, P, 0.5);
    double s1 = paint(&h, s0 + 0.5, P, 15);         /* started on its slot, shown 15 ms later */
    CHECK(NEAR(s1, 3000 + P));
    double shown1 = s1 + 15;
    double s2 = paint(&h, shown1 + 0.1, P, 0.5);
    CHECK(s2 + 0.5 - shown1 >= P / 2 - 1e-9);       /* shown at least half a refresh apart */
    /* every paint time and lateness: two frames in a row shown at least half a refresh apart */
    for (int k = 0; k <= 40; k++)
        for (int j = 0; j <= 40; j++) {
            SoftPace q = { 0 };
            double r1 = k * P / 40, l = j * P / 40;
            paint(&q, 5000, P, 0.3);
            double a1 = paint(&q, 5000 + P + l, P, r1);
            double a2 = paint(&q, a1 + r1, P, 0.3);
            CHECK(a2 + 0.3 - (a1 + r1) >= P / 2 - 1e-9);
            CHECK(a2 - a1 >= P - softpace_tol(P) - 1e-9);
        }
    /* frames painted in under half a refresh (a VM's 8 ms frames) never move the grid: 60 a second stay 60 */
    SoftPace g = { 0 };
    double t0 = paint(&g, 7000, P, 3), tl = t0;
    for (int i = 1; i <= 600; i++) tl = paint(&g, tl + 0.2, P, 8);
    CHECK(tl - (t0 + 600 * P) < 1.1);
    /* idle for a long time: starts at once, a new grid */
    SoftPace r = { 0 };
    frame(&r, 100, P);
    CHECK(softpace_wait(&r, 5000, P) == 0);
    CHECK(NEAR(r.next, 5000));
    /* early: waits for its slot */
    SoftPace e = { 0 };
    frame(&e, 100, P);
    CHECK(NEAR(softpace_wait(&e, 105, P), 100 + P - 105));
    /* the tolerance: an eighth of a period, at least 2 ms (a 144 Hz viewer's grid is not restarted by rounding) */
    CHECK(NEAR(softpace_tol(1000.0 / 60), 1000.0 / 60 / 8));
    CHECK(softpace_tol(1000.0 / 144) == 2.0);
    if (fails) return 1;
    printf("softpace_test: ok\n");
    return 0;
}
