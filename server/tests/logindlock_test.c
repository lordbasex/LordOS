/* logindlock_test.c — logind's Lock, Unlock and LockedHint (src/hw/lockstate.c, src/hw/logindlock.c),
 * with the system bus and the theme replaced by this file:
 *   - the rules alone: session paths, the own session or any of the user's, Unlock refused, the hint
 *     sent only when it changes and only for the own session, nothing off the monitor;
 *   - on the bus: the own session is asked for with GetSessionByPID once logind has an owner; a Lock of
 *     that session from logind's own connection tells the theme "lock"; another session's, another
 *     sender's or one with arguments is ignored; SetLockedHint true on the lock, false when shown again;
 *   - in no logind session: a Lock of another session asks its "User" and locks only for this uid;
 *   - Unlock never unlocks; logind restarting asks the session again. */
#include "hw/lockstate.h"
#include "hw/logindlock.h"
#include "hw/sysbus.h"
#include "hw/hw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails, total;
static void check(int ok, const char *what) { total++; if (!ok) { fails++; printf("FAIL: %s\n", what); } }

#define OWN "/org/freedesktop/login1/session/_32"
#define OTHER "/org/freedesktop/login1/session/c1"

/* ---- the system bus, replaced ---- */
static SbSignal sig_fn;
static SbOwner owner_fn;
static int matches, by_pid, gets, hints, last_hint = -1;
static SbReply session_cb, user_cb;
static char hint_path[128], get_path[128];

int sysbus_up(void) { return 1; }
void sysbus_on_signal(SbSignal fn) { sig_fn = fn; }
void sysbus_add_match(const char *rule) {
    if (strstr(rule, "sender='org.freedesktop.login1'") && strstr(rule, "interface='org.freedesktop.login1.Session'") &&
        (strstr(rule, "member='Lock'") || strstr(rule, "member='Unlock'"))) matches++;
}
void sysbus_watch_name(const char *name, SbOwner fn) { if (!strcmp(name, "org.freedesktop.login1")) owner_fn = fn; }
uint32_t sysbus_call(const char *dest, const char *path, const char *iface, const char *member,
                     const char *sig, const DBuf *body, SbReply cb, void *ctx) {
    (void)ctx;
    if (strcmp(dest, "org.freedesktop.login1")) return 0;
    DMsg m = { .sig = sig, .body = body->p, .body_len = (uint32_t)body->n };
    DRd r;
    dr_init(&r, &m);
    if (!strcmp(member, "GetSessionByPID") && !strcmp(path, "/org/freedesktop/login1") && !strcmp(sig, "u")) {
        check(dr_u32(&r) == (uint32_t)getpid(), "GetSessionByPID asks for our own pid");
        by_pid++;
        session_cb = cb;
        return 1;
    }
    if (!strcmp(member, "SetLockedHint") && !strcmp(iface, "org.freedesktop.login1.Session") && !strcmp(sig, "b")) {
        hints++;
        last_hint = dr_bool(&r);
        snprintf(hint_path, sizeof hint_path, "%s", path);
        return 2;
    }
    if (!strcmp(member, "Get") && !strcmp(iface, "org.freedesktop.DBus.Properties") && !strcmp(sig, "ss")) {
        const char *i = dr_str(&r, NULL), *p = dr_str(&r, NULL);
        check(i && p && !strcmp(i, "org.freedesktop.login1.Session") && !strcmp(p, "User"), "Get asks for the session's User");
        gets++;
        user_cb = cb;
        snprintf(get_path, sizeof get_path, "%s", path);
        return 3;
    }
    return 0;
}

/* ---- the theme, replaced: it locks on idle "lock" ---- */
static int lock_events;
void hw_emit(const char *kind, const char *json) {
    if (!strcmp(kind, "idle") && strstr(json, "\"lock\"")) lock_events++;
}

static void signal_from(const char *sender, const char *member, const char *path, int with_arg) {
    DBuf b;
    dw_init(&b);
    if (with_arg) dw_bool(&b, 1);
    DHdr h = { .type = DBUS_SIGNAL, .serial = 9, .path = path, .iface = "org.freedesktop.login1.Session",
               .member = member, .sig = with_arg ? "b" : NULL, .sender = sender };
    DBuf raw;
    DMsg m;
    if (!dbus_build(&h, &b, &raw) && !dbus_msg_parse(raw.p, raw.n, &m)) { sig_fn(&m); dbus_msg_free(&m); }
    else check(0, "building the signal");
    dw_free(&raw);
    dw_free(&b);
}

static void reply_session(const char *path) {
    DBuf b;
    dw_init(&b);
    if (path) dw_str(&b, path);
    DMsg m = { .type = path ? DBUS_RETURN : DBUS_ERROR, .sig = path ? "o" : NULL, .body = b.p, .body_len = (uint32_t)b.n,
               .error = path ? NULL : "org.freedesktop.login1.NoSessionForPID" };
    session_cb(&m, NULL);
    dw_free(&b);
}

static void reply_user(uint32_t uid) {
    DBuf b;
    dw_init(&b);
    dw_variant(&b, "(uo)");
    dw_struct(&b);
    dw_u32(&b, uid);
    dw_str(&b, "/org/freedesktop/login1/user/_1000");
    DMsg m = { .type = DBUS_RETURN, .sig = "v", .body = b.p, .body_len = (uint32_t)b.n };
    user_cb(&m, NULL);
    dw_free(&b);
}

