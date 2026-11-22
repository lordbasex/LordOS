/* vmurl_test.c — what vm-image-get-user accepts (src/vm/vmurl.c, src/pkg/pubhost.c): addresses, public names and
 * addresses, sizes, free space and the redaction of an address for logs; then a fuzz pass over mutated addresses
 * whose every accepted one must still keep the rules. Under ASan and UBSan (mk/pgpv.mk). */
#include "../src/vm/vmurl.h"
#include "../src/pkg/pubhost.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int ok_url(const char *u) { char h[256], f[128]; return !vmurl_check(u, h, sizeof h, f, sizeof f); }

static void test_urls(void) {
    static const char *const good[] = {
        "https://cdimage.debian.org/debian-cd/current/amd64/iso-cd/debian-13.2.0-amd64-netinst.iso",
        "https://mirrors.edge.kernel.org/linuxmint/stable/22.1/linuxmint-22.1-cinnamon-64bit.iso",
        "https://download.example-mirror.org:443/x/y.iso",
        "https://a.bc/f.iso",
        "https://xn--bcher-kva.xn--p1ai/os.iso",
        "https://releases.ubuntu.com/24.04/ubuntu-24.04.3-desktop-amd64.iso?raw=1&x=%2F",
        "https://files.mirror.org/path%20with%20escapes/file~name_(1).iso",
        "https://cdn.mirror.org/a/b/c/d/e/f/g/h.iso",
        "https://mirror.org/f",
        NULL };
    for (int i = 0; good[i]; i++) CHECK(ok_url(good[i]), "should pass: %s", good[i]);
    static const char *const bad[] = {
        "http://cdimage.debian.org/x.iso",                 /* not https */
        "HTTPS://cdimage.debian.org/x.iso",
        "ftp://cdimage.debian.org/x.iso",
        "https://cdimage.debian.org",                      /* no path */
        "https://cdimage.debian.org/",                     /* no file */
        "https://cdimage.debian.org/dir/",
        "https://user:pass@cdimage.debian.org/x.iso",      /* user name */
        "https://user@cdimage.debian.org/x.iso",
        "https://cdimage.debian.org:8443/x.iso",           /* another port */
        "https://cdimage.debian.org:80/x.iso",
        "https://cdimage.debian.org:/x.iso",
        "https://cdimage.debian.org:0443/x.iso",
        "https://127.0.0.1/x.iso",                         /* IP literals */
        "https://10.0.0.8/x.iso",
        "https://192.168.1.1/x.iso",
        "https://[::1]/x.iso",
        "https://[2001:4860::8888]/x.iso",
        "https://0x7f000001/x.iso",
        "https://2130706433/x.iso",
        "https://1.2.3.4.5/x.iso",
        "https://localhost/x.iso",                         /* local names */
        "https://router.localhost/x.iso",
        "https://nas.local/x.iso",
        "https://nas.lan/x.iso",
        "https://svc.internal/x.iso",
        "https://box.home.arpa/x.iso",
        "https://1.0.168.192.in-addr.arpa/x.iso",
        "https://x.test/x.iso",
        "https://x.invalid/x.iso",
        "https://x.onion/x.iso",
        "https://intranet/x.iso",                          /* one label */
        "https://-bad.org/x.iso",
        "https://bad-.org/x.iso",
        "https://a..org/x.iso",
        "https://a.org./x.iso",                            /* trailing dot */
        "https://CDIMAGE.debian.org/x.iso",                /* the app writes it in lower case */
        "https://a.b/x.iso",                               /* one-letter TLD */
        "https://a.o1/x.iso",                              /* TLD with a digit */
        "https://cdimage.debian.org/x.iso#frag",           /* fragment */
        "https://cdimage.debian.org/x .iso",               /* space */
        "https://cdimage.debian.org/x\t.iso",
        "https://cdimage.debian.org/x\\y.iso",
        "https://cdimage.debian.org/x\".iso",
        "https://cdimage.debian.org/x<y>.iso",
        "https://cdimage.debian.org/%00.iso",              /* NUL escape */
        "https://cdimage.debian.org/%zz.iso",
        "https://cdimage.debian.org/x%2",
        "https://cdimage.debian.org/../etc/passwd",        /* dot segments */
        "https://cdimage.debian.org/a/./b.iso",
        "https://cdimage.debian.org/a/%2e%2e/b.iso",
        "https://cdimage.debian.org/a/.%2E/b.iso",
        "https://cdimage.debian.org/a/..",
        "https://cdimage.debian.org/x.iso?a=b#c",
        "https://cdimage.debian.org/x.iso?a=<b>",
        "https://cdimage.debian.org/x\xc3\xa9.iso",        /* non-ASCII */
        "https://evil.org\\@good.org/x.iso",
        "https://good.org?@evil.org/x.iso",
        "",
        NULL };
    for (int i = 0; bad[i]; i++) CHECK(!ok_url(bad[i]), "should be refused: %s", bad[i]);
    { char hh[256], ff[128]; CHECK(vmurl_check(NULL, hh, sizeof hh, ff, sizeof ff) != 0, "NULL refused"); }
    /* the longest accepted, then one past it */
    char u[VMURL_MAX + 8];
    snprintf(u, sizeof u, "https://mirror.org/");
    size_t l = strlen(u);
    while (l < VMURL_MAX - 4) u[l++] = 'a';
    memcpy(u + l, ".iso", 5);
    CHECK(strlen(u) == VMURL_MAX && ok_url(u), "a %d-byte address passes", VMURL_MAX);
    u[l] = 'a'; memcpy(u + l + 1, ".iso", 5);
    CHECK(!ok_url(u), "a longer one is refused");
    /* the host and the file it names */
    char h[256], f[128];
    CHECK(!vmurl_check("https://cdimage.debian.org:443/a/debian-13.iso?x=1", h, sizeof h, f, sizeof f) && !strcmp(h, "cdimage.debian.org") && !strcmp(f, "debian-13.iso"),
          "host and file: %s %s", h, f);
    CHECK(!vmurl_check("https://m.org/a/weird%20name.iso", h, sizeof h, f, sizeof f) && !strcmp(f, "image.iso"), "a name that isn't plain: image.iso (%s)", f);
}

