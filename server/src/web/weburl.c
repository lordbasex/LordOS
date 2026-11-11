/* weburl.c — see weburl.h. */
#include "weburl.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int host_char(int c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.'; }

int weburl_parse(const char *s, WebUrl *u) {
    if (!s || !u) return -1;
    size_t n = strlen(s);
    if (!n || n > WEBURL_MAX) return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x21 || c > 0x7e || c == '\\') return -1;
    }
    memset(u, 0, sizeof *u);
    const char *p;
    if (!strncasecmp(s, "https://", 8)) { u->https = 1; p = s + 8; }
    else if (!strncasecmp(s, "http://", 7)) { u->https = 0; p = s + 7; }
    else return -1;
    size_t alen = strcspn(p, "/?#");
    if (!alen || memchr(p, '@', alen)) return -1;
    const char *port = NULL;
    size_t hlen;
    if (p[0] == '[') {
        const char *close = memchr(p, ']', alen);
        if (!close) return -1;
        hlen = (size_t)(close - p) + 1;
        if (hlen < 4) return -1;
        for (size_t i = 1; i + 1 < hlen; i++) {
            int c = lower((unsigned char)p[i]);
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == ':' || c == '.')) return -1;
        }
        if (hlen < alen) { if (p[hlen] != ':') return -1; port = p + hlen + 1; }
    } else {
        const char *colon = memchr(p, ':', alen);
        hlen = colon ? (size_t)(colon - p) : alen;
        if (colon) port = colon + 1;
        if (!hlen) return -1;
        if (p[0] == '.' || p[0] == '-') return -1;
        for (size_t i = 0; i < hlen; i++) {
            if (!host_char(lower((unsigned char)p[i]))) return -1;
            if (p[i] == '.' && i + 1 < hlen && p[i + 1] == '.') return -1;
        }
    }
    if (hlen > WEBURL_HOST_MAX) return -1;
    for (size_t i = 0; i < hlen; i++) u->host[i] = (char)lower((unsigned char)p[i]);
    u->host[hlen] = 0;
    if (port) {
        const char *end = p + alen;
        if (port == end || end - port > 5) return -1;
        long v = 0;
        for (const char *q = port; q < end; q++) {
            if (*q < '0' || *q > '9') return -1;
            v = v * 10 + (*q - '0');
        }
        if (v < 1 || v > 65535) return -1;
        u->port = (int)v == (u->https ? 443 : 80) ? 0 : (int)v;
    }
    u->path = p + alen;
    const char *hash = strchr(u->path, '#');
    u->path_len = hash ? (size_t)(hash - u->path) : strlen(u->path);
    return 0;
}

int weburl_is_local(const WebUrl *u) {
    return !strcmp(u->host, "localhost") || !strcmp(u->host, "127.0.0.1") || !strcmp(u->host, "[::1]");
}

int weburl_app_ok(const char *s) {
    WebUrl u;
    return !weburl_parse(s, &u) && (u.https || weburl_is_local(&u));
}

static int origin_of(const WebUrl *u, char *out, size_t n) {
    int k = u->port ? snprintf(out, n, "%s://%s:%d", u->https ? "https" : "http", u->host, u->port)
                    : snprintf(out, n, "%s://%s", u->https ? "https" : "http", u->host);
    return k < 0 || (size_t)k >= n ? -1 : 0;
}

int weburl_origin(const char *s, char *out, size_t n) {
    WebUrl u;
    if (!out || !n || weburl_parse(s, &u)) return -1;
    return origin_of(&u, out, n);
}

static int same(const WebUrl *a, const WebUrl *b) { return a->https == b->https && a->port == b->port && !strcmp(a->host, b->host); }

int weburl_same_origin(const char *a, const char *b) {
    WebUrl x, y;
    return !weburl_parse(a, &x) && !weburl_parse(b, &y) && same(&x, &y);
}

/* the i-th byte of the path as compared: a path that doesn't start with '/' ("" or "?q") reads as "/" + it */
static int path_at(const WebUrl *u, size_t i, size_t *len) {
    int slash = !u->path_len || u->path[0] != '/';
    *len = u->path_len + (size_t)slash;
    if (slash) return i == 0 ? '/' : (unsigned char)u->path[i - 1];
    return (unsigned char)u->path[i];
}

