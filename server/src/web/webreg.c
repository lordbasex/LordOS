/* webreg.c — see webreg.h. */
#include "webreg.h"
#include "weburl.h"
#include "../sec/json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static int bad(char *why, size_t wn, const char *fmt, ...) {
    if (why && wn) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(why, wn, fmt, ap);
        va_end(ap);
    }
    return -1;
}

/* ---- ids and names ---- */

int webreg_id_ok(const char *s) {
    if (!s || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9'))) return 0;
    size_t i = 1;
    for (; s[i]; i++) {
        char c = s[i];
        if (i >= WEBREG_ID_MAX || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return 0;
    }
    return 1;
}

/* The next code point of s[*i..n): 0 ok (and *i moved past it), -1 not valid UTF-8 (overlong, a surrogate,
 * past U+10FFFF or cut short). */
static int utf8_next(const unsigned char *s, size_t n, size_t *i, unsigned *cp) {
    unsigned c = s[*i];
    size_t k;
    unsigned v, min;
    if (c < 0x80) { *cp = c; (*i)++; return 0; }
    if ((c & 0xe0) == 0xc0) { k = 1; v = c & 0x1f; min = 0x80; }
    else if ((c & 0xf0) == 0xe0) { k = 2; v = c & 0x0f; min = 0x800; }
    else if ((c & 0xf8) == 0xf0) { k = 3; v = c & 0x07; min = 0x10000; }
    else return -1;
    if (*i + k >= n) return -1;
    for (size_t j = 1; j <= k; j++) {
        unsigned d = s[*i + j];
        if ((d & 0xc0) != 0x80) return -1;
        v = v << 6 | (d & 0x3f);
    }
    if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return -1;
    *cp = v;
    *i += k + 1;
    return 0;
}

/* control characters, and the invisible ones that could make a name read as another (bidi overrides,
 * zero-width marks, the BOM) */
static int hidden_char(unsigned cp) {
    return cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || (cp >= 0x200b && cp <= 0x200f) || (cp >= 0x202a && cp <= 0x202e) ||
           (cp >= 0x2060 && cp <= 0x2069) || cp == 0xfeff;
}

int webreg_name_ok(const char *s) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (!n || n > WEBREG_NAME_MAX || s[0] == ' ' || s[n - 1] == ' ') return 0;
    for (size_t i = 0; i < n;) {
        unsigned cp;
        if (utf8_next((const unsigned char *)s, n, &i, &cp) || hidden_char(cp)) return 0;
    }
    return 1;
}

/* ---- URLs ---- */

static int host_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.'; }

int webreg_url_origin(const char *url, char *origin, size_t n) {
    if (!url) return -1;
    size_t len = strlen(url);
    if (!len || len > WEBREG_URL_MAX) return -1;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c <= 0x20 || c >= 0x7f || c == '\\') return -1;
    }
    int https;
    const char *p;
    if (!strncmp(url, "https://", 8)) { https = 1; p = url + 8; }
    else if (!strncmp(url, "http://", 7)) { https = 0; p = url + 7; }
    else return -1;
    const char *end = p + strcspn(p, "/?#");
    if (end == p || memchr(p, '@', (size_t)(end - p))) return -1;          /* no user or password */
    const char *he, *colon = NULL;
    if (*p == '[') {                                                     /* an IPv6 address */
        const char *rb = memchr(p, ']', (size_t)(end - p));
        if (!rb || rb - p < 3) return -1;
        for (const char *q = p + 1; q < rb; q++) if (!((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f') || *q == ':' || *q == '.')) return -1;
        he = rb + 1;
        if (he < end) { if (*he != ':') return -1; colon = he; }
    } else {
        colon = memchr(p, ':', (size_t)(end - p));
        he = colon ? colon : end;
        size_t hl = (size_t)(he - p);
        if (!hl || hl > 253 || p[0] == '.' || p[0] == '-' || he[-1] == '.' || he[-1] == '-') return -1;
        for (const char *q = p; q < he; q++) {
            if (!host_char(*q)) return -1;
            if (*q == '.' && (q[1] == '.' || q[1] == '-' || q[-1] == '-')) return -1;
        }
    }
    long port = -1;
    if (colon) {
        const char *d = colon + 1;
        if (d == end || end - d > 5) return -1;
        port = 0;
        for (; d < end; d++) { if (*d < '0' || *d > '9') return -1; port = port * 10 + (*d - '0'); }
        if (port < 1 || port > 65535) return -1;
        if (port == (https ? 443 : 80)) port = -1;                          /* the default port is not part of the origin */
    }
    size_t hl = (size_t)(he - p);
    if (!weburl_app_ok(url)) return -1;                                  /* and lord-web's own reader takes it */
    if (!https && !((hl == 9 && !memcmp(p, "localhost", 9)) || (hl == 9 && !memcmp(p, "127.0.0.1", 9)) || (hl == 5 && !memcmp(p, "[::1]", 5))))
        return -1;
    if (origin) {
        int w = port > 0 ? snprintf(origin, n, "%s://%.*s:%ld", https ? "https" : "http", (int)hl, p, port)
                         : snprintf(origin, n, "%s://%.*s", https ? "https" : "http", (int)hl, p);
        if (w < 0 || (size_t)w >= n) { if (n) origin[0] = 0; return -1; }
    }
    return 0;
}

int webreg_urls_ok(const char *start_url, const char *scope, const char *origin) {
    char o1[WEBREG_URL_MAX + 1], o2[WEBREG_URL_MAX + 1];
    if (!start_url || !scope || !origin) return 0;
    if (webreg_url_origin(start_url, o1, sizeof o1) || webreg_url_origin(scope, o2, sizeof o2)) return 0;
    if (strcmp(o1, origin) || strcmp(o2, origin)) return 0;
    size_t sl = strlen(scope);
    if (strchr(scope, '?') || strchr(scope, '#')) return 0;              /* a scope is a path */
    if (strncmp(start_url, scope, sl) || !weburl_in_scope(start_url, scope)) return 0;
    /* the scope names a path of that origin: "https://host/…" (an explicit default port is refused here,
     * so a scope never reads as another origin's prefix) */
    size_t ol = strlen(origin);
    return strncmp(scope, origin, ol) == 0 && scope[ol] == '/';
}

int webreg_default_scope(const char *url, char *out, size_t n) {
    char origin[WEBREG_URL_MAX + 1];
    if (webreg_url_origin(url, origin, sizeof origin)) return -1;
    size_t ol = strlen(origin);
    if (strncmp(url, origin, ol) || url[ol] != '/') {
        int w = snprintf(out, n, "%s/", origin);
        return w < 0 || (size_t)w >= n ? -1 : 0;
    }
    size_t cut = ol + strcspn(url + ol, "?#");
    size_t last = ol;
    for (size_t i = ol; i < cut; i++) if (url[i] == '/') last = i;
    if (last + 2 > n) return -1;
    memcpy(out, url, last + 1);
    out[last + 1] = 0;
    return 0;
}

/* ---- permissions ---- */

static const char *const perm_names[WEBREG_PERM_N] = { "notifications", "camera", "microphone", "screen", "location" };
static const char *const value_names[3] = { "allow", "ask", "deny" };

const char *webreg_perm_name(int kind) { return kind >= 0 && kind < WEBREG_PERM_N ? perm_names[kind] : NULL; }
int webreg_perm_kind(const char *name) {
    for (int i = 0; name && i < WEBREG_PERM_N; i++) if (!strcmp(name, perm_names[i])) return i;
    return -1;
}
const char *webreg_perm_value_name(int v) { return v >= 0 && v < 3 ? value_names[v] : NULL; }
int webreg_perm_value(const char *name) {
    for (int i = 0; name && i < 3; i++) if (!strcmp(name, value_names[i])) return i;
    return -1;
}

void webreg_init(WebApp *a) {
    memset(a, 0, sizeof *a);
    a->source = WEBREG_SRC_URL;
    for (int i = 0; i < WEBREG_PERM_N; i++) a->perms[i] = i == WEBREG_PERM_LOCATION ? WEBREG_DENY : WEBREG_ASK;
}

/* ---- checking ---- */

static int color_ok(const char *s) {
    if (!s || s[0] != '#' || strlen(s) != 7) return 0;
    for (int i = 1; i < 7; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
}

static int icon_ok(const char *icon, const char *id) {
    char png[WEBREG_ID_MAX + 16], svg[WEBREG_ID_MAX + 16];
    snprintf(png, sizeof png, "icons/%s.png", id);
    snprintf(svg, sizeof svg, "icons/%s.svg", id);
    return !strcmp(icon, png) || !strcmp(icon, svg);
}

int webreg_check(const WebApp *a, char *why, size_t wn) {
    if (!webreg_id_ok(a->id)) return bad(why, wn, "bad id");
    if (!webreg_name_ok(a->name)) return bad(why, wn, "bad name");
    if (!webreg_urls_ok(a->start_url, a->scope, a->origin))
        return bad(why, wn, "startUrl and scope are not https addresses of origin with scope a prefix of startUrl");
    if (a->icon[0] && !icon_ok(a->icon, a->id)) return bad(why, wn, "icon is not icons/%s.png or .svg", a->id);
    if (a->theme_color[0] && !color_ok(a->theme_color)) return bad(why, wn, "themeColor is not #rrggbb");
    if (a->source != WEBREG_SRC_CATALOG && a->source != WEBREG_SRC_URL) return bad(why, wn, "source is not catalog or url");
    if (a->nprofiles < 1 || a->nprofiles > WEBREG_PROFILES_MAX) return bad(why, wn, "profiles: 1 to %d", WEBREG_PROFILES_MAX);
    for (int i = 0; i < a->nprofiles; i++) {
        if (!webreg_id_ok(a->profiles[i].id) || !webreg_name_ok(a->profiles[i].name)) return bad(why, wn, "profiles: bad id or name");
        for (int j = 0; j < i; j++) if (!strcmp(a->profiles[i].id, a->profiles[j].id)) return bad(why, wn, "profiles: %s twice", a->profiles[i].id);
    }
    for (int i = 0; i < WEBREG_PERM_N; i++) if (a->perms[i] < WEBREG_ALLOW || a->perms[i] > WEBREG_DENY) return bad(why, wn, "permissions: a bad value");
    return 0;
}

/* ---- parsing ---- */

static int take_str(const JVal *v, char *out, size_t cap) {
    if (!v || v->type != J_STR || v->len >= cap || strlen(v->str) != v->len) return -1;
    memcpy(out, v->str, v->len + 1);
    return 0;
}

static int parse_profiles(const JVal *arr, WebApp *a, char *why, size_t wn) {
    if (!arr || arr->type != J_ARR || arr->len < 1 || arr->len > WEBREG_PROFILES_MAX) return bad(why, wn, "profiles: a list of 1 to %d", WEBREG_PROFILES_MAX);
    a->nprofiles = 0;
    for (const JVal *p = arr->kid; p; p = p->next) {
        if (p->type != J_OBJ || p->len != 2) return bad(why, wn, "profiles: each is { id, name } and nothing else");
        WebProfile *w = &a->profiles[a->nprofiles++];
        if (take_str(json_get(p, "id"), w->id, sizeof w->id) || take_str(json_get(p, "name"), w->name, sizeof w->name))
            return bad(why, wn, "profiles: each is { id, name } and nothing else");
    }
    return 0;
}

static int parse_perms(const JVal *o, WebApp *a, char *why, size_t wn) {
    if (o->type != J_OBJ) return bad(why, wn, "permissions: not an object");
    for (const JVal *m = o->kid; m; m = m->next) {
        int k = webreg_perm_kind(m->key);
        int v = m->type == J_STR ? webreg_perm_value(m->str) : -1;
        if (k < 0) return bad(why, wn, "permissions: unknown key %.40s", m->key);
        if (v < 0) return bad(why, wn, "permissions: %s is not allow, ask or deny", m->key);
        a->perms[k] = v;
    }
    return 0;
}

int webreg_parse(const char *json, size_t len, WebApp *a, char *why, size_t wn) {
    if (why && wn) why[0] = 0;
    if (!json || !a) return bad(why, wn, "nothing to read");
    webreg_init(a);
    if (len > WEBREG_FILE_MAX) { memset(a, 0, sizeof *a); return bad(why, wn, "over %d bytes", WEBREG_FILE_MAX); }
    JDoc d;
    char err[96];
    if (json_parse(json, len, &d, err, sizeof err)) { memset(a, 0, sizeof *a); return bad(why, wn, "not JSON: %s", err); }
    const JVal *r = d.root;
    int rc = -1, seen = 0;
    if (!r || r->type != J_OBJ) { bad(why, wn, "not an object"); goto out; }
    for (const JVal *m = r->kid; m; m = m->next) {
        const char *k = m->key;
        int e = 0;
        if (!strcmp(k, "schema")) { e = m->type != J_NUM || m->num != WEBREG_SCHEMA; seen |= 1; }
        else if (!strcmp(k, "id")) { e = take_str(m, a->id, sizeof a->id); seen |= 2; }
        else if (!strcmp(k, "name")) { e = take_str(m, a->name, sizeof a->name); seen |= 4; }
        else if (!strcmp(k, "startUrl")) { e = take_str(m, a->start_url, sizeof a->start_url); seen |= 8; }
        else if (!strcmp(k, "scope")) { e = take_str(m, a->scope, sizeof a->scope); seen |= 16; }
        else if (!strcmp(k, "origin")) { e = take_str(m, a->origin, sizeof a->origin); seen |= 32; }
        else if (!strcmp(k, "source")) {
            e = m->type != J_STR;
            if (!e && !strcmp(m->str, "catalog")) a->source = WEBREG_SRC_CATALOG;
            else if (!e && !strcmp(m->str, "url")) a->source = WEBREG_SRC_URL;
            else e = 1;
            seen |= 64;
        }
        else if (!strcmp(k, "profiles")) { if (parse_profiles(m, a, why, wn)) goto out; seen |= 128; }
        else if (!strcmp(k, "icon")) e = take_str(m, a->icon, sizeof a->icon);
        else if (!strcmp(k, "themeColor")) e = take_str(m, a->theme_color, sizeof a->theme_color);
        else if (!strcmp(k, "background")) { e = m->type != J_BOOL; a->background = m->num != 0; }
        else if (!strcmp(k, "permissions")) { if (parse_perms(m, a, why, wn)) goto out; }
        else { bad(why, wn, "unknown key %.40s", k); goto out; }
        if (e) { bad(why, wn, "bad %s", k); goto out; }
    }
    if (seen != 255) { bad(why, wn, "schema, id, name, startUrl, scope, origin, source and profiles are needed"); goto out; }
    rc = webreg_check(a, why, wn);
out:
    json_free(&d);
    if (rc) memset(a, 0, sizeof *a);
    return rc;
}

/* ---- writing ---- */

typedef struct { char *p; size_t n, cap; int over; } Out;

static void put(Out *o, const char *s) {
    size_t l = strlen(s);
    if (o->over || o->n + l >= o->cap) { o->over = 1; return; }
    memcpy(o->p + o->n, s, l);
    o->n += l;
    o->p[o->n] = 0;
}

static void putq(Out *o, const char *s) {
    put(o, "\"");
    char e[8];
    for (; *s && !o->over; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; e[2] = 0; put(o, e); }
        else if (c < 0x20) { snprintf(e, sizeof e, "\\u%04x", c); put(o, e); }
        else { e[0] = (char)c; e[1] = 0; put(o, e); }
    }
    put(o, "\"");
}

static void member(Out *o, const char *key, const char *val, int last) {
    put(o, "  ");
    putq(o, key);
    put(o, ": ");
    putq(o, val);
    put(o, last ? "\n" : ",\n");
}

size_t webreg_write(const WebApp *a, char *out, size_t cap) {
    if (!out || !cap || webreg_check(a, NULL, 0)) return 0;
    Out o = { out, 0, cap, 0 };
    out[0] = 0;
    put(&o, "{\n  \"schema\": 1,\n");
    member(&o, "id", a->id, 0);
    member(&o, "name", a->name, 0);
    member(&o, "startUrl", a->start_url, 0);
    member(&o, "scope", a->scope, 0);
    member(&o, "origin", a->origin, 0);
    if (a->icon[0]) member(&o, "icon", a->icon, 0);
    if (a->theme_color[0]) member(&o, "themeColor", a->theme_color, 0);
    member(&o, "source", a->source == WEBREG_SRC_CATALOG ? "catalog" : "url", 0);
    put(&o, a->background ? "  \"background\": true,\n" : "  \"background\": false,\n");
    put(&o, "  \"profiles\": [");
    for (int i = 0; i < a->nprofiles; i++) {
        put(&o, i ? ", { \"id\": " : " { \"id\": ");
        putq(&o, a->profiles[i].id);
        put(&o, ", \"name\": ");
        putq(&o, a->profiles[i].name);
        put(&o, " }");
    }
    put(&o, " ],\n  \"permissions\": {");
    for (int i = 0; i < WEBREG_PERM_N; i++) {
        put(&o, i ? ", " : " ");
        putq(&o, perm_names[i]);
        put(&o, ": ");
        putq(&o, value_names[a->perms[i]]);
    }
    put(&o, " }\n}\n");
    if (o.over || o.n > WEBREG_FILE_MAX) { out[0] = 0; return 0; }
    return o.n;
}

/* ---- profile and app ids ---- */

int webreg_profile_index(const WebApp *a, const char *profile) {
    for (int i = 0; profile && i < a->nprofiles; i++) if (!strcmp(a->profiles[i].id, profile)) return i;
    return -1;
}

static int slug(const char *name, const char *fallback, char *id, size_t n, int (*taken)(const char *, void *), void *ctx) {
    char base[WEBREG_ID_MAX + 1];
    size_t o = 0;
    int dash = 0;
    for (const char *s = name ? name : ""; *s && o < WEBREG_ID_MAX - 3; s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (dash && o) base[o++] = '-';
            dash = 0;
            if (o < WEBREG_ID_MAX - 3) base[o++] = c;
        } else dash = 1;
    }
    base[o] = 0;
    if (!o) snprintf(base, sizeof base, "%s", fallback);
    for (int k = 1; k < 100; k++) {
        char cand[WEBREG_ID_MAX + 16];
        if (k == 1) snprintf(cand, sizeof cand, "%s", base);
        else snprintf(cand, sizeof cand, "%s-%d", base, k);
        if (!webreg_id_ok(cand)) return -1;
        if (!taken || !taken(cand, ctx)) {
            if (strlen(cand) + 1 > n) return -1;
            memcpy(id, cand, strlen(cand) + 1);
            return 0;
        }
    }
    return -1;
}

static int profile_taken(const char *id, void *ctx) { return webreg_profile_index((const WebApp *)ctx, id) >= 0; }

int webreg_profile_new_id(const WebApp *a, const char *name, char *id, size_t n) {
    if (a->nprofiles >= WEBREG_PROFILES_MAX) return -1;
    return slug(name, "profile", id, n, profile_taken, (void *)a);
}

int webreg_slug(const char *name, char *id, size_t n, int (*taken)(const char *id, void *ctx), void *ctx) {
    return slug(name, "app", id, n, taken, ctx);
}

/* ---- the probe's answers ---- */

/* name without hidden characters, runs of spaces made one, cut at a character boundary */
static void clean_name(const char *s, char *out, size_t cap) {
    size_t n = strlen(s), o = 0;
    int space = 0;
    for (size_t i = 0; i < n;) {
        size_t at = i;
        unsigned cp;
        if (utf8_next((const unsigned char *)s, n, &i, &cp)) { i = at + 1; continue; }
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') { space = o > 0; continue; }
        if (hidden_char(cp)) continue;
        size_t l = i - at;
        if (o + (space ? 1 : 0) + l >= cap || o + (space ? 1 : 0) + l > WEBREG_NAME_MAX) break;
        if (space) out[o++] = ' ';
        space = 0;
        memcpy(out + o, s + at, l);
        o += l;
    }
    out[o] = 0;
}

static const char *const PROBE_CODES[] = { "web.probe.request", "web.probe.load", "web.probe.tls", "web.probe.notHttps", "web.probe.page",
                                           "web.probe.timeout", "web.probe.iconType", "web.probe.iconTooBig", NULL };

int webreg_probe_code_ok(const char *code) {
    for (int i = 0; code && PROBE_CODES[i]; i++) if (!strcmp(code, PROBE_CODES[i])) return 1;
    return 0;
}

/* the largest side of "sizes" ("192x192 512x512", "any" is 0) */
static long icon_px(const char *sizes) {
    long best = 0;
    for (const char *s = sizes ? sizes : ""; *s;) {
        char *e;
        long w = strtol(s, &e, 10);
        if (e != s && (*e == 'x' || *e == 'X')) {
            long h = strtol(e + 1, &e, 10);
            if (w > best) best = w;
            if (h > best) best = h;
        }
        s = *e ? e + 1 : e;
    }
    return best;
}

static int ends_ci(const char *s, const char *tail) {
    size_t n = strcspn(s, "?#"), t = strlen(tail);
    return n >= t && !strncasecmp(s + n - t, tail, t);
}

/* the icon --probe-icon fetches: a PNG of the largest size up to 512 (else the smallest above), else an SVG */
static void pick_icon(const JVal *icons, WebProbe *p) {
    long best_png = -1, best_svg = -1;
    int png_big = 0;
    for (const JVal *i = icons && icons->type == J_ARR ? icons->kid : NULL; i; i = i->next) {
        const char *src = i->type == J_OBJ ? json_str(i, "src") : NULL, *type = i->type == J_OBJ ? json_str(i, "type") : NULL;
        if (!src || strlen(src) > WEBREG_URL_MAX || strncmp(src, "https://", 8) || webreg_url_origin(src, NULL, 0)) continue;
        int png = type && type[0] ? !strcmp(type, "image/png") : ends_ci(src, ".png");
        int svg = type && type[0] ? !strcmp(type, "image/svg+xml") : ends_ci(src, ".svg");
        long px = icon_px(json_str(i, "sizes"));
        if (png) {
            int big = px > 512;
            /* up to 512: the larger the better; above it only when nothing fits, then the smaller the better */
            int better = best_png < 0 || (png_big && !big) || (!png_big && !big && px > best_png) || (png_big && big && px < best_png);
            if (better) { best_png = px; png_big = big; memcpy(p->icon_url, src, strlen(src) + 1); }
        } else if (svg && best_png < 0 && best_svg < 0) {
            best_svg = 0;
            memcpy(p->icon_url, src, strlen(src) + 1);
        }
    }
}

int webreg_probe_parse(const char *line, size_t len, WebProbe *p) {
    memset(p, 0, sizeof *p);
    if (!line || len > WEBREG_FILE_MAX) return -1;
    JDoc d;
    if (json_parse(line, len, &d, NULL, 0)) return -1;
    int rc = -1;
    const JVal *r = d.root;
    if (r->type == J_OBJ && !json_bool(r, "ok", 1)) {
        const char *code = json_str(r, "error");
        if (webreg_probe_code_ok(code)) snprintf(p->error, sizeof p->error, "%s", code);
        goto out;
    }
    const char *start = r->type == J_OBJ ? json_str(r, "startUrl") : NULL;
    if (!start || !json_bool(r, "ok", 0) || strlen(start) > WEBREG_URL_MAX || webreg_url_origin(start, p->origin, sizeof p->origin)) goto out;
    memcpy(p->start_url, start, strlen(start) + 1);
    const char *scope = json_str(r, "scope");
    if (scope && strlen(scope) <= WEBREG_URL_MAX && webreg_urls_ok(start, scope, p->origin)) memcpy(p->scope, scope, strlen(scope) + 1);
    else if (webreg_default_scope(start, p->scope, sizeof p->scope)) goto out;
    const char *name = json_str(r, "name");
    if (name) clean_name(name, p->name, sizeof p->name);
    if (!webreg_name_ok(p->name)) {
        const char *host = strstr(p->origin, "://") + 3;
        snprintf(p->name, sizeof p->name, "%.*s", WEBREG_NAME_MAX, host);
    }
    const char *tc = json_str(r, "themeColor");
    if (tc && color_ok(tc)) memcpy(p->theme_color, tc, 8);
    p->manifest = json_bool(r, "manifest", 0);
    pick_icon(json_get(r, "icons"), p);
    p->ok = 1;
    rc = 0;
out:
    json_free(&d);
    return rc;
}

int webreg_icon_header(const char *line, size_t len, size_t *size, int *svg, char *err, size_t en) {
    JDoc d;
    if (err && en) err[0] = 0;
    if (!line || len > 4096 || json_parse(line, len, &d, NULL, 0)) return -1;
    int rc = -1;
    const JVal *ic = d.root->type == J_OBJ ? d.root : NULL;
    const JVal *ok = ic ? json_get(ic, "ok") : NULL;
    if (!ok || ok->type != J_BOOL) ic = NULL;
    else if (!ok->num) {
        const char *code = json_str(ic, "error");
        if (code && webreg_probe_code_ok(code) && err && en) snprintf(err, en, "%s", code);
        rc = 0;
    } else {
        const char *type = json_str(ic, "type");
        double n = json_num(ic, "size", -1);
        if (type && (!strcmp(type, "image/png") || !strcmp(type, "image/svg+xml")) && n >= 1 && n <= WEBREG_ICON_BYTES && n == (double)(size_t)n) {
            *size = (size_t)n;
            *svg = !strcmp(type, "image/svg+xml");
            rc = 1;
        }
    }
    json_free(&d);
    return rc;
}

static const unsigned char PNG_SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

static uint32_t be32(const unsigned char *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }

static int has_ci(const unsigned char *b, size_t n, const char *word) {
    size_t w = strlen(word);
    for (size_t i = 0; i + w <= n; i++) if (!strncasecmp((const char *)b + i, word, w)) return 1;
    return 0;
}

int webreg_icon_kind(const unsigned char *b, size_t n) {
    if (!b || !n || n > WEBREG_ICON_BYTES) return 0;
    if (n >= 33 && !memcmp(b, PNG_SIG, 8)) {
        uint32_t w = be32(b + 16), h = be32(b + 20);
        if (be32(b + 8) == 13 && !memcmp(b + 12, "IHDR", 4) && w && h && w <= 4096 && h <= 4096) return 1;
        return 0;
    }
    if (memchr(b, 0, n)) return 0;
    /* text whose first element, within the first 4 KB, is <svg> (after a byte order mark, an XML declaration,
     * comments or a doctype) */
    size_t i = n >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf ? 3 : 0, lim = n < 4096 ? n : 4096;
    int svg = 0;
    while (i < lim && !svg) {
        while (i < lim && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n')) i++;
        if (i >= lim || b[i] != '<') return 0;
        const char *end = NULL;
        if (i + 4 <= lim && !memcmp(b + i, "<!--", 4)) end = "-->";
        else if (i + 2 <= lim && (!memcmp(b + i, "<?", 2) || !memcmp(b + i, "<!", 2))) end = ">";
        if (!end) {
            if (i + 5 <= lim && !memcmp(b + i, "<svg", 4) && (b[i + 4] == ' ' || b[i + 4] == '>' || b[i + 4] == '\n' || b[i + 4] == '\t' || b[i + 4] == '\r' || b[i + 4] == '/'))
                svg = 1;
            else return 0;
            break;
        }
        size_t el = strlen(end), j = i + 2;
        while (j + el <= lim && memcmp(b + j, end, el)) j++;
        if (j + el > lim) return 0;
        i = j + el;
    }
    if (!svg) return 0;
    /* drawn as a picture, never run: still, nothing that would be code in a browser */
    if (has_ci(b, n, "<script") || has_ci(b, n, "<foreignobject") || has_ci(b, n, "javascript:")) return 0;
    return 2;
}

/* ---- lord-web's side ---- */

const WebProfile *webreg_profile(const WebApp *a, const char *id) {
    int i = a ? webreg_profile_index(a, id) : -1;
    return i >= 0 ? &a->profiles[i] : NULL;
}

int webreg_check_launch(const WebApp *a, const char *app, const char *profile, char *why, size_t wn) {
    if (!webreg_id_ok(app)) return bad(why, wn, "bad app id");
    if (!webreg_id_ok(profile)) return bad(why, wn, "bad profile id");
    if (strcmp(a->id, app)) return bad(why, wn, "the file is for %s, not %s", a->id, app);
    if (!webreg_profile(a, profile)) return bad(why, wn, "%s has no profile %s", app, profile);
    return 0;
}

int webreg_load(const char *home, const char *app, WebApp *a, char *why, size_t wn) {
    if (!home || home[0] != '/' || !webreg_id_ok(app)) return bad(why, wn, "bad home or app id");
    char path[4096];
    if (snprintf(path, sizeof path, "%s/.config/lordos/web/apps/%s.json", home, app) >= (int)sizeof path) return bad(why, wn, "path too long");
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return bad(why, wn, "%s: %s", path, strerror(errno));
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return bad(why, wn, "%s is not a file", path); }
    if (st.st_size > WEBREG_FILE_MAX) { close(fd); return bad(why, wn, "%s is over %d bytes", path, WEBREG_FILE_MAX); }
    char *buf = malloc(WEBREG_FILE_MAX + 1);
    if (!buf) { close(fd); return bad(why, wn, "out of memory"); }
    size_t len = 0;
    for (;;) {
        ssize_t k = read(fd, buf + len, WEBREG_FILE_MAX + 1 - len);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        len += (size_t)k;
        if (len > WEBREG_FILE_MAX) break;
    }
    close(fd);
    int r = len > WEBREG_FILE_MAX ? bad(why, wn, "%s is over %d bytes", path, WEBREG_FILE_MAX) : webreg_parse(buf, len, a, why, wn);
    free(buf);
    return r;
}

/* ---- files ---- */

char *webreg_read_file(const char *path, size_t max, size_t *len) {
    *len = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    char *b = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size >= 0 && (size_t)st.st_size <= max) {
        size_t want = (size_t)st.st_size, got = 0;
        b = malloc(want + 1);
        while (b && got < want) {
            ssize_t r = read(fd, b + got, want - got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            got += (size_t)r;
        }
        if (b && got == want) { b[got] = 0; *len = got; }
        else { free(b); b = NULL; }
    }
    close(fd);
    return b;
}

int webreg_write_file(const char *path, const void *data, size_t len) {
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -1;
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    const char *p = data;
    size_t done = 0;
    int ok = 1;
    while (ok && done < len) {
        ssize_t w = write(fd, p + done, len - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) ok = 0; else done += (size_t)w;
    }
    ok = ok && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    ok = ok && rename(tmp, path) == 0;
    if (!ok) { unlink(tmp); return -1; }
    return 0;
}