static void test_names(void) {
    CHECK(pubhost_name_ok("deb.debian.org"), "deb.debian.org");
    CHECK(pubhost_name_ok("Deb.Debian.ORG"), "any case");
    CHECK(!pubhost_name_ok("deb.debian.local"), ".local");
    CHECK(!pubhost_name_ok("LOCALHOST"), "localhost");
    CHECK(!pubhost_name_ok("a.LocalDomain"), ".localdomain");
    CHECK(!pubhost_name_ok("8.8.8.8"), "an IPv4 literal");
    CHECK(!pubhost_name_ok("::1"), "an IPv6 literal");
    CHECK(!pubhost_name_ok(""), "empty");
    char longl[80];
    memset(longl, 'a', 64); memcpy(longl + 64, ".org", 5);
    CHECK(!pubhost_name_ok(longl), "a 64-byte label");
    char longn[300];
    size_t o = 0;
    while (o < 260) { memcpy(longn + o, "abcdefghi.", 10); o += 10; }
    memcpy(longn + o, "org", 4);
    CHECK(!pubhost_name_ok(longn), "a name past 253 bytes");
}

static int v4(const char *s) { struct in_addr a; inet_pton(AF_INET, s, &a); return pubhost_ipv4_ok(ntohl(a.s_addr)); }
static int v6(const char *s) { struct in6_addr a; inet_pton(AF_INET6, s, &a); return pubhost_ipv6_ok(a.s6_addr); }

