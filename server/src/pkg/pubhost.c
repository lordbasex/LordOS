/* pubhost.c — public DNS names and public addresses (pubhost.h). */
#include "pubhost.h"

#include <netinet/in.h>
#include <string.h>
#include <strings.h>

static int alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int alnum(char c) { return alpha(c) || (c >= '0' && c <= '9'); }

/* suffixes (the last label, or the last two) that never name a host on the internet */
static const char *const LOCAL_TLDS[] = { "local", "localhost", "localdomain", "internal", "intranet", "lan", "home",
    "corp", "private", "arpa", "test", "invalid", "example", "onion", "alt", NULL };

int pubhost_name_ok(const char *host) {
    if (!host) return 0;
    size_t n = strlen(host);
    if (n < 4 || n > 253) return 0;
    int labels = 0;
    const char *last = host;
    for (const char *p = host;;) {
        const char *e = p;
        while (*e && *e != '.') e++;
        size_t l = (size_t)(e - p);
        if (l < 1 || l > 63 || p[0] == '-' || p[l - 1] == '-') return 0;
        for (size_t i = 0; i < l; i++) if (!alnum(p[i]) && p[i] != '-') return 0;
        labels++;
        last = p;
        if (!*e) break;
        p = e + 1;
    }
    if (labels < 2 || labels > 127) return 0;
    size_t tl = strlen(last);
    if (tl >= 4 && !strncasecmp(last, "xn--", 4)) {
        /* an IDN top-level domain: its letters and digits after xn-- */
    } else {
        if (tl < 2) return 0;
        for (size_t i = 0; i < tl; i++) if (!alpha(last[i])) return 0;
    }
    for (int i = 0; LOCAL_TLDS[i]; i++) if (!strcasecmp(last, LOCAL_TLDS[i])) return 0;
    if (!strcasecmp(host, "localhost")) return 0;
    return 1;
}

int pubhost_ipv4_ok(uint32_t a) {
    static const struct { uint32_t net; int bits; } bad[] = {
        { 0x00000000u, 8 },  { 0x0a000000u, 8 },  { 0x64400000u, 10 }, { 0x7f000000u, 8 },  { 0xa9fe0000u, 16 },
        { 0xac100000u, 12 }, { 0xc0000000u, 24 }, { 0xc0000200u, 24 }, { 0xc0a80000u, 16 }, { 0xc6120000u, 15 },
        { 0xc6336400u, 24 }, { 0xcb007100u, 24 }, { 0xe0000000u, 3 },
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        uint32_t mask = bad[i].bits ? ~0u << (32 - bad[i].bits) : 0;
        if ((a & mask) == bad[i].net) return 0;
    }
    return 1;
}

static uint32_t v4_at(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int pubhost_ipv6_ok(const uint8_t a[16]) {
    static const uint8_t zero[16];
    static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    static const uint8_t nat64[12] = { 0, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (!memcmp(a, zero, 15) && (a[15] == 0 || a[15] == 1)) return 0;          /* :: and ::1 */
    if (!memcmp(a, mapped, 12) || !memcmp(a, nat64, 12)) return pubhost_ipv4_ok(v4_at(a + 12));
    if (!memcmp(a, zero, 12)) return 0;                                          /* IPv4-compatible (deprecated) */
    if (a[0] == 0x20 && a[1] == 0x02) return pubhost_ipv4_ok(v4_at(a + 2));      /* 6to4 */
    if (a[0] == 0x01 && a[1] == 0x00 && !memcmp(a + 2, zero, 6)) return 0;       /* 100::/64 */
    if (a[0] == 0x20 && a[1] == 0x01 && a[2] == 0x0d && a[3] == 0xb8) return 0; /* 2001:db8::/32 */
    if ((a[0] & 0xfe) == 0xfc) return 0;                                         /* fc00::/7 */
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) return 0;                         /* fe80::/10 */
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0xc0) return 0;                         /* fec0::/10 */
    if (a[0] == 0xff) return 0;                                                  /* multicast */
    /* Only global unicast (2000::/3) from here on: this also refuses the local-use NAT64 prefix 64:ff9b:1::/48 and
     * the SIIT ::ffff:0:a.b.c.d/96, whose IPv4 half the network may route anywhere, and any range assigned later. */
    if ((a[0] & 0xe0) != 0x20) return 0;
    if (a[0] == 0x20 && a[1] == 0x01 && a[2] < 0x02) return 0;                   /* 2001::/23 (Teredo, ORCHID, …) */
    if (a[0] == 0x3f && a[1] == 0xff && (a[2] & 0xf0) == 0) return 0;            /* 3fff::/20 (documentation) */
    return 1;
}

int pubhost_addr_ok(const struct sockaddr *sa, socklen_t len) {
    if (!sa) return 0;
    if (sa->sa_family == AF_INET && len >= (socklen_t)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)(const void *)sa;
        return pubhost_ipv4_ok(ntohl(s->sin_addr.s_addr));
    }
    if (sa->sa_family == AF_INET6 && len >= (socklen_t)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)(const void *)sa;
        return pubhost_ipv6_ok(s->sin6_addr.s6_addr);
    }
    return 0;
}
