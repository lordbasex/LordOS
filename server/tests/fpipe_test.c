/* fpipe_test.c — the frame pipeline's decisions (src/fpipe.c, docs/GPU.md "The frame pipeline"):
 *   - LORD_PIPELINE and the auto mode's hysteresis;
 *   - what a display does next (build, submit, prepare, wait) and when input waits for the flip;
 *   - a display driven the way main.c drives it, on a vblank grid: a frame whose CPU part plus the GPU's
 *     part passes the period is shown every other vblank without the pipeline and at every vblank with
 *     it; a light frame is shown at every vblank either way; an idle display builds at once; nothing is
 *     prepared twice for one flip (no busy loop);
 *   - auto lets go after an idle gap (FPIPE_IDLE_MS without a frame);
 *   - the JS frame callbacks run once per frame shown, also when a prepare painted nothing. */
#include "fpipe.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;
static double clock_ms = 1000;               /* the frames of test_mode/test_auto come one period apart */
static void note(FPipe *p, double cpu_ms, double period_ms) { clock_ms += 16.67; fpipe_note(p, clock_ms, cpu_ms, period_ms); }
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); } } while (0)

static void test_mode(void) {
    FPipe p;
    fpipe_init(&p, NULL);                       /* unset: off (the default since the Pi 5 run) */
    CHECK(p.mode == FPIPE_OFF && !fpipe_engaged(&p));
    fpipe_init(&p, "0");
    CHECK(p.mode == FPIPE_OFF && !fpipe_engaged(&p));
    note(&p, 30, 16.67);                        /* off stays off, however heavy */
    note(&p, 30, 16.67);
    CHECK(!fpipe_engaged(&p));
    fpipe_init(&p, "2");
    CHECK(p.mode == FPIPE_ALWAYS && fpipe_engaged(&p));
    note(&p, 1, 16.67);
    CHECK(fpipe_engaged(&p));
    fpipe_init(&p, "1");
    CHECK(p.mode == FPIPE_AUTO);
    CHECK(!fpipe_engaged(&p));
    fpipe_init(&p, "yes");                      /* anything else: off */
    CHECK(p.mode == FPIPE_OFF);
    fpipe_init(&p, "");
    CHECK(p.mode == FPIPE_OFF);
}

static void test_auto(void) {
    FPipe p;
    fpipe_init(&p, "1");
    note(&p, 13, 16.67);                        /* one heavy frame: not yet */
    CHECK(!fpipe_engaged(&p));
    note(&p, 5, 16.67);                         /* the run is broken */
    note(&p, 13, 16.67);
    CHECK(!fpipe_engaged(&p));
    note(&p, 13, 16.67);                        /* two in a row above 60 % */
    CHECK(fpipe_engaged(&p));
    for (int i = 0; i < 59; i++) note(&p, 5, 16.67);
    CHECK(fpipe_engaged(&p));                   /* 59 light ones: still on */
    note(&p, 8, 16.67);                         /* 48 %: neither light nor a reason to stay; the run restarts */
    for (int i = 0; i < 59; i++) note(&p, 5, 16.67);
    CHECK(fpipe_engaged(&p));
    note(&p, 5, 16.67);                         /* the 60th light frame in a row */
    CHECK(!fpipe_engaged(&p));
    note(&p, 9, 16.67);                         /* 54 %: under 60 %, stays off */
    note(&p, 9, 16.67);
    CHECK(!fpipe_engaged(&p));
    note(&p, 13, 0);                            /* no period: ignored */
    note(&p, 13, 0);
    CHECK(!fpipe_engaged(&p));
}

