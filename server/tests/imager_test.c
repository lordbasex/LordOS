/* imager_test.c — the LordOS Imager's pure checks (src/imager/imgverify.c, docs/IMAGER.md):
 * the release signature (good, tampered, missing, wrong key, malformed), SHA256SUMS lookups, image
 * names and tags, and the .xz index reader on real xz files (one stream, many blocks, two streams,
 * stream padding) plus damaged and random ones. Fixtures in tests/imager/: a real signed
 * SHA256SUMS from the 0.8.1 release and small xz files made with xz 5. Run from server/. */
#include "imager/imgverify.h"
#include "pkg/crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("missing fixture %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)l + 1);
    if (fread(b, 1, (size_t)l, f) != (size_t)l) exit(2);
    fclose(f);
    b[l] = 0;
    *n = (size_t)l;
    return b;
}

/* the release public key, from the PEM in the tree (the last 32 bytes of the DER) */
static void release_key(uint8_t pub[32]) {
    size_t n;
    char *pem = (char *)slurp("../tools/release/lordos-release.pub.pem", &n);
    char *b = strstr(pem, "-----\n") + 6;
    char *e = strstr(b, "\n-----END");
    *e = 0;
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t der[64];
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (char *p = b; *p && *p != '='; p++) {
        const char *q = strchr(A, *p);
        if (!q) continue;
        acc = acc << 6 | (unsigned)(q - A);
        bits += 6;
        if (bits >= 8) { bits -= 8; der[o++] = (uint8_t)(acc >> bits); }
    }
    CHECK(o == 44);
    memcpy(pub, der + o - 32, 32);
    /* the fingerprint docs/SIGNING.md publishes */
    uint8_t fp[32];
    char hex[65];
    Sha256 s;
    sha256_init(&s);
    sha256_update(&s, der, o);
    sha256_final(&s, fp);
    hex_encode(fp, 32, hex);
    CHECK(!strncmp(hex, "c7155b8e", 8));
    free(pem);
}

static void signature(void) {
    uint8_t pub[32];
    release_key(pub);
    size_t n, sn;
    uint8_t *sums = slurp("tests/imager/release-SHA256SUMS", &n);
    char *sig = (char *)slurp("tests/imager/release-SHA256SUMS.sig", &sn);
    CHECK(img_sig_verify(sums, n, sig, sn, pub) == 0);                    /* good */
    CHECK(img_sig_verify(sums, n, sig, sn - 1, pub) == 0);                /* without the newline */
    sums[10] ^= 1;
    CHECK(img_sig_verify(sums, n, sig, sn, pub) != 0);                    /* tampered list */
    sums[10] ^= 1;
    CHECK(img_sig_verify(sums, n - 1, sig, sn, pub) != 0);                /* truncated list */
    char bad[100];
    memcpy(bad, sig, sn);
    bad[5] = bad[5] == 'A' ? 'B' : 'A';
    CHECK(img_sig_verify(sums, n, bad, sn, pub) != 0);                    /* tampered signature */
    CHECK(img_sig_verify(sums, n, "", 0, pub) != 0);                      /* missing */
    CHECK(img_sig_verify(sums, n, sig, 40, pub) != 0);                    /* short */
    memcpy(bad, sig, sn);
    bad[10] = '*';
    CHECK(img_sig_verify(sums, n, bad, sn, pub) != 0);                    /* not base64 */
    memcpy(bad, sig, sn);
    bad[85] = (char)(bad[85] == 'A' ? 'B' : 'A');                         /* non-zero padding bits or a different byte */
    CHECK(img_sig_verify(sums, n, bad, sn, pub) != 0);
    uint8_t other[32];
    memcpy(other, pub, 32);
    other[0] ^= 0x40;
    CHECK(img_sig_verify(sums, n, sig, sn, other) != 0);                  /* another key */
    CHECK(img_sig_verify(NULL, 0, sig, sn, pub) != 0);

    uint8_t h[32];
    char hex[65];
    CHECK(img_sums_find((char *)sums, n, "lordos-0.8.1-rpi5-arm64.img.xz", h) == 0);
    hex_encode(h, 32, hex);
    CHECK(!strcmp(hex, "a7714f4f3d437a9e48088cb89579a93638b36e8a93944add23e18a126f04ea45"));
    CHECK(img_sums_find((char *)sums, n, "lordos-live-0.8.1-amd64.iso", h) == 0);
    CHECK(img_sums_find((char *)sums, n, "lordos-0.8.1-rpi5-arm64.img", h) == -1);
    CHECK(img_sums_find((char *)sums, n, "rpi5-arm64.img.xz", h) == -1);
    CHECK(img_sums_find("zz", 2, "x", h) == -2);
    const char dup[] = "a7714f4f3d437a9e48088cb89579a93638b36e8a93944add23e18a126f04ea45  x.img\n"
                       "a7714f4f3d437a9e48088cb89579a93638b36e8a93944add23e18a126f04ea45  x.img\n";
    CHECK(img_sums_find(dup, sizeof dup - 1, "x.img", h) == -2);
    const char star[] = "a7714f4f3d437a9e48088cb89579a93638b36e8a93944add23e18a126f04ea45 *x.img\r\n";
    CHECK(img_sums_find(star, sizeof star - 1, "x.img", h) == 0);
    free(sums);
    free(sig);
}

