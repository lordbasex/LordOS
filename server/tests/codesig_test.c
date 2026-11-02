/* codesig_test.c — verified system code (src/sec/codesig.c, docs/SIGNING.md "Verified system code"):
 * the manifest parser, the signature, the per-file check, reading under the root, the development
 * and release policies, and that the compiled-in key is tools/release/lordos-release.pub.pem.
 *
 *   build/codesig-test [path to lordos-release.pub.pem]      (make test passes it) */
#include "sec/codesig.h"
#include "pkg/crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int pass, fail;
#define CHECK(cond, name) do { if (cond) pass++; else { fail++; printf("FAILED: %s (line %d)\n", name, __LINE__); } } while (0)

static const uint8_t SEED[32] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                  17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 };
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

static void sign_text(const char *text, char sig[100]) {
    uint8_t s[64];
    ed25519_sign(SEED, (const uint8_t *)text, strlen(text), s);
    b64(s, 64, sig);
    strcat(sig, "\n");
}

static void hexsha(const char *data, size_t n, char out[65]) {
    uint8_t h[32];
    sha256(data, n, h);
    hex_encode(h, 32, out);
}

/* a manifest with two files: "a/one.js" = A, "b.js" = B */
static const char A[] = "console.log('one');\n", B[] = "globalThis.x = 2;\n";
static void manifest(char *out, size_t n, const char *kind) {
    char ha[65], hb[65];
    hexsha(A, strlen(A), ha);
    hexsha(B, strlen(B), hb);
    snprintf(out, n, "{\n  \"format\": 1,\n  \"kind\": \"%s\",\n  \"version\": \"0.8.2\",\n  \"files\": [\n"
                     "    { \"path\": \"a/one.js\", \"size\": %zu, \"sha256\": \"%s\" },\n"
                     "    { \"path\": \"b.js\", \"size\": %zu, \"sha256\": \"%s\" }\n  ]\n}\n",
             kind, strlen(A), ha, strlen(B), hb);
}

static int parse(const char *json, const char *sig, CodeManifest *m) {
    char err[160];
    int r = codesig_parse(json, strlen(json), sig, sig ? strlen(sig) : 0, pub, m, err, sizeof err);
    return r;
}

