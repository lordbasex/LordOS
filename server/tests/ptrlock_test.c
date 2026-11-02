/* ptrlock_test.c — when a pointer constraint holds (wl/ptrlock.h) and when the Test Bench asks for
 * one (testbench/lockkey.h). Comes from a report on the Pi 5: pressing L while trying the bench's
 * keyboard hid the pointer, there or later as it walked into the window, until L came again. */
#include "wl/ptrlock.h"
#include "testbench/lockkey.h"

#include <stdio.h>

static int fails, n;
#define CHECK(c) do { n++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void constraints(void) {
    int active = 0, spent = 0;
    /* the pointer over a window that doesn't have the keyboard: nothing holds */
    CHECK(ptrlock_step(&active, &spent, 0, 1, 0) == PTRLOCK_SAME && !active);
    /* the keyboard without the pointer: neither */
    CHECK(ptrlock_step(&active, &spent, 0, 0, 1) == PTRLOCK_SAME && !active);
    /* both: it holds, once */
    CHECK(ptrlock_step(&active, &spent, 0, 1, 1) == PTRLOCK_ON && active);
    CHECK(ptrlock_step(&active, &spent, 0, 1, 1) == PTRLOCK_SAME && active);
    /* the app loses the keyboard: released, and held again when it comes back (persistent) */
    CHECK(ptrlock_step(&active, &spent, 0, 1, 0) == PTRLOCK_OFF && !active && !spent);
    CHECK(ptrlock_step(&active, &spent, 0, 1, 1) == PTRLOCK_ON && active);
    /* the pointer leaves */
    CHECK(ptrlock_step(&active, &spent, 0, 0, 1) == PTRLOCK_OFF && !active && !spent);

    /* oneshot: one activation only */
    active = spent = 0;
    CHECK(ptrlock_step(&active, &spent, 1, 1, 1) == PTRLOCK_ON);
    CHECK(ptrlock_step(&active, &spent, 1, 1, 0) == PTRLOCK_OFF && spent);
    CHECK(ptrlock_step(&active, &spent, 1, 1, 1) == PTRLOCK_SAME && !active);

    /* ❖Esc: released and stays released, persistent or not */
    active = 1;
    spent = 0;
    CHECK(ptrlock_escape(&active, &spent) == PTRLOCK_OFF && !active && spent);
    CHECK(ptrlock_step(&active, &spent, 0, 1, 1) == PTRLOCK_SAME && !active);
    CHECK(ptrlock_escape(&active, &spent) == PTRLOCK_SAME);
}

static void bench_key(void) {
    /* trying the keyboard: L alone does nothing, pressed or released */
    CHECK(!tb_lock_key(KEY_L, 1, 0, 0, 1));
    CHECK(!tb_lock_key(KEY_L, 0, 1, 0, 1));
    /* Ctrl+L with the pointer on the window */
    CHECK(tb_lock_key(KEY_L, 1, 1, 0, 1));
    /* with the pointer away there is nothing to wait for */
    CHECK(!tb_lock_key(KEY_L, 1, 1, 0, 0));
    /* letting go always works */
    CHECK(tb_lock_key(KEY_L, 1, 1, 1, 0));
    /* another key */
    CHECK(!tb_lock_key(KEY_K, 1, 1, 0, 1));
    /* it ends by itself, without any key */
    CHECK(!tb_lock_over(1, 1000, 1000 + TB_LOCK_MS - 1));
    CHECK(tb_lock_over(1, 1000, 1000 + TB_LOCK_MS));
    CHECK(!tb_lock_over(0, 1000, 1000 + 2 * TB_LOCK_MS));
}

int main(void) {
    constraints();
    bench_key();
    printf("ptrlock-test: %d checks, %d failed\n", n, fails);
    return fails ? 1 : 0;
}
