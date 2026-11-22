/* idlenotify_test.c — src/wl/idlenotify.h: who may be told the person is away (ext-idle-notify, with the "idle"
 * permission) and when "idled" and "resumed" go (docs/WAYLAND.md, "Idle notifications"). */
#include <stdio.h>
#include "wl/idlenotify.h"

static int fails;
static void check(const char *what, long got, long want) {
    if (got == want) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); }
}

int main(void) {
    /* the decision: 0 ask, 1 allow, 2 deny */
    check("decide: no decision yet asks", idn_decide(0, 0, 0, 1), IDN_PROMPT);
    check("decide: allowed", idn_decide(1, 0, 0, 1), IDN_GRANT);
    check("decide: denied is never told", idn_decide(2, 0, 0, 1), IDN_NEVER);
    check("decide: Xwayland never (it speaks for every X11 app)", idn_decide(1, 1, 0, 1), IDN_NEVER);
    check("decide: the login screen never", idn_decide(1, 0, 1, 1), IDN_NEVER);
    check("decide: no identity never, and no prompt", idn_decide(0, 0, 0, 0), IDN_NEVER);

    /* a 5 s notification; activity at 1000 ms */
    IdnState n = { 5000, 0, 0 };
    double wait = -1;
    check("allowed, 2 s idle: nothing yet", idn_step(&n, 1, 1000, 3000, &wait), IDN_NONE);
    check("... due in 3 s", (long)wait, 3000);
    check("allowed, 5 s idle: idled", idn_step(&n, 1, 1000, 6000, NULL), IDN_IDLED);
    check("still idle: nothing more", idn_step(&n, 1, 1000, 9000, NULL), IDN_NONE);
    check("activity after it: resumed", idn_step(&n, 1, 9500, 9600, NULL), IDN_RESUMED);
    wait = -1;
    check("then nothing until the timeout again", idn_step(&n, 1, 9500, 9700, &wait), IDN_NONE);
    check("... due 5 s after the activity", (long)wait, 4800);

    /* denied or undecided: never idled however long */
    IdnState d = { 1000, 0, 0 };
    wait = -1;
    check("not allowed: no idled after an hour", idn_step(&d, 0, 0, 3600000, &wait), IDN_NONE);
    check("not allowed: no wake-up asked", (long)wait, -1);
    /* the permission taken back while idled: resumed, once, then nothing */
    IdnState r = { 1000, 0, 0 };
    idn_step(&r, 1, 0, 2000, NULL);
    check("revoked while idled: resumed", idn_step(&r, 0, 0, 2100, NULL), IDN_RESUMED);
    check("revoked: nothing more", idn_step(&r, 0, 0, 9000, NULL), IDN_NONE);

    /* an idle inhibitor (a video): lord-server counts it as activity at every pass, so the last activity keeps up with
     * now and nobody is idled */
    IdnState v = { 1000, 0, 0 };
    int idled = 0;
    for (double t = 0; t < 60000; t += 250) idled |= idn_step(&v, 1, t, t, NULL) == IDN_IDLED;
    check("an inhibitor holding: never idled", idled, 0);
    printf("%s\n", fails ? "idlenotify-test: FAILED" : "idlenotify-test: ok");
    return fails ? 1 : 0;
}
