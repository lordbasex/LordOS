/* deb.c — versions, deb822 and relationships (see deb.h). Version comparison is dpkg's
 * (lib/dpkg/version.c): numeric epoch, then the version and the revision compared by
 * runs of non-digits (letters before symbols, ~ before the end) and of numbers. */
#include "deb.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int order(int c) {
    if (isdigit(c)) return 0;
    if (isalpha(c)) return c;
    if (c == '~') return -1;
    if (c) return c + 256;
    return 0;
}

/* dpkg's verrevcmp over (pointer, length) */
static int verrev(const char *a, const char *ae, const char *b, const char *be) {
    while (a < ae || b < be) {
        int first_diff = 0;
        while ((a < ae && !isdigit((unsigned char)*a)) || (b < be && !isdigit((unsigned char)*b))) {
            int ac = a < ae ? order((unsigned char)*a) : 0, bc = b < be ? order((unsigned char)*b) : 0;
            if (ac != bc) return ac - bc;
            a++;
            b++;
        }
        while (a < ae && *a == '0') a++;
        while (b < be && *b == '0') b++;
        while (a < ae && b < be && isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            if (!first_diff) first_diff = *a - *b;
            a++;
            b++;
        }
        if (a < ae && isdigit((unsigned char)*a)) return 1;
        if (b < be && isdigit((unsigned char)*b)) return -1;
        if (first_diff) return first_diff;
    }
    return 0;
}

typedef struct { unsigned long epoch; const char *up, *upe, *rev, *reve; } Ver;

static void split(const char *s, size_t n, Ver *v) {
    const char *e = s + n, *colon = memchr(s, ':', n);
    v->epoch = 0;
    if (colon) {
        for (const char *p = s; p < colon; p++) if (isdigit((unsigned char)*p)) v->epoch = v->epoch * 10 + (unsigned long)(*p - '0');
        s = colon + 1;
    }
    const char *dash = NULL;
    for (const char *p = s; p < e; p++) if (*p == '-') dash = p;
    v->up = s;
    v->upe = dash ? dash : e;
    v->rev = dash ? dash + 1 : e;
    v->reve = e;
}

int ver_cmp_n(const char *a, size_t an, const char *b, size_t bn) {
    Ver x, y;
    split(a, an, &x);
    split(b, bn, &y);
    if (x.epoch != y.epoch) return x.epoch < y.epoch ? -1 : 1;
    int r = verrev(x.up, x.upe, y.up, y.upe);
    if (r) return r < 0 ? -1 : 1;
    r = verrev(x.rev, x.reve, y.rev, y.reve);
    return r < 0 ? -1 : r > 0;
}

int ver_cmp(const char *a, const char *b) { return ver_cmp_n(a, strlen(a), b, strlen(b)); }

int ver_satisfies(const char *have, int op, const char *want, size_t wantlen) {
    if (op == REL_NONE) return 1;
    int c = ver_cmp_n(have, strlen(have), want, wantlen);
    switch (op) {
    case REL_LT: return c < 0;
    case REL_LE: return c <= 0;
    case REL_EQ: return c == 0;
    case REL_GE: return c >= 0;
    case REL_GT: return c > 0;
    }
    return 0;
}

int deb_valid_name(const char *s, size_t n) {
    if (n < 2 || n > 128 || !(islower((unsigned char)s[0]) || isdigit((unsigned char)s[0]))) return 0;
    for (size_t i = 1; i < n; i++)
        if (!islower((unsigned char)s[i]) && !isdigit((unsigned char)s[i]) && !strchr("+-.", s[i])) return 0;
    return 1;
}

size_t deb_base_len(const char *s, size_t n) {
    const char *c = memchr(s, ':', n);
    return c ? (size_t)(c - s) : n;
}

