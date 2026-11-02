/* hwsnap_test.c — the catalogue's signed snapshot (src/sysreport/hwsnap.c, docs/BENCHMARK.md "0.8.7 —
 * Community hardware reports"): the signature is checked before anything is parsed, a snapshot signed with
 * another key, changed by one byte, unsigned or malformed is refused, the structure's limits hold, and the
 * folder is read without following links. With a path, the snapshot tools/hwreport/catalog.mjs wrote is
 * signed with the test key and must pass the same C checks (so the builder and the reader agree).
 *
 *   build/hwsnap-test [../assets/hwreport/snapshot.json]      (make test passes it)
 *
 * A throwaway test key only: the release key never signs anything here. */
#include "sysreport/hwsnap.h"
#include "pkg/crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int pass, fail;
#define CHECK(cond, name) do { if (cond) pass++; else { fail++; printf("FAILED: %s (line %d)\n", name, __LINE__); } } while (0)

static const uint8_t SEED[32] = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 11, 12, 13, 14, 15, 16,
                                  17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 };
static const uint8_t OTHER[32] = { 1 };
static uint8_t pub[32], other_pub[32];

static void b64(const uint8_t *in, size_t n, char *out) {
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = a[v >> 18 & 63];
        out[o++] = a[v >> 12 & 63];
        out[o++] = i + 1 < n ? a[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? a[v & 63] : '=';
    }
    out[o] = 0;
}

static void sign_with(const uint8_t seed[32], const char *text, size_t len, char sig[100]) {
    uint8_t s[64];
    ed25519_sign(seed, (const uint8_t *)text, len, s);
    b64(s, 64, sig);
    strcat(sig, "\n");
}

static HwSnapState check_signed(const char *text, char *err, size_t n) {
    char sig[100];
    sign_with(SEED, text, strlen(text), sig);
    return hwsnap_check(text, strlen(text), sig, strlen(sig), pub, err, n);
}

#define DEV(key, comp, ver, w, l, no, rep, ref) \
    "{\"key\":\"" key "\",\"component\":\"" comp "\",\"name\":\"A device\",\"lordos\":\"" ver "\",\"works\":" #w ",\"limits\":" #l \
    ",\"no\":" #no ",\"reports\":" #rep ",\"references\":" #ref "}"
#define SNAP(devs) "{\"format\":\"lordos-hwsnapshot\",\"version\":1,\"generated\":\"2026-10-29\",\"reports\":2,\"devices\":[" devs "]}\n"

static const char GOOD[] = SNAP(DEV("pci:1af4:1050", "gpu", "0.8.7", 1, 0, 0, 1, 1) "," DEV("gpu:v3d", "gpu", "0.8.7", 1, 1, 0, 2, 1));

