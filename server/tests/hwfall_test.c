/* hwfall_test.c — the rules of lm_hwfall.h */
#include "../src/media/helper/lm_hwfall.h"

#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void) {
    LmHwFall s = { 0 };
    CHECK(!lm_hwfall_due(&s));
    for (int i = 0; i < LM_HWFALL_ERRS - 1; i++) lm_hwfall_note(&s, 1);
    CHECK(!lm_hwfall_due(&s));                     /* a few bad packets are not enough */
    lm_hwfall_note(&s, 0);
    CHECK(s.fails == 0);                           /* a frame clears them */
    for (int i = 0; i < LM_HWFALL_ERRS - 1; i++) lm_hwfall_note(&s, 1);
    CHECK(!lm_hwfall_due(&s));
    lm_hwfall_note(&s, 1);
    CHECK(lm_hwfall_due(&s));
    lm_hwfall_reset(&s);
    CHECK(!lm_hwfall_due(&s));
    /* every frame decodes and none can be read back, the two threads taking turns */
    for (int i = 0; i < LM_HWFALL_ERRS - 1; i++) { lm_hwfall_note(&s, 0); lm_hwfall_note_rb(&s, 1); }
    CHECK(!lm_hwfall_due(&s));
    lm_hwfall_note(&s, 0);
    lm_hwfall_note_rb(&s, 1);
    CHECK(lm_hwfall_due(&s));                      /* frames decoded don't clear read-back failures */
    lm_hwfall_reset(&s);
    CHECK(s.rb_fails == 0 && !lm_hwfall_due(&s));
    lm_hwfall_note_rb(&s, 1);
    lm_hwfall_note_rb(&s, 0);
    CHECK(s.rb_fails == 0);                        /* a frame read clears them */
    printf("hwfall-test: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
