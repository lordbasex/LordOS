/* brokerclip_test.c — an isolated app's own copy on the clipboard (src/sec/brokerclip.c): its status,
 * a secret's countdown and clearing, Keep, a copy replaced by another app or by a picture (the secret's text
 * is still wiped), the short window an app may ask in, and another app that asks. `make build/brokerclip-test` */
#include "../src/sec/brokerclip.h"

#include <stdio.h>

static int fails, total;
static void check(int ok, const char *what) { total++; printf("%s: %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

int main(void) {
    int left = -1;
    check(bclip_status("passwords", 1, 0, &left) == BCLIP_NONE && left == 0, "nothing copied yet");

    /* a plain copy: never cleared, asked about for 3 s only */
    bclip_copied("passwords", 5, 5, 0, 30, 100);
    check(bclip_status("passwords", 5, 100, &left) == BCLIP_MINE && left == 0, "plain copy: mine, no countdown");
    check(!bclip_due(5, 5, 1000), "plain copy: never due");
    check(bclip_status("other", 5, 100, &left) == BCLIP_NONE, "another app learns nothing");
    check(bclip_status("passwords", 6, 101, &left) == BCLIP_REPLACED, "replaced");
    check(bclip_status("passwords", 6, 103.5, &left) == BCLIP_NONE, "after 3 s: none (no watching the clipboard)");

    /* a secret: countdown, then cleared on time */
    bclip_copied("passwords", 10, 10, 1, 30, 200);
    check(bclip_status("passwords", 10, 200.2, &left) == BCLIP_MINE && left == 30, "secret: 30 s left");
    check(bclip_status("passwords", 10, 219.5, &left) == BCLIP_MINE && left == 11, "secret: 11 s left (past the 3 s)");
    check(!bclip_due(10, 10, 229.9), "not due before its time");
    check(bclip_due(10, 10, 230) == BCLIP_CLEAR_ALL, "due at its time: clear");
    bclip_cleared(11);
    check(!bclip_due(11, 11, 300), "due only once");
    check(bclip_status("passwords", 11, 231, &left) == BCLIP_CLEARED, "cleared on time");
    check(bclip_status("passwords", 12, 234, &left) == BCLIP_REPLACED, "something copied after the clearing");
    check(bclip_status("passwords", 12, 235.5, &left) == BCLIP_NONE, "5 s after its time: none");

    /* a secret replaced by other text before its time */
    bclip_copied("passwords", 20, 20, 1, 30, 300);
    check(bclip_status("passwords", 21, 301, &left) == BCLIP_REPLACED && left == 0, "secret replaced");
    check(!bclip_due(21, 21, 330), "replaced by text: nothing to clear (the new copy stays)");

    /* a secret replaced by a picture: the picture stays, the secret's text is wiped on time */
    bclip_copied("passwords", 25, 25, 1, 30, 350);
    check(bclip_status("passwords", 26, 351, &left) == BCLIP_REPLACED, "replaced by a picture");
    check(bclip_due(26, 25, 380) == BCLIP_WIPE_TEXT, "on time: wipe the text, keep the picture");

    /* Keep */
    bclip_copied("passwords", 30, 30, 1, 30, 400);
    check(!bclip_keep("other", 30, 401), "another app can't keep it");
    check(bclip_keep("passwords", 30, 401), "kept");
    check(!bclip_due(30, 30, 1000), "kept: never cleared");
    check(bclip_status("passwords", 30, 402, &left) == BCLIP_NONE, "kept: nothing more to ask");
    check(!bclip_keep("passwords", 31, 402), "can't keep what was replaced");

    /* out-of-range seconds keep the secret (as BOP_CLIP_SECRET did) */
    bclip_copied("passwords", 40, 40, 1, 0, 500);
    check(!bclip_due(40, 40, 9999), "0 s: never cleared");
    bclip_copied("passwords", 41, 41, 1, 601, 500);
    check(!bclip_due(41, 41, 9999), "over 600 s: never cleared");

    /* the last copy wins */
    bclip_copied("passwords", 50, 50, 1, 30, 600);
    bclip_copied("mail", 51, 51, 0, 0, 601);
    check(bclip_status("passwords", 51, 602, &left) == BCLIP_NONE, "another app copied last: none");
    check(!bclip_due(51, 51, 700), "its countdown is gone");
    check(bclip_status("mail", 51, 602, &left) == BCLIP_MINE, "the other app's copy");

    /* a paste the person asked for: one read, by that app, soon after the keys */
    check(!bclip_paste_take("mail", 700), "no gesture: no read");
    bclip_paste_gesture("mail", 700);
    check(!bclip_paste_take("calendar", 700.1), "another app can't use it");
    check(bclip_paste_take("mail", 700.5), "the app whose window got Ctrl+V reads once");
    check(!bclip_paste_take("mail", 700.6), "and only once");
    bclip_paste_gesture("mail", 710);
    check(!bclip_paste_take("mail", 712.5), "too late: none");
    bclip_paste_gesture("mail", 720);
    bclip_paste_gesture("reminders", 720.2);
    check(!bclip_paste_take("mail", 720.3) && bclip_paste_take("reminders", 720.3), "the latest gesture wins");
    bclip_paste_gesture(NULL, 730);
    bclip_paste_gesture("", 730);
    check(!bclip_paste_take("", 730.1), "no app: nothing");

    printf("brokerclip-test: %d/%d\n", total - fails, total);
    return fails != 0;
}
