/* admin_groups_test.c — who is an administrator (src/users/admingroups.c): the one closed list of
 * groups, the group a new administrator joins, who counts towards the last administrator and which
 * groups a revoke leaves. */
#include "../src/users/admingroups.h"

#include <stdio.h>
#include <string.h>

static int run, bad;
#define CHECK(c) do { run++; if (!(c)) { bad++; printf("FAILED line %d: %s\n", __LINE__, #c); } } while (0)

/* a system's /etc/group, as the names that exist */
static const char *const *sys_groups;
static int exists(const char *g) {
    for (const char *const *p = sys_groups; *p; p++) if (!strcmp(*p, g)) return 1;
    return 0;
}

/* an account: uid, primary group, then its other groups (NULL-terminated) */
static AdminMembership account(uid_t uid, const char *primary, const char *const *others) {
    AdminMembership m;
    admin_membership_init(&m, uid);
    admin_membership_add(&m, primary, 1);
    for (const char *const *p = others; p && *p; p++) admin_membership_add(&m, *p, 0);
    return m;
}

static int has(const char *const *out, int n, const char *g) {
    for (int i = 0; i < n; i++) if (!strcmp(out[i], g)) return 1;
    return 0;
}

int main(void) {
    /* the closed list */
    CHECK(admin_group_is("sudo"));
    CHECK(admin_group_is("admin"));
    CHECK(admin_group_is("wheel"));
    CHECK(!admin_group_is("sudoers"));
    CHECK(!admin_group_is("adm"));
    CHECK(!admin_group_is("Sudo"));
    CHECK(!admin_group_is("sudo "));
    CHECK(!admin_group_is(""));
    CHECK(!admin_group_is(NULL));
    CHECK(admin_group_find("wheel") == ADMIN_GROUPS[2]);

    /* the group a new administrator joins: sudo, else wheel; never admin; none without either */
    static const char *const debian[] = { "root", "sudo", "admin", "wheel", "audio", NULL };
    static const char *const ubuntu_old[] = { "root", "admin", "sudo", NULL };
    static const char *const fedora[] = { "root", "wheel", "audio", NULL };
    static const char *const only_admin[] = { "root", "admin", NULL };
    static const char *const bare[] = { "root", "users", NULL };
    sys_groups = debian; CHECK(!strcmp(admin_group_for_new(exists), "sudo"));
    sys_groups = ubuntu_old; CHECK(!strcmp(admin_group_for_new(exists), "sudo"));
    sys_groups = fedora; CHECK(!strcmp(admin_group_for_new(exists), "wheel"));
    sys_groups = only_admin; CHECK(admin_group_for_new(exists) == NULL);
    sys_groups = bare; CHECK(admin_group_for_new(exists) == NULL);

    /* who is one: any group of the list, primary or supplementary */
    AdminMembership m = account(1000, "ana", (const char *const[]){ "audio", "video", NULL });
    CHECK(!admin_membership_is_admin(&m, 0));
    CHECK(!admin_membership_counts_person(&m));
    m = account(1000, "ana", (const char *const[]){ "wheel", NULL });
    CHECK(admin_membership_is_admin(&m, 0));
    m = account(1000, "ana", (const char *const[]){ "admin", NULL });
    CHECK(admin_membership_is_admin(&m, 0));
    m = account(1000, "sudo", NULL);                         /* primary group sudo */
    CHECK(admin_membership_is_admin(&m, 0));
    CHECK(m.primary == ADMIN_GROUPS[0]);
    /* repeats (getgrouplist lists the primary group too) count once */
    m = account(1000, "ana", (const char *const[]){ "sudo", "sudo", "wheel", "sudo", NULL });
    CHECK(m.n == 2);

    /* an unreadable list: never grants, but keeps the rescue closed */
    admin_membership_init(&m, 1000);
    m.unreadable = 1;
    CHECK(!admin_membership_is_admin(&m, 0));
    CHECK(admin_membership_is_admin(&m, 1));
    CHECK(!admin_membership_counts_person(&m));

    /* revoke: a user in sudo AND wheel leaves both (before, only sudo, and stayed an administrator) */
    const char *out[ADMIN_GROUP_COUNT];
    m = account(1000, "ana", (const char *const[]){ "audio", "sudo", "wheel", NULL });
    int n = admin_revoke_plan(&m, out);
    CHECK(n == 2);
    CHECK(has(out, n, "sudo") && has(out, n, "wheel"));
    /* all three */
    m = account(1000, "ana", (const char *const[]){ "admin", "wheel", "sudo", NULL });
    n = admin_revoke_plan(&m, out);
    CHECK(n == 3 && has(out, n, "sudo") && has(out, n, "admin") && has(out, n, "wheel"));
    /* wheel-only system: leaves wheel */
    m = account(1000, "ana", (const char *const[]){ "wheel", NULL });
    n = admin_revoke_plan(&m, out);
    CHECK(n == 1 && !strcmp(out[0], "wheel"));
    /* not an administrator: nothing to do */
    m = account(1000, "ana", (const char *const[]){ "audio", NULL });
    CHECK(admin_revoke_plan(&m, out) == 0);
    /* the primary group is an administrator group: refused, nothing planned */
    m = account(1000, "wheel", (const char *const[]){ "sudo", NULL });
    CHECK(admin_revoke_plan(&m, out) == ADMIN_REVOKE_PRIMARY);
    m = account(1000, "admin", NULL);
    CHECK(admin_revoke_plan(&m, out) == ADMIN_REVOKE_PRIMARY);
    /* unreadable: refused */
    admin_membership_init(&m, 1000);
    m.unreadable = 1;
    CHECK(admin_revoke_plan(&m, out) == ADMIN_REVOKE_UNREADABLE);

    /* the last administrator, counted across groups: one in sudo, one only in wheel, one only in admin
     * are three administrators (before, a sudo-only count saw one and protected the wrong one) */
    AdminMembership people[] = {
        account(1000, "ana", (const char *const[]){ "sudo", NULL }),
        account(1001, "bob", (const char *const[]){ "wheel", NULL }),
        account(1002, "cy", (const char *const[]){ "admin", "audio", NULL }),
        account(1003, "dee", (const char *const[]){ "audio", NULL }),
        account(0, "root", (const char *const[]){ "sudo", NULL }),           /* not a person */
        account(120, "svc", (const char *const[]){ "wheel", NULL }),         /* nor a system account */
        account(65534, "nogroup", (const char *const[]){ "sudo", NULL }),    /* nor nobody */
    };
    int count = 0;
    for (size_t i = 0; i < sizeof people / sizeof *people; i++) count += admin_membership_counts_person(&people[i]);
    CHECK(count == 3);
    /* only one administrator, and only through wheel: still the last one */
    AdminMembership lone[] = {
        account(1000, "ana", (const char *const[]){ "audio", NULL }),
        account(1001, "bob", (const char *const[]){ "wheel", NULL }),
    };
    count = 0;
    for (size_t i = 0; i < sizeof lone / sizeof *lone; i++) count += admin_membership_counts_person(&lone[i]);
    CHECK(count == 1);
    CHECK(admin_membership_is_admin(&lone[1], 0));

    printf("admin-groups-test: %d/%d\n", run - bad, run);
    return bad != 0;
}