static void test_step(void) {
    /* no flip pending: a prepared frame is submitted; else built if there is something to show */
    CHECK(fpipe_step(1, 0, FPIPE_READY, 0, 0) == FPIPE_SUBMIT);
    CHECK(fpipe_step(0, 0, FPIPE_READY, 1, 1) == FPIPE_SUBMIT);     /* even after the pipeline let go */
    CHECK(fpipe_step(1, 0, FPIPE_NONE, 1, 0) == FPIPE_BUILD);
    CHECK(fpipe_step(1, 0, FPIPE_EMPTY, 1, 0) == FPIPE_BUILD);      /* the attempt painted nothing: built now */
    CHECK(fpipe_step(1, 0, FPIPE_NONE, 0, 0) == FPIPE_WAIT);
    /* flip pending: prepared only with the pipeline, once, and only with work to do ahead */
    CHECK(fpipe_step(1, 1, FPIPE_NONE, 1, 1) == FPIPE_PREPARE);
    CHECK(fpipe_step(0, 1, FPIPE_NONE, 1, 1) == FPIPE_WAIT);
    CHECK(fpipe_step(1, 1, FPIPE_NONE, 1, 0) == FPIPE_WAIT);
    CHECK(fpipe_step(1, 1, FPIPE_READY, 1, 1) == FPIPE_WAIT);
    CHECK(fpipe_step(1, 1, FPIPE_EMPTY, 1, 1) == FPIPE_WAIT);
    /* input waits: always without the pipeline while the flip is pending; with it only once prepared */
    CHECK(!fpipe_blocked(0, 0, FPIPE_NONE) && !fpipe_blocked(1, 0, FPIPE_READY));
    CHECK(fpipe_blocked(0, 1, FPIPE_NONE));
    CHECK(!fpipe_blocked(1, 1, FPIPE_NONE));
    CHECK(fpipe_blocked(1, 1, FPIPE_READY) && fpipe_blocked(1, 1, FPIPE_EMPTY));
}

/* One display driven like main.c's loop on a vblank grid (period P, µs). Every frame has cpu µs of CPU work
 * (input, JS, layout, paint) and sub µs to submit (upload, swap, flip ioctl); the GPU then needs gpu µs, and a
 * flip lands on the first vblank after both. animate: there is always a next frame to build. Returns the flips
 * shown in n periods; *prepares counts frames painted ahead, *spins render calls that did nothing. */
typedef struct { long shown, prepares, spins, max_latency; } Sim;
static Sim simulate(int mode, long cpu, long sub, long gpu, int animate, long n) {
    const long P = 16667;
    FPipe fp;
    fpipe_init(&fp, mode == FPIPE_OFF ? "0" : mode == FPIPE_ALWAYS ? "2" : "1");
    Sim r = { 0, 0, 0, 0 };
    long t = 0, flip_at = -1;                   /* when the pending flip lands (-1: none) */
    int prep = FPIPE_NONE;
    long input_t = 0, prep_input = 0, inflight_input = 0;    /* when the newest input shown by a frame came */
    int damage = 1;
    while (t < n * P) {
        int pending = flip_at >= 0;
        if (pending && t >= flip_at) {          /* the flip event */
            pending = 0;
            flip_at = -1;
            r.shown++;
            long lat = t - inflight_input;
            if (lat > r.max_latency) r.max_latency = lat;
        }
        if (animate) { damage = 1; input_t = t; }       /* new input every wake: the frame shows it */
        int eligible = fpipe_engaged(&fp);
        int step = fpipe_step(eligible, pending, prep, damage, damage);
        long busy = 0;
        if (step == FPIPE_SUBMIT) {
            busy += sub;
            prep = FPIPE_NONE;
            inflight_input = prep_input;
            flip_at = ((t + busy + gpu) / P + 1) * P;   /* the next vblank after the GPU is done */
            pending = 1;
            fpipe_note(&fp, (double)t / 1000, (double)(cpu + sub) / 1000, P / 1000.0);
        } else if (step == FPIPE_BUILD) {
            busy += cpu + sub;
            damage = 0;
            inflight_input = input_t;
            flip_at = ((t + busy + gpu) / P + 1) * P;
            pending = 1;
            fpipe_note(&fp, (double)t / 1000, (double)(cpu + sub) / 1000, P / 1000.0);
        }
        /* then, like render_frame, the next frame may be painted ahead */
        if (fpipe_step(fpipe_engaged(&fp), pending, prep, 0, damage) == FPIPE_PREPARE) {
            busy += cpu;
            prep = damage ? FPIPE_READY : FPIPE_EMPTY;
            prep_input = input_t;
            damage = 0;
            r.prepares++;
        } else if (step == FPIPE_WAIT && pending && !fpipe_blocked(fpipe_engaged(&fp), pending, prep) && damage) r.spins++;
        t += busy;
        if (!busy) t = flip_at >= 0 ? (flip_at > t ? flip_at : t + 1) : t + P / 4;   /* poll until the next event */
    }
    return r;
}