int weburl_in_scope(const char *url, const char *scope) {
    WebUrl x, s;
    if (weburl_parse(url, &x) || weburl_parse(scope, &s) || !same(&x, &s)) return 0;
    size_t xl, sl;
    path_at(&x, 0, &xl);
    path_at(&s, 0, &sl);
    if (sl > xl) return 0;
    for (size_t i = 0; i < sl; i++) if (path_at(&x, i, &xl) != path_at(&s, i, &sl)) return 0;
    return 1;
}

/* a host as browsers read an IPv4 address (WHATWG URL): 1 to 4 parts of decimal, 0x hex or 0 octal, the last one
 * filling the bytes left. 0 and *ip, -1 when it isn't one (a name). */
static int ipv4_of(const char *h, uint32_t *ip) {
    char buf[WEBURL_HOST_MAX + 1];
    size_t n = strlen(h);
    if (!n || n >= sizeof buf) return -1;
    memcpy(buf, h, n + 1);
    if (buf[n - 1] == '.') buf[--n] = 0;              /* "127.0.0.1." */
    uint64_t part[4];
    int np = 0;
    for (char *s = buf, *dot; s; s = dot ? dot + 1 : NULL) {
        dot = strchr(s, '.');
        if (dot) *dot = 0;
        if (np == 4 || !*s) return -1;
        int base = 10;
        const char *d = s;
        if (d[0] == '0' && (d[1] == 'x' || d[1] == 'X')) { base = 16; d += 2; }
        else if (d[0] == '0' && d[1]) { base = 8; d += 1; }
        if (strlen(d) > 12) return -1;
        uint64_t v = 0;
        for (const char *q = d; *q; q++) {
            int c = *q, x = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
            if (x >= base) return -1;
            v = v * (uint64_t)base + (uint64_t)x;
        }
        part[np++] = v;
    }
    uint64_t r = 0;
    for (int i = 0; i < np - 1; i++) { if (part[i] > 255) return -1; r |= part[i] << (8 * (3 - i)); }
    if (part[np - 1] >= (1ull << (8 * (5 - np)))) return -1;
    r |= part[np - 1];
    *ip = (uint32_t)r;
    return 0;
}

static int private4(uint32_t a) {
    return (a >> 24) == 127 || (a >> 24) == 0 || (a >> 24) == 10 || (a >> 22) == (100u << 2 | 1) || (a >> 20) == (172u << 4 | 1) ||
           (a >> 16) == (192u << 8 | 168) || (a >> 16) == (169u << 8 | 254);
}

static int ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), t = strlen(tail);
    return n >= t && !strcmp(s + n - t, tail);
}

static int private6(const unsigned char b[16]) {
    static const unsigned char zero[16];
    static const unsigned char mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    if (!memcmp(b, zero, 15) && (b[15] == 0 || b[15] == 1)) return 1;                  /* :: and ::1 */
    if ((b[0] & 0xfe) == 0xfc || (b[0] == 0xfe && (b[1] & 0xc0) == 0x80)) return 1;   /* fc00::/7, fe80::/10 */
    if (!memcmp(b, mapped, 12) || !memcmp(b, zero, 12))                               /* ::ffff:a.b.c.d, ::a.b.c.d */
        return private4((uint32_t)b[12] << 24 | (uint32_t)b[13] << 16 | (uint32_t)b[14] << 8 | b[15]);
    return 0;
}

int weburl_addrs_class(const WebAddr *a, int n) {
    if (!a || n <= 0) return WEBURL_PRIVATE;                                           /* nothing resolved: refused */
    for (int i = 0; i < n; i++) {
        if (a[i].family == 4) { if (private4((uint32_t)a[i].a[0] << 24 | (uint32_t)a[i].a[1] << 16 | (uint32_t)a[i].a[2] << 8 | a[i].a[3])) return WEBURL_PRIVATE; }
        else if (a[i].family == 6) { if (private6(a[i].a)) return WEBURL_PRIVATE; }
        else return WEBURL_PRIVATE;
    }
    return WEBURL_PUBLIC;
}

/* names that only mean something on a local network (or this computer) */
static const char *const LAN_SUFFIX[] = { ".localhost", ".local", ".lan", ".home", ".home.arpa", ".internal", ".intranet", ".localdomain", ".corp", NULL };

