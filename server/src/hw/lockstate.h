/* lockstate.h — what the desktop does with logind's Lock and Unlock and its LockedHint (pure, no bus;
 * hw/logindlock.c drives it, tests/logindlock_test.c tests both).
 *
 * `loginctl lock-session` / `lock-sessions` (and any program or admin that calls logind's LockSession)
 * make logind emit Lock on the session's org.freedesktop.login1.Session object; logind allows that only
 * to root, the session's owner or whoever its polkit rules let through. The desktop then locks as ❖L does.
 *
 *   - The desktop's own logind session (GetSessionByPID of lord-server): a Lock for exactly that path.
 *   - In none (lord-sessiond's sessions are not logind sessions today, docs/SESSIONS.md): a Lock for any
 *     session of the same user (asked with Properties.Get "User"), so `loginctl lock-sessions` and
 *     `loginctl lock-session <id>` of one of the user's sessions lock the desktop.
 *   - Unlock is never obeyed: only the password on the login screen gives the monitor back (lord-sessiond,
 *     root). Unlocking from logind would be a new way for root to hand over the screen: not without the
 *     maintainer's review (docs/SESSIONS.md).
 *   - LockedHint (SetLockedHint, allowed to the session's owner) follows the lock in the own session only:
 *     true once a lock is sent from the monitor, false when the monitor shows the session again. */
#ifndef LORD_HW_LOCKSTATE_H
#define LORD_HW_LOCKSTATE_H

#include <stdint.h>

#define LS_PATH_MAX 128

typedef struct {
    char own[LS_PATH_MAX];      /* this desktop's logind session object path; "" while it is in none */
    int locked;                 /* a lock was sent from the monitor and the session was not shown since */
    int hint;                   /* the LockedHint last sent: -1 none yet */
} LockState;

enum { LS_IGNORE, LS_LOCK, LS_ASK_OWNER, LS_UNLOCK_REFUSED };

void ls_init(LockState *s);
/* a logind session object path: /org/freedesktop/login1/session/<escaped id> */
int  ls_session_path(const char *p);
/* GetSessionByPID's answer (NULL or "" none). Returns the LockedHint to send (0/1) or -1. */
int  ls_own(LockState *s, const char *path);
/* logind (its own connection: the caller checks the sender) emitted member ("Lock", "Unlock") on path:
 * LS_LOCK, LS_ASK_OWNER (in no session of its own: lock if that session's user is this one), LS_IGNORE or
 * LS_UNLOCK_REFUSED. */
int  ls_signal(const LockState *s, const char *member, const char *path);
/* the answer to LS_ASK_OWNER: 1 lock */
int  ls_owner_is_me(uint32_t session_uid, uint32_t my_uid);
/* a lock was sent (lord.session.lock). Returns the LockedHint to send (1) or -1. */
int  ls_locked(LockState *s, int on_monitor);
/* the monitor shows the session again (RP_DISPLAY). Returns the LockedHint to send (0) or -1. */
int  ls_shown(LockState *s);

#endif