int deb_valid_pkgarg(const char *s, size_t n) {
    size_t b = deb_base_len(s, n);
    if (b == n) return deb_valid_name(s, n);
    return deb_valid_name(s, b) && n - b - 1 == strlen(DEB_FOREIGN_ARCH) && !memcmp(s + b + 1, DEB_FOREIGN_ARCH, n - b - 1);
}

int deb_valid_version(const char *s, size_t n) {
    if (!n || n > 128) return 0;
    for (size_t i = 0; i < n; i++) if (!isalnum((unsigned char)s[i]) && !strchr(".+-~:", s[i])) return 0;
    return 1;
}

int d822_next_stanza(const char **pos, const char *end, const char **start, const char **stop) {
    const char *p = *pos;
    while (p < end && (*p == '\n' || *p == '\r')) p++;
    if (p >= end) { *pos = end; return 0; }
    *start = p;
    for (;;) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) { *stop = end; *pos = end; return 1; }
        p = nl + 1;
        if (p >= end || *p == '\n' || (*p == '\r' && p + 1 < end && p[1] == '\n')) { *stop = p; *pos = p; return 1; }
    }
}

int d822_next_field(D822 *it, D822Field *f) {
    const char *p = it->p, *end = it->end;
    for (;;) {
        if (p >= end) return 0;
        if (*p == '#' || *p == ' ' || *p == '\t') {        /* comment or stray continuation */
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            p = nl ? nl + 1 : end;
            continue;
        }
        break;
    }
    const char *colon = memchr(p, ':', (size_t)(end - p)), *nl = memchr(p, '\n', (size_t)(end - p));
    if (!nl) nl = end;
    if (!colon || colon > nl) { it->p = nl < end ? nl + 1 : end; return d822_next_field(it, f); }
    f->name = p;
    f->nlen = (size_t)(colon - p);
    const char *v = colon + 1;
    while (v < nl && (*v == ' ' || *v == '\t')) v++;
    /* continuations: lines starting with a space */
    const char *ve = nl;
    while (ve < end && ve + 1 < end && (ve[1] == ' ' || ve[1] == '\t')) {
        const char *n2 = memchr(ve + 1, '\n', (size_t)(end - ve - 1));
        ve = n2 ? n2 : end;
    }
    const char *vend = ve;
    while (vend > v && (vend[-1] == '\r' || vend[-1] == ' ' || vend[-1] == '\t')) vend--;
    f->value = v;
    f->vlen = (size_t)(vend - v);
    it->p = ve < end ? ve + 1 : end;
    return 1;
}

