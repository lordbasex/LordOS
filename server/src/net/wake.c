/* wake.c — Wi-Fi that looks connected after the computer wakes from sleep but carries nothing.
 *
 * Some radios (issue #14: the built-in card of a 2015 laptop) lose their association while
 * asleep and come back with the link "up": wpa_supplicant and the kernel never notice, there is no
 * carrier change, and nothing reconnects until the person turns the card off and on. lord-netd hears
 * the computer wake (logind's PrepareForSleep(false), or a jump of CLOCK_BOOTTIME over
 * CLOCK_MONOTONIC) and runs this for each Wi-Fi interface it manages:
 *
 *   1. after WAKE_SETTLE_MS, once per second: is it associated, with carrier, an address, and does
 *      its gateway answer ARP? Working: the DHCP client confirms its lease (the lease's timers run
 *      on CLOCK_MONOTONIC, which stood still while asleep) and that's it.
 *   2. still not working WAKE_REASSOC_MS after waking: wpa_supplicant re-associates and DHCP renews.
 *   3. still not working WAKE_RESET_MS after that: what the person did by hand, the card off and on
 *      (daemons stopped, link down, then the saved config as at boot).
 *   4. still not working WAKE_GIVEUP_MS after that: it says so in the log and stops.
 *
 * Every step is logged. No system calls here: actions.c supplies them, tests/net_test.c fakes them. */
#include "wake.h"

#include <stdio.h>
#include <string.h>

const char *wake_why(int why) {
    switch (why) {
    case WK_OK: return "ok";
    case WK_NOT_ASSOCIATED: return "not-associated";
    case WK_NO_CARRIER: return "no-carrier";
    case WK_NO_ADDRESS: return "no-address";
    case WK_NO_GATEWAY: return "gateway-unreachable";
    }
    return "unknown";
}

void wake_begin(WakeTable *t, const char *const *names, int n, long ms) {
    t->n = 0;
    for (int i = 0; i < n && t->n < WAKE_MAX; i++) {
        WakeIf *e = &t->v[t->n++];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", names[i]);
        e->phase = WS_WATCH;
        e->since_ms = ms;
        e->next_ms = ms + WAKE_SETTLE_MS;
    }
}

/* 1: done with it */
static int step_one(WakeIf *e, const WakeOps *o, long ms) {
    if (ms < e->next_ms) return 0;
    if (e->phase == WS_STOPPING) {
        if (o->busy(e->name) && ms - e->since_ms < WAKE_STOP_MS) { e->next_ms = ms + 200; return 0; }
        o->reset_start(e->name);
        e->phase = WS_RESET;
        e->since_ms = ms;
        e->next_ms = ms + WAKE_SETTLE_MS;
        return 0;
    }
    if (!o->active(e->name)) { o->log(e->name, "skipped", WK_OK); return 1; }    /* turned off meanwhile */
    int h = o->health(e->name);
    if (h == WK_OK) {
        if (e->phase == WS_WATCH) { o->renew(e->name); o->log(e->name, "ok", WK_OK); }
        else o->log(e->name, e->phase == WS_REASSOC ? "recovered-reassociate" : "recovered-reset", e->why);
        return 1;
    }
    e->why = h;
    long age = ms - e->since_ms;
    if (e->phase == WS_WATCH && age >= WAKE_REASSOC_MS) {
        o->log(e->name, "reassociate", h);
        o->reassociate(e->name);
        e->phase = WS_REASSOC;
        e->since_ms = ms;
        e->next_ms = ms + WAKE_SETTLE_MS;
        return 0;
    }
    if (e->phase == WS_REASSOC && age >= WAKE_RESET_MS) {
        o->log(e->name, "reset", h);
        o->reset_stop(e->name);
        e->phase = WS_STOPPING;
        e->since_ms = ms;
        e->next_ms = ms + 200;
        return 0;
    }
    if (e->phase == WS_RESET && age >= WAKE_GIVEUP_MS) { o->log(e->name, "failed", h); return 1; }
    e->next_ms = ms + WAKE_CHECK_MS;
    return 0;
}

long wake_step(WakeTable *t, const WakeOps *o, long ms) {
    for (int i = 0; i < t->n;) {
        if (step_one(&t->v[i], o, ms)) t->v[i] = t->v[--t->n];
        else i++;
    }
    long next = -1;
    for (int i = 0; i < t->n; i++) if (next < 0 || t->v[i].next_ms < next) next = t->v[i].next_ms;
    return next;
}
