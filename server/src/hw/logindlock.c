/* logindlock.c — logind's Lock locks the desktop; LockedHint follows the lock (see logindlock.h). */
#include "logindlock.h"
#include "lockstate.h"
#include "hw.h"
#include "sysbus.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LOGIN1      "org.freedesktop.login1"
#define LOGIN1_PATH "/org/freedesktop/login1"
#define LOGIN1_MGR  "org.freedesktop.login1.Manager"
#define LOGIN1_SESS "org.freedesktop.login1.Session"

static LockState st;
static char owner[128];                 /* logind's unique name; "" while it has none */
static char asked[LS_PATH_MAX];         /* the session whose User is being asked */
static int asking_owner, hint_failed;

static void got_hint(const DMsg *m, void *ctx) {
    (void)ctx;
    if (m && m->type == DBUS_ERROR && !hint_failed++)
        fprintf(stderr, "lock: logind refused LockedHint (%s)\n", m->error ? m->error : "?");
}

static void send_hint(int h) {
    if (h < 0 || !owner[0]) return;
    DBuf b;
    dw_init(&b);
    dw_bool(&b, h);
    sysbus_call(LOGIN1, st.own, LOGIN1_SESS, "SetLockedHint", "b", &b, got_hint, NULL);
    dw_free(&b);
}

static void got_session(const DMsg *m, void *ctx) {
    (void)ctx;
    const char *path = NULL;
    DRd r;
    if (m && m->type == DBUS_RETURN && m->sig && !strcmp(m->sig, "o")) {
        dr_init(&r, m);
        path = dr_str(&r, NULL);
        if (r.err) path = NULL;
    }
    send_hint(ls_own(&st, path));
    if (st.own[0]) fprintf(stderr, "lock: logind's Lock of session %s locks the screen\n", st.own);
    else if (m) fprintf(stderr, "lock: not in a logind session: logind's Lock of any of this user's sessions locks the screen\n");
}

static void find_session(void) {
    DBuf b;
    dw_init(&b);
    dw_u32(&b, (uint32_t)getpid());
    sysbus_call(LOGIN1, LOGIN1_PATH, LOGIN1_MGR, "GetSessionByPID", "u", &b, got_session, NULL);
    dw_free(&b);
}

static void lock_now(const char *path) {
    if (st.locked) return;              /* already behind the login screen */
    fprintf(stderr, "lock: logind asked to lock session %s: locking the screen\n", path);
    hw_emit("idle", "{\"stage\":\"lock\",\"from\":\"logind\"}");     /* the theme locks (sdk/hardware.jsx) */
}

/* Properties.Get(Session, "User") answers v (uo): the session's user */
static void got_user(const DMsg *m, void *ctx) {
    (void)ctx;
    asking_owner = 0;
    if (!m || m->type != DBUS_RETURN || !m->sig || strcmp(m->sig, "v")) return;
    DRd r;
    dr_init(&r, m);
    const char *vs = dr_variant(&r);
    if (r.err || !vs || strcmp(vs, "(uo)")) return;
    dr_struct(&r);
    uint32_t uid = dr_u32(&r);
    const char *upath = dr_str(&r, NULL);
    if (r.err || !upath) return;
    if (ls_owner_is_me(uid, (uint32_t)getuid())) lock_now(asked);
}

static void ask_user(const char *path) {
    if (asking_owner) return;           /* one at a time: a lock is already on its way */
    DBuf b;
    dw_init(&b);
    dw_str(&b, LOGIN1_SESS);
    dw_str(&b, "User");
    if (sysbus_call(LOGIN1, path, "org.freedesktop.DBus.Properties", "Get", "ss", &b, got_user, NULL)) {
        asking_owner = 1;
        snprintf(asked, sizeof asked, "%s", path);
    }
    dw_free(&b);
}

static void on_signal(const DMsg *m) {
    if (m->type != DBUS_SIGNAL || !m->iface || strcmp(m->iface, LOGIN1_SESS) || !m->member || !m->path ||
        (m->sig && *m->sig) || !m->sender || !owner[0] || strcmp(m->sender, owner))
        return;                         /* only logind itself, and Lock/Unlock carry no arguments */
    switch (ls_signal(&st, m->member, m->path)) {
    case LS_LOCK: lock_now(m->path); break;
    case LS_ASK_OWNER: ask_user(m->path); break;
    case LS_UNLOCK_REFUSED:
        fprintf(stderr, "lock: logind asked to unlock session %s: ignored, only the password on the login screen unlocks\n", m->path);
        break;
    default: break;
    }
}

static void on_owner(const char *name, const char *o) {
    (void)name;
    snprintf(owner, sizeof owner, "%s", o ? o : "");
    asking_owner = 0;
    if (owner[0]) find_session();       /* a new logind: which session we are in, and its hint again */
    else ls_own(&st, NULL);
}

void logindlock_start(void) {
    if (!sysbus_up()) return;
    ls_init(&st);
    sysbus_on_signal(on_signal);
    sysbus_add_match("type='signal',sender='" LOGIN1 "',interface='" LOGIN1_SESS "',member='Lock'");
    sysbus_add_match("type='signal',sender='" LOGIN1 "',interface='" LOGIN1_SESS "',member='Unlock'");
    sysbus_watch_name(LOGIN1, on_owner);
}

void logindlock_lock_sent(int on_monitor) { send_hint(ls_locked(&st, on_monitor)); }

void logindlock_shown(void) { send_hint(ls_shown(&st)); }