static void test_addrs(void) {
    static const char *const pub4[] = { "8.8.8.8", "1.1.1.1", "151.101.2.132", "100.63.255.255", "100.128.0.1", "172.15.255.255", "172.32.0.1", "192.169.0.1", "223.255.255.254", NULL };
    static const char *const priv4[] = { "0.0.0.0", "0.1.2.3", "10.1.2.3", "100.64.0.1", "100.127.255.255", "127.0.0.1", "127.255.255.255", "169.254.169.254",
        "172.16.0.1", "172.31.255.255", "192.0.0.1", "192.0.2.1", "192.168.0.1", "198.18.0.1", "198.19.255.255", "198.51.100.7", "203.0.113.9",
        "224.0.0.1", "239.255.255.250", "240.0.0.1", "255.255.255.255", NULL };
    for (int i = 0; pub4[i]; i++) CHECK(v4(pub4[i]), "public: %s", pub4[i]);
    for (int i = 0; priv4[i]; i++) CHECK(!v4(priv4[i]), "not public: %s", priv4[i]);
    static const char *const pub6[] = { "2001:4860:4860::8888", "2606:4700::1111", "2a00:1450:4001::1", "::ffff:8.8.8.8", "64:ff9b::808:808", "2002:808:808::1", NULL };
    static const char *const priv6[] = { "::", "::1", "::ffff:127.0.0.1", "::ffff:10.0.0.1", "::ffff:192.168.1.1", "64:ff9b::a00:1", "2002:a00:1::1", "2002:7f00:1::",
        "fc00::1", "fd12:3456::1", "fe80::1", "febf::1", "fec0::1", "ff02::1", "2001:db8::1", "100::1", "::7f00:1",
        "64:ff9b:1::a00:1", "64:ff9b:1::808:808", "::ffff:0:a00:1", "::ffff:0:808:808", "2001::1", "2001:0:4136:e378::1", "2001:10::1",
        "2001:1ff::1", "3fff::1", "5f00::1", "4000::1", "e000::1", NULL };
    for (int i = 0; pub6[i]; i++) CHECK(v6(pub6[i]), "public: %s", pub6[i]);
    for (int i = 0; priv6[i]; i++) CHECK(!v6(priv6[i]), "not public: %s", priv6[i]);
    struct sockaddr_in s4 = { .sin_family = AF_INET };
    inet_pton(AF_INET, "127.0.0.1", &s4.sin_addr);
    CHECK(!pubhost_addr_ok((struct sockaddr *)&s4, sizeof s4), "sockaddr loopback");
    inet_pton(AF_INET, "8.8.4.4", &s4.sin_addr);
    CHECK(pubhost_addr_ok((struct sockaddr *)&s4, sizeof s4), "sockaddr public");
    CHECK(!pubhost_addr_ok((struct sockaddr *)&s4, 4), "short sockaddr");
    struct sockaddr un = { .sa_family = AF_UNIX };
    CHECK(!pubhost_addr_ok(&un, sizeof un), "AF_UNIX");
}

static void test_sizes(void) {
    uint64_t v = 7;
    CHECK(!vmurl_size("-", &v) && v == 0, "- is unknown");
    CHECK(!vmurl_size("1", &v) && v == 1, "1");
    CHECK(!vmurl_size("2900000000", &v) && v == 2900000000ull, "2.9 GB");
    char s[32];
    snprintf(s, sizeof s, "%llu", (unsigned long long)VMURL_IMAGE_MAX);
    CHECK(!vmurl_size(s, &v) && v == VMURL_IMAGE_MAX, "16 GiB");
    snprintf(s, sizeof s, "%llu", (unsigned long long)VMURL_IMAGE_MAX + 1);
    CHECK(vmurl_size(s, &v), "past 16 GiB");
    static const char *const bad[] = { "", "0", "01", "-1", "+5", "1e9", "12a", " 1", "1 ", "99999999999999", "0x10", NULL };
    for (int i = 0; bad[i]; i++) CHECK(vmurl_size(bad[i], &v), "size refused: '%s'", bad[i]);
    CHECK(vmurl_size(NULL, &v), "NULL size");

    const uint64_t G = 1ull << 30;
    uint64_t need = 0;
    /* known size: the size, while the store keeps the reserve */
    CHECK(vmurl_room(3 * G, 0, 10 * G, &need) == 3 * G && need == 3 * G + VMURL_RESERVE, "3 GB in 10 GB free");
    CHECK(vmurl_room(3 * G, 0, 5 * G, &need) == 3 * G, "exactly fits with the reserve");
    CHECK(vmurl_room(3 * G, 0, 5 * G - 1, &need) == 0 && need == 5 * G, "one byte short: need %llu", (unsigned long long)need);
    CHECK(vmurl_room(3 * G, 2 * G, 3 * G, &need) == 3 * G && need == G + VMURL_RESERVE, "a kept part counts");
    CHECK(vmurl_room(3 * G, 4 * G, 5 * G, &need) == 3 * G && need == 3 * G + VMURL_RESERVE, "a part larger than the file starts again");
    CHECK(vmurl_room(VMURL_IMAGE_MAX + 1, 0, 100 * G, &need) == 0, "past the cap");
    /* unknown size: what fits, at most the cap */
    CHECK(vmurl_room(0, 0, 100 * G, &need) == VMURL_IMAGE_MAX, "unknown, plenty of room: the cap");
    CHECK(vmurl_room(0, 0, 6 * G, &need) == 4 * G, "unknown, 6 GB free: 4 GB");
    CHECK(vmurl_room(0, G, 6 * G, &need) == 5 * G, "unknown with a part");
    CHECK(vmurl_room(0, 0, VMURL_RESERVE, &need) == 0, "unknown, only the reserve free");
    CHECK(vmurl_room(0, 0, 0, NULL) == 0, "nothing free, no need pointer");
}

