/* lidwake_test.c — the lid wake hook's parsing and decisions (src/hw/lidwake.h, GitHub #172). */
#include <stdio.h>
#include <string.h>

#include "../src/hw/lidwake.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define S(x) x, strlen(x)

/* /proc/acpi/wakeup of a MacBookPro12,1 like the one in #172 (lid open, LID0 enabled). */
static const char *MBP =
    "Device\tS-state\t  Status   Sysfs node\n"
    "P0P2\t  S3\t*disabled\n"
    "EC\t  S3\t*disabled  platform:PNP0C09:00\n"
    "HDEF\t  S3\t*disabled  pci:0000:00:1b.0\n"
    "RP01\t  S3\t*enabled   pci:0000:00:1c.0\n"
    "ARPT\t  S4\t*disabled  pci:0000:03:00.0\n"
    "LID0\t  S4\t*enabled   platform:PNP0C0D:00\n"
    "XHC1\t  S3\t*enabled   pci:0000:00:14.0\n"
    "\t\t*enabled   usb:usb1\n";

static void test_names(void) {
    CHECK(lidwake_name_valid(S("LID0")));
    CHECK(lidwake_name_valid(S("LID")));
    CHECK(lidwake_name_valid(S("A_B9")));
    CHECK(lidwake_name_valid(S("ABCDEFGH")));
    CHECK(!lidwake_name_valid(S("ABCDEFGHI")));
    CHECK(!lidwake_name_valid(S("")));
    CHECK(!lidwake_name_valid(S("lid0")));
    CHECK(!lidwake_name_valid(S("LID0\n")));
    CHECK(!lidwake_name_valid(S("LID-0")));
    CHECK(!lidwake_name_valid(S("LID 0")));
    CHECK(!lidwake_name_valid(S("../x")));
}

static void test_chassis(void) {
    CHECK(lidwake_chassis(S("9\n")) == 9);
    CHECK(lidwake_chassis(S("10\n")) == 10);
    CHECK(lidwake_chassis(S("3")) == 3);
    CHECK(lidwake_chassis(S("")) == -1);
    CHECK(lidwake_chassis(S("\n")) == -1);
    CHECK(lidwake_chassis(S(" 9\n")) == -1);
    CHECK(lidwake_chassis(S("9 \n")) == -1);
    CHECK(lidwake_chassis(S("9\n\n")) == -1);
    CHECK(lidwake_chassis(S("1000\n")) == -1);
    CHECK(lidwake_chassis(S("-9\n")) == -1);
    CHECK(lidwake_chassis(S("0x9\n")) == -1);
    /* the same list as the on-screen keyboard's (osk_chassis_portable) */
    int portable[] = { 8, 9, 10, 14, 31, 32 }, other[] = { 0, 1, 2, 3, 4, 6, 7, 11, 13, 17, 23, 30, 35, -1 };
    for (size_t i = 0; i < sizeof portable / sizeof portable[0]; i++) CHECK(chassis_portable(portable[i]));
    for (size_t i = 0; i < sizeof other / sizeof other[0]; i++) CHECK(!chassis_portable(other[i]));
}

static void test_lid(void) {
    CHECK(lidwake_lid_state(S("state:      open\n")) == LIDWAKE_LID_OPEN);
    CHECK(lidwake_lid_state(S("state:      closed\n")) == LIDWAKE_LID_CLOSED);
    CHECK(lidwake_lid_state(S("state:\topen")) == LIDWAKE_LID_OPEN);
    CHECK(lidwake_lid_state(S("state:open\n")) == LIDWAKE_LID_UNKNOWN);
    CHECK(lidwake_lid_state(S("state:      opened\n")) == LIDWAKE_LID_UNKNOWN);
    CHECK(lidwake_lid_state(S("state:      open \n")) == LIDWAKE_LID_UNKNOWN);
    CHECK(lidwake_lid_state(S("state:      unsupported\n")) == LIDWAKE_LID_UNKNOWN);
    CHECK(lidwake_lid_state(S("State:      open\n")) == LIDWAKE_LID_UNKNOWN);
    CHECK(lidwake_lid_state(S("")) == LIDWAKE_LID_UNKNOWN);
    CHECK(lidwake_lid_state(S("state:      open\nstate: closed\n")) == LIDWAKE_LID_UNKNOWN);

    int O = LIDWAKE_LID_OPEN, C = LIDWAKE_LID_CLOSED, U = LIDWAKE_LID_UNKNOWN;
    CHECK(lidwake_lid_fold(U, 0, O) == O);
    CHECK(lidwake_lid_fold(O, 1, O) == O);
    CHECK(lidwake_lid_fold(O, 1, C) == C);
    CHECK(lidwake_lid_fold(C, 1, O) == C);
    CHECK(lidwake_lid_fold(O, 1, U) == U);
    CHECK(lidwake_lid_fold(U, 1, O) == U);   /* unreadable first, open next: still unknown */
    CHECK(lidwake_lid_fold(U, 1, C) == C);
    CHECK(lidwake_lid_fold(U, 0, U) == U);
}