int d822_is(const D822Field *f, const char *name) {
    size_t n = strlen(name);
    return f->nlen == n && !strncasecmp(f->name, name, n);
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

int deb_next_group(const char **pp, const char *end, DebRel *alts, int max) {
    const char *p = *pp;
    for (;;) {
        while (p < end && (is_space(*p) || *p == ',')) p++;
        if (p >= end) { *pp = end; return 0; }
        const char *ge = p;
        /* up to the comma, not counting those inside (), [] or <> */
        int paren = 0, brack = 0, angle = 0;
        while (ge < end) {
            char c = *ge;
            if (paren) { if (c == ')') paren = 0; }
            else if (brack) { if (c == ']') brack = 0; }
            else if (angle) { if (c == '>') angle = 0; }
            else if (c == '(') paren = 1;
            else if (c == '[') brack = 1;
            else if (c == '<') angle = 1;
            else if (c == ',') break;
            ge++;
        }
        int n = 0;
        const char *a = p;
        while (a < ge && n < max) {
            while (a < ge && (is_space(*a) || *a == '|')) a++;
            if (a >= ge) break;
            DebRel *r = &alts[n];
            r->name = a;
            while (a < ge && !is_space(*a) && *a != '(' && *a != '|' && *a != '[' && *a != '<' && *a != ':') a++;
            r->nlen = (size_t)(a - r->name);
            r->arch = NULL;
            r->alen = 0;
            if (a < ge && *a == ':') {          /* :any, :native, :i386 */
                r->arch = ++a;
                while (a < ge && !is_space(*a) && *a != '(' && *a != '|' && *a != '[' && *a != '<') a++;
                r->alen = (size_t)(a - r->arch);
                if (!r->alen) r->arch = NULL;
            }
            r->op = REL_NONE;
            r->ver = NULL;
            r->vlen = 0;
            while (a < ge && is_space(*a)) a++;
            if (a < ge && *a == '(') {
                a++;
                while (a < ge && is_space(*a)) a++;
                if (a + 1 < ge && a[0] == '<' && a[1] == '<') { r->op = REL_LT; a += 2; }
                else if (a + 1 < ge && a[0] == '<' && a[1] == '=') { r->op = REL_LE; a += 2; }
                else if (a + 1 < ge && a[0] == '>' && a[1] == '>') { r->op = REL_GT; a += 2; }
                else if (a + 1 < ge && a[0] == '>' && a[1] == '=') { r->op = REL_GE; a += 2; }
                else if (a < ge && a[0] == '=') { r->op = REL_EQ; a++; }
                else if (a < ge && a[0] == '<') { r->op = REL_LE; a++; }          /* obsolete */
                else if (a < ge && a[0] == '>') { r->op = REL_GE; a++; }
                while (a < ge && is_space(*a)) a++;
                r->ver = a;
                while (a < ge && *a != ')' && !is_space(*a)) a++;
                r->vlen = (size_t)(a - r->ver);
                while (a < ge && *a != ')') a++;
                if (a < ge) a++;
                if (!r->vlen) r->op = REL_NONE;
            }
            /* [architectures] and <profiles>: only exist in source packages; skipped */
            while (a < ge && *a != '|') a++;
            if (r->nlen) n++;
        }
        *pp = ge < end ? ge + 1 : end;
        if (n) return n;
        p = *pp;
    }
}

/* Release notes (docs/UPDATES.md): deb822 stanzas "Package", "Version" and a multi-line "Notes"
 * whose continuation lines start with one space and use " ." for an empty line, like a Debian
 * "Description". Returns the Markdown text of the matching stanza (malloc'd), or NULL. */
char *deb_notes_find(const char *buf, size_t len, const char *pkg, const char *version, size_t *outlen) {
    const char *pos = buf, *end = buf + len, *st, *se;
    size_t pl = strlen(pkg), vl = strlen(version);
    while (d822_next_stanza(&pos, end, &st, &se)) {
        D822 it = { st, se };
        D822Field f;
        int name_ok = 0, ver_ok = 0;
        const char *notes = NULL;
        size_t nlen = 0;
        while (d822_next_field(&it, &f)) {
            if (d822_is(&f, "Package")) name_ok = f.vlen == pl && !memcmp(f.value, pkg, pl);
            else if (d822_is(&f, "Version")) ver_ok = f.vlen == vl && !memcmp(f.value, version, vl);
            else if (d822_is(&f, "Notes")) { notes = f.value; nlen = f.vlen; }
        }
        if (!name_ok || !ver_ok || !notes) continue;
        char *out = malloc(nlen + 1);
        if (!out) return NULL;
        size_t o = 0;
        const char *p = notes, *e = notes + nlen;
        int first = 1;
        while (p < e) {
            const char *nl = memchr(p, '\n', (size_t)(e - p));
            if (!nl) nl = e;
            const char *l = p;
            size_t ll = (size_t)(nl - p);
            if (ll && l[ll - 1] == '\r') ll--;
            if (!first && ll && *l == ' ') { l++; ll--; }
            if (!first && ll == 1 && *l == '.') ll = 0;
            if (!(first && !ll)) {                  /* the empty text after "Notes:" */
                if (o) out[o++] = '\n';
                memcpy(out + o, l, ll);
                o += ll;
            }
            first = 0;
            p = nl + 1;
        }
        out[o] = 0;
        if (outlen) *outlen = o;
        return out;
    }
    return NULL;
}
