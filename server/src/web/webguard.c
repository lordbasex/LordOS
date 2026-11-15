/* webguard.c — see webguard.h. */
#include "webguard.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

const unsigned WEBGUARD_BACKEND_PORTS[] = { 6665, 6666, 6667, 6668, 6669, 6697, 10080, 1719, 1720, 1723, 3659, 4045, 5060, 5061, 6566 };
const size_t WEBGUARD_BACKEND_NPORTS = sizeof WEBGUARD_BACKEND_PORTS / sizeof WEBGUARD_BACKEND_PORTS[0];

int webguard_backend_port_ok(unsigned port) {
    for (size_t i = 0; i < WEBGUARD_BACKEND_NPORTS; i++) if (WEBGUARD_BACKEND_PORTS[i] == port) return 1;
    return 0;
}

size_t webguard_b64url(const unsigned char *in, size_t n, char *out, size_t outn) {
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t need = (n * 4 + 2) / 3;
    if (!out || outn <= need) return 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16 | (i + 1 < n ? (unsigned)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = a[v >> 18 & 63];
        out[o++] = a[v >> 12 & 63];
        if (i + 1 < n) out[o++] = a[v >> 6 & 63];
        if (i + 2 < n) out[o++] = a[v & 63];
    }
    out[o] = 0;
    return o;
}

static int b64url_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static int token_ok(const char *t) {
    if (!t || strlen(t) != WEBGUARD_TOKEN_LEN) return 0;
    for (size_t i = 0; i < WEBGUARD_TOKEN_LEN; i++) if (!b64url_char((unsigned char)t[i])) return 0;
    return 1;
}

int webguard_set_keys(WebGuardKeys *k, unsigned port, const char *secret, const char *cookie) {
    if (!k || port < 1024 || port > 65535 || !token_ok(secret) || !token_ok(cookie)) return -1;
    k->port = port;
    memcpy(k->secret, secret, WEBGUARD_TOKEN_LEN + 1);
    memcpy(k->cookie, cookie, WEBGUARD_TOKEN_LEN + 1);
    return 0;
}

/* the same n bytes, in a time that doesn't depend on where they differ */
static int same(const char *a, const char *b, size_t n) {
    unsigned char d = 0;
    for (size_t i = 0; i < n; i++) d |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return d == 0;
}

/* a token of a value (len bytes) that is exactly the WEBGUARD_TOKEN_LEN bytes of want (which may be "": never) */
static int token_is(const char *v, size_t len, const char *want) {
    if (strlen(want) != WEBGUARD_TOKEN_LEN || len != WEBGUARD_TOKEN_LEN) return 0;
    return same(v, want, WEBGUARD_TOKEN_LEN);
}

long webguard_head_end(const char *buf, size_t len) {
    size_t lim = len < WEBGUARD_HEAD_MAX ? len : WEBGUARD_HEAD_MAX;
    for (size_t i = 3; i < lim; i++)
        if (buf[i] == '\n' && buf[i - 1] == '\r' && buf[i - 2] == '\n' && buf[i - 3] == '\r') return (long)(i + 1);
    return len >= WEBGUARD_HEAD_MAX ? -1 : 0;
}

/* the next CRLF from p whose LF is before end + 2, or NULL: heads aren't NUL-terminated */
static const char *crlf(const char *p, const char *end) {
    for (; p + 1 < end + 2; p++) if (p[0] == '\r' && p[1] == '\n') return p;
    return NULL;
}

static int tchar(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c));
}

static int is(const char *s, size_t n, const char *word) { return strlen(word) == n && !strncasecmp(s, word, n); }

/* value (n bytes) is exactly prefix followed by the proxy's port; the prefix's letters in any case */
static int addr_is(const char *v, size_t n, const char *prefix, unsigned port) {
    char want[64];
    int w = snprintf(want, sizeof want, "%s%u", prefix, port);
    return w > 0 && (size_t)w == n && !strncasecmp(v, want, n);
}

