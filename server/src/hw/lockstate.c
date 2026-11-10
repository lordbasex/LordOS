/* lockstate.c — logind's Lock, Unlock and LockedHint for the desktop (see lockstate.h). */
#include "lockstate.h"

#include <stdio.h>
#include <string.h>

#define SESSION_BASE "/org/freedesktop/login1/session/"

void ls_init(LockState *s) {
    s->own[0] = 0;
    s->locked = 0;
    s->hint = -1;
}

int ls_session_path(const char *p) {
    size_t b = sizeof SESSION_BASE - 1;
    if (!p || strncmp(p, SESSION_BASE, b) || !p[b] || strlen(p) >= LS_PATH_MAX) return 0;
    for (const char *c = p + b; *c; c++)          /* logind escapes ids to [A-Za-z0-9_] */
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) return 0;
    return 1;
}

/* the hint logind should have now, or -1 if it already has it (or there is no session to set it on) */
static int want(LockState *s) {
    if (!s->own[0] || s->hint == s->locked) return -1;
    s->hint = s->locked;
    return s->hint;
}

int ls_own(LockState *s, const char *path) {
    snprintf(s->own, sizeof s->own, "%s", ls_session_path(path) ? path : "");
    s->hint = -1;
    return s->locked ? want(s) : -1;             /* logind starts every session unlocked */
}

int ls_signal(const LockState *s, const char *member, const char *path) {
    if (!member || !ls_session_path(path)) return LS_IGNORE;
    int lock = !strcmp(member, "Lock"), unlock = !strcmp(member, "Unlock");
    if (!lock && !unlock) return LS_IGNORE;
    if (s->own[0] && strcmp(path, s->own)) return LS_IGNORE;      /* another session's */
    if (unlock) return LS_UNLOCK_REFUSED;
    return s->own[0] ? LS_LOCK : LS_ASK_OWNER;
}

int ls_owner_is_me(uint32_t session_uid, uint32_t my_uid) { return session_uid == my_uid; }

int ls_locked(LockState *s, int on_monitor) {
    if (!on_monitor) return -1;                  /* off the monitor it is behind the login screen already */
    s->locked = 1;
    return want(s);
}

int ls_shown(LockState *s) {
    s->locked = 0;
    return want(s);
}