static void test_parser(void) {
    char j[2048], sig[100];
    CodeManifest m;
    manifest(j, sizeof j, CODE_KIND);
    sign_text(j, sig);
    CHECK(parse(j, sig, &m) == 0 && m.signed_ok && m.nfiles == 2 && !strcmp(m.version, "0.8.2"), "signed manifest parses");
    CHECK(codesig_find(&m, "b.js") && codesig_find(&m, "a/one.js") && !codesig_find(&m, "c.js"), "find");
    CHECK(codesig_check_buf(&m, "a/one.js", A, strlen(A), NULL, 0) == 0, "right bytes pass");
    char bad[64];
    snprintf(bad, sizeof bad, "%s", A);
    bad[3] ^= 1;                                          /* one modified byte */
    char why[96];
    CHECK(codesig_check_buf(&m, "a/one.js", bad, strlen(bad), why, sizeof why) != 0 && strstr(why, "SHA-256"), "one modified byte is refused");
    CHECK(codesig_check_buf(&m, "a/one.js", A, strlen(A) - 1, why, sizeof why) != 0 && strstr(why, "size"), "truncated file is refused");
    CHECK(codesig_check_buf(&m, "evil.js", A, strlen(A), why, sizeof why) != 0 && strstr(why, "not listed"), "unlisted file is refused");
    codesig_free(&m);

    CHECK(parse(j, NULL, &m) == 0 && !m.signed_ok && m.nfiles == 2, "unsigned manifest parses as development");
    codesig_free(&m);

    /* the signature covers every byte */
    char j2[2100];
    snprintf(j2, sizeof j2, "%s", j);
    char *p = strstr(j2, "0.8.2");
    p[4] = '3';
    CHECK(parse(j2, sig, &m) != 0, "altered manifest: bad signature");
    snprintf(j2, sizeof j2, "%s ", j);
    CHECK(parse(j2, sig, &m) != 0, "one extra byte: bad signature");
    /* another key */
    {
        char err[96];
        CHECK(codesig_parse(j, strlen(j), sig, strlen(sig), other_pub, &m, err, sizeof err) != 0 && strstr(err, "signature"), "other key: refused");
    }
    /* broken signature encodings */
    char s2[100];
    snprintf(s2, sizeof s2, "%s", sig);
    s2[10] = s2[10] == 'A' ? 'B' : 'A';
    CHECK(parse(j, s2, &m) != 0, "flipped signature character");
    CHECK(parse(j, "AAAA\n", &m) != 0, "short signature");
    snprintf(s2, sizeof s2, "%s", sig);
    s2[3] = '*';
    CHECK(parse(j, s2, &m) != 0, "signature with a non-base64 character");
    uint8_t raw[64];
    CHECK(codesig_sig_decode(sig, strlen(sig), raw) == 0, "signature decodes");
    snprintf(s2, sizeof s2, "%s", sig);
    s2[85] = s2[85] == 'B' ? 'C' : 'B';                    /* leftover bits not zero → refused or different */
    CHECK(codesig_sig_decode(s2, 88, raw) != 0 || memcmp(s2, sig, 88), "non-canonical base64");

    /* wrong kind (a signature for something else must not pass as this) */
    manifest(j2, sizeof j2, "something-else");
    sign_text(j2, s2);
    CHECK(parse(j2, s2, &m) != 0, "wrong kind is refused");

    /* strict structure (unsigned, to reach the parser) */
    const char *bad_manifests[] = {
        "{\"format\":2,\"kind\":\"lordos-system-code\",\"version\":\"1\",\"files\":[]}",
        "{\"format\":1,\"kind\":\"lordos-system-code\",\"files\":[]}",
        "{\"format\":1,\"kind\":\"lordos-system-code\",\"version\":\"1\",\"files\":{}}",
        "{\"format\":1,\"kind\":\"lordos-system-code\",\"version\":\"1\",\"files\":[{\"path\":\"../etc/passwd\",\"size\":1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}",
        "{\"format\":1,\"kind\":\"lordos-system-code\",\"version\":\"1\",\"files\":[{\"path\":\"/abs.js\",\"size\":1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}",
        "{\"format\":1,\"kind\":\"lordos-system-code\",\"version\":\"1\",\"files\":[{\"path\":\"a/./b.js\",\"size\":1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}",
        "{\"format\":1,\"kind\":\"lordos-system-code\",\"version\":\"1\",\"files\":[{\"path\":\"a//b.js\",\"size\":1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}",
        "{\"format\":1,\"kind\":\"lordos-system-code\",\"version\":\"1\",\"files\":[{\"path\":\"a b.js\",\"size\":1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}",
        "{\"format\":1,\"kind\":\"lordos-system-code\",\"version\":\"1\",\"files\":[{\"path\":\"a.js\",\"size\":-1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}",
        "{\"format\":1,\"kind\":\"lordos-system-code\",\"version\":\"1\",\"files\":[{\"path\":\"a.js\",\"size\":1.5,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}",
        "{\"format\":1,\"kind\":\"lordos-system-code\",\"version\":\"1\",\"files\":[{\"path\":\"a.js\",\"size\":1,\"sha256\":\"00000000000000000000000000000000000000000000000000000000000000\"}]}",
        "{\"format\":1,\"kind\":\"lordos-system-code\",\"version\":\"1\",\"files\":[{\"path\":\"a.js\",\"size\":1,\"sha256\":\"ABCDEF0000000000000000000000000000000000000000000000000000000000\"}]}",
        "{\"format\":1,\"kind\":\"lordos-system-code\",\"version\":\"1\",\"files\":[{\"path\":\"b.js\",\"size\":1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"},"
            "{\"path\":\"a.js\",\"size\":1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}",
        "{\"format\":1,\"kind\":\"lordos-system-code\",\"version\":\"1\",\"files\":[{\"path\":\"a.js\",\"size\":1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"},"
            "{\"path\":\"a.js\",\"size\":1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}",
        "{\"format\":1,\"kind\":\"lordos-system-code\",\"version\":\"1\",\"files\":[]} trailing",
        "not json",
    };
    for (size_t i = 0; i < sizeof bad_manifests / sizeof bad_manifests[0]; i++) {
        char name[64];
        snprintf(name, sizeof name, "strict manifest #%zu refused", i);
        CHECK(parse(bad_manifests[i], NULL, &m) != 0, name);
    }
    CHECK(parse("{\"format\":1,\"kind\":\"lordos-system-code\",\"version\":\"1\",\"files\":[]}", NULL, &m) == 0 && m.nfiles == 0, "empty list is fine");
    codesig_free(&m);
}