/* a comma-separated list (n bytes) has this token, in any case */
static int list_has(const char *v, size_t n, const char *tok) {
    size_t i = 0;
    while (i < n) {
        while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
        size_t s = i;
        while (i < n && v[i] != ',') i++;
        size_t e = i;
        while (e > s && (v[e - 1] == ' ' || v[e - 1] == '\t')) e--;
        if (e > s && is(v + s, e - s, tok)) return 1;
    }
    return 0;
}

/* a Cookie value (n bytes) has name=<cookie> */
static int cookie_has(const char *v, size_t n, const WebGuardKeys *k) {
    char name[64];
    snprintf(name, sizeof name, WEBGUARD_COOKIE "-%u", k->port);
    size_t nl = strlen(name), i = 0;
    int ok = 0;
    while (i < n) {
        while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ';')) i++;
        size_t s = i;
        while (i < n && v[i] != ';') i++;
        size_t e = i;
        while (e > s && (v[e - 1] == ' ' || v[e - 1] == '\t')) e--;
        if (e - s > nl && !memcmp(v + s, name, nl) && v[s + nl] == '=') ok |= token_is(v + s + nl + 1, e - s - nl - 1, k->cookie);
    }
    return ok;
}

int webguard_check(const char *head, size_t head_len, const WebGuardKeys *k) {
    if (!head || !k || head_len < 4 || head_len > WEBGUARD_HEAD_MAX || memcmp(head + head_len - 4, "\r\n\r\n", 4)) return WEBGUARD_DENY;
    /* every line ends in CRLF; no other control byte (but a tab) anywhere */
    for (size_t i = 0; i < head_len; i++) {
        unsigned char c = (unsigned char)head[i];
        if (c == '\r') { if (i + 1 >= head_len || head[i + 1] != '\n') return WEBGUARD_DENY; i++; continue; }
        if (c == '\n' || c == 0x7f || (c < 0x20 && c != '\t')) return WEBGUARD_DENY;
    }
    /* the request line: GET <origin-form target> HTTP/1.1 */
    const char *p = head, *end = head + head_len - 2;       /* end: the last line's CRLF */
    const char *eol = crlf(p, end);
    if (!eol || eol - p < 14 || memcmp(p, "GET /", 5) || memcmp(eol - 9, " HTTP/1.1", 9)) return WEBGUARD_DENY;
    const char *target = p + 4;
    size_t tlen = (size_t)(eol - 9 - target);
    for (size_t i = 0; i < tlen; i++) if ((unsigned char)target[i] <= 0x20 || (unsigned char)target[i] >= 0x7f) return WEBGUARD_DENY;

    int hosts = 0, host_ok = 0, origins = 0, origin_ok = 0, fetch_sites = 0, fetch_site_ok = 0, cookie_ok = 0, body = 0, upgrade = 0, upgrade_ws = 0, conn_up = 0;
    for (p = eol + 2; p < end; p = eol + 2) {
        eol = crlf(p, end);
        if (!eol) return WEBGUARD_DENY;
        if (*p == ' ' || *p == '\t') return WEBGUARD_DENY;  /* a folded line */
        const char *colon = p;
        while (colon < eol && tchar((unsigned char)*colon)) colon++;
        if (colon == p || colon >= eol || *colon != ':') return WEBGUARD_DENY;
        size_t nlen = (size_t)(colon - p);
        const char *v = colon + 1, *ve = eol;
        while (v < ve && (*v == ' ' || *v == '\t')) v++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        size_t vlen = (size_t)(ve - v);
        if (is(p, nlen, "host")) {
            hosts++;
            host_ok = addr_is(v, vlen, "127.0.0.1:", k->port) || addr_is(v, vlen, "localhost:", k->port);
        } else if (is(p, nlen, "origin")) {
            origins++;
            origin_ok = addr_is(v, vlen, "http://127.0.0.1:", k->port) || addr_is(v, vlen, "http://localhost:", k->port);
        } else if (is(p, nlen, "sec-fetch-site")) {
            fetch_sites++;
            fetch_site_ok = is(v, vlen, "same-origin") || is(v, vlen, "none");
        } else if (is(p, nlen, "cookie")) {
            cookie_ok |= cookie_has(v, vlen, k);
        } else if (is(p, nlen, "content-length")) {
            if (!(vlen == 1 && *v == '0')) body = 1;
        } else if (is(p, nlen, "transfer-encoding")) {
            body = 1;
        } else if (is(p, nlen, "upgrade")) {
            upgrade++;
            upgrade_ws = is(v, vlen, "websocket");
        } else if (is(p, nlen, "connection")) {
            conn_up |= list_has(v, vlen, "upgrade");
        }
    }
    if (hosts != 1 || !host_ok || origins > 1 || (origins && !origin_ok) || fetch_sites > 1 || (fetch_sites && !fetch_site_ok) || body)
        return WEBGUARD_DENY;
    /* /<secret> or /<secret>/ opens */
    if (k->secret[0] && (tlen == 1 + WEBGUARD_TOKEN_LEN || (tlen == 2 + WEBGUARD_TOKEN_LEN && target[tlen - 1] == '/'))
        && token_is(target + 1, WEBGUARD_TOKEN_LEN, k->secret))
        return WEBGUARD_OPEN;
    if (!cookie_ok) return WEBGUARD_DENY;
    if (upgrade) return upgrade == 1 && upgrade_ws && conn_up ? WEBGUARD_UPGRADE : WEBGUARD_DENY;
    return WEBGUARD_FORWARD;
}