static void test_wakeup(void) {
    LidWakeEntry e[LIDWAKE_MAX];
    CHECK(lidwake_parse_wakeup(S(MBP), e, LIDWAKE_MAX) == 1);
    CHECK(!strcmp(e[0].name, "LID0") && e[0].enabled && e[0].valid);

    /* no lid: a desktop's list */
    CHECK(lidwake_parse_wakeup(S("Device\tS-state\t  Status   Sysfs node\nPWRB\t  S4\t*enabled   platform:PNP0C0C:00\n"),
                               e, LIDWAKE_MAX) == 0);
    CHECK(lidwake_parse_wakeup(S(""), e, LIDWAKE_MAX) == 0);
    /* disabled, and without '*' (the kernel can't make it wake) */
    CHECK(lidwake_parse_wakeup(S("LID0\t  S4\t*disabled  platform:PNP0C0D:00\n"), e, LIDWAKE_MAX) == 1);
    CHECK(!e[0].enabled && e[0].valid);
    CHECK(lidwake_parse_wakeup(S("LID0\t  S4\t enabled   platform:PNP0C0D:00\n"), e, LIDWAKE_MAX) == 1);
    CHECK(e[0].enabled && !e[0].valid);
    /* no sysfs node, and no final newline */
    CHECK(lidwake_parse_wakeup(S("LID\t  S3\t*enabled"), e, LIDWAKE_MAX) == 1);
    CHECK(!strcmp(e[0].name, "LID") && e[0].enabled);
    /* two lids */
    CHECK(lidwake_parse_wakeup(S("LID0\t  S4\t*enabled\nLID1\t  S4\t*disabled\n"), e, LIDWAKE_MAX) == 2);
    CHECK(!strcmp(e[1].name, "LID1") && !e[1].enabled);
    /* "LIDX" counts, "SLID" doesn't */
    CHECK(lidwake_parse_wakeup(S("SLID\t  S4\t*enabled\n"), e, LIDWAKE_MAX) == 0);

    /* strict: anything unexpected is -1 */
    CHECK(lidwake_parse_wakeup(S("lid0\t  S4\t*enabled\n"), e, LIDWAKE_MAX) == -1);
    CHECK(lidwake_parse_wakeup(S("LID0/..\t  S4\t*enabled\n"), e, LIDWAKE_MAX) == -1);
    CHECK(lidwake_parse_wakeup(S("LID000000\t  S4\t*enabled\n"), e, LIDWAKE_MAX) == -1);
    CHECK(lidwake_parse_wakeup(S("LID0\t  S\t*enabled\n"), e, LIDWAKE_MAX) == -1);
    CHECK(lidwake_parse_wakeup(S("LID0\t  S44\t*enabled\n"), e, LIDWAKE_MAX) == -1);
    CHECK(lidwake_parse_wakeup(S("LID0\t  S4\t*enabledX\n"), e, LIDWAKE_MAX) == -1);
    CHECK(lidwake_parse_wakeup(S("LID0\t  S4\t*invalid\n"), e, LIDWAKE_MAX) == -1);
    CHECK(lidwake_parse_wakeup(S("LID0\n"), e, LIDWAKE_MAX) == -1);
    CHECK(lidwake_parse_wakeup(S("LID0\t  S4\t*enabled\nLID0\t  S4\t*enabled\n"), e, LIDWAKE_MAX) == -1);
    CHECK(lidwake_parse_wakeup(S("LID0\t  S4\t*enabled\nLID1\t  S4\t*enabled\nLID2\t  S4\t*enabled\n"), e, 2) == -1);
    /* the header is skipped only on the first line */
    CHECK(lidwake_parse_wakeup(S("LID0\t  S4\t*enabled\nDevice\tS-state\t  Status   Sysfs node\n"), e, LIDWAKE_MAX) == -1);
    /* a NUL inside a name */
    static const char nul[] = "LI\0D\t  S4\t*enabled\n";
    CHECK(lidwake_parse_wakeup(nul, sizeof nul - 1, e, LIDWAKE_MAX) == -1);
}

