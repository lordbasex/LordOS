/* mimeapps.c — mimeapps.list and globs2 (see mimeapps.h). */
#include "mimeapps.h"

#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mime_valid(const char *m) {
    size_t n = m ? strlen(m) : 0;
    if (n < 3 || n >= MIME_MAX) return 0;
    const char *slash = strchr(m, '/');
    if (!slash || slash == m || !slash[1] || strchr(slash + 1, '/')) return 0;
    for (const char *p = m; *p; p++) {
        if (p == slash) continue;
        int first = p == m || p == slash + 1;
        if (first ? !(islower((unsigned char)*p) || isdigit((unsigned char)*p))
                  : !(isalnum((unsigned char)*p) || strchr(".+_-", *p))) return 0;
    }
    return 1;
}

int desktop_id_valid(const char *id) {
    size_t n = id ? strlen(id) : 0;
    if (n < 9 || n >= DESKTOP_ID_MAX || strcmp(id + n - 8, ".desktop")) return 0;
    if (strstr(id, "..") || id[0] == '.' || id[0] == '-') return 0;
    for (const char *p = id; *p; p++)
        if (!(isalnum((unsigned char)*p) || strchr("._-", *p))) return 0;
    return 1;
}

/* ---- line scanner ---- */

typedef struct { const char *p, *end; } Line;

static int next_line(const char **cur, Line *l) {
    if (!*cur || !**cur) return 0;
    l->p = *cur;
    const char *e = strchr(*cur, '\n');
    l->end = e ? e : *cur + strlen(*cur);
    *cur = e ? e + 1 : l->end;
    return 1;
}

static const char *skip_ws(const char *p, const char *end) { while (p < end && (*p == ' ' || *p == '\t')) p++; return p; }

/* Group header "[Name]": 1 and the name range, else 0. */
static int group_of(const Line *l, const char **name, size_t *len) {
    const char *p = skip_ws(l->p, l->end), *e = l->end;
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    if (p >= e || *p != '[' || e[-1] != ']') return 0;
    *name = p + 1;
    *len = (size_t)(e - p - 2);
    return 1;
}

/* "key = value": 1 if the key is `key`; *val = start of the value (trimmed range). */
static int key_is(const Line *l, const char *key, const char **val, const char **vend) {
    const char *p = skip_ws(l->p, l->end);
    size_t kl = strlen(key);
    if ((size_t)(l->end - p) < kl || memcmp(p, key, kl)) return 0;
    p = skip_ws(p + kl, l->end);
    if (p >= l->end || *p != '=') return 0;
    p = skip_ws(p + 1, l->end);
    const char *e = l->end;
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    *val = p;
    *vend = e;
    return 1;
}

static int in_group(const char *name, size_t len, const char *want) { return strlen(want) == len && !memcmp(name, want, len); }

/* Finds `mime=` in group `grp`; returns 1 with the value range. */
static int find_value(const char *text, const char *grp, const char *mime, const char **v, const char **ve) {
    const char *cur = text, *gn;
    size_t gl;
    int inside = 0;
    Line l;
    while (next_line(&cur, &l)) {
        if (group_of(&l, &gn, &gl)) { inside = in_group(gn, gl, grp); continue; }
        if (inside && key_is(&l, mime, v, ve)) return 1;
    }
    return 0;
}

int mimeapps_entry(const char *text, const char *group, const char *mime, int idx, char *out, size_t n) {
    const char *v, *ve;
    if (!text || !group || idx < 0 || !find_value(text, group, mime, &v, &ve)) return -1;
    for (const char *p = v; p < ve;) {
        const char *s = memchr(p, ';', (size_t)(ve - p));
        if (!s) s = ve;
        size_t len = (size_t)(s - p);
        if (len && len < n) {
            memcpy(out, p, len);
            out[len] = 0;
            if (desktop_id_valid(out) && idx-- == 0) return 0;
        }
        p = s + 1;
    }
    return -1;
}

int mimeapps_default(const char *text, const char *mime, char *out, size_t n) {
    return mimeapps_entry(text, "Default Applications", mime, 0, out, n);
}

static int list_has(const char *v, const char *ve, const char *id) {
    size_t il = strlen(id);
    for (const char *p = v; p < ve;) {
        const char *s = memchr(p, ';', (size_t)(ve - p));
        if (!s) s = ve;
        if ((size_t)(s - p) == il && !memcmp(p, id, il)) return 1;
        p = s + 1;
    }
    return 0;
}

int mimeapps_removed(const char *text, const char *mime, const char *id) {
    const char *v, *ve;
    return text && find_value(text, "Removed Associations", mime, &v, &ve) && list_has(v, ve, id);
}

/* ---- editing ---- */

typedef struct { char *p; size_t n, cap; int oom; } Buf;

