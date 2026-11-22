/* mok_admin_test.c — the closed admin action dkms-mok-enroll (server/src/hw/mok_admin.c; docs/NVIDIA-PLAN.md A.5,
 * docs/CAMERA-STUDIO-PLAN.md 11.4) with fake firmware variables, a fake DKMS key pair and a fake mokutil, all under
 * build/mok-test (MOK_EFIVARS, MOK_DKMS_DIR, MOK_UTIL; the owner check accepts the test's own uid). Checks the states,
 * which key files are refused, that the one-time password goes to mokutil on standard input only (never in argv) and
 * comes back once, that a pending request is withdrawn before a new one, and that every refusal runs nothing.
 * Nothing here touches the machine's firmware or runs the real mokutil. Run from server/. */
#include "../src/hw/mok_admin.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int bad;
#define CHECK(c, ...) do { if (!(c)) { bad++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define DIR_ "build/mok-test"
#define EFI DIR_ "/efivars"
#define DKMS DIR_ "/dkms"
#define LOG DIR_ "/log"
#define SHIM "-605dab50-e046-4300-abb6-3dd810dd8b23"

/* ---- the admin helper's side, faked ---- */
static char said[300];
void admin_say(const char *kind, const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    snprintf(said, sizeof said, "%s %s", kind, line);
}

static void put(const char *path, const void *data, size_t n, mode_t mode) {
    unlink(path);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("cannot write %s: %s\n", path, strerror(errno)); exit(1); }
    fwrite(data, 1, n, f);
    fclose(f);
    chmod(path, mode);
}
static size_t get(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(out, 1, cap - 1, f) : 0;
    if (f) fclose(f);
    out[n] = 0;
    return n;
}

/* a fake certificate: SEQUENCE, two-byte length, then bytes */
static uint8_t cert[300];
static void make_cert(uint8_t seed) {
    cert[0] = 0x30; cert[1] = 0x82; cert[2] = 0x01; cert[3] = 0x28;       /* 296 + 4 = 300 */
    for (size_t i = 4; i < sizeof cert; i++) cert[i] = (uint8_t)(i * 7 + seed);
}
/* a shim variable: 4 bytes of attributes, a fake EFI_SIGNATURE_LIST header, then the certificate */
static void put_var(const char *name, const uint8_t *der, size_t n) {
    uint8_t v[512] = { 7, 0, 0, 0 };
    memset(v + 4, 0xa5, 44);
    memcpy(v + 48, der, n);
    char p[256];
    snprintf(p, sizeof p, EFI "/%s" SHIM, name);
    put(p, v, 48 + n, 0644);
}
static void secure_boot(int on) {
    uint8_t v[5] = { 6, 0, 0, 0, (uint8_t)on };
    put(EFI "/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c", v, 5, 0644);
}
static void reset(void) {
    if (system("rm -rf " DIR_)) {}
    mkdir(DIR_, 0755);
    mkdir(EFI, 0755);
    mkdir(DKMS, 0755);
    chmod(DKMS, 0755);
    make_cert(1);
    put(DKMS "/mok.pub", cert, sizeof cert, 0644);
    put(DKMS "/mok.key", "private", 7, 0600);
    /* the fake mokutil: records its arguments and standard input; --import puts the key into MokNew (as shim's
     * request), --revoke-import removes it; exits 1 when DIR_/fail exists, and does nothing when DIR_/idle exists */
    const char *sh =
        "#!/bin/sh\n"
        "D=" DIR_ "\n"
        "echo \"argv: $*\" >> $D/log\n"
        "cat >> $D/log\n"
        "[ -e $D/fail ] && exit 1\n"
        "[ -e $D/idle ] && exit 0\n"
        "case \"$1\" in\n"
        "  --import) { printf '\\007\\000\\000\\000'; head -c 44 /dev/zero; cat \"$2\"; } > $D/efivars/MokNew" SHIM " ;;\n"
        "  --revoke-import) rm -f $D/efivars/MokNew" SHIM " ;;\n"
        "esac\n"
        "exit 0\n";
    put(DIR_ "/mokutil", sh, strlen(sh), 0755);
    secure_boot(1);
}
static int state(void) { return mok_state(EFI, DKMS); }
static void act(const char *arg) { said[0] = 0; hw_act_dkms_mok_enroll(NULL, arg, strlen(arg)); }
static int ran(void) { struct stat st; return stat(LOG, &st) == 0 && st.st_size > 0; }
static int is_digits(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) if (!isdigit((unsigned char)s[i])) return 0;
    return s[n] == 0;
}

