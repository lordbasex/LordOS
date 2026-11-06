/* davutil.c — the pure parts of the CalDAV/CardDAV sync (see davutil.h). */
#include "davutil.h"
#include "xml.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- hrefs ---- */

typedef struct {
    int https;
    char host[256];
    int port;
    const char *path;       /* points into the source; "" means "/" */
    size_t plen;
} Url;

static int bad_char(unsigned char c) { return c < 0x20 || c == 0x7f; }

static int url_parse(const char *s, Url *u) {
    memset(u, 0, sizeof *u);
    if (!s) return -1;
    if (!strncasecmp(s, "https://", 8)) { u->https = 1; s += 8; }
    else if (!strncasecmp(s, "http://", 7)) s += 7;
    else return -1;
    const char *e = s + strcspn(s, "/?#");
    if (memchr(s, '@', (size_t)(e - s))) return -1;          /* no user info in a DAV URL */
    const char *colon = NULL;
    if (*s == '[') {
        const char *rb = memchr(s, ']', (size_t)(e - s));
        if (!rb) return -1;
        if (rb + 1 < e) { if (rb[1] != ':') return -1; colon = rb + 1; }
    } else colon = memchr(s, ':', (size_t)(e - s));
    size_t hl = (size_t)((colon ? colon : e) - s);
    if (!hl || hl >= sizeof u->host) return -1;
    for (size_t i = 0; i < hl; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '-' || c == '.' || c == '[' || c == ']' || c == ':')) return -1;
        u->host[i] = (char)tolower(c);
    }
    u->host[hl] = 0;
    if (colon) {
        const char *p = colon + 1;
        if (p == e || e - p > 5) return -1;
        int port = 0;
        for (; p < e; p++) { if (!isdigit((unsigned char)*p)) return -1; port = port * 10 + (*p - '0'); }
        if (port <= 0 || port > 65535) return -1;
        u->port = port;
    } else u->port = u->https ? 443 : 80;
    u->path = e;
    u->plen = strlen(e);
    return 0;
}

/* appends the path, percent-encoding spaces; -1 on control characters or overflow */
static int put_path(char *out, size_t cap, size_t *o, const char *p, size_t n) {
    if (!n) { p = "/"; n = 1; }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (bad_char(c)) return -1;
        if (c == ' ') { if (*o + 3 >= cap) return -1; memcpy(out + *o, "%20", 3); *o += 3; continue; }
        if (*o + 1 >= cap) return -1;
        out[(*o)++] = (char)c;
    }
    out[*o] = 0;
    return 0;
}

static int put_origin(char *out, size_t cap, const Url *u) {
    int dflt = u->port == (u->https ? 443 : 80);
    int n = dflt ? snprintf(out, cap, "%s://%s", u->https ? "https" : "http", u->host)
                 : snprintf(out, cap, "%s://%s:%d", u->https ? "https" : "http", u->host, u->port);
    if (n < 0 || (size_t)n >= cap) { out[0] = 0; return -1; }
    return n;
}

int dav_href_resolve(const char *base, const char *href, char *out, size_t cap) {
    if (!out || !cap) return -1;
    out[0] = 0;
    if (!href) return -1;
    while (*href == ' ' || *href == '\t' || *href == '\r' || *href == '\n') href++;
    size_t hl = strlen(href);
    while (hl && (href[hl - 1] == ' ' || href[hl - 1] == '\t' || href[hl - 1] == '\r' || href[hl - 1] == '\n')) hl--;
    if (!hl) return -1;
    char h[DAV_URL_MAX * 2];
    if (hl >= sizeof h) return -1;
    memcpy(h, href, hl);
    h[hl] = 0;
    if (strstr(h, "/../") || !strncmp(h, "../", 3) || (hl >= 3 && !strcmp(h + hl - 3, "/.."))) return -1;
    Url u;
    if (!strncasecmp(h, "https://", 8) || !strncasecmp(h, "http://", 7)) {
        if (url_parse(h, &u)) return -1;
        int n = put_origin(out, cap, &u);
        if (n < 0) return -1;
        size_t o = (size_t)n;
        if (put_path(out, cap, &o, u.path, u.plen)) { out[0] = 0; return -1; }
        return 0;
    }
    if (strchr(h, ':') && strcspn(h, ":") < strcspn(h, "/")) return -1;   /* another scheme ("mailto:", "javascript:") */
    Url b;
    if (url_parse(base, &b)) return -1;
    int n = put_origin(out, cap, &b);
    if (n < 0) return -1;
    size_t o = (size_t)n;
    int r;
    if (h[0] == '/') r = put_path(out, cap, &o, h, hl);
    else {
        /* relative: base's folder (without query or fragment) + href */
        size_t bp = strcspn(b.path, "?#");
        while (bp && b.path[bp - 1] != '/') bp--;
        if (!bp) r = put_path(out, cap, &o, "/", 1);
        else r = put_path(out, cap, &o, b.path, bp);
        if (!r) r = put_path(out, cap, &o, h, hl);
    }
    if (r) { out[0] = 0; return -1; }
    return 0;
}

