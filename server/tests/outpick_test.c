/* outpick_test.c — src/wl/outpick.h: a Linux window's main display, the displays its new content must reach, and whose
 * frames and flips its frame callbacks and presentation feedback follow (docs/WAYLAND.md, "Frames, titles and damage").
 * The last part runs three displays (60, 60 and 144 Hz, as headless lord-server can't) with a window on each that
 * draws again on every frame callback, the way wl.c and main.c's render_frame decide, for one simulated second. */
#include <stdio.h>
#include "wl/outpick.h"

static int fails;
static void check(const char *what, long got, long want) {
    if (got == want) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); }
}
static void range(const char *what, long got, long lo, long hi) {
    if (got >= lo && got <= hi) printf("ok   %s (%ld)\n", what, got);
    else { fails++; printf("FAIL %s: got %ld, want %ld..%ld\n", what, got, lo, hi); }
}

/* three displays side by side: 1920 + 1920 + 2560 logical px; slot 1 is free, so slots and keys differ */
static OutPickSlot slots[4] = {
    { 1, 0, 0, 0, 1920, 1080 },
    { 0, 0, 0, 0, 0, 0 },
    { 1, 2, 1920, 0, 1920, 1080 },
    { 1, 1, 3840, 0, 2560, 1440 },
};

/* ---- the frame loop: displays flip on their own vblank, a render runs whenever a flip completes ---- */
typedef struct { int period_us, pending_until, work, frames; } Disp;
typedef struct { int slot, waiting, callbacks; } Win;

static void simulate(int per_display, Win *w, int nw, Disp *d, int nd) {
    for (int t = 0; t < 1000000; t += 100) {
        int any_event = t == 0;
        for (int i = 0; i < nd; i++)
            if (d[i].pending_until && t >= d[i].pending_until) { d[i].pending_until = 0; any_event = 1; }
        for (int i = 0; i < nd; i++) if (!d[i].pending_until && d[i].work) any_event = 1;   /* a commit wakes the loop */
        if (!any_event) continue;
        /* render_frame: every display with work and no flip pending submits a frame (flips at its next vblank) */
        unsigned keys = 0;
        for (int i = 0; i < nd; i++) {
            int done = 0;
            if (!d[i].pending_until && d[i].work) {
                d[i].pending_until = (t / d[i].period_us + 1) * d[i].period_us;
                d[i].work = 0;
                d[i].frames++;
                done = 1;
            }
            if (done || !d[i].pending_until) keys |= 1u << i;
        }
        if (!per_display) keys = OUTPICK_ALL;          /* before: every window's callbacks with every frame */
        /* wl_frame_done: a due window gets its callback and at once commits its next frame (work for its display) */
        for (int k = 0; k < nw; k++) {
            if (!w[k].waiting || !outpick_due(outpick_key(slots, 4, w[k].slot), keys)) continue;
            w[k].callbacks++;
            d[slots[w[k].slot].key].work = 1;
        }
    }
}

int main(void) {
    /* the main display: most of the box, the first on a tie, -1 off every display */
    check("main: a window inside the first display", outpick_main(slots, 4, 100, 100, 900, 700), 0);
    check("main: mostly on the third slot", outpick_main(slots, 4, 1800, 0, 2600, 600), 2);
    check("main: across the last two, more on the last", outpick_main(slots, 4, 3500, 0, 4500, 500), 3);
    check("main: a free slot never", outpick_main(slots, 4, 0, 0, 10, 10), 0);
    check("main: off every display", outpick_main(slots, 4, -500, -500, -100, -100), -1);
    OutPickSlot mirror[2] = { { 1, 0, 0, 0, 1920, 1080 }, { 1, 1, 0, 0, 1920, 1080 } };
    check("main: mirrored displays, the first", outpick_main(mirror, 2, 10, 10, 500, 500), 0);
    /* the displays new content reaches (keys) */
    check("keys: inside the first display", outpick_keys(slots, 4, 100, 100, 900, 700), 1u << 0);
    check("keys: across the second and third", outpick_keys(slots, 4, 3500, 0, 4500, 500), (1u << 2) | (1u << 1));
    check("keys: an empty box is every display", outpick_keys(slots, 4, 5, 5, 5, 5), OUTPICK_ALL);
    check("keys: off every display is every display", outpick_keys(slots, 4, -500, -500, -100, -100), OUTPICK_ALL);
    check("keys: mirrored, both", outpick_keys(mirror, 2, 10, 10, 500, 500), 3);
    /* slot → key */
    check("key of slot 2", outpick_key(slots, 4, 2), 2);
    check("key of slot 3", outpick_key(slots, 4, 3), 1);
    check("key of a free slot", outpick_key(slots, 4, 1), -1);
    check("key of no slot", outpick_key(slots, 4, -1), -1);
    /* due and presented */
    check("due: its display in keys", outpick_due(2, 1u << 2), 1);
    check("due: its display waits for a flip", outpick_due(2, 1u << 0), 0);
    check("due: on no display, any frame", outpick_due(-1, 0), 1);
    check("due: no monitor", outpick_due(1, OUTPICK_ALL), 1);
    check("presented: its display's flip", outpick_presents(1, 1, 0), 1);
    check("presented: another display's flip", outpick_presents(1, 2, 0), 0);
    check("presented: on no display, the main display's flip", outpick_presents(-1, 0, 0), 1);
    check("presented: on no display, another flip", outpick_presents(-1, 2, 0), 0);
    check("presented: no monitor", outpick_presents(2, -1, 0), 1);

    /* three displays at 60, 60 and 144 Hz (keys 0, 1, 2 = slots 0, 3, 2), a window drawing on each */
    for (int per = 0; per < 2; per++) {
        Disp d[3] = { { 16667, 0, 0, 0 }, { 16667, 0, 0, 0 }, { 6944, 0, 0, 0 } };
        Win w[3] = { { 0, 1, 0 }, { 3, 1, 0 }, { 2, 1, 0 } };
        simulate(per, w, 3, d, 3);
        printf("     %s: callbacks a second %d, %d, %d; frames shown %d, %d, %d\n", per ? "per display" : "before (every frame)",
               w[0].callbacks, w[1].callbacks, w[2].callbacks, d[0].frames, d[1].frames, d[2].frames);
        if (per) {
            range("per display: the 60 Hz window draws about 60 frames", w[0].callbacks, 58, 62);
            range("per display: the other 60 Hz window too", w[1].callbacks, 58, 62);
            range("per display: the 144 Hz window draws about 144", w[2].callbacks, 140, 146);
        } else {
            range("before: the 60 Hz window was paced by every display's frames", w[0].callbacks, 150, 300);
        }
    }
    printf("%s\n", fails ? "outpick-test: FAILED" : "outpick-test: ok");
    return fails ? 1 : 0;
}