static void test_check(void) {
    char err[160], sig[100];
    CHECK(check_signed(GOOD, err, sizeof err) == HWSNAP_OK, "a good snapshot");
    CHECK(check_signed(SNAP(""), err, sizeof err) == HWSNAP_OK, "an empty device list");

    /* the signature, before anything is parsed */
    CHECK(hwsnap_check(GOOD, strlen(GOOD), NULL, 0, pub, err, sizeof err) == HWSNAP_UNSIGNED, "no signature");
    sign_with(OTHER, GOOD, strlen(GOOD), sig);
    CHECK(hwsnap_check(GOOD, strlen(GOOD), sig, strlen(sig), pub, err, sizeof err) == HWSNAP_BAD_SIG, "another key");
    sign_with(SEED, GOOD, strlen(GOOD), sig);
    CHECK(hwsnap_check(GOOD, strlen(GOOD), sig, strlen(sig), other_pub, err, sizeof err) == HWSNAP_BAD_SIG, "verified with another key");
    char changed[sizeof GOOD];
    memcpy(changed, GOOD, sizeof GOOD);
    char *p = strstr(changed, "\"works\":1");
    p[8] = '2';
    CHECK(hwsnap_check(changed, strlen(changed), sig, strlen(sig), pub, err, sizeof err) == HWSNAP_BAD_SIG, "one byte changed");
    CHECK(hwsnap_check(GOOD, strlen(GOOD) - 1, sig, strlen(sig), pub, err, sizeof err) == HWSNAP_BAD_SIG, "one byte less");
    CHECK(hwsnap_check(GOOD, strlen(GOOD), "AAAA\n", 5, pub, err, sizeof err) == HWSNAP_BAD_SIG, "a short signature");
    char bad[100];
    memcpy(bad, sig, sizeof bad);
    bad[3] = '*';
    CHECK(hwsnap_check(GOOD, strlen(GOOD), bad, strlen(bad), pub, err, sizeof err) == HWSNAP_BAD_SIG, "not base64");
    /* a signed but malformed text is still refused: garbage is never parsed into JS */
    CHECK(check_signed("not json", err, sizeof err) == HWSNAP_INVALID, "signed garbage");

    /* the structure */
    static const char *const invalid[] = {
        "{\"format\":\"lordos-hwsnapshot\",\"version\":2,\"generated\":\"2026-10-29\",\"reports\":2,\"devices\":[]}",
        "{\"format\":\"other\",\"version\":1,\"generated\":\"2026-10-29\",\"reports\":2,\"devices\":[]}",
        "{\"format\":\"lordos-hwsnapshot\",\"version\":1,\"generated\":\"29/10/2026\",\"reports\":2,\"devices\":[]}",
        "{\"format\":\"lordos-hwsnapshot\",\"version\":1,\"generated\":\"2026-10-29\",\"reports\":-1,\"devices\":[]}",
        "{\"format\":\"lordos-hwsnapshot\",\"version\":1,\"generated\":\"2026-10-29\",\"reports\":2,\"devices\":{}}",
        "{\"format\":\"lordos-hwsnapshot\",\"version\":1,\"generated\":\"2026-10-29\",\"reports\":2,\"devices\":[],\"extra\":1}",
        "[1,2,3]",
        SNAP(DEV("nocolon", "gpu", "0.8.7", 1, 0, 0, 1, 0)),
        SNAP(DEV("pci:1:2", "camera", "0.8.7", 1, 0, 0, 1, 0)),
        SNAP(DEV("pci:1:2", "gpu", "0.8", 1, 0, 0, 1, 0)),
        SNAP(DEV("pci:1:2", "gpu", "0.8.7-beta", 1, 0, 0, 1, 0)),
        SNAP(DEV("pci:1:2", "gpu", "0.8.7", 1.5, 0, 0, 1, 0)),
        SNAP(DEV("pci:1:2", "gpu", "0.8.7", -1, 0, 0, 1, 0)),
        SNAP(DEV("pci:1:2", "gpu", "0.8.7", 1, 0, 0, 2, 0)),           /* counts don't add up */
        SNAP(DEV("pci:1:2", "gpu", "0.8.7", 1, 0, 0, 1, 2)),           /* more references than reports */
        SNAP(DEV("pci:1:2", "gpu", "0.8.7", 0, 0, 0, 0, 0)),           /* no report */
        SNAP("{\"key\":\"pci:1:2\",\"component\":\"gpu\",\"name\":\"bad\\u0007name\",\"lordos\":\"0.8.7\",\"works\":1,\"limits\":0,\"no\":0,\"reports\":1,\"references\":0}"),
        SNAP("{\"key\":\"pci:1:2\",\"component\":\"gpu\",\"name\":\"x\",\"lordos\":\"0.8.7\",\"works\":1,\"limits\":0,\"no\":0,\"reports\":1}"),
        SNAP("{\"key\":\"pci:1:2\",\"component\":\"gpu\",\"name\":\"x\",\"lordos\":\"0.8.7\",\"works\":1,\"limits\":0,\"no\":0,\"reports\":1,\"references\":0,\"x\":1}"),
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        HwSnapState s = check_signed(invalid[i], err, sizeof err);
        if (s != HWSNAP_INVALID) printf("  case %zu: %s\n", i, hwsnap_state_name(s));
        CHECK(s == HWSNAP_INVALID, "a malformed snapshot is refused");
    }
    /* a key at the limit and one past it */
    char key[HWSNAP_KEY_MAX + 2], text[1024];
    memset(key, 'k', sizeof key);
    key[0] = 'p'; key[1] = ':';
    key[HWSNAP_KEY_MAX] = 0;
    snprintf(text, sizeof text, SNAP("{\"key\":\"%s\",\"component\":\"gpu\",\"name\":\"x\",\"lordos\":\"0.8.7\",\"works\":1,\"limits\":0,\"no\":0,\"reports\":1,\"references\":0}"), key);
    CHECK(check_signed(text, err, sizeof err) == HWSNAP_OK, "a key at the limit");
    key[HWSNAP_KEY_MAX] = 'k'; key[HWSNAP_KEY_MAX + 1] = 0;
    snprintf(text, sizeof text, SNAP("{\"key\":\"%s\",\"component\":\"gpu\",\"name\":\"x\",\"lordos\":\"0.8.7\",\"works\":1,\"limits\":0,\"no\":0,\"reports\":1,\"references\":0}"), key);
    CHECK(check_signed(text, err, sizeof err) == HWSNAP_INVALID, "a key past the limit");
    /* too big: refused before the signature */
    char *big = malloc(HWSNAP_MAX + 2);
    memset(big, ' ', HWSNAP_MAX + 1);
    big[HWSNAP_MAX + 1] = 0;
    CHECK(hwsnap_check(big, HWSNAP_MAX + 1, sig, strlen(sig), pub, err, sizeof err) == HWSNAP_INVALID, "too big");
    free(big);
}

