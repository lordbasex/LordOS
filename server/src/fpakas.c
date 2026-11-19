/* fpakas.c — Flathub's AppStream XML to a compact index, and that index's lines checked again (fpakas.h). A small XML
 * reader for this one format: tags with attributes, text, entities, comments, processing instructions and CDATA
 * skipped; a stack of element names so <developer><name> is never the app's name. Any input is safe: it never reads
 * past len, and a broken document only ends the index early. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "fpakas.h"
#include "fpak.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEPTH 32
#define NAME_LEN 32

typedef struct { const char *p, *end; } Cur;

static const char *find(const char *p, const char *end, const char *s) {
    size_t n = strlen(s);
    while (p && (size_t)(end - p) >= n) {
        const char *q = memchr(p, s[0], (size_t)(end - p - (ptrdiff_t)n + 1));
        if (!q) return NULL;
        if (!memcmp(q, s, n)) return q;
        p = q + 1;
    }
    return NULL;
}

/* UTF-8 of a code point (0 for one not to keep) */
static int put_cp(unsigned long cp, char *o) {
    if (!cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* text [s, e) with entities decoded, control characters and tabs as spaces, spaces collapsed and trimmed, valid UTF-8
 * only, cut at max bytes on a character boundary */
static void text_of(const char *s, const char *e, char *out, size_t max) {
    size_t o = 0;
    int space = 0;
    char tmp[4];
    while (s < e && o < max) {
        int n = 0;
        unsigned char c = (unsigned char)*s;
        if (c == '&') {
            const char *semi = memchr(s, ';', (size_t)(e - s) < 12 ? (size_t)(e - s) : 12);
            static const struct { const char *name; char c; } ents[] = { { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' } };
            int done = 0;
            if (semi) {
                size_t l = (size_t)(semi - s - 1);
                for (size_t k = 0; k < sizeof ents / sizeof ents[0] && !done; k++)
                    if (l == strlen(ents[k].name) && !memcmp(s + 1, ents[k].name, l)) { tmp[0] = ents[k].c; n = 1; done = 1; }
                if (!done && l > 1 && s[1] == '#') {
                    char num[12];
                    memcpy(num, s + 2, l - 1);
                    num[l - 1] = 0;
                    char *endp;
                    unsigned long cp = (num[0] == 'x' || num[0] == 'X') ? strtoul(num + 1, &endp, 16) : strtoul(num, &endp, 10);
                    if (*endp == 0 && endp != num) { n = put_cp(cp, tmp); done = 1; }
                }
            }
            if (done) s = semi + 1; else { tmp[0] = '&'; n = 1; s++; }
        } else {
            int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
            if (!len || s + len > e) { s++; continue; }
            int ok = 1;
            for (int k = 1; k < len; k++) if (((unsigned char)s[k] >> 6) != 2) ok = 0;
            if (!ok) { s++; continue; }
            memcpy(tmp, s, (size_t)len);
            n = len;
            s += len;
        }
        if (n == 1 && ((unsigned char)tmp[0] < 0x20 || tmp[0] == 0x7f || tmp[0] == ' ')) { space = o > 0; continue; }
        if (space) { if (o + 1 > max) break; out[o++] = ' '; space = 0; }
        if (o + (size_t)n > max) break;
        memcpy(out + o, tmp, (size_t)n);
        o += (size_t)n;
    }
    out[o] = 0;
}

/* the value of attribute name in a tag's attribute text [a, e), decoded, or "" */
static void attr(const char *a, const char *e, const char *name, char *out, size_t max) {
    size_t nl = strlen(name);
    out[0] = 0;
    for (const char *p = a; p < e; p++) {
        if ((p == a || isspace((unsigned char)p[-1])) && (size_t)(e - p) > nl + 2 && !memcmp(p, name, nl)) {
            const char *q = p + nl;
            while (q < e && isspace((unsigned char)*q)) q++;
            if (q >= e || *q != '=') continue;
            q++;
            while (q < e && isspace((unsigned char)*q)) q++;
            if (q >= e || (*q != '"' && *q != '\'')) continue;
            char quote = *q++;
            const char *v = memchr(q, quote, (size_t)(e - q));
            if (!v) return;
            text_of(q, v, out, max);
            return;
        }
    }
}

typedef struct {
    FpakasApp a;
    int name_score, summary_score, icon_score, dev_score, is_app;
    char bundle_id[64], bundle_arch[24], bundle_branch[64];
} Comp;

/* lang "es-AR" against xml:lang: 3 the same locale, 2 the same language, 1 none given, 0 another */
static int lang_score(const char *want, const char *got) {
    if (!got[0]) return 1;
    char w[24], g[24];
    snprintf(w, sizeof w, "%s", want);
    snprintf(g, sizeof g, "%s", got);
    for (char *p = w; *p; p++) if (*p == '-') *p = '_';
    for (char *p = g; *p; p++) if (*p == '-') *p = '_';
    if (!strcasecmp(w, g)) return 3;
    size_t wl = strcspn(w, "_"), gl = strcspn(g, "_");
    return wl == gl && !strncasecmp(w, g, wl) && !g[gl] ? 2 : 0;
}

static void clean_field(char *s) { for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' '; }

static int emit(Comp *c, const char *arch, int (*out)(void *, const char *, size_t), void *ud) {
    if (!c->is_app || !c->bundle_id[0] || strcmp(c->bundle_arch, arch) || !fpak_id_ok(c->bundle_id) || !fpak_branch_ok(c->bundle_branch) || !c->a.name[0])
        return 1;
    snprintf(c->a.id, sizeof c->a.id, "%s", c->bundle_id);
    snprintf(c->a.branch, sizeof c->a.branch, "%s", c->bundle_branch);
    char *f[] = { c->a.name, c->a.summary, c->a.icon, c->a.categories, c->a.license, c->a.developer };
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) clean_field(f[i]);
    char line[2048];
    int n = snprintf(line, sizeof line, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\n", c->a.id, c->a.branch, c->a.name, c->a.summary,
                     c->a.icon, c->a.categories, c->a.license, c->a.developer, c->a.verified ? 1 : 0);
    if (n <= 0 || n >= (int)sizeof line) return 1;
    return out(ud, line, (size_t)n) ? -1 : 0;
}

int fpakas_index(const char *xml, size_t len, const char *lang, const char *arch,
                 int (*out)(void *ud, const char *line, size_t n), void *ud) {
    if (!xml || !lang || !arch || !out) return -1;
    const char *p = xml, *end = xml + len;
    if (!find(p, end, "<components")) return -1;
    char stack[DEPTH][NAME_LEN];
    int depth = 0, count = 0, in_comp = 0;
    Comp c;
    memset(&c, 0, sizeof c);
    char value_key[96] = "", icon_type[16] = "", icon_w[8] = "", xml_lang[24] = "", bundle_type[16] = "";
    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) break;
        const char *text = p;
        p = lt;
        if ((size_t)(end - p) >= 4 && !memcmp(p, "<!--", 4)) { const char *q = find(p + 4, end, "-->"); if (!q) break; p = q + 3; continue; }
        if ((size_t)(end - p) >= 9 && !memcmp(p, "<![CDATA[", 9)) { const char *q = find(p + 9, end, "]]>"); if (!q) break; p = q + 3; continue; }
        if ((size_t)(end - p) >= 2 && (p[1] == '?' || p[1] == '!')) { const char *q = memchr(p, '>', (size_t)(end - p)); if (!q) break; p = q + 1; continue; }
        const char *gt = memchr(p, '>', (size_t)(end - p));
        if (!gt) break;
        int closing = p[1] == '/', self = gt[-1] == '/';
        const char *ns = p + 1 + closing, *ne = ns;
        while (ne < gt && !isspace((unsigned char)*ne) && *ne != '/') ne++;
        char name[NAME_LEN];
        size_t nl = (size_t)(ne - ns);
        if (!nl || nl >= sizeof name) { p = gt + 1; continue; }
        memcpy(name, ns, nl);
        name[nl] = 0;
        const char *parent = depth > 0 ? stack[depth - 1] : "";
        if (closing) {
            /* text of the element now closing: [text, lt) */
            const char *grand = depth > 1 ? stack[depth - 2] : "";
            if (in_comp && depth > 0 && !strcmp(stack[depth - 1], name)) {
                if (!strcmp(name, "name") && !strcmp(grand, "component")) {
                    int sc = lang_score(lang, xml_lang);
                    if (sc > c.name_score) { text_of(text, lt, c.a.name, FPAKAS_NAME_MAX); c.name_score = sc; }
                } else if (!strcmp(name, "summary") && !strcmp(grand, "component")) {
                    int sc = lang_score(lang, xml_lang);
                    if (sc > c.summary_score) { text_of(text, lt, c.a.summary, FPAKAS_SUMMARY_MAX); c.summary_score = sc; }
                } else if (!strcmp(name, "icon") && !strcmp(grand, "component") && !strcmp(icon_type, "cached")) {
                    int sc = !strcmp(icon_w, "64") ? 3 : !strcmp(icon_w, "128") ? 2 : 0;
                    char f[FPAKAS_ICON_MAX];
                    text_of(text, lt, f, 100);
                    int plain = f[0] && f[0] != '.';    /* a plain file name of the cache, never a path */
                    for (const char *q = f; *q && plain; q++) plain = isalnum((unsigned char)*q) || strchr("._+-", *q);
                    if (sc > c.icon_score && plain) { snprintf(c.a.icon, sizeof c.a.icon, "%sx%s/%.100s", sc == 3 ? "64" : "128", sc == 3 ? "64" : "128", f); c.icon_score = sc; }
                } else if (!strcmp(name, "category") && !strcmp(grand, "categories")) {
                    char cat[64];
                    text_of(text, lt, cat, sizeof cat - 1);
                    size_t have = strlen(c.a.categories);
                    if (cat[0] && have + strlen(cat) + 2 < sizeof c.a.categories) snprintf(c.a.categories + have, sizeof c.a.categories - have, "%s;", cat);
                } else if (!strcmp(name, "bundle") && !strcmp(grand, "component") && !strcmp(bundle_type, "flatpak")) {
                    char b[160];
                    text_of(text, lt, b, sizeof b - 1);
                    char *parts[4] = { 0 };
                    int k = 0;
                    for (char *s = b, *tok; k < 4 && (tok = strsep(&s, "/")); ) parts[k++] = tok;
                    if (k == 4 && !strcmp(parts[0], "app")) {
                        snprintf(c.bundle_id, sizeof c.bundle_id, "%s", parts[1]);
                        snprintf(c.bundle_arch, sizeof c.bundle_arch, "%s", parts[2]);
                        snprintf(c.bundle_branch, sizeof c.bundle_branch, "%s", parts[3]);
                    }
                } else if (!strcmp(name, "value") && !strcmp(grand, "custom") && !strcmp(value_key, "flathub::verification::verified")) {
                    char v[16];
                    text_of(text, lt, v, sizeof v - 1);
                    c.a.verified = !strcmp(v, "true");
                } else if (!strcmp(name, "project_license") && !strcmp(grand, "component")) {
                    text_of(text, lt, c.a.license, FPAKAS_LICENSE_MAX);
                } else if ((!strcmp(name, "developer_name") && !strcmp(grand, "component")) ||
                           (!strcmp(name, "name") && !strcmp(grand, "developer"))) {
                    int sc = lang_score(lang, xml_lang);       /* translated like the app's name */
                    if (sc > c.dev_score) { text_of(text, lt, c.a.developer, FPAKAS_DEV_MAX); c.dev_score = sc; }
                }
            }
            if (!strcmp(name, "component") && in_comp) {
                count++;
                if (emit(&c, arch, out, ud) < 0) return count;
                in_comp = 0;
            }
            /* pop to the matching element (a broken nesting only loses what is inside it) */
            for (int k = depth - 1; k >= 0; k--) if (!strcmp(stack[k], name)) { depth = k; break; }
            p = gt + 1;
            continue;
        }
        /* an opening (or empty) tag: its attributes */
        if (!strcmp(name, "component") && !self) {
            memset(&c, 0, sizeof c);
            char type[40];
            attr(ne, gt, "type", type, sizeof type - 1);
            c.is_app = !strcmp(type, "desktop-application") || !strcmp(type, "desktop");
            in_comp = 1;
        } else if (in_comp) {
            if (!strcmp(name, "name") || !strcmp(name, "summary") || !strcmp(name, "developer_name")) attr(ne, gt, "xml:lang", xml_lang, sizeof xml_lang - 1);
            else if (!strcmp(name, "icon")) { attr(ne, gt, "type", icon_type, sizeof icon_type - 1); attr(ne, gt, "width", icon_w, sizeof icon_w - 1); }
            else if (!strcmp(name, "value")) attr(ne, gt, "key", value_key, sizeof value_key - 1);
            else if (!strcmp(name, "bundle")) attr(ne, gt, "type", bundle_type, sizeof bundle_type - 1);
        }
        (void)parent;
        if (!self) {
            if (depth == DEPTH) return count;               /* deeper than AppStream ever is */
            snprintf(stack[depth++], NAME_LEN, "%s", name);
        }
        p = gt + 1;
    }
    return count;
}

/* printable UTF-8 of at most max bytes, no tab or control character */
static int text_ok(const char *s, size_t n, size_t max) {
    if (n > max) return 0;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + (size_t)len > n || (len == 1 && (c < 0x20 || c == 0x7f))) return 0;
        for (int k = 1; k < len; k++) if (((unsigned char)s[i + k] >> 6) != 2) return 0;
        i += (size_t)len;
    }
    return 1;
}