static void test_sim(void) {
    /* the Pi 5 under an all-core load (docs/BENCHMARK.md): about 13 ms of CPU, 1 ms to submit, 3 ms of GPU */
    Sim off = simulate(FPIPE_OFF, 13000, 1000, 3000, 1, 600);
    Sim on = simulate(FPIPE_ALWAYS, 13000, 1000, 3000, 1, 600);
    Sim au = simulate(FPIPE_AUTO, 13000, 1000, 3000, 1, 600);
    CHECK(off.shown >= 299 && off.shown <= 301 && off.prepares == 0);   /* every other vblank: 30 Hz */
    CHECK(on.shown >= 598 && on.prepares >= 597);                       /* every vblank: 60 Hz */
    CHECK(au.shown >= 596);                                             /* auto engages after two frames */
    CHECK(on.max_latency <= 3 * 16667 && off.max_latency <= 3 * 16667); /* input to screen: within 3 periods */
    /* a light frame: every vblank either way, and auto never engages */
    Sim l_off = simulate(FPIPE_OFF, 5000, 1000, 2000, 1, 600), l_au = simulate(FPIPE_AUTO, 5000, 1000, 2000, 1, 600);
    CHECK(l_off.shown >= 598 && l_au.shown >= 598 && l_au.prepares == 0);
    CHECK(l_au.max_latency <= 2 * 16667);                               /* not one frame more than without it */
    /* heavier than a period even pipelined: one frame per 21 ms of CPU (about 47 a second instead of 30),
     * never more than one ahead (input within 3 periods), and no busy loop */
    Sim h = simulate(FPIPE_ALWAYS, 20000, 1000, 3000, 1, 600), h_off = simulate(FPIPE_OFF, 20000, 1000, 3000, 1, 600);
    CHECK(h.shown >= 460 && h.shown <= 480 && h.spins == 0 && h.max_latency <= 3 * 16667);
    CHECK(h_off.shown >= 299 && h_off.shown <= 301);
    CHECK(on.spins == 0 && au.spins == 0);
    /* idle: one change is built and shown at once, nothing is prepared */
    Sim idle = simulate(FPIPE_ALWAYS, 13000, 1000, 3000, 0, 60);
    CHECK(idle.shown == 1 && idle.prepares == 0);
}

/* Auto mode after an idle desktop: no frames come, so nothing counted the light ones; the first frame after a gap
 * longer than FPIPE_IDLE_MS lets go (a heavy one counts as the first of the two that engage it again). */
static void test_idle(void) {
    FPipe p;
    fpipe_init(&p, "1");
    fpipe_note(&p, 1000, 13, 16.67);
    fpipe_note(&p, 1016.7, 13, 16.67);
    CHECK(fpipe_engaged(&p));
    fpipe_note(&p, 1416.7, 13, 16.67);          /* 400 ms later: not idle */
    CHECK(fpipe_engaged(&p));
    fpipe_note(&p, 1916.7, 5, 16.67);           /* exactly FPIPE_IDLE_MS later: not yet idle */
    CHECK(fpipe_engaged(&p));
    fpipe_note(&p, 2600, 5, 16.67);             /* 683 ms without a frame: off at once, not after 60 light frames */
    CHECK(!fpipe_engaged(&p) && p.light == 0 && p.heavy == 0);
    fpipe_note(&p, 2616.7, 13, 16.67);          /* heavy again: two in a row engage it, as from the start */
    CHECK(!fpipe_engaged(&p));
    fpipe_note(&p, 2633.4, 13, 16.67);
    CHECK(fpipe_engaged(&p));
    fpipe_note(&p, 5000, 13, 16.67);            /* a heavy frame after a gap: off, and it is the first of two */
    CHECK(!fpipe_engaged(&p) && p.heavy == 1);
    fpipe_note(&p, 5016.7, 13, 16.67);
    CHECK(fpipe_engaged(&p));
    /* the gap restarts the light count too (the light frames before it are not carried over) */
    for (int i = 0; i < 59; i++) fpipe_note(&p, 5100 + i * 16.67, 5, 16.67);
    CHECK(fpipe_engaged(&p) && p.light == 59);
    /* always and off ignore the gap */
    fpipe_init(&p, "2");
    fpipe_note(&p, 1000, 5, 16.67);
    fpipe_note(&p, 9000, 5, 16.67);
    CHECK(fpipe_engaged(&p));
    fpipe_init(&p, "0");
    fpipe_note(&p, 1000, 13, 16.67);
    fpipe_note(&p, 1016.7, 13, 16.67);
    fpipe_note(&p, 9000, 13, 16.67);
    CHECK(!fpipe_engaged(&p));
    /* a frame-by-frame run: a heavy drag engages it, the desktop goes idle for 2 s, then a light click: the
     * click's frame lets go, so the next input waits for the flip as without the pipeline */
    fpipe_init(&p, "1");
    double t = 1000;
    for (int i = 0; i < 100; i++, t += 16.67) fpipe_note(&p, t, 13, 16.67);
    CHECK(fpipe_engaged(&p));
    t += 2000;
    fpipe_note(&p, t, 4, 16.67);
    CHECK(!fpipe_engaged(&p));
    CHECK(fpipe_blocked(fpipe_engaged(&p), 1, FPIPE_NONE));      /* input during that frame's flip waits for it */
}

