/* sleeplock_test.c — the lock before sleep (src/hw/sleeplock.c, issue #36), with the system bus, the
 * display and the theme replaced by this file:
 *   - the delay inhibitor is asked for once logind has an owner, and kept (its fd) from the reply;
 *   - PrepareForSleep(true) from logind tells the theme "suspend"; a lock asked for keeps the inhibitor
 *     until the monitor is taken, or at most 4 s; no lock lets it go at once;
 *   - a session that is not on the screen is not told and does not delay the sleep;
 *   - PrepareForSleep(false) tells "resume" with the seconds asleep, and asks for a new inhibitor;
 *   - the signal from anyone but logind's own connection, or with the wrong signature, is ignored. */
#include "hw/sleeplock.h"
#include "hw/sysbus.h"
#include "hw/display.h"
#include "hw/hw.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails, total;
static void check(int ok, const char *what) { total++; if (!ok) { fails++; printf("FAIL: %s\n", what); } }

/* ---- the system bus, replaced ---- */
static SbSignal sig_fn;
static SbOwner owner_fn;
static SbReply reply_fn;
static int inhibit_calls, matches;
static char inhibit_args[160];

int sysbus_up(void) { return 1; }
void sysbus_on_signal(SbSignal fn) { sig_fn = fn; }
void sysbus_add_match(const char *rule) { if (strstr(rule, "member='PrepareForSleep'") && strstr(rule, "sender='org.freedesktop.login1'")) matches++; }
void sysbus_watch_name(const char *name, SbOwner fn) { if (!strcmp(name, "org.freedesktop.login1")) owner_fn = fn; }
uint32_t sysbus_call(const char *dest, const char *path, const char *iface, const char *member,
                     const char *sig, const DBuf *body, SbReply cb, void *ctx) {
    (void)ctx;
    if (strcmp(dest, "org.freedesktop.login1") || strcmp(path, "/org/freedesktop/login1") ||
        strcmp(iface, "org.freedesktop.login1.Manager") || strcmp(member, "Inhibit") || strcmp(sig, "ssss")) return 0;
    inhibit_calls++;
    reply_fn = cb;
    /* the four strings, for the check below */
    DMsg m = { .sig = "ssss", .body = body->p, .body_len = (uint32_t)body->n };
    DRd r;
    dr_init(&r, &m);
    const char *what = dr_str(&r, NULL), *who = dr_str(&r, NULL), *why = dr_str(&r, NULL), *mode = dr_str(&r, NULL);
    snprintf(inhibit_args, sizeof inhibit_args, "%s|%s|%s|%s", what ? what : "", who ? who : "", why ? why : "", mode ? mode : "");
    return (uint32_t)inhibit_calls;
}

/* ---- the display and the theme, replaced ---- */
static int on_monitor = 1, theme_locks = 1;
static char last_idle[96];
static int idle_events;
int display_on_monitor(void) { return on_monitor; }
double display_dark_ms(double now) { (void)now; return 2000; }
void hw_emit(const char *kind, const char *json) {
    if (strcmp(kind, "idle")) return;
    idle_events++;
    snprintf(last_idle, sizeof last_idle, "%s", json);
    if (strstr(json, "\"suspend\"") && theme_locks) sleeplock_lock_sent();   /* lord.session.lock() */
}

/* logind answers Inhibit with the read end of a pipe; the test keeps the write end to see it closed */
static int give_inhibitor(int *wr) {
    int pp[2];
    if (pipe(pp)) return -1;
    uint8_t body[4] = { 0, 0, 0, 0 };
    DMsg m = { .type = DBUS_RETURN, .sig = "h", .body = body, .body_len = 4, .n_fds = 1 };
    m.fds[0] = pp[0];
    reply_fn(&m, NULL);
    close(pp[0]);                       /* as dbus_msg_free does */
    *wr = pp[1];
    return 0;
}

/* closed by sleeplock: the pipe's last read end is gone, so writing fails with EPIPE */
static int closed(int wr) { return write(wr, "x", 1) < 0; }