int fpakas_line(const char *line, size_t n, FpakasApp *a) {
    if (!line || !a || !n || n > 2048) return -1;
    if (line[n - 1] == '\n') n--;
    const char *f[9];
    size_t fl[9];
    int k = 0;
    const char *s = line, *e = line + n;
    while (k < 9) {
        const char *tab = memchr(s, '\t', (size_t)(e - s));
        f[k] = s;
        fl[k] = (size_t)((tab ? tab : e) - s);
        k++;
        if (!tab) break;
        s = tab + 1;
    }
    if (k != 9 || f[8] + fl[8] != e) return -1;            /* exactly nine fields, the last to the end */
    memset(a, 0, sizeof *a);
    if (fl[0] >= sizeof a->id || fl[1] >= sizeof a->branch) return -1;
    memcpy(a->id, f[0], fl[0]);
    memcpy(a->branch, f[1], fl[1]);
    if (!fpak_id_ok(a->id) || !fpak_branch_ok(a->branch)) return -1;
    if (!text_ok(f[2], fl[2], FPAKAS_NAME_MAX) || !fl[2] || !text_ok(f[3], fl[3], FPAKAS_SUMMARY_MAX) ||
        !text_ok(f[6], fl[6], FPAKAS_LICENSE_MAX) || !text_ok(f[7], fl[7], FPAKAS_DEV_MAX))
        return -1;
    /* icon: "64x64/<file>.png" or "128x128/<file>.png", a plain file name; or none */
    if (fl[4]) {
        if (fl[4] > FPAKAS_ICON_MAX) return -1;
        size_t dl = fl[4] > 6 && !memcmp(f[4], "64x64/", 6) ? 6 : fl[4] > 8 && !memcmp(f[4], "128x128/", 8) ? 8 : 0;
        if (!dl || fl[4] - dl < 5 || memcmp(f[4] + fl[4] - 4, ".png", 4) || f[4][dl] == '.') return -1;
        for (size_t i = dl; i < fl[4]; i++) if (!isalnum((unsigned char)f[4][i]) && !strchr("._+-", f[4][i])) return -1;
    }
    if (fl[5] > FPAKAS_CATS_MAX) return -1;
    for (size_t i = 0; i < fl[5]; i++) if (!isalnum((unsigned char)f[5][i]) && f[5][i] != ';' && f[5][i] != '-') return -1;
    if (fl[8] != 1 || (f[8][0] != '0' && f[8][0] != '1')) return -1;
    memcpy(a->name, f[2], fl[2]);
    memcpy(a->summary, f[3], fl[3]);
    memcpy(a->icon, f[4], fl[4]);
    memcpy(a->categories, f[5], fl[5]);
    memcpy(a->license, f[6], fl[6]);
    memcpy(a->developer, f[7], fl[7]);
    a->verified = f[8][0] == '1';
    return 0;
}