/* The JS frame callbacks against the frames shown: one display, light frames, an animation callback that never
 * changes anything and always asks for the next frame, and an app on the GPU path that sends a new frame after each
 * frame callback (so the display keeps flipping without UI damage). Driven like main.c's loop and render_frame:
 * a render runs when the display is not waiting for its flip or a frame can be prepared; the callbacks run in it
 * unless held (hold: fpipe_hold_callbacks; 0 simulates the code before the fix); a frame is built after the flip
 * for the app; while the flip is pending the callbacks make a prepare, which paints nothing (FPIPE_EMPTY). */
static void sim_callbacks(int mode, int hold, long n, long *shown, long *cbs) {
    const long P = 16667, cpu = 2000, gpu = 2000;
    FPipe fp;
    fpipe_init(&fp, mode == FPIPE_OFF ? "0" : mode == FPIPE_ALWAYS ? "2" : "1");
    long t = 0, flip_at = -1;
    int prep = FPIPE_NONE, prep_js = 0, app_new = 1;
    *shown = *cbs = 0;
    while (t < n * P) {
        int pending = flip_at >= 0;
        if (pending && t >= flip_at) { pending = 0; flip_at = -1; (*shown)++; }
        int eligible = fpipe_engaged(&fp);
        int ahead = fpipe_step(eligible, pending, prep, 0, 1) == FPIPE_PREPARE;
        if (pending && !ahead) { t = flip_at; continue; }
        int prep_only = pending, held = 0, ran = 0;
        long busy = 0;
        if (!prep_only) {
            held = hold && fpipe_hold_callbacks(prep, prep_js);
            prep_js = 0;
        }
        if (!held) { (*cbs)++; ran = 1; }         /* the callback runs, changes nothing, asks again */
        busy += cpu;
        if (!pending && (fpipe_step(eligible, 0, prep, app_new, 0) == FPIPE_BUILD)) {
            prep = FPIPE_NONE;                     /* (prep_undo) */
            prep_js = 0;
            flip_at = ((t + busy + gpu) / P + 1) * P;
            pending = 1;
            fpipe_note(&fp, (double)t / 1000, (double)busy / 1000, P / 1000.0);
        }
        if (fpipe_step(eligible, pending, prep, 0, prep_only) == FPIPE_PREPARE) {
            prep = FPIPE_EMPTY;                    /* nothing to paint */
            prep_js = ran;                         /* (the callback asked again) */
        }
        if (!prep_only) app_new = 1;              /* wl_frame_done: the app sends its next frame at once */
        t += busy;
    }
}

static void test_callbacks(void) {
    long shown, cbs;
    sim_callbacks(FPIPE_OFF, 1, 600, &shown, &cbs);
    CHECK(shown >= 598 && cbs >= shown && cbs <= shown + 1);         /* once per frame shown */
    long off_cbs = cbs;
    sim_callbacks(FPIPE_ALWAYS, 0, 600, &shown, &cbs);               /* before the fix: twice per frame */
    CHECK(shown >= 598 && cbs >= 2 * shown - 1);
    sim_callbacks(FPIPE_ALWAYS, 1, 600, &shown, &cbs);               /* with it: once, as with the pipeline off */
    CHECK(shown >= 598 && cbs >= shown && cbs <= shown + 2 && cbs <= off_cbs + 1);   /* (+1: one prepared at the end) */
    sim_callbacks(FPIPE_AUTO, 1, 600, &shown, &cbs);                 /* light frames: auto never engages */
    CHECK(shown >= 598 && cbs == off_cbs);
    /* the rule: only after a prepare that painted nothing and whose callbacks asked again */
    CHECK(fpipe_hold_callbacks(FPIPE_EMPTY, 1));
    CHECK(!fpipe_hold_callbacks(FPIPE_EMPTY, 0) && !fpipe_hold_callbacks(FPIPE_READY, 1) && !fpipe_hold_callbacks(FPIPE_NONE, 1));
}

int main(void) {
    test_mode();
    test_auto();
    test_step();
    test_sim();
    test_idle();
    test_callbacks();
    printf("fpipe-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
