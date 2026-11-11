/* weburl.c — see weburl.h. */
#include "weburl.h"

#include <stdio.h>
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