int dav_same_origin(const char *a, const char *b) {
    Url x, y;
    if (url_parse(a, &x) || url_parse(b, &y)) return 0;
    return x.https == y.https && x.port == y.port && !strcmp(x.host, y.host);
}

/* the host's last two labels ("example.test" of "p07-caldav.example.test") */
static const char *site_of(const char *host) {
    const char *last = strrchr(host, '.');
    if (!last || last == host) return host;
    const char *p = last;
    while (p > host && p[-1] != '.') p--;
    return p;
}

static int is_ipv4(const char *h) { return strspn(h, "0123456789.") == strlen(h); }

int dav_same_site(const char *a, const char *b) {
    Url x, y;
    if (url_parse(a, &x) || url_parse(b, &y) || !x.https || !y.https) return 0;
    if (x.host[0] == '[' || y.host[0] == '[' || is_ipv4(x.host) || is_ipv4(y.host)) return !strcmp(x.host, y.host);
    return !strcmp(site_of(x.host), site_of(y.host));
}

const char *dav_url_path(const char *href) {
    if (!href) return "";
    const char *p;
    if (!strncasecmp(href, "https://", 8)) p = href + 8;
    else if (!strncasecmp(href, "http://", 7)) p = href + 7;
    else return href;
    p += strcspn(p, "/");
    return *p ? p : "/";
}

/* ---- discovery ---- */

static int status_ok(const MxNode *propstat) {
    char st[64];
    mx_child_text(propstat, MX_NS_DAV, "status", st, sizeof st);
    return !st[0] || strstr(st, " 200") != NULL;
}

static void href_of(const MxNode *prop, const char *ns, const char *name, const char *base, char *out, size_t cap) {
    if (out[0]) return;
    MxNode *p = mx_child(prop, ns, name);
    if (!p) return;
    MxNode *h = mx_child(p, MX_NS_DAV, "href");
    if (!h) return;
    char raw[DAV_URL_MAX];
    mx_child_text(p, MX_NS_DAV, "href", raw, sizeof raw);
    if (!raw[0] || dav_href_resolve(base, raw, out, cap)) out[0] = 0;
}

int dav_parse_homes(const char *body, size_t len, const char *base, DavHomes *out) {
    memset(out, 0, sizeof *out);
    if (!body) return -1;
    const char *err;
    MxNode *root = mx_parse(body, len, &err);
    if (!root) return -1;
    for (MxNode *r = mx_child(root, MX_NS_DAV, "response"); r; r = mx_next(r, MX_NS_DAV, "response")) {
        for (MxNode *ps = mx_child(r, MX_NS_DAV, "propstat"); ps; ps = mx_next(ps, MX_NS_DAV, "propstat")) {
            if (!status_ok(ps)) continue;
            MxNode *p = mx_child(ps, MX_NS_DAV, "prop");
            if (!p) continue;
            href_of(p, MX_NS_DAV, "current-user-principal", base, out->principal, sizeof out->principal);
            href_of(p, MX_NS_CAL, "calendar-home-set", base, out->calhome, sizeof out->calhome);
            href_of(p, MX_NS_CARD, "addressbook-home-set", base, out->cardhome, sizeof out->cardhome);
            MxNode *rt = mx_child(p, MX_NS_DAV, "resourcetype");
            if (rt && mx_child(rt, MX_NS_CAL, "calendar")) out->is_cal = 1;
            if (rt && mx_child(rt, MX_NS_CARD, "addressbook")) out->is_book = 1;
        }
    }
    mx_free(root);
    return 0;
}

/* ---- sync plan ---- */

static int is_self(const char *coll, const char *href) {
    size_t n = strlen(href);
    return !strcmp(href, coll) || (n && href[n - 1] == '/');
}

void dav_plan(const char *coll_href, const DavLocal *loc, int nl, const DavRemote *rem, int nr,
              unsigned char *fetch, unsigned char *drop) {
    for (int i = 0; i < nr; i++) {
        fetch[i] = 0;
        if (!rem[i].href || !rem[i].href[0] || is_self(coll_href, rem[i].href)) continue;
        const DavLocal *l = NULL;
        for (int k = 0; k < nl && !l; k++) if (loc[k].href && !strcmp(loc[k].href, rem[i].href)) l = &loc[k];
        if (l && l->dirty) continue;                       /* our change goes first; the push decides */
        if (!l || !l->etag || !rem[i].etag || strcmp(l->etag, rem[i].etag) || !rem[i].etag[0]) fetch[i] = 1;
    }
    for (int k = 0; k < nl; k++) {
        drop[k] = 0;
        if (loc[k].dirty || !loc[k].href || !loc[k].href[0]) continue;
        int found = 0;
        for (int i = 0; i < nr && !found; i++)
            if (rem[i].href && !is_self(coll_href, rem[i].href) && !strcmp(rem[i].href, loc[k].href)) found = 1;
        if (!found) drop[k] = 1;
    }
}

