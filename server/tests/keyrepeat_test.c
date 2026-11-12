/* keyrepeat_test.c — the repeat of a held key in an isolated app (src/app/keyrepeat.h). */
#include "app/keyrepeat.h"

#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "keyrepeat_test: line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void) {
    KeyRepeat r;
    keyrepeat_init(&r);
    CHECK(keyrepeat_wait(&r, 0) == -1);
    CHECK(keyrepeat_due(&r, 1e6) == -1);

    /* the compositor's 30/s after 500 ms: Backspace (14) held */
    keyrepeat_info(&r, 30, 500);
    keyrepeat_key(&r, 14, 1, 1, 1000);
    CHECK(keyrepeat_wait(&r, 1000) == 500);
    CHECK(keyrepeat_due(&r, 1499) == -1);
    CHECK(keyrepeat_due(&r, 1500) == 14);
    CHECK(keyrepeat_due(&r, 1500) == -1);
    CHECK(keyrepeat_wait(&r, 1500) == 34);          /* 33.3 ms, rounded up: never woken early */
    CHECK(keyrepeat_due(&r, 1534) == 14);
    /* a stalled app: one repeat, not a burst */
    CHECK(keyrepeat_due(&r, 3000) == 14);
    CHECK(keyrepeat_due(&r, 3000) == -1);
    CHECK(keyrepeat_wait(&r, 3000) == 34);
    /* another key's release doesn't end it; its own does */
    keyrepeat_key(&r, 30, 0, 0, 3010);
    CHECK(keyrepeat_wait(&r, 3010) >= 0);
    keyrepeat_key(&r, 14, 0, 0, 3020);
    CHECK(keyrepeat_wait(&r, 3020) == -1);
    CHECK(keyrepeat_due(&r, 9000) == -1);

    /* a new press takes over; a modifier's press (doesn't repeat) ends it */
    keyrepeat_key(&r, 105, 1, 1, 10000);             /* Left */
    keyrepeat_key(&r, 106, 1, 1, 10200);             /* Right */
    CHECK(keyrepeat_due(&r, 10500) == -1);
    CHECK(keyrepeat_due(&r, 10700) == 106);
    keyrepeat_key(&r, 105, 0, 0, 10710);             /* Left up: Right keeps repeating */
    CHECK(keyrepeat_wait(&r, 10710) >= 0);
    keyrepeat_key(&r, 42, 1, 0, 10720);              /* Shift down */
    CHECK(keyrepeat_wait(&r, 10720) == -1);

    /* focus left the window */
    keyrepeat_key(&r, 14, 1, 1, 20000);
    keyrepeat_stop(&r);
    CHECK(keyrepeat_due(&r, 30000) == -1);

    /* rate 0: no repeat at all, and a held key stops */
    keyrepeat_key(&r, 14, 1, 1, 40000);
    keyrepeat_info(&r, 0, 500);
    CHECK(keyrepeat_wait(&r, 40000) == -1);
    keyrepeat_key(&r, 14, 1, 1, 41000);
    CHECK(keyrepeat_due(&r, 50000) == -1);
    /* out of range values: rate capped, delay back to the default */
    keyrepeat_info(&r, 1000, -5);
    CHECK(r.rate == 100 && r.delay == KEYREPEAT_DELAY);
    keyrepeat_key(&r, 14, 1, 1, 60000);
    CHECK(keyrepeat_wait(&r, 60000) == KEYREPEAT_DELAY);

    if (fails) return 1;
    printf("keyrepeat_test: ok\n");
    return 0;
}