static void put(const char *dir, const char *name, const char *text) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(2); }
    fputs(text, f);
    fclose(f);
}
static void rm(const char *dir, const char *name) { char p[512]; snprintf(p, sizeof p, "%s/%s", dir, name); unlink(p); }

static void test_load(void) {
    char dir[] = "/tmp/hwsnap-test-XXXXXX", sig[100], err[160];
    if (!mkdtemp(dir)) { perror("mkdtemp"); exit(2); }
    char *json;
    size_t len;
    CHECK(hwsnap_load(dir, pub, &json, &len, err, sizeof err) == HWSNAP_MISSING && !json, "an empty folder");
    CHECK(hwsnap_load("/nonexistent/hwreport", pub, &json, &len, err, sizeof err) == HWSNAP_MISSING, "no folder");
    put(dir, HWSNAP_FILE, GOOD);
    CHECK(hwsnap_load(dir, pub, &json, &len, err, sizeof err) == HWSNAP_UNSIGNED && !json, "unsigned");
    sign_with(SEED, GOOD, strlen(GOOD), sig);
    put(dir, HWSNAP_SIG, sig);
    CHECK(hwsnap_load(dir, pub, &json, &len, err, sizeof err) == HWSNAP_OK && json && len == strlen(GOOD) && !strcmp(json, GOOD), "signed");
    free(json);
    CHECK(hwsnap_load(dir, other_pub, &json, &len, err, sizeof err) == HWSNAP_BAD_SIG && !json, "another key's build");
    /* links are not followed: neither for the snapshot nor for its signature */
    char target[600], link[600];
    snprintf(target, sizeof target, "%s/real.json", dir);
    snprintf(link, sizeof link, "%s/%s", dir, HWSNAP_FILE);
    rename(link, target);
    CHECK(symlink(target, link) == 0, "symlink");
    CHECK(hwsnap_load(dir, pub, &json, &len, err, sizeof err) == HWSNAP_MISSING && !json, "the snapshot is a link");
    unlink(link);
    rename(target, link);
    snprintf(target, sizeof target, "%s/real.sig", dir);
    snprintf(link, sizeof link, "%s/%s", dir, HWSNAP_SIG);
    rename(link, target);
    CHECK(symlink(target, link) == 0, "symlink");
    CHECK(hwsnap_load(dir, pub, &json, &len, err, sizeof err) == HWSNAP_UNSIGNED && !json, "the signature is a link");
    unlink(link);
    rm(dir, "real.sig");
    /* a folder where the file should be */
    rm(dir, HWSNAP_FILE);
    snprintf(link, sizeof link, "%s/%s", dir, HWSNAP_FILE);
    mkdir(link, 0700);
    CHECK(hwsnap_load(dir, pub, &json, &len, err, sizeof err) == HWSNAP_MISSING && !json, "a folder, not a file");
    rmdir(link);
    rmdir(dir);
}

/* the snapshot catalog.mjs wrote: signed with the test key, it passes the C checks */
static void test_built(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAILED: cannot read %s\n", path); fail++; return; }
    static char buf[HWSNAP_MAX + 1];
    size_t n = fread(buf, 1, HWSNAP_MAX, f);
    fclose(f);
    buf[n] = 0;
    char sig[100], err[160] = "";
    sign_with(SEED, buf, n, sig);
    HwSnapState s = hwsnap_check(buf, n, sig, strlen(sig), pub, err, sizeof err);
    if (s != HWSNAP_OK) printf("  %s: %s (%s)\n", path, hwsnap_state_name(s), err);
    CHECK(s == HWSNAP_OK, "the built snapshot passes the C checks");
}

int main(int argc, char **argv) {
    ed25519_public(SEED, pub);
    ed25519_public(OTHER, other_pub);
    test_check();
    test_load();
    if (argc > 1) test_built(argv[1]);
    printf("hwsnap-test: %d ok, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