/* ---- uploads ---- */

int dav_push_outcome(int is_delete, int status) {
    if (status >= 200 && status < 300) return DAV_PUSH_DONE;
    if (is_delete && (status == 404 || status == 410)) return DAV_PUSH_DONE;
    if (status == 412) return DAV_PUSH_CONFLICT;
    if (status == 401 || status == 407 || status == 408 || status == 423 || status == 429) return DAV_PUSH_RETRY;
    if (status >= 400 && status < 500) return DAV_PUSH_REJECTED;
    return DAV_PUSH_RETRY;
}

int dav_push_keeps_copy(int is_delete, int outcome) {
    return !is_delete && (outcome == DAV_PUSH_CONFLICT || outcome == DAV_PUSH_REJECTED);
}

/* ---- UIDs ---- */

static int uid_ok(const char *u) {
    size_t n = u ? strlen(u) : 0;
    if (!n || n > 200) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)u[i];
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.' || c == '@')) return 0;
    }
    return 1;
}

static int line_is(const char *l, size_t n, const char *what) {
    size_t w = strlen(what);
    while (n && (l[n - 1] == '\r' || l[n - 1] == ' ' || l[n - 1] == '\t')) n--;
    return n == w && !strncasecmp(l, what, w);
}

static int is_component_begin(const char *l, size_t n) {
    return line_is(l, n, "BEGIN:VEVENT") || line_is(l, n, "BEGIN:VTODO") || line_is(l, n, "BEGIN:VJOURNAL") || line_is(l, n, "BEGIN:VCARD");
}

char *dav_set_uid(const char *text, size_t len, const char *uid, size_t *olen) {
    if (olen) *olen = 0;
    if (!text || !uid_ok(uid)) return NULL;
    size_t ul = strlen(uid);
    /* worst case: every line becomes a UID line, plus one added */
    size_t lines = 1;
    for (size_t i = 0; i < len; i++) if (text[i] == '\n') lines++;
    if (lines > ((size_t)-1 - len) / (ul + 8)) return NULL;
    size_t cap = len + (lines + 1) * (ul + 8) + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t o = 0, replaced = 0, comp = 0;
    size_t insert_at = (size_t)-1;
    int skipping = 0;
    for (size_t i = 0; i < len;) {
        size_t e = i;
        while (e < len && text[e] != '\n') e++;
        size_t ll = e - i;                         /* without '\n' */
        size_t next = e < len ? e + 1 : e;
        const char *l = text + i;
        if (skipping && ll && (l[0] == ' ' || l[0] == '\t')) { i = next; continue; }    /* folded rest of a UID */
        skipping = 0;
        if (ll >= 4 && !strncasecmp(l, "UID", 3) && (l[3] == ':' || l[3] == ';')) {
            memcpy(out + o, "UID:", 4); o += 4;
            memcpy(out + o, uid, ul); o += ul;
            memcpy(out + o, "\r\n", 2); o += 2;
            replaced++;
            skipping = 1;
            i = next;
            continue;
        }
        memcpy(out + o, l, next - i);
        o += next - i;
        if (e == len && ll) { memcpy(out + o, "\r\n", 2); o += 2; }   /* a last line without a newline */
        if (!comp && is_component_begin(l, ll)) { comp = 1; insert_at = o; }
        i = next;
    }
    if (!comp) { free(out); return NULL; }
    if (!replaced) {
        memmove(out + insert_at + 4 + ul + 2, out + insert_at, o - insert_at);
        memcpy(out + insert_at, "UID:", 4);
        memcpy(out + insert_at + 4, uid, ul);
        memcpy(out + insert_at + 4 + ul, "\r\n", 2);
        o += 4 + ul + 2;
    }
    out[o] = 0;
    if (olen) *olen = o;
    return out;
}

static int has_line(const char *text, size_t len, const char *what) {
    for (size_t i = 0; i < len;) {
        size_t e = i;
        while (e < len && text[e] != '\n') e++;
        if (line_is(text + i, e - i, what)) return 1;
        i = e < len ? e + 1 : e;
    }
    return 0;
}

int dav_is_todo(const char *text, size_t len) {
    return text && has_line(text, len, "BEGIN:VTODO") && !has_line(text, len, "BEGIN:VEVENT");
}