static void test_states(void) {
    reset();
    if (system("rm -rf " EFI)) {}
    CHECK(state() == MOK_UNSUPPORTED, "no EFI variables: unsupported");
    act("status");
    CHECK(!strcmp(said, "OK dkms.mokState state=unsupported"), "status answers the state: %s", said);
    reset();
    secure_boot(0);
    CHECK(state() == MOK_OFF, "Secure Boot off");
    reset();
    unlink(EFI "/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c");
    CHECK(state() == MOK_OFF, "no SecureBoot variable: off");
    reset();
    CHECK(state() == MOK_NEEDED, "on, key there, not enrolled: needed");
    put_var("MokListRT", (const uint8_t *)"other", 5);
    CHECK(state() == MOK_NEEDED, "another key enrolled: still needed");
    put_var("MokListRT2", cert, sizeof cert);
    CHECK(state() == MOK_ENROLLED, "enrolled in a split list (MokListRT2)");
    reset();
    put_var("MokListRTx", cert, sizeof cert);
    put_var("MokListX", cert, sizeof cert);
    CHECK(state() == MOK_NEEDED, "variables with other names don't count");
    reset();
    put_var("MokNew", cert, sizeof cert);
    CHECK(state() == MOK_PENDING, "in shim's request: pending");
    make_cert(2);
    put_var("MokListRT", cert, sizeof cert);
    make_cert(1);
    CHECK(state() == MOK_PENDING, "a different certificate enrolled doesn't count");
}

static void test_keys(void) {
    reset();
    unlink(DKMS "/mok.key");
    CHECK(state() == MOK_NOKEY, "no private key beside it: nokey");
    reset();
    chmod(DKMS "/mok.pub", 0664);
    CHECK(state() == MOK_NOKEY, "a group-writable key");
    reset();
    chmod(DKMS, 0777);
    CHECK(state() == MOK_NOKEY, "a folder others can write");
    reset();
    if (rename(DKMS "/mok.pub", DIR_ "/real.pub") || symlink("../real.pub", DKMS "/mok.pub")) printf("cannot link\n");
    CHECK(state() == MOK_NOKEY, "a link instead of the key");
    reset();
    uint8_t pem[300];
    memset(pem, '-', sizeof pem);
    put(DKMS "/mok.pub", pem, sizeof pem, 0644);
    CHECK(state() == MOK_NOKEY, "not DER");
    reset();
    put(DKMS "/mok.pub", cert, sizeof cert - 1, 0644);
    CHECK(state() == MOK_NOKEY, "a DER length that doesn't cover the file");
    reset();
    uint8_t big[MOK_KEY_MAX + 8];
    memset(big, 0, sizeof big);
    big[0] = 0x30; big[1] = 0x82; big[2] = (MOK_KEY_MAX + 4) >> 8; big[3] = (MOK_KEY_MAX + 4) & 0xff;
    put(DKMS "/mok.pub", big, sizeof big, 0644);
    CHECK(state() == MOK_NOKEY, "a key over the limit");
    reset();
    act("import");
    CHECK(!strncmp(said, "OK dkms.mokPassword", 19), "a good key imports: %s", said);
}