static void test_redact(void) {
    char o[1200];
    vmurl_redact("https://m.org/a/b.iso?token=SECRET&x=1", o, sizeof o);
    CHECK(!strcmp(o, "https://m.org/a/b.iso?…") && !strstr(o, "SECRET"), "query dropped: %s", o);
    vmurl_redact("https://user:pw@m.org/a.iso", o, sizeof o);
    CHECK(!strcmp(o, "https://m.org/a.iso") && !strstr(o, "pw"), "credentials dropped: %s", o);
    vmurl_redact("https://m.org/a.iso#frag-SECRET", o, sizeof o);
    CHECK(!strstr(o, "SECRET"), "fragment dropped: %s", o);
    vmurl_redact("https://m.org/a.iso", o, sizeof o);
    CHECK(!strcmp(o, "https://m.org/a.iso"), "a plain address unchanged: %s", o);
    vmurl_redact("https://m.org/a\n.iso", o, sizeof o);
    CHECK(!strchr(o, '\n'), "no control character: %s", o);
    char small[12];
    vmurl_redact("https://m.org/a.iso?k=SECRET", small, sizeof small);
    CHECK(strlen(small) < sizeof small && !strstr(small, "SECRET"), "cut, never the query");
    vmurl_redact(NULL, o, sizeof o);
    CHECK(o[0] == 0, "NULL");
    vmurl_redact("x", o, 0);
}

/* every accepted address keeps the rules, whatever mutation made it */
static void fuzz(void) {
    static const char *const seeds[] = { "https://cdimage.debian.org/debian-cd/13.2.0/amd64/iso-cd/debian-13.2.0-amd64-netinst.iso",
        "https://a.bc:443/x/y.iso?q=1", "https://xn--bcher-kva.xn--p1ai/os.iso" };
    static const char alphabet[] = "abcz09AZ.-_~:/@?#%[]\\\"' <>\x01\x7f\xc3\xa9!$&()*+,;=2e";
    unsigned seed = 12345;
    int accepted = 0;
    for (int round = 0; round < 200000; round++) {
        char u[VMURL_MAX + 64];
        snprintf(u, sizeof u, "%s", seeds[round % 3]);
        int muts = 1 + (int)(seed % 4);
        for (int m = 0; m < muts; m++) {
            seed = seed * 1103515245u + 12345u;
            size_t l = strlen(u);
            size_t pos = (seed >> 8) % (l + 1);
            char ch = alphabet[(seed >> 16) % (sizeof alphabet - 1)];
            switch ((seed >> 24) % 3) {
            case 0: if (pos < l) u[pos] = ch; break;                                    /* replace */
            case 1: if (l + 1 < sizeof u) { memmove(u + pos + 1, u + pos, l - pos + 1); u[pos] = ch; } break;   /* insert */
            default: if (pos < l) memmove(u + pos, u + pos + 1, l - pos); break;        /* delete */
            }
        }
        char h[256], f[128];
        if (vmurl_check(u, h, sizeof h, f, sizeof f)) continue;
        accepted++;
        size_t l = strlen(u);
        CHECK(l <= VMURL_MAX && !strncmp(u, "https://", 8), "fuzz: shape %s", u);
        const char *a = u + 8, *sl = strchr(a, '/');
        CHECK(sl && !memchr(a, '@', (size_t)(sl - a)), "fuzz: user name %s", u);
        CHECK(pubhost_name_ok(h) && !strchr(h, ':'), "fuzz: host %s", h);
        CHECK(!strpbrk(u, " #\\\"<>"), "fuzz: forbidden character %s", u);
        for (size_t i = 0; i < l; i++) CHECK((unsigned char)u[i] > 0x20 && (unsigned char)u[i] < 0x7f, "fuzz: byte %s", u);
        CHECK(!strstr(u, "/../") && !strstr(u, "/./") && !strstr(u, "%00"), "fuzz: dot segment %s", u);
        const char *port = memchr(a, ':', (size_t)(sl - a));
        CHECK(!port || !strncmp(port, ":443/", 5), "fuzz: port %s", u);
        char r[1200];
        vmurl_redact(u, r, sizeof r);
        CHECK(!strchr(r, '?') || strstr(r, "?…"), "fuzz: redaction %s", r);
    }
    CHECK(accepted > 1000, "fuzz accepted %d mutated addresses", accepted);
    printf("vmurl fuzz: %d of 200000 mutated addresses accepted, all within the rules\n", accepted);
}

int main(void) {
    test_urls();
    test_names();
    test_addrs();
    test_sizes();
    test_redact();
    fuzz();
    if (fails) { fprintf(stderr, "vmurl-test: %d failed\n", fails); return 1; }
    printf("vmurl-test: all passed\n");
    return 0;
}