static void rules(void) {
    LockState s;
    ls_init(&s);
    check(ls_session_path(OWN) && ls_session_path(OTHER), "session paths");
    check(!ls_session_path("/org/freedesktop/login1/session/") && !ls_session_path("/org/freedesktop/login1/user/_1000") &&
          !ls_session_path("/org/freedesktop/login1/session/a/b") && !ls_session_path(NULL), "not session paths");
    /* in no session: any session's Lock asks its owner; the hint is never sent */
    check(ls_signal(&s, "Lock", OTHER) == LS_ASK_OWNER, "no own session: ask the owner");
    check(ls_signal(&s, "Unlock", OTHER) == LS_UNLOCK_REFUSED, "Unlock is refused");
    check(ls_signal(&s, "Terminate", OTHER) == LS_IGNORE && ls_signal(&s, "Lock", "/x") == LS_IGNORE, "others ignored");
    check(ls_owner_is_me(1000, 1000) && !ls_owner_is_me(1001, 1000), "the owner is this user");
    check(ls_locked(&s, 1) == -1 && ls_shown(&s) == -1, "no own session: no hint");
    /* the own session */
    check(ls_own(&s, OWN) == -1, "found, unlocked: nothing to send");
    check(ls_signal(&s, "Lock", OWN) == LS_LOCK && ls_signal(&s, "Lock", OTHER) == LS_IGNORE, "only the own session's Lock");
    check(ls_signal(&s, "Unlock", OWN) == LS_UNLOCK_REFUSED && ls_signal(&s, "Unlock", OTHER) == LS_IGNORE, "Unlock refused");
    check(ls_locked(&s, 0) == -1 && !s.locked, "off the monitor: not locked by us, no hint");
    check(ls_locked(&s, 1) == 1 && ls_locked(&s, 1) == -1, "locked: true, once");
    check(ls_shown(&s) == 0 && ls_shown(&s) == -1, "shown: false, once");
    ls_locked(&s, 1);
    check(ls_own(&s, OWN) == 1, "logind again while locked: true again");
    check(ls_own(&s, "bogus") == -1 && !s.own[0], "a bad path is no session");
}

int main(void) {
    rules();

    logindlock_start();
    check(sig_fn && owner_fn && matches == 2, "subscribed to logind's Lock and Unlock and its owner");
    check(by_pid == 0, "nothing asked before logind has an owner");
    owner_fn("org.freedesktop.login1", ":1.4");
    check(by_pid == 1, "GetSessionByPID once logind is there");

    /* in no logind session (today's lord-sessiond sessions) */
    reply_session(NULL);
    signal_from(":1.4", "Lock", OTHER, 0);
    check(gets == 1 && !strcmp(get_path, OTHER) && lock_events == 0, "another session's Lock: its User is asked");
    signal_from(":1.4", "Lock", OTHER, 0);
    check(gets == 1, "one question at a time");
    reply_user((uint32_t)getuid() + 1);
    check(lock_events == 0, "another user's session: no lock");
    signal_from(":1.4", "Lock", OTHER, 0);
    reply_user((uint32_t)getuid());
    check(lock_events == 1, "this user's session: the theme locks");
    logindlock_lock_sent(1);
    check(hints == 0, "no own session: no LockedHint");
    logindlock_shown();
    signal_from(":1.4", "Unlock", OTHER, 0);
    check(lock_events == 1 && gets == 2, "Unlock does nothing");

    /* logind restarts and the desktop is in a session of its own */
    owner_fn("org.freedesktop.login1", ":1.50");
    check(by_pid == 2, "a new logind: asked again");
    reply_session(OWN);
    signal_from(":1.4", "Lock", OWN, 0);
    check(lock_events == 1, "the old owner's Lock is ignored");
    signal_from(":1.99", "Lock", OWN, 0);
    check(lock_events == 1, "a Lock from another connection is ignored");
    signal_from(":1.50", "Lock", OWN, 1);
    check(lock_events == 1, "a Lock with arguments is ignored");
    signal_from(":1.50", "Lock", OTHER, 0);
    check(lock_events == 1 && gets == 2, "another session's Lock is ignored once we have our own");
    signal_from(":1.50", "Lock", OWN, 0);
    check(lock_events == 2, "our session's Lock: the theme locks");
    logindlock_lock_sent(1);
    check(hints == 1 && last_hint == 1 && !strcmp(hint_path, OWN), "SetLockedHint(true) on our session");
    signal_from(":1.50", "Lock", OWN, 0);
    check(lock_events == 2, "already locked: not again");
    signal_from(":1.50", "Unlock", OWN, 0);
    check(lock_events == 2 && hints == 1, "Unlock: still locked, the hint unchanged");
    logindlock_shown();
    check(hints == 2 && last_hint == 0, "shown again after the password: SetLockedHint(false)");
    logindlock_shown();
    check(hints == 2, "not sent twice");

    /* logind goes away: nothing is sent to nobody */
    owner_fn("org.freedesktop.login1", "");
    logindlock_lock_sent(1);
    check(hints == 2, "no logind: no hint");

    printf("logindlock-test: %d/%d\n", total - fails, total);
    return fails ? 1 : 0;
}
