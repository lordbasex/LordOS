/* pkg_test.c — tests of the package engine (server/src/pkg) without root or network, and a tool
 * to run the helper's actions by hand.
 *
 *   build/pkg-test                       unit tests (versions, deb822, relations, SHA,
 *                                        RSA, Ed25519, xz/gzip, index and resolver)
 *   build/pkg-test pgp <keyring> <InRelease>        verifies a real signature
 *   build/pkg-test plan <dir> <status> [-r<to-remove>] [to-install…] [--up] [--rec] [--yes]
 *   sudo build/pkg-test run <action> [argument]     pkg-update, pkg-install "a b", pkg-autoremove…, as
 *                                        admin.c runs them (progress goes to stdout) */
#include "pkg/crypto.h"
#include "pkg/deb.h"
#include "pkg/idx.h"
#include "pkg/match.h"
#include "pkg/pgp.h"
#include "pkg/pin.h"
#include "pkg/pkg.h"
#include "pkg/resolve.h"
#include "pkg/status.h"
#include "pkg/xz.h"
#include "errcode.h"

#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* what admin.c gives the actions */
void admin_say(const char *kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("%s ", kind);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

void admin_event(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("EVENT ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint8_t rsa_n[] = {
    0x70, 0xd2, 0xb9, 0x14, 0x94, 0x74, 0x1a, 0xa6, 0xd7, 0x9e, 0xd0, 0x90, 0x57, 0x3e, 0xf8, 0x2a, 0xeb,
    0xa7, 0x67, 0x42, 0xcc, 0x63, 0xb2, 0x44, 0x61, 0x6b, 0xc4, 0x93, 0x23, 0xa9, 0x69, 0x92, 0xcb, 0x56,
    0x93, 0x74, 0xc9, 0xa9, 0xf0, 0x5a, 0x43, 0xc7, 0xd1, 0x8d, 0xef, 0xdd, 0x2f, 0x26, 0x14, 0x4e, 0x05,
    0x24, 0xe2, 0x8c, 0xf7, 0x90, 0x07, 0x22, 0x5f, 0x66, 0xd0, 0xd9, 0xde, 0x7c, 0xc0, 0xa0, 0xf2, 0x22,
    0x5e, 0x2d, 0x5e, 0x65, 0x18, 0x05, 0x67, 0x46, 0x77, 0x56, 0x8b, 0x35, 0x0d, 0xed, 0xa3, 0x5a, 0xf1,
    0x5f, 0x1e, 0x3d, 0x45, 0x31, 0x58, 0x83, 0x07, 0xc5, 0x90, 0x7f, 0x05, 0x20, 0x91, 0x32, 0x32, 0x0f,
    0x8c, 0xa3, 0x8e, 0xdb, 0x04, 0x6c, 0x5f, 0xdb, 0xbc, 0xa1, 0xeb, 0x57, 0x48, 0xaa, 0x3f, 0x31, 0xea,
    0xdd, 0xfe, 0x2f, 0xc6, 0xf9, 0x9a, 0x39, 0x9e, 0x6b, 0x0f, 0xf7, 0x64, 0x48, 0xf4, 0xb7, 0x40, 0x6f,
    0x10, 0x0f, 0x52, 0x2c, 0xd5, 0xa9, 0x52, 0x79, 0x08, 0x22, 0x06, 0x49, 0xbf, 0x33, 0xd0, 0x99, 0xeb,
    0x9c, 0xc6, 0x6a, 0x7f, 0x6f, 0xc1, 0xaf, 0x07, 0x1e, 0x68, 0xd8, 0x57, 0x56, 0xa3, 0x63, 0x0a, 0x19,
    0x70, 0x4b, 0x71, 0x86, 0xc2, 0x2b, 0xb8, 0x9d, 0xe3, 0x96, 0x67, 0x9b, 0xbe, 0x6a, 0x61, 0xcc, 0x22,
    0x52, 0xbe, 0xab, 0x93, 0x12, 0x2c, 0xa2, 0x29, 0xa1, 0x46, 0x9b, 0x59, 0x0c, 0x63, 0x12, 0xe5, 0x6c,
    0xe2, 0xed, 0xc7, 0x6d, 0x78, 0xac, 0x79, 0x7b, 0x75, 0xeb, 0x05, 0x02, 0xc1, 0xd3, 0xb2, 0x12, 0xe3,
    0x08, 0xe2, 0x41, 0xa9, 0x02, 0xf6, 0xda, 0x28, 0xcb, 0xd3, 0xfe, 0xd4, 0x4a, 0x55, 0xa1, 0x7d, 0xf1,
    0x02, 0x25, 0xad, 0x0a, 0xd5, 0xe2, 0xc6, 0x82, 0x33, 0x2e, 0xa9, 0x6a, 0x83, 0x24, 0x9e, 0xd0, 0xaf,
    0xf5,
};
static const uint8_t rsa_sig[] = {
    0x00, 0xb0, 0x0f, 0x92, 0xe5, 0x01, 0x88, 0x1b, 0xa5, 0x4d, 0x60, 0x7d, 0x50, 0x4e, 0xfa, 0x2b, 0xa3,
    0x47, 0x05, 0x99, 0x75, 0x15, 0x26, 0xd7, 0xa4, 0x4e, 0x81, 0x6d, 0x0a, 0x10, 0x21, 0x3b, 0x91, 0xa4,
    0x37, 0x7c, 0xf7, 0xee, 0xe1, 0xbd, 0x4a, 0x3d, 0x10, 0x94, 0xd8, 0xf6, 0xd1, 0xdc, 0x9c, 0x96, 0x9d,
    0x67, 0x17, 0x82, 0x35, 0x82, 0x38, 0x72, 0x5f, 0x66, 0x11, 0x69, 0x73, 0x23, 0xa4, 0x95, 0xfe, 0x13,
    0xc6, 0xa5, 0xd4, 0xee, 0xdb, 0x91, 0x7d, 0xdc, 0x15, 0x64, 0xae, 0x06, 0xc1, 0xe5, 0x40, 0x63, 0x51,
    0x5b, 0x95, 0xc8, 0xde, 0xfd, 0x0f, 0xa0, 0xd7, 0xac, 0xba, 0xde, 0x25, 0x9e, 0x4b, 0x4b, 0xd3, 0x16,
    0x0a, 0xd8, 0x5c, 0xb1, 0x91, 0x68, 0xd0, 0x48, 0x55, 0x55, 0xa6, 0x56, 0x24, 0x6a, 0x38, 0x0a, 0x82,
    0x2b, 0x09, 0x28, 0x87, 0x68, 0x89, 0xcd, 0x8a, 0x6f, 0xcc, 0x12, 0x1f, 0xf6, 0x49, 0x3e, 0x97, 0xf5,
    0x9e, 0xfc, 0xb9, 0xf8, 0x95, 0xf6, 0xcc, 0x7c, 0x59, 0x11, 0x09, 0x25, 0x22, 0xd2, 0x82, 0x05, 0x89,
    0xec, 0x35, 0x85, 0x4e, 0xaf, 0xe7, 0xc7, 0x2f, 0xc0, 0x74, 0x6a, 0x65, 0xc1, 0xc4, 0x67, 0xb4, 0x66,
    0x9e, 0x61, 0x67, 0x9b, 0xe9, 0x00, 0x2c, 0x98, 0x02, 0x51, 0x3b, 0x25, 0x38, 0x12, 0xe7, 0xa7, 0x0c,
    0x8d, 0xdd, 0x43, 0xbe, 0x7e, 0xd9, 0xb2, 0x72, 0x99, 0x01, 0xf2, 0x26, 0x40, 0x08, 0x41, 0x24, 0x91,
    0x17, 0x2d, 0x8b, 0x7b, 0x00, 0xdd, 0x96, 0xf7, 0x10, 0xfa, 0x0b, 0xb7, 0xdb, 0xf8, 0xad, 0x67, 0xbe,
    0x6f, 0x72, 0x79, 0x3b, 0x07, 0xfe, 0x6c, 0x83, 0x33, 0x4f, 0x95, 0x13, 0x5a, 0x38, 0x4e, 0x0b, 0xf5,
    0x26, 0x91, 0x51, 0x57, 0xe6, 0x2c, 0x71, 0x17, 0x06, 0x4d, 0xe8, 0x61, 0x17, 0xfc, 0x22, 0x32, 0x69,
    0x0f,
};
static const uint8_t small_xz[] = {
    0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00, 0x00, 0x04, 0xe6, 0xd6, 0xb4, 0x46, 0x04, 0xc0, 0x37, 0x50, 0x21,
    0x01, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe9, 0xec, 0xcd, 0x13, 0xe0, 0x00,
    0x4f, 0x00, 0x2f, 0x5d, 0x00, 0x28, 0x18, 0x48, 0x66, 0xdb, 0xda, 0x30, 0x85, 0xfe, 0x16, 0xe5, 0x64,
    0xc7, 0x0f, 0x26, 0xc7, 0x95, 0x13, 0x7b, 0x5d, 0xaf, 0xa2, 0x3f, 0x11, 0xaa, 0x81, 0x9e, 0x85, 0x9e,
    0xd1, 0x49, 0x25, 0xe5, 0xed, 0x72, 0x7f, 0xea, 0xf5, 0x5d, 0xf4, 0x60, 0xba, 0x22, 0x0f, 0xde, 0x60,
    0x00, 0x00, 0x00, 0xb3, 0x77, 0x42, 0x90, 0x59, 0x52, 0x7b, 0xca, 0x00, 0x01, 0x53, 0x50, 0x48, 0xea,
    0x7b, 0xda, 0x1f, 0xb6, 0xf3, 0x7d, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x59, 0x5a,
};
static const uint8_t small_gz[] = {
    0x1f, 0x8b, 0x08, 0x08, 0x9f, 0x08, 0xd2, 0x6a, 0x00, 0x03, 0x73, 0x6d, 0x61, 0x6c, 0x6c, 0x2e, 0x74,
    0x78, 0x74, 0x00, 0x0b, 0x48, 0x4c, 0xce, 0x4e, 0x4c, 0x4f, 0xb5, 0x52, 0xc8, 0xc8, 0xcf, 0x49, 0xe4,
    0x0a, 0x4b, 0x2d, 0x2a, 0xce, 0xcc, 0xcf, 0xb3, 0x52, 0x30, 0xd4, 0x33, 0xe0, 0x72, 0x49, 0x2d, 0x4e,
    0x2e, 0xca, 0x2c, 0x28, 0x01, 0x0b, 0x80, 0xa4, 0x89, 0x20, 0xb8, 0x00, 0xc0, 0xa7, 0x24, 0xc0, 0x50,
    0x00, 0x00, 0x00,
};

static void test_versions(void) {
    static const struct { const char *a, *b; int r; } v[] = {
        { "1.0", "1.0", 0 }, { "1.0", "1.1", -1 }, { "1.0-1", "1.0-2", -1 }, { "1:0.1", "2.0", 1 },
        { "1.0~rc1", "1.0", -1 }, { "1.0~rc1", "1.0~rc2", -1 }, { "1.0", "1.0+b1", -1 }, { "1.0a", "1.0", 1 },
        { "1.0", "1.0.0", -1 }, { "2.30-1", "2.4-1", 1 }, { "0:1.2", "1.2", 0 }, { "1.2-0", "1.2", 0 },
        { "1.2.3-1~deb13u1", "1.2.3-1", -1 }, { "6.8.2+dfsg-9+deb13u2", "6.8.2+dfsg-9", 1 }, { "1.0-1", "1.0-1.1", -1 },
        { "a", "b", -1 }, { "1.0~~", "1.0~", -1 }, { "10", "9", 1 }, { "1.001", "1.1", 0 },
    };
    for (size_t i = 0; i < sizeof v / sizeof *v; i++) {
        int r = ver_cmp(v[i].a, v[i].b);
        CHECK(r == v[i].r, "ver_cmp(%s, %s) = %d, esperaba %d", v[i].a, v[i].b, r, v[i].r);
        CHECK(ver_cmp(v[i].b, v[i].a) == -v[i].r, "ver_cmp invertido %s %s", v[i].b, v[i].a);
    }
    CHECK(ver_satisfies("2.36-9", REL_GE, "2.34", 4), ">=");
    CHECK(!ver_satisfies("2.36-9", REL_LT, "2.34", 4), "<<");
    CHECK(ver_satisfies("1.0", REL_EQ, "1.0", 3), "=");
    CHECK(deb_valid_name("libc6", 5) && deb_valid_name("g++", 3) && !deb_valid_name("Ab", 2) && !deb_valid_name("a", 1) && !deb_valid_name("a/b", 3), "nombres");
}

static void test_deb822(void) {
    const char *t = "Package: a\nDepends: b (>= 1.0) | c, d:any,\n e (<< 2) [amd64]\nDescription: corto\n largo\n .\n fin\n\nPackage: z\n";
    const char *pos = t, *end = t + strlen(t), *st, *se;
    int n = 0;
    while (d822_next_stanza(&pos, end, &st, &se)) {
        D822 it = { st, se };
        D822Field f;
        int fields = 0;
        while (d822_next_field(&it, &f)) {
            fields++;
            if (d822_is(&f, "Depends")) {
                const char *p = f.value, *e = f.value + f.vlen;
                DebRel r[8];
                int k = deb_next_group(&p, e, r, 8);
                CHECK(k == 2 && r[0].nlen == 1 && r[0].op == REL_GE && r[0].vlen == 3 && r[1].op == REL_NONE, "grupo 1");
                k = deb_next_group(&p, e, r, 8);
                CHECK(k == 1 && r[0].nlen == 1 && r[0].name[0] == 'd', "d:any");
                k = deb_next_group(&p, e, r, 8);
                CHECK(k == 1 && r[0].name[0] == 'e' && r[0].op == REL_LT, "e (<< 2) [amd64]: op %d", r[0].op);
                CHECK(deb_next_group(&p, e, r, 8) == 0, "end of groups");
            }
            if (d822_is(&f, "Description")) CHECK(f.vlen == strlen("corto\n largo\n .\n fin"), "continuation %zu", f.vlen);
        }
        CHECK(fields == (n ? 1 : 3), "campos %d", fields);
        n++;
    }
    CHECK(n == 2, "paragraphs %d", n);
}

static void test_crypto(void) {
    uint8_t o[64];
    char h[129];
    sha256("abc", 3, o);
    hex_encode(o, 32, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256 %s", h);
    Sha512 s;
    sha512_init(&s);
    sha512_update(&s, "abc", 3);
    sha512_final(&s, o);
    hex_encode(o, 64, h);
    CHECK(!strncmp(h, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a", 64), "sha512 %s", h);
    /* RFC 8032, test 1 */
    uint8_t pk[32], sg[64];
    hex_decode("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", pk, 32);
    hex_decode("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", sg, 64);
    CHECK(ed25519_verify(pk, sg, (const uint8_t *)"", 0) == 1, "ed25519 valid");
    sg[10] ^= 1;
    CHECK(ed25519_verify(pk, sg, (const uint8_t *)"", 0) == 0, "ed25519 alterada");
    /* RSA-2048 generated for the test (tests: e = 65537, message "LordOS pkg test") */
    static const uint8_t e[] = { 1, 0, 1 };
    uint8_t dig[32];
    sha256("LordOS pkg test", 15, dig);
    CHECK(rsa_verify(rsa_n, sizeof rsa_n, e, 3, rsa_sig, sizeof rsa_sig, HASH_SHA256, dig) == 1, "rsa valid");
    dig[0] ^= 1;
    CHECK(rsa_verify(rsa_n, sizeof rsa_n, e, 3, rsa_sig, sizeof rsa_sig, HASH_SHA256, dig) == 0, "rsa with another hash");
}

static void test_compress(void) {
    const char *want = "Package: hola\nVersion: 1.0\nDescription: hola hola hola hola hola hola hola hola\n";
    uint8_t *o;
    size_t ol;
    CHECK(!xz_decode(small_xz, sizeof small_xz, &o, &ol, 1 << 20) && ol == strlen(want) && !memcmp(o, want, ol), "xz");
    if (ol) free(o);
    ol = 0;
    CHECK(!gz_decode(small_gz, sizeof small_gz, &o, &ol, 1 << 20) && ol == strlen(want) && !memcmp(o, want, ol), "gz");
    if (ol) free(o);
    uint8_t bad[sizeof small_xz];
    memcpy(bad, small_xz, sizeof bad);
    bad[30] ^= 0x55;
    o = NULL;
    int r = xz_decode(bad, sizeof bad, &o, &ol, 1 << 20);
    CHECK(r != 0 || ol != strlen(want) || memcmp(o, want, ol), "damaged xz does not yield the same text");
    if (!r) free(o);
}

/* A small fake repository for the resolver. */
static const char *REPO =
    "Package: app\nVersion: 2.0\nArchitecture: amd64\nDepends: libfoo (>= 1.5), web-browser | firefox-esr\nRecommends: extra\nFilename: pool/a/app.deb\nSize: 100\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: app\n\n"
    "Package: libfoo\nVersion: 1.6\nArchitecture: amd64\nBreaks: oldtool (<< 3)\nFilename: pool/l/libfoo.deb\nSize: 10\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: lib\n\n"
    "Package: oldtool\nVersion: 3.0\nArchitecture: amd64\nDepends: libfoo\nFilename: pool/o/oldtool.deb\nSize: 5\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: tool\n\n"
    "Package: lynx\nVersion: 1\nArchitecture: amd64\nProvides: web-browser\nPriority: optional\nFilename: pool/l/lynx.deb\nSize: 7\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: b\n\n"
    "Package: firefox-esr\nVersion: 1\nArchitecture: amd64\nProvides: web-browser\nPriority: optional\nFilename: pool/f/ff.deb\nSize: 70\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: f\n\n"
    "Package: extra\nVersion: 1\nArchitecture: all\nFilename: pool/e/extra.deb\nSize: 1\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: e\n\n"
    "Package: mta-a\nVersion: 1\nArchitecture: amd64\nProvides: mail-transport-agent\nConflicts: mail-transport-agent\nFilename: pool/m/a.deb\nSize: 1\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: a\n\n"
    "Package: armonly\nVersion: 1\nArchitecture: arm64\nFilename: pool/x.deb\nSize: 1\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: x\n\n";

static const char *STATUS =
    "Package: libfoo\nStatus: install ok installed\nVersion: 1.0\nArchitecture: amd64\nDescription: lib\n\n"
    "Package: oldtool\nStatus: install ok installed\nVersion: 2.0\nArchitecture: amd64\nDepends: libfoo\nDescription: tool\n\n"
    "Package: user\nStatus: install ok installed\nVersion: 1\nArchitecture: amd64\nDepends: oldtool\nDescription: u\n\n"
    "Package: mta-b\nStatus: install ok installed\nVersion: 1\nArchitecture: amd64\nProvides: mail-transport-agent\nConflicts: mail-transport-agent\nDescription: b\n\n"
    "Package: dpkg\nStatus: install ok installed\nVersion: 1.22\nArchitecture: amd64\nEssential: yes\nDescription: d\n\n"
    "Package: user2\nStatus: install ok installed\nVersion: 1\nArchitecture: amd64\nDepends: libdep | libalt\nRecommends: librec\nDescription: u2\n\n"
    "Package: libdep\nStatus: install ok installed\nVersion: 1\nArchitecture: amd64\nDepends: libdeep\nDescription: d\n\n"
    "Package: libdeep\nStatus: install ok installed\nVersion: 1\nArchitecture: amd64\nDescription: d\n\n"
    "Package: librec\nStatus: install ok installed\nVersion: 1\nArchitecture: amd64\nDescription: r\n\n"
    "Package: libold\nStatus: install ok installed\nVersion: 1\nArchitecture: amd64\nDescription: o\n\n"
    "Package: libmanual\nStatus: install ok installed\nVersion: 1\nArchitecture: amd64\nDescription: m\n\n"
    "Package: gone\nStatus: deinstall ok config-files\nVersion: 1\nArchitecture: amd64\nDescription: g\n\n";

static int has(const Plan *p, int action, const char *name) {
    for (int i = 0; i < p->n; i++) if (p->items[i].action == action && !strcmp(p->items[i].name, name)) return 1;
    return 0;
}

static void test_resolver(const char *dir) {
    IdxBuilder *b = idxb_new();
    int s = idxb_source(b, "http://x", "test", "main", "test", "T", "T", 0, 0, 500);
    idxb_packages(b, s, REPO, strlen(REPO), "amd64");
    char err[200];
    CHECK(!idxb_write(b, dir, "amd64", err, sizeof err), "write index: %s", err);
    idxb_free(b);
    char sp[600];
    snprintf(sp, sizeof sp, "%s/status", dir);
    FILE *f = fopen(sp, "w");
    fputs(STATUS, f);
    fclose(f);
    Idx x;
    Status st = { 0 };
    CHECK(!idx_open(&x, dir) && x.h->npkg == 7, "index opened (the arm64 one is left out): %u", x.map ? x.h->npkg : 0);
    CHECK(!status_load(&st, sp, "amd64") && st.n == 11, "status: %d instalados", st.n);
    Plan p;
    PlanOpts o = { 0 };
    const char *app[] = { "app" };
    plan_make(&x, &st, app, 1, NULL, 0, &o, &p);
    /* libfoo goes up to 1.6, which breaks oldtool (<< 3): oldtool is upgraded to 3.0; the virtual
     * browser is provided by one of the two (the first by priority and name) */
    CHECK(p.ok, "plan app: %s", p.err);
    CHECK(has(&p, PA_INSTALL, "app") && has(&p, PA_UPGRADE, "libfoo") && has(&p, PA_UPGRADE, "oldtool"), "app + libfoo + oldtool");
    CHECK(has(&p, PA_INSTALL, "lynx") ^ has(&p, PA_INSTALL, "firefox-esr"), "one browser");
    CHECK(!has(&p, PA_INSTALL, "extra"), "no recommends");
    plan_free(&p);
    o.recommends = 1;
    plan_make(&x, &st, app, 1, NULL, 0, &o, &p);
    CHECK(p.ok && has(&p, PA_INSTALL, "extra"), "with recommends");
    plan_free(&p);
    /* removing libfoo: oldtool and user must go too (cascade) */
    o = (PlanOpts){ 0 };
    const char *rm[] = { "libfoo" };
    plan_make(&x, &st, NULL, 0, rm, 1, &o, &p);
    CHECK(!p.ok && lerr_has(p.err, "pkg.removeAlso") && has(&p, PA_REMOVE, "oldtool") && has(&p, PA_REMOVE, "user"), "cascade asks for confirmation: %s", p.err);
    plan_free(&p);
    o.cascade = 1;
    plan_make(&x, &st, NULL, 0, rm, 1, &o, &p);
    CHECK(p.ok && p.n == 3, "cascada confirmada: %d", p.n);
    plan_free(&p);
    const char *ess[] = { "dpkg" };
    plan_make(&x, &st, NULL, 0, ess, 1, &o, &p);
    CHECK(!p.ok && lerr_has(p.err, "pkg.removeEssential"), "dpkg is not removed: %s", p.err);
    plan_free(&p);
    /* two MTAs: installing mta-a removes mta-b (with confirmation) */
    o = (PlanOpts){ 0 };
    const char *mta[] = { "mta-a" };
    plan_make(&x, &st, mta, 1, NULL, 0, &o, &p);
    CHECK(!p.ok && has(&p, PA_REMOVE, "mta-b"), "conflict with mta-b asks for confirmation: %s", p.err);
    plan_free(&p);
    o.allow_remove = 1;
    plan_make(&x, &st, mta, 1, NULL, 0, &o, &p);
    CHECK(p.ok && has(&p, PA_REMOVE, "mta-b") && has(&p, PA_INSTALL, "mta-a"), "mta-a replaces mta-b: %s", p.err);
    plan_free(&p);
    /* virtual with several providers */
    const char *wb[] = { "web-browser" };
    plan_make(&x, &st, wb, 1, NULL, 0, &o, &p);
    CHECK(!p.ok && lerr_has(p.err, "pkg.virtual"), "virtual: %s", p.err);
    plan_free(&p);
    /* orphans: libdep, libdeep and librec are automatic dependencies of user2; libold is used by
     * nobody; libmanual is not automatic */
    static const char *const autos[] = { "libdep", "libdeep", "librec", "libold" };
    o = (PlanOpts){ .orphans = 1, .autos = autos, .nautos = 4 };
    plan_make(&x, &st, NULL, 0, NULL, 0, &o, &p);
    CHECK(p.ok && p.n == 1 && has(&p, PA_REMOVE, "libold") && !strcmp(p.items[0].reason, "pkg.why.auto") && p.items[0].orphan, "autoremove: only libold (%d)", p.n);
    plan_free(&p);
    const char *u2[] = { "user2" };
    plan_make(&x, &st, NULL, 0, u2, 1, &o, &p);
    CHECK(p.ok && p.n == 5 && has(&p, PA_REMOVE, "libdep") && has(&p, PA_REMOVE, "libdeep") && has(&p, PA_REMOVE, "librec") &&
          has(&p, PA_REMOVE, "libold") && !has(&p, PA_REMOVE, "libmanual"), "removing user2 takes its dependencies, without --cascade: %d %s", p.n, p.err);
    plan_free(&p);
    o.orphans = 0;
    plan_make(&x, &st, NULL, 0, u2, 1, &o, &p);
    CHECK(p.ok && p.n == 1, "--keep-orphans: %d", p.n);
    plan_free(&p);
    /* upgrade everything */
    o = (PlanOpts){ .upgrade_all = 1 };
    plan_make(&x, &st, NULL, 0, NULL, 0, &o, &p);
    CHECK(p.ok && has(&p, PA_UPGRADE, "libfoo") && has(&p, PA_UPGRADE, "oldtool"), "upgrade: %s", p.err);
    int ord[16], grp[16];
    int n = plan_order(&x, &p, ord, grp);
    CHECK(n == 2 && !strcmp(p.items[ord[0]].name, "libfoo"), "order: libfoo before oldtool");
    plan_free(&p);
    status_free(&st);
    idx_close(&x);
}

/* apt's preferences: the Raspberry Pi image's pins (tools/rpi/rootfs/etc/apt/preferences.d/raspberrypi
 * and build-img.sh's lordos-kernel), checked against what apt chooses. */
static const char *PI_DEBIAN =
    "Package: chromium\nVersion: 140.0-1~deb13u1\nArchitecture: arm64\nFilename: pool/c/chromium.deb\nSize: 9\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: c\n\n"
    "Package: bluez\nVersion: 5.82-1\nArchitecture: arm64\nFilename: pool/b/bluez.deb\nSize: 9\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: b\n\n"
    "Package: mesa\nVersion: 25.0-1\nArchitecture: arm64\nFilename: pool/m/mesa.deb\nSize: 9\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: m\n\n";
static const char *PI_RPI =
    "Package: chromium\nVersion: 1:141.0-1+rpt1\nArchitecture: arm64\nFilename: pool/c/chromium-rpt.deb\nSize: 9\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: c\n\n"
    "Package: bluez\nVersion: 5.82-1+rpt2\nArchitecture: arm64\nFilename: pool/b/bluez-rpt.deb\nSize: 9\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: b\n\n"
    "Package: rpi-eeprom\nVersion: 28.4-1\nArchitecture: all\nFilename: pool/r/rpi-eeprom.deb\nSize: 9\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: r\n\n"
    "Package: raspi-config\nVersion: 2025\nArchitecture: all\nFilename: pool/r/raspi-config.deb\nSize: 9\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: r\n\n"
    "Package: linux-image-rpi-2712\nVersion: 1:6.12.47-1+rpt1\nArchitecture: arm64\nFilename: pool/l/k.deb\nSize: 9\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: k\n\n";
static const char *PI_LORDOS =
    "Package: rpi-eeprom\nVersion: 28.3-1\nArchitecture: all\nFilename: pool/rpi/r/rpi-eeprom.deb\nSize: 9\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: r\n\n";
static const char *PI_PREFS =
    "# LordOS on Raspberry Pi\n"
    "Package: *\nPin: origin archive.raspberrypi.com\nPin-Priority: 100\n\n"
    "Package: raspi-firmware linux-image-rpi-* linux-image-*-rpi-* rpi-eeprom raspi-utils* raspberrypi-archive-keyring\n"
    "Pin: origin archive.raspberrypi.com\nPin-Priority: 990\n";
static const char *PI_KERNEL =
    "Explanation: LordOS's own kernel\nPackage: linux-image-rpi-2712 linux-image-*+rpt-rpi-2712\nPin: version *\nPin-Priority: -1\n";

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static const char *cand_ver(const Idx *x, const char *name, const StPkg *inst) {
    long c = pkg_candidate(x, name, strlen(name), inst);
    return c < 0 ? "-" : idx_s(x, x->pkg[c].version);
}

/* (0.8.7) lordos-hwdata, the signed tested-hardware snapshot (tools/iso/mkdeb-hwdata.sh): an Architecture: all
 * package that lordos depends on, whose versions are dates. Updating lordos from a release without it brings it
 * in, and a newer snapshot alone is an ordinary upgrade (docs/BENCHMARK.md, "Newer snapshots between releases"). */
static const char *HW_REPO =
    "Package: lordos\nVersion: 0.8.7\nArchitecture: amd64\nDepends: libc6 (>= 2.38), lordos-hwdata (>= 2026.10.27)\nFilename: pool/main/l/lordos/lordos_0.8.7_amd64.deb\nSize: 9000\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: l\n\n"
    "Package: lordos-hwdata\nVersion: 2026.10.29\nArchitecture: all\nMulti-Arch: foreign\nFilename: pool/main/l/lordos-hwdata/lordos-hwdata_2026.10.29_all.deb\nSize: 3\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: h\n\n"
    "Package: lordos-hwdata\nVersion: 2026.11.05.1\nArchitecture: all\nMulti-Arch: foreign\nFilename: pool/main/l/lordos-hwdata/lordos-hwdata_2026.11.05.1_all.deb\nSize: 3\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: h\n\n"
    "Package: lordos-hwdata\nVersion: 2026.11.05\nArchitecture: all\nMulti-Arch: foreign\nFilename: pool/main/l/lordos-hwdata/lordos-hwdata_2026.11.05_all.deb\nSize: 3\nSHA256: 0000000000000000000000000000000000000000000000000000000000000000\nDescription: h\n\n";
static const char *HW_OLD =        /* a 0.8.5 system: no lordos-hwdata */
    "Package: libc6\nStatus: install ok installed\nVersion: 2.41\nArchitecture: amd64\nDescription: c\n\n"
    "Package: lordos\nStatus: install ok installed\nVersion: 0.8.5\nArchitecture: amd64\nDepends: libc6 (>= 2.38)\nDescription: l\n\n";
static const char *HW_NEW =        /* 0.8.7 with an older snapshot */
    "Package: libc6\nStatus: install ok installed\nVersion: 2.41\nArchitecture: amd64\nDescription: c\n\n"
    "Package: lordos\nStatus: install ok installed\nVersion: 0.8.7\nArchitecture: amd64\nDepends: libc6 (>= 2.38), lordos-hwdata (>= 2026.10.27)\nDescription: l\n\n"
    "Package: lordos-hwdata\nStatus: install ok installed\nVersion: 2026.10.29\nArchitecture: all\nDescription: h\n\n";

static const char *item_ver(const Plan *p, const char *name) {
    for (int i = 0; i < p->n; i++) if (!strcmp(p->items[i].name, name)) return p->items[i].version;
    return "-";
}

static void test_hwdata(const char *dir) {
    char idir[700], sp[700], err[200];
    snprintf(idir, sizeof idir, "%s/hwidx", dir);
    mkdir(idir, 0755);
    CHECK(ver_cmp("2026.11.05.1", "2026.11.05") > 0 && ver_cmp("2026.11.05", "2026.10.29.9") > 0 && ver_cmp("2026.10.29", "2026.10.27") > 0,
          "snapshot versions: the date, then .N for a second one that day");
    IdxBuilder *b = idxb_new();
    int s = idxb_source(b, "https://lordos.org/apt", "stable", "main", "stable", "LordOS", "LordOS", 0, 0, 500);
    idxb_packages(b, s, HW_REPO, strlen(HW_REPO), "amd64");
    CHECK(!idxb_write(b, idir, "amd64", err, sizeof err), "write hwdata index: %s", err);
    idxb_free(b);
    Idx x;
    CHECK(!idx_open(&x, idir) && x.h->npkg == 4, "hwdata index: the all packages are in the amd64 one (%u)", x.map ? x.h->npkg : 0);
    Status st = { 0 };
    Plan p;
    PlanOpts o = { 0 };
    /* Store → Updates' LordOS card: pkg-install lordos brings the new dependency, its newest version */
    snprintf(sp, sizeof sp, "%s/hwstatus-old", dir);
    write_text(sp, HW_OLD);
    CHECK(!status_load(&st, sp, "amd64") && st.n == 2, "old status");
    const char *lo[] = { "lordos" };
    plan_make(&x, &st, lo, 1, NULL, 0, &o, &p);
    CHECK(p.ok && p.n == 2 && has(&p, PA_UPGRADE, "lordos") && has(&p, PA_INSTALL, "lordos-hwdata") && !strcmp(item_ver(&p, "lordos-hwdata"), "2026.11.05.1"),
          "updating lordos installs lordos-hwdata 2026.11.05.1: %d %s %s", p.n, item_ver(&p, "lordos-hwdata"), p.err);
    plan_free(&p);
    /* Update all (pkg-upgrade) does the same */
    o = (PlanOpts){ .upgrade_all = 1 };
    plan_make(&x, &st, NULL, 0, NULL, 0, &o, &p);
    CHECK(p.ok && has(&p, PA_UPGRADE, "lordos") && has(&p, PA_INSTALL, "lordos-hwdata"), "update all brings lordos-hwdata too: %s", p.err);
    plan_free(&p);
    status_free(&st);
    /* between releases: only the snapshot is newer, an upgrade of one package */
    snprintf(sp, sizeof sp, "%s/hwstatus-new", dir);
    write_text(sp, HW_NEW);
    memset(&st, 0, sizeof st);
    CHECK(!status_load(&st, sp, "amd64") && st.n == 3, "new status");
    plan_make(&x, &st, NULL, 0, NULL, 0, &o, &p);
    CHECK(p.ok && p.n == 1 && has(&p, PA_UPGRADE, "lordos-hwdata") && !strcmp(item_ver(&p, "lordos-hwdata"), "2026.11.05.1"),
          "a newer snapshot alone: one upgrade, %d %s", p.n, p.err);
    plan_free(&p);
    status_free(&st);
    idx_close(&x);
}

static void test_pins(const char *dir) {
    char root[600], path[700];
    snprintf(root, sizeof root, "%s/pins", dir);
    snprintf(path, sizeof path, "%s/etc", root); mkdir(root, 0755); mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/etc/apt", root); mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/etc/apt/preferences.d", root); mkdir(path, 0755);
    pin_root = root;
    IdxBuilder *b = idxb_new();
    int deb = idxb_source(b, "http://deb.debian.org/debian", "stable", "main", "trixie", "Debian", "Debian", 0, 0, 500);
    int rpi = idxb_source(b, "http://archive.raspberrypi.com/debian", "trixie", "main", "trixie", "Raspberry Pi Foundation", "Raspberry Pi Foundation", 0, 0, 500);
    int lord = idxb_source(b, "https://lordos.org/apt", "stable", "rpi", "lordos", "LordOS", "LordOS", 0, 0, 500);
    idxb_packages(b, deb, PI_DEBIAN, strlen(PI_DEBIAN), "arm64");
    idxb_packages(b, rpi, PI_RPI, strlen(PI_RPI), "arm64");
    idxb_packages(b, lord, PI_LORDOS, strlen(PI_LORDOS), "arm64");
    char idir[700], err[200];
    snprintf(idir, sizeof idir, "%s/pinidx", dir);
    mkdir(idir, 0755);
    CHECK(!idxb_write(b, idir, "arm64", err, sizeof err), "write pinned index: %s", err);
    idxb_free(b);
    Idx x;
    CHECK(!idx_open(&x, idir) && !x.prio, "no preferences: source priorities only");
    CHECK(!strcmp(cand_ver(&x, "chromium", NULL), "1:141.0-1+rpt1"), "without pins the newest wins (the old bug): %s", cand_ver(&x, "chromium", NULL));
    idx_close(&x);

    snprintf(path, sizeof path, "%s/etc/apt/preferences.d/raspberrypi", root); write_text(path, PI_PREFS);
    snprintf(path, sizeof path, "%s/etc/apt/preferences.d/lordos-kernel", root); write_text(path, PI_KERNEL);
    snprintf(path, sizeof path, "%s/etc/apt/preferences.d/ignored.list", root); write_text(path, "Package: *\nPin: version *\nPin-Priority: -1\n");
    CHECK(!idx_open(&x, idir) && x.prio, "preferences read");
    CHECK(!strcmp(cand_ver(&x, "chromium", NULL), "140.0-1~deb13u1"), "chromium from Debian: %s", cand_ver(&x, "chromium", NULL));
    CHECK(!strcmp(cand_ver(&x, "bluez", NULL), "5.82-1"), "bluez from Debian: %s", cand_ver(&x, "bluez", NULL));
    CHECK(!strcmp(cand_ver(&x, "rpi-eeprom", NULL), "28.4-1"), "rpi-eeprom pinned 990 to Raspberry Pi: %s", cand_ver(&x, "rpi-eeprom", NULL));
    CHECK(!strcmp(cand_ver(&x, "raspi-config", NULL), "2025"), "only in Raspberry Pi's archive (100): still installable");
    CHECK(!strcmp(cand_ver(&x, "linux-image-rpi-2712", NULL), "-"), "Raspberry Pi's kernel never (-1)");
    CHECK(!strcmp(cand_ver(&x, "mesa", NULL), "25.0-1"), "untouched package");
    /* installed versions */
    StPkg deb_chr = { .name = "chromium", .version = "140.0-1~deb13u1" };
    StPkg rpt_chr = { .name = "chromium", .version = "1:141.0-1+rpt1" };
    StPkg old_chr = { .name = "chromium", .version = "139.0-1" };
    CHECK(!strcmp(cand_ver(&x, "chromium", &deb_chr), "140.0-1~deb13u1"), "Debian's chromium installed: stays (no switch to +rpt)");
    CHECK(!strcmp(cand_ver(&x, "chromium", &rpt_chr), "1:141.0-1+rpt1"), "an installed +rpt build is not downgraded below 1000");
    CHECK(!strcmp(cand_ver(&x, "chromium", &old_chr), "140.0-1~deb13u1"), "an older chromium upgrades to Debian's");
    StPkg rpt_cfg = { .name = "raspi-config", .version = "2024" };
    CHECK(!strcmp(cand_ver(&x, "raspi-config", &rpt_cfg), "2025"), "same priority (100): the newer one upgrades");
    /* a change in preferences.d is noticed without a new index */
    snprintf(path, sizeof path, "%s/etc/apt/preferences.d/zz-rpt.pref", root);
    write_text(path, "Package: chromium\nPin: release o=Raspberry Pi Foundation\nPin-Priority: 1001\n\nbroken stanza\n\nPackage: bluez\nPin: release v=1\nPin-Priority: 2000\n");
    struct timespec ts = { 0, 20000000 };
    nanosleep(&ts, NULL);
    CHECK(idx_refresh(&x, idir) == 1, "preferences changed: priorities recomputed");
    CHECK(!strcmp(cand_ver(&x, "chromium", &deb_chr), "1:141.0-1+rpt1"), "chromium pinned 1001 to Raspberry Pi: %s", cand_ver(&x, "chromium", &deb_chr));
    StPkg new_chr = { .name = "chromium", .version = "1:150" };
    CHECK(!strcmp(cand_ver(&x, "chromium", &new_chr), "1:141.0-1+rpt1"), "priority 1000 or more downgrades, as in apt");
    CHECK(!strcmp(cand_ver(&x, "bluez", NULL), "5.82-1"), "release v= is not checked: never matches");
    idx_close(&x);
    /* the first specific pin naming a package decides; later ones for it are ignored */
    snprintf(path, sizeof path, "%s/etc/apt/preferences.d/0-first", root);
    write_text(path, "Package: /^chrom/\nPin: origin deb.debian.org\nPin-Priority: 600\n");
    CHECK(!idx_open(&x, idir), "reopened");
    CHECK(!strcmp(cand_ver(&x, "chromium", &deb_chr), "140.0-1~deb13u1"), "a /regex/ pin read first wins over zz-rpt.pref: %s", cand_ver(&x, "chromium", &deb_chr));
    CHECK(!strcmp(cand_ver(&x, "chromium", &new_chr), "-"), "nothing newer than the installed one: no candidate");
    idx_close(&x);
    pin_root = dir;                     /* the other tests: no preferences */
}

static int cmd_plan(int argc, char **argv) {
    Idx x;
    Status st = { 0 };
    if (idx_open(&x, argv[2]) || status_load(&st, argv[3], "amd64")) { fprintf(stderr, "could not open\n"); return 1; }
    const char *ins[64], *rem[64];
    int ni = 0, nr = 0;
    PlanOpts o = { 0 };
    for (int i = 4; i < argc && ni < 64 && nr < 64; i++) {
        if (!strncmp(argv[i], "-r", 2)) rem[nr++] = argv[i] + 2;
        else if (!strcmp(argv[i], "--up")) o.upgrade_all = 1;
        else if (!strcmp(argv[i], "--rec")) o.recommends = 1;
        else if (!strcmp(argv[i], "--yes")) o.allow_remove = o.cascade = 1;
        else ins[ni++] = argv[i];
    }
    Plan p;
    plan_make(&x, &st, ins, ni, rem, nr, &o, &p);
    printf("ok=%d %s · download %llu bytes · %lld KB\n", p.ok, p.err, (unsigned long long)p.download, (long long)p.isize_delta);
    for (int i = 0; i < p.n; i++)
        printf("  %-7s %s %s%s%s (%s)\n", p.items[i].action == PA_INSTALL ? "install" : p.items[i].action == PA_UPGRADE ? "upgrade" : "remove",
               p.items[i].name, p.items[i].old_version ? p.items[i].old_version : "", p.items[i].old_version ? " -> " : "", p.items[i].version, p.items[i].reason);
    plan_free(&p);
    return 0;
}

/* The search (pkg/match.c, #37): words in any order, case and accents ignored, joined or apart,
 * every word first, then some of them */
static int score_of(const char *query, const char *name, const char *app, const char *app_sum, const char *sum, int *full) {
    PkgQuery q;
    pkg_query_parse(&q, query);
    return pkg_match_score(&q, name, app, app_sum, sum, full);
}
static void test_search(void) {
    char b[64];
    pkg_fold("Ação Ñandú ÉCOLE Łódź", b, sizeof b);
    CHECK(!strcmp(b, "acao nandu ecole lodz"), "fold: [%s]", b);
    pkg_fold("Cafe\xcc\x81", b, sizeof b);
    CHECK(!strcmp(b, "cafe"), "fold drops a combining mark: [%s]", b);
    pkg_fold("日本 x", b, sizeof b);
    CHECK(!strcmp(b, "日本 x"), "fold keeps other scripts: [%s]", b);
    pkg_fold("abcdef", b, 4);
    CHECK(!strcmp(b, "abc"), "fold stops at the buffer: [%s]", b);
    pkg_compact("VS Code (OSS)", b, sizeof b);
    CHECK(!strcmp(b, "vscodeoss"), "compact: [%s]", b);
    PkgQuery q;
    pkg_query_parse(&q, "  VS   Code ");
    CHECK(q.n == 2 && !strcmp(q.w[0], "vs") && !strcmp(q.w[1], "code") && !strcmp(q.joined, "vscode"), "query words: %d [%s]", q.n, q.joined);
    pkg_query_parse(&q, "brave-browser");
    CHECK(q.n == 2 && !strcmp(q.joined, "bravebrowser"), "punctuation separates words");
    pkg_query_parse(&q, "   ");
    CHECK(q.n == 0, "an empty query has no words");
    CHECK(pkg_match_level("code", NULL, "code", 4) == 4 && pkg_match_level("codium", NULL, "cod", 3) == 3 &&
          pkg_match_level("node-vscode", NULL, "vscode", 6) == 2 && pkg_match_level("libvscode", NULL, "vscode", 6) == 1 &&
          pkg_match_level("libvs", NULL, "vs", 2) == 0 && pkg_match_level("vs code", "vscode", "vscode", 6) == 1,
          "match levels");

    int full = 0, s;
    s = score_of("", "code", NULL, NULL, "x", &full);
    CHECK(s == 0, "an empty query matches nothing");
    /* the words joined: "vs code" finds the vscode packages, ahead of a package with both words apart */
    int a = score_of("vs code", "node-vscode-jsonrpc", NULL, NULL, "Node.js module", &full);
    CHECK(a > 0 && full, "vs code finds node-vscode-jsonrpc (%d)", a);
    int r = score_of("vs code", "librust-vsock-dev", NULL, NULL, "Virtio socket support for Rust - Rust source code", &full);
    CHECK(r > 0 && full && r < a, "vsock with \"code\" in its summary ranks below (%d < %d)", r, a);
    /* the words apart in the name: "vscode" finds "VS Code" */
    s = score_of("vscode", "code-oss", "VS Code OSS", NULL, NULL, &full);
    CHECK(s > 0 && full, "vscode finds the app VS Code OSS (%d)", s);
    /* the app's whole name, words in any order, case and accents */
    int e = score_of("Visual Studio Code", "code", "Visual Studio Code", "Editor", "Code editing.", &full);
    CHECK(e > 0 && full, "the whole app name (%d)", e);
    s = score_of("code studio", "code", "Visual Studio Code", NULL, NULL, &full);
    CHECK(s > 0 && full && s < e, "words in another order, below the exact name (%d)", s);
    s = score_of("camara", "cheese", "Cámara", NULL, NULL, &full);
    CHECK(s > 0 && full, "accents ignored (%d)", s);
    s = score_of("CODIUM", "codium", NULL, NULL, "Free/Libre Open Source Software Binaries of VS Code", &full);
    int s2 = score_of("codium", "codium-insiders", NULL, NULL, "x", &full), s3 = score_of("codium", "vscodium-tools", NULL, NULL, "x", &full);
    CHECK(s > s2 && s2 > s3 && s3 > 0, "the exact package name, then its start, then inside (%d > %d > %d)", s, s2, s3);
    s = score_of("vs code", "cursor", "Cursor", "Code editor, based on VS Code", NULL, &full);
    CHECK(s > r && full, "the words together in a summary rank above them apart (%d > %d)", s, r);
    /* some words: only when one starts a word of the name, and below every full match */
    int p = score_of("vs code", "code-aster", NULL, NULL, "finite element", &full);
    CHECK(p > 0 && !full && p < r, "code-aster is a partial match, below the full ones (%d)", p);
    s = score_of("vs code", "librust-foo", NULL, NULL, "Rust source code", &full);
    CHECK(s == 0, "a word only in the summary is not enough for a partial match (%d)", s);
    s = score_of("code", "librust-foo", NULL, NULL, "Rust source code", &full);
    CHECK(s > 0 && full, "one word in the summary is a match (%d)", s);
    s = score_of("x code", "codeblocks", NULL, NULL, "IDE", &full);
    CHECK(s > 0 && !full, "partial with the other word missing (%d)", s);
    s = score_of("zz yy", "codeblocks", NULL, NULL, "IDE", &full);
    CHECK(s == 0, "no word at all");
    s = score_of("a code", "a-tool", NULL, NULL, "IDE", &full);
    CHECK(s == 0, "a one-letter word alone doesn't make a partial match (%d)", s);
}

/* ReleaseNotes (docs/UPDATES.md): the stanza of that package and version, " ." as an empty line */
static void test_notes(void) {
    const char *t = "Package: lordos\nVersion: 0.8.1\nNotes:\n ### Added\n .\n - Updates from the Store\n\n"
                    "Package: lordos\nVersion: 0.8.2~beta1\nNotes: one line\n\nPackage: other\nVersion: 0.8.1\nNotes:\n x\n";
    size_t n = 0;
    char *s = deb_notes_find(t, strlen(t), "lordos", "0.8.1", &n);
    CHECK(s && !strcmp(s, "### Added\n\n- Updates from the Store") && n == strlen(s), "notes 0.8.1: [%s]", s ? s : "(null)");
    free(s);
    s = deb_notes_find(t, strlen(t), "lordos", "0.8.2~beta1", &n);
    CHECK(s && !strcmp(s, "one line"), "notes on the field line: [%s]", s ? s : "(null)");
    free(s);
    CHECK(!deb_notes_find(t, strlen(t), "lordos", "0.8.3", &n), "no notes for another version");
    CHECK(!deb_notes_find(t, strlen(t), "lordo", "0.8.1", &n), "the package name must match whole");
    CHECK(ver_cmp("0.8.1~staging1", "0.8.0") > 0 && ver_cmp("0.8.1~staging1", "0.8.1") < 0 && ver_cmp("0.8.1~beta2", "0.8.1~beta10") < 0,
          "pre-release versions sort between releases");
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "pgp")) {
        PgpKeyring kr = { 0 };
        if (pgp_keyring_add_file(&kr, argv[2])) { printf("llavero ilegible\n"); return 1; }
        FILE *f = fopen(argv[3], "rb");
        if (!f) return 1;
        static char buf[16 << 20];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        char *body, err[256];
        size_t bl;
        int g = pgp_verify_clearsigned(&kr, buf, n, (int64_t)time(NULL), &body, &bl, err, sizeof err);
        printf("%d keys, %d valid signatures %s\n", kr.n, g, err);
        return g > 0 ? 0 : 1;
    }
    if (argc >= 4 && !strcmp(argv[1], "plan")) return cmd_plan(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "run")) {
        const char *u = getenv("SUDO_USER");
        struct passwd *pw = getpwnam(u ? u : "root");
        AdminReq r = { 1, pw ? pw->pw_name : "root", pw ? pw->pw_dir : "/root", pw ? pw->pw_uid : 0, pw ? pw->pw_gid : 0, 1 };
        const char *arg = argc > 3 ? argv[3] : "";
        size_t len = strlen(arg);
        if (!strcmp(argv[2], "pkg-update")) pkg_act_update(&r, arg, len);
        else if (!strcmp(argv[2], "pkg-install")) pkg_act_install(&r, arg, len);
        else if (!strcmp(argv[2], "pkg-remove")) pkg_act_remove(&r, arg, len);
        else if (!strcmp(argv[2], "pkg-upgrade")) pkg_act_upgrade(&r, arg, len);
        else if (!strcmp(argv[2], "pkg-autoremove")) pkg_act_autoremove(&r, arg, len);
        else if (!strcmp(argv[2], "pkg-install-file")) pkg_act_install_file(&r, arg, len);
        else { printf("unknown action\n"); return 2; }
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        printf("peak memory: %ld KB\n", ru.ru_maxrss);
        return 0;
    }
    char dir[] = "/tmp/pkg-test-XXXXXX";
    if (!mkdtemp(dir)) return 1;
    test_versions();
    test_deb822();
    test_notes();
    test_search();
    test_crypto();
    test_compress();
    pin_root = dir;                     /* never the machine's own /etc/apt/preferences */
    test_resolver(dir);
    test_hwdata(dir);
    test_pins(dir);
    char cmd[256];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd)) {}
    printf("pkg-test: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
