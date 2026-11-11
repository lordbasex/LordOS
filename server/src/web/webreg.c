/* webreg.c — see webreg.h. */
#include "webreg.h"
#include "../sec/json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static int bad(char *why, size_t wn, const char *s) { if (why && wn) snprintf(why, wn, "%s", s); return -1; }

/* ---- ids and names ---- */

int webreg_id_ok(const char *s) {
    if (!s || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9'))) return 0;
    size_t i = 1;
    for (; s[i]; i++) {
        char c = s[i];
        if (i >= WEB_ID_MAX || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return 0;
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
    if (!n || n > WEB_NAME_MAX || s[0] == ' ' || s[n - 1] == ' ') return 0;
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
    if (!len || len > WEB_URL_MAX) return -1;
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
    char o1[WEB_URL_MAX + 1], o2[WEB_URL_MAX + 1];
    if (!start_url || !scope || !origin) return 0;
    if (webreg_url_origin(start_url, o1, sizeof o1) || webreg_url_origin(scope, o2, sizeof o2)) return 0;
    if (strcmp(o1, origin) || strcmp(o2, origin)) return 0;
    size_t sl = strlen(scope);
    if (strncmp(start_url, scope, sl)) return 0;
    /* the scope names a path of that origin: "https://host/…" (an explicit default port is refused here,
     * so a scope never reads as another origin's prefix) */
    size_t ol = strlen(origin);
    return strncmp(scope, origin, ol) == 0 && scope[ol] == '/';
}

int webreg_default_scope(const char *url, char *out, size_t n) {
    char origin[WEB_URL_MAX + 1];
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

static const char *const perm_names[WEB_PERM_N] = { "notifications", "camera", "microphone", "screen", "location" };
static const char *const value_names[3] = { "allow", "ask", "deny" };

const char *webreg_perm_name(int kind) { return kind >= 0 && kind < WEB_PERM_N ? perm_names[kind] : NULL; }
int webreg_perm_kind(const char *name) {
    for (int i = 0; name && i < WEB_PERM_N; i++) if (!strcmp(name, perm_names[i])) return i;
    return -1;
}
const char *webreg_perm_value_name(int v) { return v >= 0 && v < 3 ? value_names[v] : NULL; }
int webreg_perm_value(const char *name) {
    for (int i = 0; name && i < 3; i++) if (!strcmp(name, value_names[i])) return i;
    return -1;
}

void webreg_init(WebApp *a) {
    memset(a, 0, sizeof *a);
    a->source = WEB_SRC_URL;
    for (int i = 0; i < WEB_PERM_N; i++) a->perms[i] = i == WEB_PERM_LOCATION ? WEB_DENY : WEB_ASK;
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
    char png[WEB_ID_MAX + 16], svg[WEB_ID_MAX + 16];
    snprintf(png, sizeof png, "icons/%s.png", id);
    snprintf(svg, sizeof svg, "icons/%s.svg", id);
    return !strcmp(icon, png) || !strcmp(icon, svg);
}

int webreg_check(const WebApp *a, char *why, size_t wn) {
    if (!webreg_id_ok(a->id)) return bad(why, wn, "id");
    if (!webreg_name_ok(a->name)) return bad(why, wn, "name");
    if (!webreg_urls_ok(a->start_url, a->scope, a->origin)) return bad(why, wn, "startUrl, scope or origin");
    if (a->icon[0] && !icon_ok(a->icon, a->id)) return bad(why, wn, "icon");
    if (a->theme_color[0] && !color_ok(a->theme_color)) return bad(why, wn, "themeColor");
    if (a->source != WEB_SRC_CATALOG && a->source != WEB_SRC_URL) return bad(why, wn, "source");
    if (a->nprofiles < 1 || a->nprofiles > WEB_PROFILES_MAX) return bad(why, wn, "profiles");
    for (int i = 0; i < a->nprofiles; i++) {
        if (!webreg_id_ok(a->profiles[i].id) || !webreg_name_ok(a->profiles[i].name)) return bad(why, wn, "profile");
        for (int j = 0; j < i; j++) if (!strcmp(a->profiles[i].id, a->profiles[j].id)) return bad(why, wn, "profile id repeated");
    }
    for (int i = 0; i < WEB_PERM_N; i++) if (a->perms[i] < WEB_ALLOW || a->perms[i] > WEB_DENY) return bad(why, wn, "permission");
    return 0;
}

/* ---- parsing ---- */

static int take_str(const JVal *v, char *out, size_t cap) {
    if (!v || v->type != J_STR || v->len >= cap || strlen(v->str) != v->len) return -1;
    memcpy(out, v->str, v->len + 1);
    return 0;
}

static int parse_profiles(const JVal *arr, WebApp *a, char *why, size_t wn) {
    if (!arr || arr->type != J_ARR || arr->len < 1 || arr->len > WEB_PROFILES_MAX) return bad(why, wn, "profiles");
    a->nprofiles = 0;
    for (const JVal *p = arr->kid; p; p = p->next) {
        if (p->type != J_OBJ || p->len != 2) return bad(why, wn, "profile");
        WebProfile *w = &a->profiles[a->nprofiles++];
        if (take_str(json_get(p, "id"), w->id, sizeof w->id) || take_str(json_get(p, "name"), w->name, sizeof w->name))
            return bad(why, wn, "profile");
    }
    return 0;
}

static int parse_perms(const JVal *o, WebApp *a, char *why, size_t wn) {
    if (o->type != J_OBJ) return bad(why, wn, "permissions");
    for (const JVal *m = o->kid; m; m = m->next) {
        int k = webreg_perm_kind(m->key);
        int v = m->type == J_STR ? webreg_perm_value(m->str) : -1;
        if (k < 0) return bad(why, wn, "unknown permission");
        if (v < 0) return bad(why, wn, "permission value");
        a->perms[k] = v;
    }
    return 0;
}

int webreg_parse(const char *json, size_t len, WebApp *a, char *why, size_t wn) {
    if (!json || len > WEB_FILE_MAX) return bad(why, wn, "too big");
    JDoc d;
    char err[96];
    if (json_parse(json, len, &d, err, sizeof err)) return bad(why, wn, err);
    webreg_init(a);
    const JVal *r = d.root;
    int rc = -1, seen = 0;
    if (r->type != J_OBJ) { bad(why, wn, "not an object"); goto out; }
    for (const JVal *m = r->kid; m; m = m->next) {
        const char *k = m->key;
        int e = 0;
        if (!strcmp(k, "schema")) { e = m->type != J_NUM || m->num != WEB_SCHEMA; seen |= 1; }
        else if (!strcmp(k, "id")) { e = take_str(m, a->id, sizeof a->id); seen |= 2; }
        else if (!strcmp(k, "name")) { e = take_str(m, a->name, sizeof a->name); seen |= 4; }
        else if (!strcmp(k, "startUrl")) { e = take_str(m, a->start_url, sizeof a->start_url); seen |= 8; }
        else if (!strcmp(k, "scope")) { e = take_str(m, a->scope, sizeof a->scope); seen |= 16; }
        else if (!strcmp(k, "origin")) { e = take_str(m, a->origin, sizeof a->origin); seen |= 32; }
        else if (!strcmp(k, "source")) {
            e = m->type != J_STR;
            if (!e && !strcmp(m->str, "catalog")) a->source = WEB_SRC_CATALOG;
            else if (!e && !strcmp(m->str, "url")) a->source = WEB_SRC_URL;
            else e = 1;
            seen |= 64;
        }
        else if (!strcmp(k, "profiles")) { if (parse_profiles(m, a, why, wn)) goto out; seen |= 128; }
        else if (!strcmp(k, "icon")) e = take_str(m, a->icon, sizeof a->icon);
        else if (!strcmp(k, "themeColor")) e = take_str(m, a->theme_color, sizeof a->theme_color);
        else if (!strcmp(k, "background")) { e = m->type != J_BOOL; a->background = m->num != 0; }
        else if (!strcmp(k, "permissions")) { if (parse_perms(m, a, why, wn)) goto out; }
        else { bad(why, wn, "unknown key"); goto out; }
        if (e) { bad(why, wn, k); goto out; }
    }
    if (seen != 255) { bad(why, wn, "a required key is missing"); goto out; }
    rc = webreg_check(a, why, wn);
out:
    json_free(&d);
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
    member(&o, "source", a->source == WEB_SRC_CATALOG ? "catalog" : "url", 0);
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
    for (int i = 0; i < WEB_PERM_N; i++) {
        put(&o, i ? ", " : " ");
        putq(&o, perm_names[i]);
        put(&o, ": ");
        putq(&o, value_names[a->perms[i]]);
    }
    put(&o, " }\n}\n");
    if (o.over || o.n > WEB_FILE_MAX) { out[0] = 0; return 0; }
    return o.n;
}

/* ---- profile and app ids ---- */

int webreg_profile_index(const WebApp *a, const char *profile) {
    for (int i = 0; profile && i < a->nprofiles; i++) if (!strcmp(a->profiles[i].id, profile)) return i;
    return -1;
}

static int slug(const char *name, const char *fallback, char *id, size_t n, int (*taken)(const char *, void *), void *ctx) {
    char base[WEB_ID_MAX + 1];
    size_t o = 0;
    int dash = 0;
    for (const char *s = name ? name : ""; *s && o < WEB_ID_MAX - 3; s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (dash && o) base[o++] = '-';
            dash = 0;
            if (o < WEB_ID_MAX - 3) base[o++] = c;
        } else dash = 1;
    }
    base[o] = 0;
    if (!o) snprintf(base, sizeof base, "%s", fallback);
    for (int k = 1; k < 100; k++) {
        char cand[WEB_ID_MAX + 16];
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
    if (a->nprofiles >= WEB_PROFILES_MAX) return -1;
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
        if (o + (space ? 1 : 0) + l >= cap || o + (space ? 1 : 0) + l > WEB_NAME_MAX) break;
        if (space) out[o++] = ' ';
        space = 0;
        memcpy(out + o, s + at, l);
        o += l;
    }
    out[o] = 0;
}

int webreg_probe_parse(const char *line, size_t len, WebProbe *p) {
    memset(p, 0, sizeof *p);
    if (!line || len > WEB_FILE_MAX) return -1;
    JDoc d;
    if (json_parse(line, len, &d, NULL, 0)) return -1;
    int rc = -1;
    const JVal *r = d.root;
    const char *start = r->type == J_OBJ ? json_str(r, "startUrl") : NULL;
    if (!start || !json_bool(r, "ok", 0) || strlen(start) > WEB_URL_MAX || webreg_url_origin(start, p->origin, sizeof p->origin)) goto out;
    memcpy(p->start_url, start, strlen(start) + 1);
    const char *scope = json_str(r, "scope");
    if (scope && strlen(scope) <= WEB_URL_MAX && webreg_urls_ok(start, scope, p->origin)) memcpy(p->scope, scope, strlen(scope) + 1);
    else if (webreg_default_scope(start, p->scope, sizeof p->scope)) goto out;
    const char *name = json_str(r, "name");
    if (name) clean_name(name, p->name, sizeof p->name);
    if (!webreg_name_ok(p->name)) {
        const char *host = strstr(p->origin, "://") + 3;
        snprintf(p->name, sizeof p->name, "%.*s", WEB_NAME_MAX, host);
    }
    const char *tc = json_str(r, "themeColor");
    if (tc && color_ok(tc)) memcpy(p->theme_color, tc, 8);
    p->manifest = json_bool(r, "manifest", 0);
    p->ok = 1;
    rc = 0;
out:
    json_free(&d);
    return rc;
}

int webreg_icon_header(const char *line, size_t len, size_t *size, int *svg) {
    JDoc d;
    if (!line || len > 4096 || json_parse(line, len, &d, NULL, 0)) return -1;
    int rc = -1;
    const JVal *ic = d.root->type == J_OBJ ? json_get(d.root, "icon") : NULL;
    if (ic && ic->type == J_NULL) rc = 0;
    else if (ic && ic->type == J_OBJ) {
        const char *type = json_str(ic, "type");
        double n = json_num(ic, "size", -1);
        if (type && (!strcmp(type, "image/png") || !strcmp(type, "image/svg+xml")) && n >= 1 && n <= WEB_ICON_MAX && n == (double)(size_t)n) {
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
    if (!b || !n || n > WEB_ICON_MAX) return 0;
    if (n >= 33 && !memcmp(b, PNG_SIG, 8)) {
        uint32_t w = be32(b + 16), h = be32(b + 20);
        if (be32(b + 8) == 13 && !memcmp(b + 12, "IHDR", 4) && w && h && w <= 4096 && h <= 4096) return 1;
        return 0;
    }
    if (memchr(b, 0, n)) return 0;
    size_t i = n >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf ? 3 : 0;
    while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n')) i++;
    if (i >= n || b[i] != '<') return 0;
    size_t head = n - i < 4096 ? n - i : 4096;
    if (!has_ci(b + i, head, "<svg")) return 0;
    /* drawn as a picture, never run: still, nothing that would be code in a browser */
    if (has_ci(b, n, "<script") || has_ci(b, n, "<foreignobject") || has_ci(b, n, "javascript:")) return 0;
    return 2;
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
