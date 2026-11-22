/* drmlease_test.c — src/wl/drmlcore.c against a fake DRM layer: only non-desktop connectors are offered, who may lease,
 * the "vr" prompt (once, always, deny), and every way a lease ends (docs/WAYLAND.md, "DRM leases"). */
#include <stdio.h>
#include <string.h>
#include "wl/drmlcore.h"

static int fails;
static void check(const char *what, long got, long want) {
    if (got == want) printf("ok   %s\n", what);
    else { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); }
}

/* ---- the fake card: connectors, CRTCs (4), leases in the kernel ---- */
static DrmlConn card[6];
static int ncard;
static uint32_t kernel_leases[16];     /* lessee ids alive in the "kernel" */
static int nkl, next_lessee = 100, next_fd = 40, master = 1, crtc_count = 4;
static uint32_t heads = 0x1;           /* CRTC 0 drives LordOS's own display */
static uint32_t reserved;
static int perm_state = DRML_ASK, asks, offers, granted_fd, finished_n, revokes, revoke_fail;
static void *last_finished, *last_granted;
static char asked_app[64], asked_headset[96];

static void add_conn(uint32_t id, int connected, int non_desktop, const char *name, const char *desc) {
    DrmlConn *c = &card[ncard++];
    memset(c, 0, sizeof *c);
    c->id = id;
    c->connected = connected;
    c->non_desktop = non_desktop;
    snprintf(c->name, sizeof c->name, "%s", name);
    snprintf(c->desc, sizeof c->desc, "%s", desc);
}
static void del_conn(uint32_t id) {
    for (int i = 0; i < ncard; i++) if (card[i].id == id) { card[i] = card[--ncard]; return; }
}
static int alive(uint32_t lessee) {
    for (int i = 0; i < nkl; i++) if (kernel_leases[i] == lessee) return 1;
    return 0;
}

static int f_scan(void *u, DrmlConn *out, int max) {
    (void)u;
    int n = ncard < max ? ncard : max;
    memcpy(out, card, sizeof card[0] * (size_t)n);
    return n;
}
static int f_lease(void *u, const uint32_t *conns, int n, uint32_t avoid, uint32_t *lessee, uint32_t *crtcs) {
    (void)u; (void)conns;
    if (!master) return -1;
    uint32_t got = 0;
    for (int j = 0; j < n; j++) {
        int k = -1;
        for (int c = 0; c < crtc_count && k < 0; c++) if (!((heads | avoid | got) >> c & 1)) k = c;
        if (k < 0) return -1;
        got |= 1u << k;
    }
    *lessee = (uint32_t)next_lessee++;
    *crtcs = got;
    kernel_leases[nkl++] = *lessee;
    return next_fd++;
}
static int f_revoke(void *u, uint32_t lessee) {
    (void)u;
    revokes++;
    if (!master) { revoke_fail++; return -1; }
    for (int i = 0; i < nkl; i++) if (kernel_leases[i] == lessee) { kernel_leases[i] = kernel_leases[--nkl]; return 0; }
    return -1;
}
static void f_reserve(void *u, uint32_t crtcs) { (void)u; reserved = crtcs; }
static int f_perm(void *u, const char *app) { (void)u; (void)app; return perm_state; }
static void f_granted(void *u, void *token, int fd) { (void)u; last_granted = token; granted_fd = fd; }
static void f_finished(void *u, void *token) { (void)u; last_finished = token; finished_n++; }
static void f_offers(void *u) { (void)u; offers++; }
static void f_ask(void *u, const char *app, const DrmlConn *c) {
    (void)u;
    asks++;
    snprintf(asked_app, sizeof asked_app, "%s", app);
    snprintf(asked_headset, sizeof asked_headset, "%s", c ? c->desc : "");
}
static const DrmlOps ops = { f_scan, f_lease, f_revoke, f_reserve, f_perm, f_granted, f_finished, f_offers, f_ask, NULL };

static Drml D;
static void reset(void) {
    ncard = nkl = 0;
    master = 1;
    perm_state = DRML_ASK;
    asks = offers = granted_fd = finished_n = revokes = revoke_fail = 0;
    last_finished = last_granted = NULL;
    reserved = 0;
    add_conn(10, 1, 0, "DP-1", "DELL U2720Q");          /* LordOS's own display */
    add_conn(11, 0, 0, "HDMI-A-1", "HDMI-A-1");         /* nothing plugged in */
    add_conn(12, 1, 1, "DP-2", "Valve Index");          /* a VR headset */
    drml_init(&D, &ops);
    drml_card(&D, 1);
}

#define APP "program:/usr/bin/monado-service"

