/* admin_rescue.c — the decision of admin_rescue.h. */
#include "admin_rescue.h"

#include <string.h>

RescueState rescue_state(const RescueFacts *f) {
    if (!f->person) return RESCUE_NOT_ALLOWED;
    if (f->any_admin) return f->self_member ? RESCUE_RESTART : RESCUE_HAS_ADMIN;
    if (!f->sudo_ready) return RESCUE_NO_SUDO;
    if (!f->root_password) return RESCUE_ROOT_LOCKED;
    return RESCUE_OFFER;
}

const char *rescue_word(RescueState s) {
    switch (s) {
    case RESCUE_OFFER: return "offer";
    case RESCUE_RESTART: return "restart";
    case RESCUE_NO_SUDO: return "no-sudo";
    case RESCUE_ROOT_LOCKED: return "root-locked";
    default: return "no";
    }
}

const char *rescue_refusal(RescueState s) {
    switch (s) {
    case RESCUE_OFFER: return NULL;
    case RESCUE_HAS_ADMIN: case RESCUE_RESTART: return "users.rescueHasAdmin";
    case RESCUE_NO_SUDO: return "users.rescueNoSudo";
    case RESCUE_ROOT_LOCKED: return "users.rescueRootLocked";
    default: return "users.rescueNotAllowed";
    }
}

int rescue_group_is_admin(const char *group) {
    return !strcmp(group, "sudo") || !strcmp(group, "admin") || !strcmp(group, "wheel");
}

int rescue_account_counts(uid_t uid, int member) {
    return uid != 0 && member;
}
