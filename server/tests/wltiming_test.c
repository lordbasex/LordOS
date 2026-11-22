/* wltiming_test.c — when a held content update may apply: fifo-v1 and commit-timing-v1 (src/wl/wltiming.h). */
#include "wl/wltiming.h"

#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "wltiming_test: line %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define MS 1000000ull

int main(void) {
    /* the vblank grid: 60 Hz from a flip at 1 s */
    WltClock c = { 1000 * MS, 16666667u };
    CHECK(wlt_period(&c) == 16666667u);
    CHECK(wlt_slack(&c) == 16666667u / 16);
    CHECK(wlt_next_vblank(&c, 1000 * MS) == 1000 * MS + 16666667u);            /* exactly on one: the next */
    CHECK(wlt_next_vblank(&c, 1000 * MS + 1) == 1000 * MS + 16666667u);
    CHECK(wlt_next_vblank(&c, 1000 * MS + 16666667u) == 1000 * MS + 2 * 16666667ull);
    CHECK(wlt_next_vblank(&c, 1100 * MS) == 1000 * MS + 6 * 16666667ull);     /* 6 periods: 100.000002 ms */
    /* nothing presented yet: a period from now; a refresh never told: 60 Hz */
    WltClock none = { 0, 0 };
    CHECK(wlt_next_vblank(&none, 5 * MS) == 5 * MS + WLT_REFRESH_NS);
    CHECK(wlt_next_vblank(NULL, 5 * MS) == 5 * MS + WLT_REFRESH_NS);
    /* a flip time a moment after now */
    CHECK(wlt_next_vblank(&c, 999 * MS) == 1000 * MS + 16666667u);

    /* fifo: wait_barrier holds only while the barrier is up; a synchronized subsurface ignores it */
    WltUpdate u = { 0 };
    CHECK(wlt_ready(&u, 1, 0, 0, 0));
    u.wait_barrier = 1;
    CHECK(!wlt_ready(&u, 1, 0, 0, 0));
    CHECK(wlt_ready(&u, 0, 0, 0, 0));
    CHECK(wlt_ready(&u, 1, 1, 0, 0));
    /* set_barrier alone never holds the update that carries it */
    WltUpdate sb = { .set_barrier = 1 };
    CHECK(wlt_ready(&sb, 1, 0, 0, 0));

    /* commit timing: not before the target, less the slack */
    WltUpdate t = { .has_target = 1, .target = 1050 * MS };
    CHECK(!wlt_ready(&t, 0, 0, 1033 * MS, 1 * MS));
    CHECK(wlt_ready(&t, 0, 0, 1049 * MS + 1, 1 * MS));
    CHECK(wlt_ready(&t, 0, 0, 1050 * MS, 0));
    CHECK(wlt_ready(&t, 0, 0, 1070 * MS, 0));
    /* both: each must hold */
    WltUpdate both = { .wait_barrier = 1, .has_target = 1, .target = 1050 * MS };
    CHECK(!wlt_ready(&both, 1, 0, 2000 * MS, 0));
    CHECK(!wlt_ready(&both, 0, 0, 1000 * MS, 0));
    CHECK(wlt_ready(&both, 0, 0, 1050 * MS, 0));

    /* when a target is due: at the latch of the frame presented first at or after it */
    uint64_t r = 16666667u, s = r / 16;
    CHECK(wlt_due_in(&c, 1000 * MS + r, 1000 * MS + 1) == 0);                  /* the next vblank is it */
    CHECK(wlt_due_in(&c, 1000 * MS + r + s, 1000 * MS + 1) == 0);              /* within the slack */
    /* three vblanks ahead: due after the second (the frame latched then is shown at the third) */
    CHECK(wlt_due_in(&c, 1000 * MS + 3 * r, 1000 * MS + 1) == 2 * r - 1);
    /* between vblanks: the first one after it */
    CHECK(wlt_due_in(&c, 1000 * MS + 2 * r + r / 2, 1000 * MS + 1) == 2 * r - 1);
    /* a target in the past: now */
    CHECK(wlt_due_in(&c, 10 * MS, 1000 * MS) == 0);

    /* Mesa's FIFO present, frame by frame: each update sets the barrier and waits for the one before it; held ones
     * stay in order (wl.c's queue), so at most one applies per latch */
    {
        WltUpdate fifo = { .set_barrier = 1, .wait_barrier = 1 };
        int barrier = 0, applied = 0, held = 0;
        for (int i = 0; i < 3; i++) {           /* three presents before any frame */
            if (!held && wlt_ready(&fifo, barrier, 0, 0, 0)) { applied++; barrier = 1; }
            else held++;
        }
        CHECK(applied == 1 && held == 2);
        for (int frame = 0; frame < 2; frame++) {   /* each frame done: the barrier clears, one more applies */
            barrier = 0;
            while (held && wlt_ready(&fifo, barrier, 0, 0, 0)) { held--; applied++; barrier = 1; }
            CHECK(applied == 2 + frame);
        }
        CHECK(held == 0);
    }

    if (fails) return 1;
    printf("wltiming_test: ok\n");
    return 0;
}