/* ---- the process state against a real folder ---- */

static char root[256];
static void put(const char *rel, const char *data) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "wb");
    if (f) { fputs(data, f); fclose(f); }
}

static int loads(const char *rel, const char *want) {
    char p[512], why[160];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    size_t n = 0;
    char *b = codesig_load(p, CODE_FILE_MAX, &n, why, sizeof why);
    int ok = b && n == strlen(want) && !memcmp(b, want, n) && b[n] == 0;
    free(b);
    return ok;
}

static void test_root(void) {
    snprintf(root, sizeof root, "%s/codesig-test.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(root)) { fail++; printf("FAILED: mkdtemp\n"); return; }
    char p[512];
    snprintf(p, sizeof p, "%s/a", root);
    mkdir(p, 0755);
    put("a/one.js", A);
    put("b.js", B);
    put("evil.js", "steal();\n");
    char j[2048], sig[100];
    manifest(j, sizeof j, CODE_KIND);
    sign_text(j, sig);

    /* no manifest: development (everything loads, flagged) / release: nothing */
    CHECK(codesig_init_root(root, pub, 0) == CODE_DEVELOPMENT && !codesig_manifest(), "no manifest: development");
    CHECK(loads("b.js", B) && loads("evil.js", "steal();\n"), "development without manifest loads");
    CHECK(codesig_init_root(root, pub, 1) == CODE_INVALID, "no manifest in a release build: invalid");
    CHECK(!loads("b.js", B) && codesig_refused_count() == 1, "release without manifest refuses");

    /* unsigned manifest: development with hashes; release: invalid */
    put(CODE_MANIFEST, j);
    CHECK(codesig_init_root(root, pub, 0) == CODE_DEVELOPMENT && codesig_manifest(), "unsigned: development");
    CHECK(loads("a/one.js", A) && loads("b.js", B), "development checks and loads listed files");
    CHECK(!loads("evil.js", "steal();\n"), "development refuses an unlisted file");
    CHECK(codesig_init_root(root, pub, 1) == CODE_INVALID, "unsigned in a release build: invalid");

    /* signed */
    put(CODE_SIGNATURE, sig);
    CHECK(codesig_init_root(root, pub, 1) == CODE_VERIFIED, "signed: verified (release)");
    CHECK(codesig_init_root(root, pub, 0) == CODE_VERIFIED, "signed: verified (development build)");
    CHECK(loads("a/one.js", A) && loads("b.js", B), "verified files load");
    CHECK(codesig_refused_count() == 0, "no refusals yet");

    /* one modified byte in a bundle */
    char mod[64];
    snprintf(mod, sizeof mod, "%s", B);
    mod[0] = 'G';
    put("b.js", mod);
    CHECK(!loads("b.js", mod), "modified bundle is refused");
    CHECK(codesig_refused_count() == 1 && !strcmp(codesig_refused(0)->path, "b.js") && !codesig_refused(0)->reported, "refusal recorded");
    CHECK(!loads("b.js", mod) && codesig_refused_count() == 1, "same refusal recorded once");
    put("b.js", B);
    CHECK(loads("b.js", B), "restored bundle loads again");

    /* a link that leaves the root, and a file outside it */
    snprintf(p, sizeof p, "%s/a/one.js", root);
    unlink(p);
    char target[512];
    snprintf(target, sizeof target, "%s/../codesig-outside.js", root);
    FILE *f = fopen(target, "wb");
    if (f) { fputs(A, f); fclose(f); }
    if (symlink(target, p) == 0) CHECK(!loads("a/one.js", A), "link out of the root is refused");
    char why[160];
    size_t n;
    char *b = codesig_load(target, CODE_FILE_MAX, &n, why, sizeof why);
    CHECK(!b && strstr(why, "not part"), "file outside the root is refused when verified");
    free(b);
    unlink(p);
    unlink(target);
    put("a/one.js", A);

    /* a forged signature next to a modified manifest */
    char j2[2048];
    snprintf(j2, sizeof j2, "%s", j);
    char hx[65];
    hexsha("steal();\n", 9, hx);
    char *at = strstr(j2, "\"sha256\": \"") + 11;                  /* a/one.js's hash */
    memcpy(at, hx, 64);
    put(CODE_MANIFEST, j2);
    CHECK(codesig_init_root(root, pub, 0) == CODE_INVALID, "modified manifest with the old signature: invalid");
    CHECK(!loads("b.js", B), "nothing loads with an invalid manifest");
    CHECK(codesig_init_root(root, other_pub, 0) == CODE_INVALID, "manifest signed by another key: invalid");

    /* clean up */
    const char *rm[] = { "a/one.js", "b.js", "evil.js", CODE_MANIFEST, CODE_SIGNATURE };
    for (size_t i = 0; i < sizeof rm / sizeof rm[0]; i++) { snprintf(p, sizeof p, "%s/%s", root, rm[i]); unlink(p); }
    snprintf(p, sizeof p, "%s/a", root);
    rmdir(p);
    rmdir(root);
}

/* The compiled-in key is the last 32 bytes of the PEM's DER. */
static void test_release_key(const char *pem) {
    FILE *f = pem ? fopen(pem, "r") : NULL;
    if (!f) { printf("skipped: release key check (no PEM path)\n"); return; }
    char line[256], b[256] = "";
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '-') continue;
        line[strcspn(line, "\r\n")] = 0;
        strncat(b, line, sizeof b - strlen(b) - 1);
    }
    fclose(f);
    /* decode base64 (44 bytes of DER for Ed25519) */
    uint8_t der[64];
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (const char *c = b; *c && *c != '='; c++) {
        const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const char *q = strchr(a, *c);
        if (!q) break;
        acc = acc << 6 | (uint32_t)(q - a);
        bits += 6;
        if (bits >= 8 && o < sizeof der) { bits -= 8; der[o++] = (uint8_t)(acc >> bits); }
    }
    CHECK(o == 44 && !memcmp(der + 12, codesig_release_key, 32), "compiled-in key matches lordos-release.pub.pem");
}