static void names(void) {
    CHECK(img_valid_name("lordos-0.8.4-rpi5-arm64.img.xz"));
    CHECK(img_valid_name("lordos-live-0.8.4-amd64.iso"));
    CHECK(!img_valid_name("../x.img"));
    CHECK(!img_valid_name(".x.img"));
    CHECK(!img_valid_name("-x.img"));
    CHECK(!img_valid_name("a b.img"));
    CHECK(!img_valid_name("a/b.img"));
    CHECK(!img_valid_name(""));
    CHECK(img_kind("a.img.xz") == 2);
    CHECK(img_kind("a.img") == 1);
    CHECK(img_kind("a.iso") == 1);
    CHECK(img_kind("a.zip") == 0);
    CHECK(img_kind(".img") == 0);
    CHECK(img_valid_tag("v0.8.4-beta"));
    CHECK(img_valid_tag("v1.0.0"));
    CHECK(img_valid_tag("v0.8.10-rc.1"));
    CHECK(img_valid_tag("v0.8.4.1-beta"));
    CHECK(img_valid_tag("v0.8.4.1"));
    CHECK(!img_valid_tag("v0.8.4.1.2"));
    CHECK(!img_valid_tag("v0.8.4."));
    CHECK(!img_valid_tag("v0.8.4.-beta"));
    CHECK(!img_valid_tag("0.8.4"));
    CHECK(!img_valid_tag("v0.8"));
    CHECK(!img_valid_tag("v0.8.4-"));
    CHECK(!img_valid_tag("v0.8.4-Beta"));
    CHECK(!img_valid_tag("v0.8.4/../x"));
}

typedef struct { const uint8_t *b; size_t n; } Mem;
static int mem_read(void *ud, uint64_t off, void *buf, size_t n) {
    Mem *m = ud;
    if (off > m->n || n > m->n - off) return -1;
    memcpy(buf, m->b + off, n);
    return 0;
}

static uint64_t xz_size_of(const uint8_t *b, size_t n, int *rc) {
    Mem m = { b, n };
    uint64_t s = 0;
    *rc = img_xz_size(mem_read, &m, n, &s);
    return s;
}

static unsigned long long rng = 88172645463325252ull;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)rng; }

static void xz(void) {
    static const struct { const char *f; uint64_t size; } ok[] = {
        { "tests/imager/one.xz", 23000 }, { "tests/imager/blocks.xz", 23000 },
        { "tests/imager/two.xz", 24000 }, { "tests/imager/padded.xz", 23000 },
    };
    for (size_t i = 0; i < sizeof ok / sizeof *ok; i++) {
        size_t n;
        uint8_t *b = slurp(ok[i].f, &n);
        int rc;
        uint64_t s = xz_size_of(b, n, &rc);
        if (rc || s != ok[i].size) printf("  %s: rc %d size %llu\n", ok[i].f, rc, (unsigned long long)s);
        CHECK(rc == 0 && s == ok[i].size);
        /* damage: every byte of the last 64 flipped in turn must never give a different valid size */
        for (size_t k = n > 64 ? n - 64 : 0; k < n; k++) {
            b[k] ^= 0x55;
            uint64_t s2 = xz_size_of(b, n, &rc);
            CHECK(rc != 0 || s2 == ok[i].size);
            b[k] ^= 0x55;
        }
        /* truncations */
        for (size_t cut = 1; cut < 40; cut++) {
            xz_size_of(b, n - cut, &rc);
            CHECK(rc != 0 || (strstr(ok[i].f, "padded") && cut % 4 == 0 && cut <= 8));   /* less padding is still valid */
        }
        /* random garbage in the same buffer: no crash (ASan in the fuzz build) */
        for (int it = 0; it < 2000; it++) {
            size_t at = rnd() % n;
            uint8_t old = b[at];
            b[at] = (uint8_t)rnd();
            xz_size_of(b, n, &rc);
            b[at] = old;
        }
        free(b);
    }
    int rc;
    uint8_t zeros[64] = { 0 };
    xz_size_of(zeros, sizeof zeros, &rc);
    CHECK(rc != 0);
    xz_size_of((const uint8_t *)"not xz at all, nope", 20, &rc);
    CHECK(rc != 0);
    CHECK(img_crc32(0, "123456789", 9) == 0xcbf43926u);
}

int main(void) {
    signature();
    names();
    xz();
    printf("imager-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