static void test_import(void) {
    reset();
    act("import");
    const char *p = strstr(said, "password=");
    CHECK(!strncmp(said, "OK dkms.mokPassword password=", 29) && p && is_digits(p + 9, MOK_PASSWORD_DIGITS),
          "import answers 8 digits once: %s", said);
    char pw[16] = "";
    if (p) snprintf(pw, sizeof pw, "%s", p + 9);
    char log[2048], want[256];
    get(LOG, log, sizeof log);
    snprintf(want, sizeof want, "argv: --import " DKMS "/mok.pub\n%s\n%s\n", pw, pw);
    CHECK(!strcmp(log, want), "mokutil got the fixed arguments and the password twice on standard input:\n%s", log);
    const char *nl = strchr(log, '\n');
    CHECK(pw[0] && nl && !memmem(log, (size_t)(nl - log), pw, strlen(pw)), "the password is never in argv");
    CHECK(state() == MOK_PENDING, "now pending");

    /* again while pending: the request is withdrawn first, then a new password */
    unlink(LOG);
    act("import");
    p = strstr(said, "password=");
    CHECK(!strncmp(said, "OK dkms.mokPassword password=", 29), "a second import: %s", said);
    get(LOG, log, sizeof log);
    const char *both = "argv: --revoke-import\nargv: --import " DKMS "/mok.pub\n";
    CHECK(!strncmp(log, both, strlen(both)), "revoke, then import:\n%s", log);
    CHECK(state() == MOK_PENDING, "pending again");

    /* cancel */
    unlink(LOG);
    act("cancel");
    CHECK(!strcmp(said, "OK dkms.mokState state=needed"), "cancel withdraws it: %s", said);
    get(LOG, log, sizeof log);
    CHECK(!strcmp(log, "argv: --revoke-import\n"), "cancel runs only --revoke-import: %s", log);
    unlink(LOG);
    act("cancel");
    CHECK(!strcmp(said, "ERR dkms.mokNothingPending") && !ran(), "cancel with nothing pending runs nothing: %s", said);
}

static void test_refusals(void) {
    const char *args[] = { "", "Import", "import ", "import /tmp/x.der", "--import", "status x", "cancel\n", "revoke", "gen" };
    for (size_t i = 0; i < sizeof args / sizeof args[0]; i++) {
        reset();
        act(args[i]);
        CHECK(!strncmp(said, "ERR ", 4) && !ran(), "\"%s\" refused without running anything: %s", args[i], said);
    }
    reset();
    secure_boot(0);
    act("import");
    CHECK(!strcmp(said, "ERR dkms.mokNotNeeded state=off") && !ran(), "Secure Boot off: %s", said);
    reset();
    put_var("MokListRT", cert, sizeof cert);
    act("import");
    CHECK(!strcmp(said, "ERR dkms.mokNotNeeded state=enrolled") && !ran(), "already enrolled: %s", said);
    reset();
    unlink(DKMS "/mok.key");
    act("import");
    CHECK(!strcmp(said, "ERR dkms.mokNotNeeded state=nokey") && !ran(), "no key: %s", said);
    reset();
    unlink(DIR_ "/mokutil");
    act("import");
    CHECK(!strcmp(said, "ERR dkms.mokNoTool"), "no mokutil: %s", said);
    reset();
    chmod(DIR_ "/mokutil", 0775);
    act("import");
    CHECK(!strcmp(said, "ERR dkms.mokNoTool") && !ran(), "a group-writable mokutil isn't run: %s", said);
    reset();
    put(DIR_ "/fail", "", 0, 0644);
    act("import");
    CHECK(!strcmp(said, "ERR dkms.mokFailed"), "mokutil failing: no password said: %s", said);
    reset();
    put(DIR_ "/idle", "", 0, 0644);
    act("import");
    CHECK(!strcmp(said, "ERR dkms.mokFailed"), "mokutil exiting 0 without a request: no password said: %s", said);
    reset();
    put_var("MokNew", cert, sizeof cert);
    put(DIR_ "/idle", "", 0, 0644);
    act("cancel");
    CHECK(!strcmp(said, "ERR dkms.mokFailed"), "a revoke that didn't remove the request: %s", said);
}

static void test_password(void) {
    int count[10] = { 0 };
    for (int i = 0; i < 2000; i++) {
        char pw[MOK_PASSWORD_DIGITS + 1];
        CHECK(mok_password(pw) == 0 && is_digits(pw, MOK_PASSWORD_DIGITS), "8 digits");
        for (int k = 0; k < MOK_PASSWORD_DIGITS; k++) count[pw[k] - '0']++;
    }
    for (int d = 0; d < 10; d++) CHECK(count[d] > 1400 && count[d] < 1800, "digit %d about 1600 times: %d", d, count[d]);
    CHECK(!strcmp(mok_state_name(MOK_NEEDED), "needed") && !strcmp(mok_state_name(99), "unsupported"), "state names");
}

int main(void) {
    test_states();
    test_keys();
    test_import();
    test_refusals();
    test_password();
    if (system("rm -rf " DIR_)) {}
    if (bad) { printf("mok-admin-test: %d failures\n", bad); return 1; }
    printf("mok-admin-test: ok\n");
    return 0;
}
