/* admin_rescue.h — who may become an administrator on a system that has none.
 *
 * A Debian installed with a root password and later given LordOS by an old install.sh has nobody in
 * sudo, admin or wheel, and LordOS has no way to name an administrator from inside. The actions
 * admin-rescue-status and admin-rescue (users_admin.c) are that way out, approved by the maintainer
 * for exactly this case: with the root password, the session user joins `sudo`. The decision lives
 * here, without the system, so it can be tested (build/admin-rescue-test). */
#ifndef LORD_ADMIN_RESCUE_H
#define LORD_ADMIN_RESCUE_H

#include <sys/types.h>

typedef struct {
    int person;                         /* the session user is a person's account */
    int any_admin;                      /* a non-root account is in sudo, admin or wheel */
    int self_member;                    /* the session user is, by the system's files */
    int sudo_ready;                     /* the sudo group and the sudo program are there */
    int root_password;                  /* root has a password that can be typed */
} RescueFacts;

typedef enum {
    RESCUE_OFFER,                       /* the only state that lets the action go on */
    RESCUE_NOT_ALLOWED,
    RESCUE_HAS_ADMIN,
    RESCUE_RESTART,                     /* already added: waits for the restart */
    RESCUE_NO_SUDO,
    RESCUE_ROOT_LOCKED,
} RescueState;

RescueState rescue_state(const RescueFacts *f);
const char *rescue_word(RescueState s);         /* the state= of LOG users.rescueState */
const char *rescue_refusal(RescueState s);      /* its error code; NULL for RESCUE_OFFER */

int rescue_group_is_admin(const char *group);   /* sudo, admin or wheel */
/* Does this account count as an administrator? member: it is in one of those groups. Never root. */
int rescue_account_counts(uid_t uid, int member);

#endif
