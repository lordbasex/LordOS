/* shake_test.c — shake the pointer to find it (src/shake.c): what counts as a shake and what doesn't,
 * and how the cursor grows and shrinks back. */
#include "shake.h"

#include <stdio.h>

static int fails, checks;
static void check(int ok, const char *what) { checks++; if (!ok) { fails++; printf("FAIL %s\n", what); } }

/* strokes of len px each way, every one taking ms, in steps of 8 ms; returns the time after them */
static double strokes(Shake *s, double t, int n, double len, double ms, double dy) {
    for (int i = 0; i < n; i++) {
        int steps = (int)(ms / 8);
        double dir = i % 2 ? -1 : 1;
        for (int k = 0; k < steps; k++) { t += 8; shake_motion(s, t, dir * len / steps, dy); }
    }
    return t;
}

int main(void) {
    Shake s;
    double t;

    shake_reset(&s);
    t = strokes(&s, 1000, 6, 200, 120, 0);
    check(shake_level(&s, t + 1) > 0, "six quick 200 px strokes: it grows");
    check(shake_timeout(&s, t + 1) > 0, "while growing there is a next frame");
    double top = shake_level(&s, t + 200);
    check(top > 0.99, "it reaches the largest size");
    double turn = t - 120;                         /* the last turn: where the sixth stroke began */
    check(shake_level(&s, turn + SHAKE_HOLD_MS - 10) > 0.99, "and holds it a moment after the last turn");
    double after = turn + SHAKE_HOLD_MS + SHAKE_SHRINK_MS + 50;
    for (double u = turn + SHAKE_HOLD_MS - 10; u < after; u += 16) shake_level(&s, u);
    check(shake_level(&s, after) == 0, "then it shrinks back to normal");
    check(shake_timeout(&s, after) < 0, "and no more frames are asked for");

    shake_reset(&s);
    t = strokes(&s, 1000, 6, 30, 120, 0);
    check(shake_level(&s, t + 50) == 0, "short 30 px strokes: not a shake");

    shake_reset(&s);
    t = strokes(&s, 1000, 6, 200, 400, 0);
    check(shake_level(&s, t + 50) == 0, "slow strokes (400 ms each): not a shake");

    shake_reset(&s);
    t = strokes(&s, 1000, 6, 200, 120, 400);
    check(shake_level(&s, t + 50) == 0, "mostly vertical motion: not a shake");

    shake_reset(&s);
    t = strokes(&s, 1000, 1, 1500, 300, 0);
    check(shake_level(&s, t + 50) == 0, "one long move across the screen: not a shake");

    shake_reset(&s);
    t = strokes(&s, 1000, 3, 200, 120, 0);
    t = strokes(&s, t, 1, 20, 40, 0);
    t = strokes(&s, t, 2, 200, 120, 0);
    check(shake_level(&s, t + 50) == 0, "a short stroke in the middle starts the count again");

    shake_reset(&s);
    check(shake_timeout(&s, 0) < 0 && shake_level(&s, 0) == 0, "untouched: normal size, no frames");

    printf("shake-test: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