static void prepare(const char *sender, int sleeping, const char *sig) {
    DBuf b;
    dw_init(&b);
    if (!strcmp(sig, "b")) dw_bool(&b, sleeping);
    else dw_str(&b, "x");
    DHdr h = { .type = DBUS_SIGNAL, .serial = 9, .path = "/org/freedesktop/login1", .iface = "org.freedesktop.login1.Manager",
               .member = "PrepareForSleep", .sig = sig, .sender = sender };
    DBuf raw;
    DMsg m;
    if (!dbus_build(&h, &b, &raw) && !dbus_msg_parse(raw.p, raw.n, &m)) { sig_fn(&m); dbus_msg_free(&m); }
    else check(0, "building PrepareForSleep");
    dw_free(&raw);
    dw_free(&b);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    sleeplock_start();
    check(sig_fn && owner_fn && matches == 1, "subscribed to logind's PrepareForSleep and its owner");
    check(inhibit_calls == 0, "no inhibitor before logind has an owner");
    owner_fn("org.freedesktop.login1", ":1.4");
    check(inhibit_calls == 1 && !strcmp(inhibit_args, "sleep|LordOS|Locking the screen before sleep|delay"),
          "a delay sleep inhibitor is asked for");
    int wr = -1;
    check(give_inhibitor(&wr) == 0 && sleeplock_inhibitor() >= 0 && !closed(wr), "the inhibitor's fd is kept");

    /* not logind: ignored */
    prepare(":1.99", 1, "b");
    check(idle_events == 0 && sleeplock_inhibitor() >= 0, "PrepareForSleep from another connection is ignored");
    prepare(":1.4", 1, "s");
    check(idle_events == 0, "PrepareForSleep with the wrong signature is ignored");

    /* going to sleep, the theme locks: the inhibitor holds until the monitor is taken */
    prepare(":1.4", 1, "b");
    check(idle_events == 1 && strstr(last_idle, "\"suspend\""), "the theme hears suspend");
    check(sleeplock_inhibitor() >= 0 && !closed(wr), "a lock was asked for: the sleep waits");
    check(sleeplock_timeout(0) > 0, "with a deadline");
    sleeplock_monitor_taken();
    check(sleeplock_inhibitor() < 0 && closed(wr), "the monitor taken: the inhibitor is let go");
    close(wr);

    /* waking: resume with the seconds asleep (+ 2 s dark before), and a new inhibitor */
    prepare(":1.4", 0, "b");
    check(idle_events == 2 && strstr(last_idle, "\"resume\"") && strstr(last_idle, "\"slept\":2"), "the theme hears resume and how long");
    check(inhibit_calls == 2, "a new inhibitor for the next sleep");
    check(give_inhibitor(&wr) == 0 && sleeplock_inhibitor() >= 0, "kept again");

    /* the monitor is never taken: let go after at most 4 s */
    prepare(":1.4", 1, "b");
    double left = sleeplock_timeout(0);
    check(left > 0 && sleeplock_inhibitor() >= 0, "holding");
    sleeplock_tick(left + 1e9);
    check(sleeplock_inhibitor() < 0 && closed(wr), "let go at the deadline (logind's 5 s maximum is never reached)");
    close(wr);
    prepare(":1.4", 0, "b");
    give_inhibitor(&wr);

    /* no lock wanted ("Never"): the sleep is not delayed */
    theme_locks = 0;
    prepare(":1.4", 1, "b");
    check(sleeplock_inhibitor() < 0 && closed(wr) && sleeplock_timeout(0) < 0, "no lock: the inhibitor goes at once");
    close(wr);
    prepare(":1.4", 0, "b");
    give_inhibitor(&wr);

    /* a session that is not on the screen is not told and does not delay the sleep */
    theme_locks = 1;
    on_monitor = 0;
    int before = idle_events;
    prepare(":1.4", 1, "b");
    check(idle_events == before && sleeplock_inhibitor() < 0 && closed(wr), "not on the screen: nothing to lock, no delay");
    close(wr);
    prepare(":1.4", 0, "b");
    check(idle_events == before, "not on the screen: no resume either");

    /* logind restarts: a new owner, a new inhibitor */
    int calls = inhibit_calls;
    reply_fn(NULL, NULL);               /* the pending one: the bus went away */
    owner_fn("org.freedesktop.login1", ":1.50");
    check(inhibit_calls == calls + 1, "logind's new owner: asked again");
    on_monitor = 1;
    prepare(":1.4", 1, "b");
    check(idle_events == before, "the old owner's signal is ignored");

    printf("sleeplock-test: %d/%d\n", total - fails, total);
    return fails ? 1 : 0;
}