size_t webguard_rewrite(const char *head, size_t head_len, char *out, size_t n) {
    if (!head || head_len < 4 || !out) return 0;
    const char *end = head + head_len - 2, *p = head, *eol;
    size_t o = 0;
    for (int first = 1; p < end; p = eol + 2, first = 0) {
        eol = crlf(p, end);
        if (!eol || eol > end) return 0;
        size_t l = (size_t)(eol - p) + 2;
        const char *colon = first ? NULL : memchr(p, ':', l);
        size_t nlen = colon ? (size_t)(colon - p) : 0;
        if (colon && (is(p, nlen, "connection") || is(p, nlen, "keep-alive") || is(p, nlen, "proxy-connection") || is(p, nlen, "upgrade"))) continue;
        if (o + l >= n) return 0;
        memcpy(out + o, p, l);
        o += l;
    }
    static const char tail[] = "Connection: close\r\n\r\n";
    if (o + sizeof tail > n) return 0;
    memcpy(out + o, tail, sizeof tail);
    return o + sizeof tail - 1;
}

static size_t put(size_t n, int w) { return w < 0 || (size_t)w >= n ? 0 : (size_t)w; }

size_t webguard_open_reply(const WebGuardKeys *k, char *out, size_t n) {
    if (!k || !out || !n) return 0;
    return put(n, snprintf(out, n,
        "HTTP/1.1 302 Found\r\nLocation: /\r\n"
        "Set-Cookie: " WEBGUARD_COOKIE "-%u=%s; HttpOnly; SameSite=Strict; Path=/\r\n"
        "Cache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        k->port, k->cookie));
}

size_t webguard_deny_reply(char *out, size_t n) {
    if (!out || !n) return 0;
    return put(n, snprintf(out, n, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"));
}

int webguard_open_url(const WebGuardKeys *k, char *out, size_t n) {
    if (!out || !n) return -1;
    out[0] = 0;
    if (!k || !token_ok(k->secret)) return -1;
    int w = snprintf(out, n, "http://127.0.0.1:%u/%s/", k->port, k->secret);
    if (w < 0 || (size_t)w >= n) { out[0] = 0; return -1; }
    return 0;
}

int webguard_switched(const char *buf, size_t len) {
    static const char want[] = "HTTP/1.1 101 ";
    size_t m = len < sizeof want - 1 ? len : sizeof want - 1;
    if (memcmp(buf, want, m)) return 0;
    return m < sizeof want - 1 ? -1 : 1;
}