int weburl_host_is_literal(const WebUrl *u) {
    const char *h = u->host;
    if (h[0] == '[') return 1;
    size_t n = strlen(h);
    if (n && h[n - 1] == '.') n--;                       /* "a.b." */
    size_t i = n;
    while (i > 0 && h[i - 1] != '.') i--;
    return i < n && h[i] >= '0' && h[i] <= '9';          /* a last label that is a number: browsers read an IPv4 address */
}

int weburl_host_class(const WebUrl *u) {
    const char *h = u->host;
    if (h[0] == '[') {
        char a[64];
        size_t n = strlen(h);
        unsigned char b[16];
        if (n < 3 || n - 2 >= sizeof a) return WEBURL_PRIVATE;      /* nothing we can read: never trusted as public */
        memcpy(a, h + 1, n - 2);
        a[n - 2] = 0;
        if (inet_pton(AF_INET6, a, b) != 1) return WEBURL_PRIVATE;
        return private6(b) ? WEBURL_PRIVATE : WEBURL_PUBLIC;
    }
    char name[WEBURL_HOST_MAX + 1];
    snprintf(name, sizeof name, "%s", h);
    size_t n = strlen(name);
    if (n && name[n - 1] == '.') name[n - 1] = 0;
    if (!strcmp(name, "localhost")) return WEBURL_PRIVATE;
    for (int i = 0; LAN_SUFFIX[i]; i++) if (ends_with(name, LAN_SUFFIX[i]) || !strcmp(name, LAN_SUFFIX[i] + 1)) return WEBURL_PRIVATE;
    uint32_t ip;
    if (weburl_host_is_literal(u)) return ipv4_of(name, &ip) || private4(ip) ? WEBURL_PRIVATE : WEBURL_PUBLIC;
    if (!strchr(name, '.')) return WEBURL_PRIVATE;        /* a single label: a name of the local network ("router", "nas") */
    return WEBURL_PUBLIC;
}

int weburl_probe_hop_ok(const char *from, const char *to) {
    WebUrl f, t;
    if (weburl_parse(from, &f) || weburl_parse(to, &t)) return 0;
    if (f.https && !t.https) return 0;
    if (weburl_host_class(&f) == WEBURL_PUBLIC && weburl_host_class(&t) != WEBURL_PUBLIC) return 0;
    return 1;
}

int weburl_probe_final_ok(const char *typed, const char *final) {
    WebUrl q, f;
    if (weburl_parse(typed, &q) || weburl_parse(final, &f) || !(f.https || weburl_is_local(&f))) return 0;
    if (q.https && !f.https) return 0;
    if (weburl_host_class(&f) != WEBURL_PUBLIC && weburl_host_class(&q) == WEBURL_PUBLIC) return 0;
    return 1;
}

int weburl_probe_step(int *seen_public, int cls) {
    if (*seen_public && cls != WEBURL_PUBLIC) return 0;
    if (cls != WEBURL_PRIVATE) *seen_public = 1;
    return 1;
}

int weburl_perm_allowed(const char *view_uri, const char *asker, const char *scope, const char *app_origin) {
    if (!view_uri || !scope || !app_origin || !weburl_app_ok(view_uri)) return 0;
    if (!weburl_in_scope(view_uri, scope) || !weburl_same_origin(view_uri, app_origin)) return 0;
    if (!asker) return 1;
    char o[WEBURL_ORIGIN_MAX + 1], a[WEBURL_ORIGIN_MAX + 1];
    return !weburl_origin(asker, o, sizeof o) && !weburl_origin(app_origin, a, sizeof a) && !strcmp(o, a);
}

int weburl_dir(const char *url, char *out, size_t n) {
    WebUrl u;
    if (!out || !n || weburl_parse(url, &u) || origin_of(&u, out, n)) return -1;
    size_t plen = strcspn(u.path, "?#");
    size_t keep = 0;
    for (size_t i = 0; i < plen; i++) if (u.path[i] == '/') keep = i + 1;
    size_t o = strlen(out);
    if (!keep) { if (o + 2 > n) return -1; out[o] = '/'; out[o + 1] = 0; return 0; }
    if (o + keep + 1 > n) return -1;
    memcpy(out + o, u.path, keep);
    out[o + keep] = 0;
    return 0;
}