/* SHA-256: NIST vectors and the CPU's instructions (aarch64) against the C code, every length. */
static void test_sha256(void) {
    static const struct { const char *in; const char *hex; } v[] = {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
        { "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
          "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1" },
    };
    for (int hw = 0; hw <= 1; hw++) {
        sha256_select(hw);
        for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
            char hx[65];
            hexsha(v[i].in, strlen(v[i].in), hx);
            CHECK(!strcmp(hx, v[i].hex), hw ? "SHA-256 NIST vector (best path)" : "SHA-256 NIST vector (C)");
        }
    }
    size_t big = 3u << 20;
    uint8_t *buf = malloc(big);
    if (!buf) return;
    uint32_t x = 2463534242u;
    for (size_t i = 0; i < big; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; buf[i] = (uint8_t)x; }
    int same = 1;
    for (size_t n = 0; n <= 300 && same; n++) {
        uint8_t a[32], b[32];
        sha256_select(0); sha256(buf + n, n, a);
        sha256_select(1); sha256(buf + n, n, b);
        same = !memcmp(a, b, 32);
    }
    uint8_t a[32], b[32];
    sha256_select(0); sha256(buf, big, a);
    sha256_select(1); sha256(buf, big, b);
    CHECK(same && !memcmp(a, b, 32), "SHA-256: the CPU's instructions match the C code");
    /* speed of what axia-server uses here (docs/SIGNING.md, "Startup cost") */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    sha256(buf, big, a);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    sha256_select(0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    sha256(buf, big, a);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double msc = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    sha256_select(1);
    printf("sha256: %.0f MB/s (%s), C code %.0f MB/s\n", 3.0 / (ms / 1e3), sha256_accelerated() ? "CPU instructions" : "C code", 3.0 / (msc / 1e3));
    uint8_t s[64];
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ed25519_sign(SEED, buf, 1000, s);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int ok = ed25519_verify(pub, s, buf, 1000);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CHECK(ok, "ed25519 verify");
    printf("ed25519 verify: %.2f ms\n", (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    free(buf);
}

int main(int argc, char **argv) {
    ed25519_public(SEED, pub);
    uint8_t s2[32];
    memcpy(s2, SEED, 32);
    s2[0] ^= 0xff;
    ed25519_public(s2, other_pub);
    test_sha256();
    test_parser();
    test_root();
    test_release_key(argc > 1 ? argv[1] : NULL);
    printf("codesig: %d/%d\n", pass, pass + fail);
    return fail ? 1 : 0;
}