static void test_plans(void) {
    LidWakeEntry e[LIDWAKE_MAX];
    int n = lidwake_parse_wakeup(S(MBP), e, LIDWAKE_MAX);
    CHECK(n == 1);
    /* laptop, lid open, LID0 enabled: off */
    CHECK(lidwake_plan_pre(10, LIDWAKE_LID_OPEN, e, n) == 1u);
    /* a desktop does nothing, whatever the list says */
    CHECK(lidwake_plan_pre(3, LIDWAKE_LID_OPEN, e, n) == 0);
    CHECK(lidwake_plan_pre(0, LIDWAKE_LID_OPEN, e, n) == 0);
    CHECK(lidwake_plan_pre(-1, LIDWAKE_LID_OPEN, e, n) == 0);
    CHECK(lidwake_plan_pre(30, LIDWAKE_LID_OPEN, e, n) == 0);
    /* lid closed or unknown: the lid must still wake the computer */
    CHECK(lidwake_plan_pre(10, LIDWAKE_LID_CLOSED, e, n) == 0);
    CHECK(lidwake_plan_pre(10, LIDWAKE_LID_UNKNOWN, e, n) == 0);
    /* no lid entry */
    CHECK(lidwake_plan_pre(10, LIDWAKE_LID_OPEN, e, 0) == 0);
    /* already disabled, or not able to wake: not touched */
    n = lidwake_parse_wakeup(S("LID0\t  S4\t*disabled\nLID1\t  S4\t enabled\nLID2\t  S4\t*enabled\n"), e, LIDWAKE_MAX);
    CHECK(n == 3);
    CHECK(lidwake_plan_pre(9, LIDWAKE_LID_OPEN, e, n) == 4u);

    /* post: only what the note names and is now disabled */
    char names[LIDWAKE_MAX][LIDWAKE_NAME + 1];
    CHECK(lidwake_parse_note(S("LID0\n"), names, LIDWAKE_MAX) == 1);
    n = lidwake_parse_wakeup(S("LID0\t  S4\t*disabled\nLID1\t  S4\t*disabled\n"), e, LIDWAKE_MAX);
    CHECK(lidwake_plan_post(names, 1, e, n) == 1u);          /* LID1 was off before: stays off */
    n = lidwake_parse_wakeup(S("LID0\t  S4\t*enabled\n"), e, LIDWAKE_MAX);
    CHECK(lidwake_plan_post(names, 1, e, n) == 0);           /* someone turned it on: left alone */
    n = lidwake_parse_wakeup(S("LID1\t  S4\t*disabled\n"), e, LIDWAKE_MAX);
    CHECK(lidwake_plan_post(names, 1, e, n) == 0);           /* gone */
    CHECK(lidwake_plan_post(names, 0, e, n) == 0);           /* empty note */
}

static void test_note(void) {
    char names[LIDWAKE_MAX][LIDWAKE_NAME + 1];
    CHECK(lidwake_parse_note(S(""), names, LIDWAKE_MAX) == 0);
    CHECK(lidwake_parse_note(S("LID0\nLID1\n"), names, LIDWAKE_MAX) == 2);
    CHECK(!strcmp(names[0], "LID0") && !strcmp(names[1], "LID1"));
    CHECK(lidwake_parse_note(S("LID0"), names, LIDWAKE_MAX) == -1);
    CHECK(lidwake_parse_note(S("\n"), names, LIDWAKE_MAX) == -1);
    CHECK(lidwake_parse_note(S("LID0\nLID0\n"), names, LIDWAKE_MAX) == -1);
    CHECK(lidwake_parse_note(S("lid0\n"), names, LIDWAKE_MAX) == -1);
    CHECK(lidwake_parse_note(S("LID0 \n"), names, LIDWAKE_MAX) == -1);
    CHECK(lidwake_parse_note(S("A\nB\nC\nD\nE\n"), names, LIDWAKE_MAX) == -1);
}

static void test_args(void) {
    char *pre[] = { "x", "pre", "suspend", NULL }, *post[] = { "x", "post", "hibernate", NULL };
    char *hy[] = { "x", "pre", "hybrid-sleep", NULL }, *sth[] = { "x", "post", "suspend-then-hibernate", NULL };
    char *bad1[] = { "x", "pre", NULL }, *bad2[] = { "x", "pre", "suspend", "more", NULL };
    char *bad3[] = { "x", "PRE", "suspend", NULL }, *bad4[] = { "x", "pre", "sleep", NULL };
    CHECK(lidwake_args(3, pre) == 1);
    CHECK(lidwake_args(3, post) == 2);
    CHECK(lidwake_args(3, hy) == 1);
    CHECK(lidwake_args(3, sth) == 2);
    CHECK(lidwake_args(2, bad1) == 0);
    CHECK(lidwake_args(4, bad2) == 0);
    CHECK(lidwake_args(3, bad3) == 0);
    CHECK(lidwake_args(3, bad4) == 0);
}

int main(void) {
    test_names();
    test_chassis();
    test_lid();
    test_wakeup();
    test_plans();
    test_note();
    test_args();
    if (fails) { fprintf(stderr, "lidwake-test: %d failed\n", fails); return 1; }
    printf("lidwake-test: ok\n");
    return 0;
}