int main(void) {
    int t1, t2, t3;                                     /* tokens: any distinct addresses */

    /* the pure decisions */
    check("decide: no decision yet asks", drml_decide(DRML_ASK, 1, 1), DRML_PROMPT);
    check("decide: always allowed leases", drml_decide(DRML_ALLOW, 1, 1), DRML_GRANT);
    check("decide: denied never", drml_decide(DRML_DENY, 1, 1), DRML_NEVER);
    check("decide: sandboxed, isolated, Xwayland or the login screen never, even allowed", drml_decide(DRML_ALLOW, 0, 1), DRML_NEVER);
    check("decide: no identity never, and no prompt", drml_decide(DRML_ASK, 1, 0), DRML_NEVER);
    DrmlConn c = { .connected = 1, .non_desktop = 0 };
    check("offerable: a desktop display never", drml_offerable(&c), 0);
    c.non_desktop = 1;
    check("offerable: a plugged-in non-desktop one", drml_offerable(&c), 1);
    c.connected = 0;
    check("offerable: an unplugged one never", drml_offerable(&c), 0);

    /* what is offered */
    reset();
    check("one headset present", drml_present(&D), 1);
    check("the headset is offered", drml_offered(&D, 12), 1);
    check("LordOS's own display is never offered", drml_offered(&D, 10), 0);
    check("an empty connector is never offered", drml_offered(&D, 11), 0);
    check("asking for LordOS's display: finished at once", drml_submit(&D, &t1, APP, (uint32_t[]){ 10 }, 1, 1), DRML_NEVER);
    check("... and no prompt", asks, 0);

    /* the prompt, then "allow once" */
    reset();
    check("first request waits for the person", drml_submit(&D, &t1, APP, (uint32_t[]){ 12 }, 1, 1), DRML_PROMPT);
    check("... one prompt", asks, 1);
    check("... naming the headset", !strcmp(asked_headset, "Valve Index"), 1);
    check("... and the app's identity", !strcmp(asked_app, APP), 1);
    check("nothing leased before the answer", nkl, 0);
    check("a second request of the same app: no second prompt", drml_submit(&D, &t2, APP, (uint32_t[]){ 12 }, 1, 1), DRML_PROMPT);
    check("... still one prompt", asks, 1);
    check("an answer for another app is ignored", drml_answer(&D, "program:/usr/bin/other", DRML_ANSWER_ONCE), 0);
    check("allow once: taken", drml_answer(&D, APP, DRML_ANSWER_ONCE), 1);
    check("... the first request is leased", last_granted == &t1 || last_granted == &t2, 1);
    check("... one kernel lease (the second finds the headset taken)", nkl, 1);
    check("... the second request finished", finished_n, 1);
    check("... the leased CRTC is reserved from LordOS's heads", reserved != 0 && !(reserved & heads), 1);
    check("... the headset is withdrawn while leased", drml_offered(&D, 12), 0);
    check("an answer when nothing waits is ignored", drml_answer(&D, APP, DRML_ANSWER_ALWAYS), 0);
    void *holder = last_granted;
    drml_destroy(&D, holder);
    check("the client destroys its lease: revoked in the kernel", nkl, 0);
    check("... nothing reserved", reserved, 0);
    check("... the headset offered again", drml_offered(&D, 12), 1);
    check("... no finished to a destroyed object", finished_n, 1);

    /* allow once is not saved: the next request asks again */
    check("after allow once the next request asks again", drml_submit(&D, &t3, APP, (uint32_t[]){ 12 }, 1, 1), DRML_PROMPT);
    check("... a new prompt", asks, 2);
    check("deny: taken", drml_answer(&D, APP, DRML_ANSWER_DENY), 1);
    check("... finished, nothing leased", finished_n == 2 && nkl == 0, 1);

    /* always allowed: no prompt; taken back in Settings */
    reset();
    perm_state = DRML_ALLOW;
    check("always allowed: leased at once", drml_submit(&D, &t1, APP, (uint32_t[]){ 12 }, 1, 1), DRML_GRANT);
    check("... no prompt", asks, 0);
    check("... lease_fd sent", last_granted == &t1 && granted_fd >= 40, 1);
    perm_state = DRML_ASK;
    drml_perm_changed(&D);
    check("set back to ask in Settings: revoked", nkl, 0);
    check("... finished", last_finished == &t1, 1);

    /* allow once survives "ask", not "deny" */
    reset();
    drml_submit(&D, &t1, APP, (uint32_t[]){ 12 }, 1, 1);
    drml_answer(&D, APP, DRML_ANSWER_ONCE);
    drml_perm_changed(&D);
    check("allowed once, the decision still ask: kept", nkl, 1);
    perm_state = DRML_DENY;
    drml_perm_changed(&D);
    check("denied in Settings: revoked", nkl, 0);
    check("... finished", last_finished == &t1, 1);
    check("denied: a new request finishes at once, no prompt", drml_submit(&D, &t2, APP, (uint32_t[]){ 12 }, 1, 1), DRML_NEVER);
    check("... no prompt", asks, 1);

    /* a waiting request follows a decision made in Settings */
    reset();
    drml_submit(&D, &t1, APP, (uint32_t[]){ 12 }, 1, 1);
    perm_state = DRML_ALLOW;
    drml_perm_changed(&D);
    check("allowed in Settings while the prompt is up: leased", nkl == 1 && last_granted == &t1, 1);

    /* who may never lease */
    reset();
    perm_state = DRML_ALLOW;
    check("an untrusted client (sandboxed, isolated, Xwayland, login screen): finished", drml_submit(&D, &t1, APP, (uint32_t[]){ 12 }, 1, 0), DRML_NEVER);
    check("... no lease, no prompt", nkl == 0 && asks == 0, 1);
    check("no identity: finished", drml_submit(&D, &t1, "", (uint32_t[]){ 12 }, 1, 1), DRML_NEVER);
    check("too many connectors: finished", drml_submit(&D, &t1, APP, (uint32_t[]){ 12, 12, 12, 12, 12 }, 5, 1), DRML_NEVER);

    /* unplugged */
    reset();
    perm_state = DRML_ALLOW;
    drml_submit(&D, &t1, APP, (uint32_t[]){ 12 }, 1, 1);
    del_conn(12);
    offers = 0;
    drml_rescan(&D);
    check("headset unplugged: revoked", nkl, 0);
    check("... finished", last_finished == &t1, 1);
    check("... nothing present (the global goes)", drml_present(&D), 0);
    check("... the offers changed", offers > 0, 1);
    add_conn(12, 1, 1, "DP-2", "Valve Index");
    drml_rescan(&D);
    check("plugged in again: offered", drml_offered(&D, 12), 1);
    /* a waiting request whose headset goes */
    perm_state = DRML_ASK;
    drml_submit(&D, &t2, APP, (uint32_t[]){ 12 }, 1, 1);
    del_conn(12);
    drml_rescan(&D);
    check("unplugged while the prompt is up: finished", last_finished == &t2, 1);
    check("... a late answer is ignored", drml_answer(&D, APP, DRML_ANSWER_ONCE), 0);
    /* the kernel stops calling it non-desktop (a quirk changed): never offered again, the lease ends */
    reset();
    perm_state = DRML_ALLOW;
    drml_submit(&D, &t1, APP, (uint32_t[]){ 12 }, 1, 1);
    card[2].non_desktop = 0;
    drml_rescan(&D);
    check("no longer non-desktop: revoked", nkl, 0);
    check("... not offered", drml_offered(&D, 12), 0);

    /* the session leaves the monitor */
    reset();
    perm_state = DRML_ALLOW;
    drml_submit(&D, &t1, APP, (uint32_t[]){ 12 }, 1, 1);
    drml_card(&D, 0);
    check("lock, user switch or text console while master: revoked", nkl, 0);
    check("... finished", last_finished == &t1, 1);
    check("... nothing offered while away", drml_offered(&D, 12) || drml_present(&D), 0);
    check("... a request while away finishes", drml_submit(&D, &t2, APP, (uint32_t[]){ 12 }, 1, 1), DRML_NEVER);
    drml_card(&D, 1);
    check("back on the monitor: offered again", drml_offered(&D, 12), 1);
    /* under lord-sessiond master is dropped before the notice: the revoke fails, the lease still ends for the client */
    drml_submit(&D, &t1, APP, (uint32_t[]){ 12 }, 1, 1);
    master = 0;
    finished_n = 0;
    drml_card(&D, 0);
    check("master already gone: the revoke is tried", revoke_fail, 1);
    check("... and the client is told finished", finished_n, 1);
    check("... nothing reserved", reserved, 0);

    /* no free CRTC */
    reset();
    perm_state = DRML_ALLOW;
    heads = 0xf;
    check("every CRTC drives LordOS's displays: finished", drml_submit(&D, &t1, APP, (uint32_t[]){ 12 }, 1, 1), DRML_NEVER);
    check("... no lease", nkl, 0);
    heads = 0x1;

    /* two headsets, two apps: separate CRTCs */
    reset();
    add_conn(13, 1, 1, "DP-3", "Second headset");
    drml_rescan(&D);
    perm_state = DRML_ALLOW;
    drml_submit(&D, &t1, APP, (uint32_t[]){ 12 }, 1, 1);
    uint32_t first = reserved;
    drml_submit(&D, &t2, "program:/usr/bin/other", (uint32_t[]){ 13 }, 1, 1);
    check("two leases", nkl, 2);
    check("... on different CRTCs, none of LordOS's", (first & (reserved & ~first)) == 0 && !(reserved & heads) && reserved != first, 1);
    drml_destroy(&D, &t1);
    check("one ends: the other is still leased", nkl == 1 && alive((uint32_t)next_lessee - 1), 1);

    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
