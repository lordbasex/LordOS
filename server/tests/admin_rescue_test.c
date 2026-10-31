/* admin_rescue_test.c — who may become an administrator on a system that has none. */
#include "../src/users/admin_rescue.h"

#include <stdio.h>
#include <string.h>

static int run, bad;
#define CHECK(c) do { run++; if (!(c)) { bad++; printf("FAILED line %d: %s\n", __LINE__, #c); } } while (0)

static const RescueFacts ok = { .person = 1, .sudo_ready = 1, .root_password = 1 };

int main(void) {
    RescueFacts f = ok;
    CHECK(rescue_state(&f) == RESCUE_OFFER);
    CHECK(rescue_refusal(RESCUE_OFFER) == NULL);

    /* every fact that fails closes it, with its own answer */
    f = ok; f.person = 0;
    CHECK(rescue_state(&f) == RESCUE_NOT_ALLOWED);
    f = ok; f.any_admin = 1;
    CHECK(rescue_state(&f) == RESCUE_HAS_ADMIN);
    f = ok; f.sudo_ready = 0;
    CHECK(rescue_state(&f) == RESCUE_NO_SUDO);
    f = ok; f.root_password = 0;
    CHECK(rescue_state(&f) == RESCUE_ROOT_LOCKED);

    /* already added, waiting for the restart: never a second time */
    f = ok; f.any_admin = 1; f.self_member = 1;
    CHECK(rescue_state(&f) == RESCUE_RESTART);
    CHECK(rescue_refusal(RESCUE_RESTART) != NULL);

    /* an account that isn't a person's never gets it, whatever the rest says */
    f = ok; f.person = 0; f.any_admin = 1; f.self_member = 1;
    CHECK(rescue_state(&f) == RESCUE_NOT_ALLOWED);

    /* an administrator somewhere wins over a missing sudo or a locked root */
    f = ok; f.any_admin = 1; f.sudo_ready = 0; f.root_password = 0;
    CHECK(rescue_state(&f) == RESCUE_HAS_ADMIN);

    /* the only state with no refusal is OFFER, across all 32 sets of facts */
    int offers = 0;
    for (int i = 0; i < 32; i++) {
        RescueFacts x = { i & 1, !!(i & 2), !!(i & 4), !!(i & 8), !!(i & 16) };
        RescueState s = rescue_state(&x);
        CHECK(rescue_word(s) != NULL);
        if (!rescue_refusal(s)) {
            offers++;
            CHECK(x.person && !x.any_admin && x.sudo_ready && x.root_password);
        }
    }
    CHECK(offers == 2);                 /* with self_member on or off: it can't be true with no admin */

    /* the words the Store reads and the codes it shows */
    CHECK(!strcmp(rescue_word(RESCUE_OFFER), "offer"));
    CHECK(!strcmp(rescue_word(RESCUE_RESTART), "restart"));
    CHECK(!strcmp(rescue_word(RESCUE_NO_SUDO), "no-sudo"));
    CHECK(!strcmp(rescue_word(RESCUE_ROOT_LOCKED), "root-locked"));
    CHECK(!strcmp(rescue_word(RESCUE_HAS_ADMIN), "no"));
    CHECK(!strcmp(rescue_word(RESCUE_NOT_ALLOWED), "no"));
    CHECK(!strcmp(rescue_refusal(RESCUE_NO_SUDO), "users.rescueNoSudo"));
    CHECK(!strcmp(rescue_refusal(RESCUE_ROOT_LOCKED), "users.rescueRootLocked"));

    /* who counts as an administrator */
    CHECK(rescue_group_is_admin("sudo"));
    CHECK(rescue_group_is_admin("admin"));
    CHECK(rescue_group_is_admin("wheel"));
    CHECK(!rescue_group_is_admin("sudoers"));
    CHECK(!rescue_group_is_admin("adm"));
    CHECK(!rescue_group_is_admin(""));
    CHECK(!rescue_account_counts(0, 1));          /* root in sudo is still "nobody" */
    CHECK(rescue_account_counts(1000, 1));
    CHECK(rescue_account_counts(120, 1));         /* a system account in sudo does count */
    CHECK(!rescue_account_counts(1000, 0));

    printf("admin-rescue-test: %d/%d\n", run - bad, run);
    return bad != 0;
}