static void put(Buf *b, const char *s, size_t n) {
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = (b->cap ? b->cap * 2 : 256) + n;
        char *np = realloc(b->p, cap);
        if (!np) { b->oom = 1; return; }
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void puts_(Buf *b, const char *s) { put(b, s, strlen(s)); }

static void put_key(Buf *b, const char *mime, const char *id, const char *v, const char *ve, int keep_rest) {
    puts_(b, mime);
    puts_(b, "=");
    puts_(b, id);
    puts_(b, ";");
    size_t il = strlen(id);
    for (const char *p = v; keep_rest && p < ve;) {
        const char *s = memchr(p, ';', (size_t)(ve - p));
        if (!s) s = ve;
        size_t len = (size_t)(s - p);
        if (len && !(len == il && !memcmp(p, id, len))) { put(b, p, len); puts_(b, ";"); }
        p = s + 1;
    }
    puts_(b, "\n");
}

static int blank(const Line *l) { const char *p = skip_ws(l->p, l->end); return p >= l->end || *p == '\r'; }

/* Rewrites `text` so that in group grp, mime's list is: id first, then (if keep_rest) the old
 * entries without id. The key goes at the end of the group (before its trailing blank lines) if
 * missing, and the group at the end of the file if missing. Other lines are copied unchanged. */
static char *set_list(const char *text, const char *grp, const char *mime, const char *id, int keep_rest) {
    Buf b = { 0 };
    const char *cur = text ? text : "", *gn;
    size_t gl;
    int inside = 0, done = 0, held = 0;         /* held: blank lines kept back inside the group */
    Line l;
    while (next_line(&cur, &l)) {
        const char *v, *ve;
        int hdr = group_of(&l, &gn, &gl);
        if (hdr && inside && !done) { put_key(&b, mime, id, NULL, NULL, 0); done = 1; }
        if (hdr) inside = in_group(gn, gl, grp);
        if (!hdr && inside && !done && blank(&l)) { held++; continue; }
        for (; held; held--) puts_(&b, "\n");
        if (!hdr && inside && !done && key_is(&l, mime, &v, &ve)) { put_key(&b, mime, id, v, ve, keep_rest); done = 1; continue; }
        put(&b, l.p, (size_t)(l.end - l.p));
        puts_(&b, "\n");
    }
    if (inside && !done) { put_key(&b, mime, id, NULL, NULL, 0); done = 1; }
    for (; held; held--) puts_(&b, "\n");
    if (!done) {
        if (b.n) puts_(&b, "\n");
        puts_(&b, "[");
        puts_(&b, grp);
        puts_(&b, "]\n");
        put_key(&b, mime, id, NULL, NULL, 0);
    }
    if (b.oom) { free(b.p); return NULL; }
    return b.p;
}

char *mimeapps_set_default(const char *text, const char *mime, const char *id) {
    if (!mime_valid(mime) || !desktop_id_valid(id)) return NULL;
    char *a = set_list(text, "Default Applications", mime, id, 0);
    if (!a) return NULL;
    char *b = set_list(a, "Added Associations", mime, id, 1);
    free(a);
    return b;
}

/* Copies `text` without mime's key line in group grp (every other line byte for byte). */
static char *drop_key(const char *text, const char *grp, const char *mime) {
    Buf b = { 0 };
    const char *cur = text ? text : "", *gn;
    size_t gl;
    int inside = 0;
    Line l;
    puts_(&b, "");
    while (next_line(&cur, &l)) {
        const char *v, *ve;
        if (group_of(&l, &gn, &gl)) inside = in_group(gn, gl, grp);
        else if (inside && key_is(&l, mime, &v, &ve)) continue;
        put(&b, l.p, (size_t)(l.end - l.p));
        if (*l.end == '\n') puts_(&b, "\n");
    }
    if (b.oom) { free(b.p); return NULL; }
    return b.p;
}

char *mimeapps_unset_default(const char *text, const char *mime) {
    if (!mime_valid(mime)) return NULL;
    char *a = drop_key(text, "Default Applications", mime);
    if (!a) return NULL;
    char *b = drop_key(a, "Added Associations", mime);
    free(a);
    return b;
}

/* ---- globs2 ---- */

int mime_guess(const char *globs2, const char *name, char *out, size_t n) {
    if (!globs2 || !name || !*name) return -1;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    char lower[256];
    size_t bl = strlen(base);
    if (bl >= sizeof lower) return -1;
    for (size_t i = 0; i <= bl; i++) lower[i] = (char)tolower((unsigned char)base[i]);
    int best_w = -1;
    size_t best_len = 0;
    int best_cs = 0;                            /* on a tie, a case-sensitive match wins */
    const char *cur = globs2;
    Line l;
    while (next_line(&cur, &l)) {
        if (l.p == l.end || *l.p == '#') continue;
        char line[512];
        size_t ll = (size_t)(l.end - l.p);
        if (ll >= sizeof line) continue;
        memcpy(line, l.p, ll);
        line[ll] = 0;
        char *f[4] = { line, NULL, NULL, NULL };
        for (int k = 1; k < 4; k++) { char *c = f[k - 1] ? strchr(f[k - 1], ':') : NULL; if (c) { *c = 0; f[k] = c + 1; } }
        if (!f[1] || !f[2]) continue;
        int w = atoi(f[0]);
        int cs = f[3] && strstr(f[3], "cs");
        const char *glob = f[2];
        size_t gl = strlen(glob);
        if (w < best_w || (w == best_w && (gl < best_len || (gl == best_len && (best_cs || !cs))))) continue;
        int ok;
        if (cs) ok = fnmatch(glob, base, 0) == 0;
        else {
            char g[256];
            if (gl >= sizeof g) continue;
            for (size_t i = 0; i <= gl; i++) g[i] = (char)tolower((unsigned char)glob[i]);
            ok = fnmatch(g, lower, 0) == 0;
        }
        if (!ok || !mime_valid(f[1]) || strlen(f[1]) >= n) continue;
        snprintf(out, n, "%s", f[1]);
        best_w = w;
        best_len = gl;
        best_cs = cs;
    }
    return best_w >= 0 ? 0 : -1;
}
