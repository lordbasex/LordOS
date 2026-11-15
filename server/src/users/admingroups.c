/* admingroups.c — the administrator groups (admingroups.h). */
#include "admingroups.h"

#include <grp.h>
#include <pwd.h>
#include <string.h>

const char *const ADMIN_GROUPS[ADMIN_GROUP_COUNT] = { "sudo", "admin", "wheel" };

const char *admin_group_find(const char *group) {
    for (int i = 0; group && i < ADMIN_GROUP_COUNT; i++)
        if (!strcmp(group, ADMIN_GROUPS[i])) return ADMIN_GROUPS[i];
    return NULL;
}

const char *admin_group_for_new(int (*exists)(const char *group)) {
    if (exists("sudo")) return ADMIN_GROUPS[0];
    if (exists("wheel")) return ADMIN_GROUPS[2];
    return NULL;
}

void admin_membership_init(AdminMembership *m, uid_t uid) {
    memset(m, 0, sizeof *m);
    m->uid = uid;
}

void admin_membership_add(AdminMembership *m, const char *group, int primary) {
    const char *g = admin_group_find(group);
    if (!g) return;
    if (primary) m->primary = g;
    for (int i = 0; i < m->n; i++) if (m->in[i] == g) return;
    if (m->n < ADMIN_GROUP_COUNT) m->in[m->n++] = g;
}

int admin_membership_load(const char *user, AdminMembership *m) {
    struct passwd *pw = getpwnam(user);
    if (!pw) return -1;
    admin_membership_read(user, pw->pw_uid, pw->pw_gid, m);
    return 0;
}

void admin_membership_read(const char *user, uid_t uid, gid_t primary, AdminMembership *m) {
    admin_membership_init(m, uid);
    gid_t groups[256];
    int n = 256;
    if (getgrouplist(user, primary, groups, &n) < 0) { m->unreadable = 1; return; }
    for (int i = 0; i < n; i++) {
        struct group *g = getgrgid(groups[i]);
        if (g) admin_membership_add(m, g->gr_name, groups[i] == primary);
    }
}

int admin_membership_is_admin(const AdminMembership *m, int unreadable_counts) {
    return m->unreadable ? unreadable_counts : m->n > 0;
}

int admin_membership_counts_person(const AdminMembership *m) {
    return m->uid >= 1000 && m->uid < 60000 && admin_membership_is_admin(m, 0);
}

int admin_revoke_plan(const AdminMembership *m, const char *out[ADMIN_GROUP_COUNT]) {
    if (m->unreadable) return ADMIN_REVOKE_UNREADABLE;
    if (m->primary) return ADMIN_REVOKE_PRIMARY;
    for (int i = 0; i < m->n; i++) out[i] = m->in[i];
    return m->n;
}

int admin_gids_any(const gid_t *gids, int n, const char *(*name_of)(gid_t gid)) {
    for (int i = 0; gids && i < n; i++)
        if (admin_group_is(name_of(gids[i]))) return 1;
    return 0;
}

const char *admin_gid_name(gid_t gid) {
    struct group *g = getgrgid(gid);
    return g ? g->gr_name : NULL;
}

int user_is_admin(const char *user) {
    AdminMembership m;
    return !admin_membership_load(user, &m) && admin_membership_is_admin(&m, 0);
}
