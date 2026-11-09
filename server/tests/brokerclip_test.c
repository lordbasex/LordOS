/* brokerclip_test.c — an isolated app's own copy on the clipboard (src/sec/brokerclip.c): its status,
 * a secret's countdown and clearing, Keep, a copy replaced by another app or by a picture, and another app
 * that asks. `make build/brokerclip-test` */
#include "../src/sec/brokerclip.h"

#include <stdio.h>

static int fails, total;
static void check(int ok, const char *what) { total++; printf("%s: %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

int main(void) {
    int left = -1;
    check(bclip_status("passwords", 1, 0, &left) == BCLIP_NONE && left == 0, "nothing copied yet");

    /* a plain copy: never cleared */
    bclip_copied("passwords", 5, 0, 30, 100);
    check(bclip_status("passwords", 5, 100, &left) == BCLIP_MINE && left == 0, "plain copy: mine, no countdown");
    check(!bclip_due(5, 1000), "plain copy: never due");
    check(bclip_status("other", 5, 100, &left) == BCLIP_NONE, "another app learns nothing");
    check(bclip_status("passwords", 6, 101, &left) == BCLIP_REPLACED, "replaced");

    /* a secret: countdown, then cleared on time */
    bclip_copied("passwords", 10, 1, 30, 200);
    check(bclip_status("passwords", 10, 200.2, &left) == BCLIP_MINE && left == 30, "secret: 30 s left");
    check(bclip_status("passwords", 10, 219.5, &left) == BCLIP_MINE && left == 11, "secret: 11 s left");
    check(!bclip_due(10, 229.9), "not due before its time");
    check(bclip_due(10, 230), "due at its time");
    bclip_cleared(11);
    check(!bclip_due(11, 300), "due only once");
    check(bclip_status("passwords", 11, 231, &left) == BCLIP_CLEARED, "cleared on time");
    check(bclip_status("passwords", 12, 240, &left) == BCLIP_REPLACED, "something copied after the clearing");

    /* a secret replaced before its time (another app, a machine, a picture: a new generation) */
    bclip_copied("passwords", 20, 1, 30, 300);
    check(bclip_status("passwords", 21, 301, &left) == BCLIP_REPLACED && left == 0, "secret replaced");
    check(!bclip_due(21, 330), "a replaced secret is not cleared (the new copy stays)");

    /* Keep */
    bclip_copied("passwords", 30, 1, 30, 400);
    check(!bclip_keep("other", 30), "another app can't keep it");
    check(bclip_keep("passwords", 30), "kept");
    check(bclip_status("passwords", 30, 450, &left) == BCLIP_MINE && left == 0, "kept: no countdown");
    check(!bclip_due(30, 1000), "kept: never cleared");
    check(!bclip_keep("passwords", 31), "can't keep what was replaced");

    /* out-of-range seconds keep the secret (as BOP_CLIP_SECRET did) */
    bclip_copied("passwords", 40, 1, 0, 500);
    check(!bclip_due(40, 9999), "0 s: never cleared");
    bclip_copied("passwords", 41, 1, 601, 500);
    check(!bclip_due(41, 9999), "over 600 s: never cleared");

    /* the last copy wins */
    bclip_copied("passwords", 50, 1, 30, 600);
    bclip_copied("mail", 51, 0, 0, 601);
    check(bclip_status("passwords", 51, 602, &left) == BCLIP_NONE, "another app copied last: none");
    check(!bclip_due(51, 700), "its countdown is gone");
    check(bclip_status("mail", 51, 602, &left) == BCLIP_MINE, "the other app's copy");

    printf("brokerclip-test: %d/%d\n", total - fails, total);
    return fails != 0;
}
