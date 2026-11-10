/* xdgact_test.c — xdg-activation tokens and input serials (src/wl/xdgact.c). */
#include "../src/wl/xdgact.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

int main(void) {
    char app[128], tok[XACT_TOKEN_LEN + 1], other[XACT_TOKEN_LEN + 1];

    /* a launch token: random hex, works once, carries what it was issued for */
    const char *t = xact_issue(1000, XACT_LAUNCH_MS, 1, "firefox-esr");
    CHECK(t && strlen(t) == XACT_TOKEN_LEN && strspn(t, "0123456789abcdef") == XACT_TOKEN_LEN);
    snprintf(tok, sizeof tok, "%s", t ? t : "");
    t = xact_issue(1000, XACT_LAUNCH_MS, 1, "");
    snprintf(other, sizeof other, "%s", t ? t : "");
    CHECK(strcmp(tok, other));
    CHECK(xact_take(tok, 2000, app, sizeof app) == XACT_OK);
    CHECK(!strcmp(app, "firefox-esr"));
    CHECK(xact_take(tok, 2000, app, sizeof app) == XACT_UNKNOWN);    /* used up */
    CHECK(!app[0]);

    /* stale: past its life it only asks for attention, and is gone too */
    CHECK(xact_take(other, 1000 + XACT_LAUNCH_MS, NULL, 0) == XACT_STALE);
    CHECK(xact_take(other, 1000, NULL, 0) == XACT_UNKNOWN);

    /* issued without a grant: denied, never OK */
    snprintf(tok, sizeof tok, "%s", xact_issue(5000, XACT_CLIENT_MS, 0, "other"));
    CHECK(xact_take(tok, 5001, NULL, 0) == XACT_DENIED);

    /* anything else is unknown: not a token, the right length but never issued, empty, NULL */
    CHECK(xact_take("none", 5000, NULL, 0) == XACT_UNKNOWN);
    CHECK(xact_take("", 5000, NULL, 0) == XACT_UNKNOWN);
    CHECK(xact_take(NULL, 5000, NULL, 0) == XACT_UNKNOWN);
    CHECK(xact_take("0123456789abcdef0123456789abcdef", 5000, NULL, 0) == XACT_UNKNOWN);

    /* a full table drops the oldest, the newest stay */
    xact_reset();
    snprintf(tok, sizeof tok, "%s", xact_issue(0, XACT_LAUNCH_MS, 1, "first"));
    for (int i = 1; i < XACT_MAX; i++) xact_issue(i, XACT_LAUNCH_MS, 1, "");
    snprintf(other, sizeof other, "%s", xact_issue(XACT_MAX, XACT_LAUNCH_MS, 1, "second"));
    CHECK(xact_take(tok, 100, NULL, 0) == XACT_UNKNOWN);
    CHECK(xact_take(other, 100, app, sizeof app) == XACT_OK && !strcmp(app, "second"));

    /* an expired slot is reused before the oldest live one */
    xact_reset();
    xact_issue(0, 10, 1, "short");
    snprintf(tok, sizeof tok, "%s", xact_issue(1, XACT_LAUNCH_MS, 1, "kept"));
    for (int i = 2; i < XACT_MAX; i++) xact_issue(i, XACT_LAUNCH_MS, 1, "");
    xact_issue(100, XACT_LAUNCH_MS, 1, "new");
    CHECK(xact_take(tok, 200, app, sizeof app) == XACT_OK && !strcmp(app, "kept"));

    /* serials: only the client that got it, only for a while, and not after it left */
    xact_reset();
    int a, b;
    xact_note_serial(42, &a, 1000);
    CHECK(xact_serial_recent(42, &a, 1000 + XACT_SERIAL_MS));
    CHECK(!xact_serial_recent(42, &a, 1001 + XACT_SERIAL_MS));
    CHECK(!xact_serial_recent(42, &b, 1000));
    CHECK(!xact_serial_recent(43, &a, 1000));
    CHECK(!xact_serial_recent(42, NULL, 1000));
    xact_client_gone(&a);
    CHECK(!xact_serial_recent(42, &a, 1000));
    /* the ring forgets the oldest */
    for (uint32_t i = 0; i < 40; i++) xact_note_serial(100 + i, &b, 2000);
    CHECK(!xact_serial_recent(100, &b, 2000));
    CHECK(xact_serial_recent(139, &b, 2000));

    /* an app's token: revoked when the keyboard moves to another app, kept while it stays */
    xact_reset();
    int c1, c2;
    char t1[XACT_TOKEN_LEN + 1], t2[XACT_TOKEN_LEN + 1], launch[XACT_TOKEN_LEN + 1];
    snprintf(t1, sizeof t1, "%s", xact_issue_for(1000, XACT_CLIENT_MS, 1, "a", &c1));
    snprintf(t2, sizeof t2, "%s", xact_issue_for(1000, XACT_CLIENT_MS, 1, "b", &c1));
    snprintf(launch, sizeof launch, "%s", xact_issue(1000, XACT_LAUNCH_MS, 1, "launched"));
    CHECK(xact_focus_moved(&c1) == 0);                          /* still the same app */
    CHECK(xact_take(t1, 1001, NULL, 0) == XACT_OK);
    snprintf(t1, sizeof t1, "%s", xact_issue_for(1000, XACT_CLIENT_MS, 1, "a", &c1));
    CHECK(xact_focus_moved(&c2) == 2);                          /* focus to another app: both revoked */
    CHECK(xact_focus_moved(&c1) == 0);                          /* coming back does not restore them */
    CHECK(xact_take(t1, 1002, app, sizeof app) == XACT_DENIED && !strcmp(app, "a"));
    CHECK(xact_take(t1, 1002, NULL, 0) == XACT_UNKNOWN);       /* revoked is still single use */
    CHECK(xact_take(t2, 1002, NULL, 0) == XACT_DENIED);
    CHECK(xact_take(launch, 1002, app, sizeof app) == XACT_OK && !strcmp(app, "launched"));   /* LordOS's survives */

    /* focus to the desktop itself (no client) revokes too; the new focus's own tokens stay */
    snprintf(t1, sizeof t1, "%s", xact_issue_for(2000, XACT_CLIENT_MS, 1, "", &c1));
    snprintf(t2, sizeof t2, "%s", xact_issue_for(2000, XACT_CLIENT_MS, 1, "", &c2));
    CHECK(xact_focus_moved(&c2) == 1);
    CHECK(xact_take(t2, 2001, NULL, 0) == XACT_OK);
    CHECK(xact_take(t1, 2001, NULL, 0) == XACT_DENIED);
    snprintf(t1, sizeof t1, "%s", xact_issue_for(2000, XACT_CLIENT_MS, 1, "", &c1));
    CHECK(xact_focus_moved(NULL) == 1);
    CHECK(xact_take(t1, 2001, NULL, 0) == XACT_DENIED);

    /* an app's token lives at most XACT_CLIENT_MS (at most 10 s), whatever it was asked for */
    CHECK(XACT_CLIENT_MS <= 10000);
    snprintf(t1, sizeof t1, "%s", xact_issue_for(3000, XACT_LAUNCH_MS, 1, "", &c1));
    CHECK(xact_take(t1, 3000 + XACT_CLIENT_MS, NULL, 0) == XACT_STALE);
    snprintf(t1, sizeof t1, "%s", xact_issue_for(3000, 0, 1, "", &c1));
    CHECK(xact_take(t1, 2999 + XACT_CLIENT_MS, NULL, 0) == XACT_OK);
    snprintf(t1, sizeof t1, "%s", xact_issue_for(3000, XACT_CLIENT_MS, 1, "", &c1));
    CHECK(xact_take(t1, 3000 + XACT_CLIENT_MS - 1, NULL, 0) == XACT_OK);
    CHECK(xact_take(t1, 3000 + XACT_CLIENT_MS - 1, NULL, 0) == XACT_UNKNOWN);   /* reused */

    /* the app left: its tokens stop granting, even if another app gets the same address */
    snprintf(t1, sizeof t1, "%s", xact_issue_for(4000, XACT_CLIENT_MS, 1, "", &c1));
    snprintf(launch, sizeof launch, "%s", xact_issue(4000, XACT_LAUNCH_MS, 1, ""));
    xact_client_gone(&c1);
    CHECK(xact_focus_moved(&c2) == 0);
    CHECK(xact_take(t1, 4001, NULL, 0) == XACT_DENIED);
    CHECK(xact_take(launch, 4001, NULL, 0) == XACT_OK);
    xact_client_gone(NULL);

    if (fails) { printf("xdgact-test: %d failure(s)\n", fails); return 1; }
    printf("xdgact-test: ok\n");
    return 0;
}
