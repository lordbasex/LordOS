/* admingroups.h — which groups make someone an administrator, for every root-side check.
 *
 * One closed list, fixed here and never read from JS, a file or any input: sudo (Debian, Ubuntu),
 * admin (old Ubuntu) and wheel (Arch, Fedora). admin.c's is_admin, the admin rescue
 * (users_admin.c, admin_rescue.c), Settings → Users (user-add, user-del, user-admin),
 * lordos-firstrun, lord-sessiond (the login screen's list, the shutdown check), lord-fand (who may
 * change the fans), lord-netd and lord-server's lord.sys.info().admin and lord.users.list() all use
 * it, and no other file carries its own copy, so "administrator" means the same thing everywhere: a
 * member of any group in the list, primary or supplementary, by the system's files right now.
 *
 * The decisions (which group a new administrator joins, who counts, which groups a revoke leaves)
 * are pure and tested in build/admin-groups-test; admin_membership_load() is the only part that
 * reads the system. */
#ifndef LORD_ADMINGROUPS_H
#define LORD_ADMINGROUPS_H

#include <sys/types.h>

#define ADMIN_GROUP_COUNT 3
extern const char *const ADMIN_GROUPS[ADMIN_GROUP_COUNT];       /* "sudo", "admin", "wheel" */

/* The list's own string for an administrator group, or NULL. */
const char *admin_group_find(const char *group);
static inline int admin_group_is(const char *group) { return admin_group_find(group) != 0; }

/* The group a new administrator joins: sudo, else wheel (never "admin"); NULL if neither exists.
 * exists() answers for the system being changed (getgrnam, or an image's /etc/group). */
const char *admin_group_for_new(int (*exists)(const char *group));

/* One account's administrator groups. */
typedef struct {
    uid_t uid;
    int unreadable;                     /* its list of groups couldn't be read (too long, no entry) */
    const char *primary;                /* its primary group, when that is an administrator group */
    const char *in[ADMIN_GROUP_COUNT];  /* every administrator group it is in (list strings, no repeats) */
    int n;
} AdminMembership;

void admin_membership_init(AdminMembership *m, uid_t uid);
/* One of the account's groups; primary: it is the account's primary group. */
void admin_membership_add(AdminMembership *m, const char *group, int primary);
/* Reads the system now (getpwnam, getgrouplist, getgrgid). -1 if the account doesn't exist; an
 * unreadable list leaves m->unreadable set. */
int admin_membership_load(const char *user, AdminMembership *m);
/* The same for an account already looked up (getpwent's): its uid and primary gid. */
void admin_membership_read(const char *user, uid_t uid, gid_t primary, AdminMembership *m);

/* Is it an administrator? An unreadable list counts as one only where that fails closed: never to
 * grant a privilege (unreadable_counts = 0), yes to keep the rescue closed (= 1). */
int admin_membership_is_admin(const AdminMembership *m, int unreadable_counts);
/* Does it count towards "the last administrator"? A person's account (uid 1000 to 59999) that is one. */
int admin_membership_counts_person(const AdminMembership *m);

/* Which groups to leave so it is no longer an administrator: fills out (ADMIN_GROUP_COUNT entries)
 * and returns how many; 0 when it isn't one. ADMIN_REVOKE_PRIMARY when its primary group is an
 * administrator group (gpasswd can't take it out of that one), ADMIN_REVOKE_UNREADABLE when its
 * groups couldn't be read: both mean refuse and change nothing. */
#define ADMIN_REVOKE_PRIMARY (-1)
#define ADMIN_REVOKE_UNREADABLE (-2)
int admin_revoke_plan(const AdminMembership *m, const char *out[ADMIN_GROUP_COUNT]);

/* Is any of these group ids an administrator group? name_of answers a gid's group name (NULL when
 * it has none): admin_gid_name for the system's /etc/group, a table in the tests. For a check by a
 * process's own credentials (getgroups), where there is no account to look up. */
int admin_gids_any(const gid_t *gids, int n, const char *(*name_of)(gid_t gid));
/* getgrgid's name for a gid, or NULL. */
const char *admin_gid_name(gid_t gid);

/* user_is_admin: the rule of admin.c's is_admin, by the system's files right now (an unreadable
 * list is not an administrator). */
int user_is_admin(const char *user);

#endif
