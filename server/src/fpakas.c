/* fpakas.c — Flathub's AppStream XML to a compact index, and that index's lines checked again (fpakas.h). A small XML
 * reader for this one format: tags with attributes, text, entities, comments, processing instructions and CDATA
 * skipped; a stack of element names so <developer><name> is never the app's name (nor a release's <description> the
 * app's). Any input is safe: it never reads past len, and a broken document only ends the index early. */
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

/* the next character of [*sp, e) into tmp, entities decoded: its length, 0 for one not to keep (a byte that isn't
 * UTF-8, a reference to no character); *sp moved past it */
static int next_char(const char **sp, const char *e, char tmp[4]) {
    const char *s = *sp;
    unsigned char c = (unsigned char)*s;
    *sp = s + 1;
    if (c == '&') {
        const char *semi = memchr(s, ';', (size_t)(e - s) < 12 ? (size_t)(e - s) : 12);
        static const struct { const char *name; char c; } ents[] = { { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' } };
        if (semi) {
            size_t l = (size_t)(semi - s - 1);
            for (size_t k = 0; k < sizeof ents / sizeof ents[0]; k++)
                if (l == strlen(ents[k].name) && !memcmp(s + 1, ents[k].name, l)) { tmp[0] = ents[k].c; *sp = semi + 1; return 1; }
            if (l > 1 && s[1] == '#') {
                char num[12];
                memcpy(num, s + 2, l - 1);
                num[l - 1] = 0;
                char *endp;
                unsigned long cp = (num[0] == 'x' || num[0] == 'X') ? strtoul(num + 1, &endp, 16) : strtoul(num, &endp, 10);
                if (*endp == 0 && endp != num) { *sp = semi + 1; return put_cp(cp, tmp); }
            }
        }
        tmp[0] = '&';
        return 1;
    }
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!len || s + len > e) return 0;
    for (int k = 1; k < len; k++) if (((unsigned char)s[k] >> 6) != 2) return 0;
    memcpy(tmp, s, (size_t)len);
    *sp = s + len;
    return len;
}

/* text [s, e) with entities decoded, control characters and tabs as spaces, spaces collapsed and trimmed, valid UTF-8
 * only, cut at max bytes on a character boundary */
static void text_of(const char *s, const char *e, char *out, size_t max) {
    size_t o = 0;
    int space = 0;
    char tmp[4];
    while (s < e && o < max) {
        int n = next_char(&s, e, tmp);
        if (!n) continue;
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
    int name_score, summary_score, icon_score, dev_score, desc_score, is_app;
    char bundle_id[64], bundle_arch[24], bundle_branch[64];
    char desc[FPAKAS_DESC_MAX + 1];
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

/* ---- the description: AppStream's little markup (<p>, <ul>/<ol> of <li>, inline <em> and <code>) to plain text ---- */
typedef struct {
    char *out;
    size_t o, max;                      /* max: room left for the "…" of a cut text */
    int space, brk, cut;                /* brk: the break owed before the next text, 1 a line, 2 a paragraph */
    char prefix[16];                    /* "• " or "3. ", written before an item's first character */
} Desc;

static void desc_put(Desc *d, const char *s, size_t n) {
    if (d->cut) return;
    if (d->o + n > d->max) { d->cut = 1; return; }
    memcpy(d->out + d->o, s, n);
    d->o += n;
}

static void desc_char(Desc *d, const char *ch, int n) {
    if (n == 1 && ((unsigned char)ch[0] < 0x20 || ch[0] == 0x7f || ch[0] == ' ')) { d->space = 1; return; }
    if (d->brk && d->o) desc_put(d, "\n\n", (size_t)d->brk);
    if (d->prefix[0]) desc_put(d, d->prefix, strlen(d->prefix));
    else if (d->space && d->o && !d->brk && d->out[d->o - 1] != '\n') desc_put(d, " ", 1);
    d->brk = d->space = 0;
    d->prefix[0] = 0;
    desc_put(d, ch, (size_t)n);
}

/* the markup [s, e) into d; a <p> or <li> whose xml:lang scores under min is left out, with all inside it */
static void desc_walk(const char *s, const char *e, const char *lang, int min, Desc *d) {
    int skip = 0, ordered = 0, item = 0, last_li = 0;
    char tmp[4];
    while (s < e && !d->cut) {
        if (*s != '<') {
            int n = next_char(&s, e, tmp);
            if (n && !skip) desc_char(d, tmp, n);
            continue;
        }
        if ((size_t)(e - s) >= 4 && !memcmp(s, "<!--", 4)) { const char *q = find(s + 4, e, "-->"); if (!q) return; s = q + 3; continue; }
        if ((size_t)(e - s) >= 9 && !memcmp(s, "<![CDATA[", 9)) { const char *q = find(s + 9, e, "]]>"); if (!q) return; s = q + 3; continue; }
        const char *gt = memchr(s, '>', (size_t)(e - s));
        if (!gt) return;
        if (s[1] == '?' || s[1] == '!') { s = gt + 1; continue; }
        int closing = s[1] == '/', self = gt[-1] == '/' && gt - 1 > s;
        const char *ns = s + 1 + closing, *ne = ns;
        while (ne < gt && !isspace((unsigned char)*ne) && *ne != '/') ne++;
        char name[NAME_LEN];
        size_t nl = (size_t)(ne - ns);
        s = gt + 1;
        if (!nl || nl >= sizeof name) continue;
        memcpy(name, ns, nl);
        name[nl] = 0;
        if (skip) { if (closing) skip--; else if (!self) skip++; continue; }
        int block = !strcmp(name, "p"), li = !strcmp(name, "li"), list = !strcmp(name, "ul") || !strcmp(name, "ol");
        if (closing) {
            if (list || block) { d->brk = 2; last_li = 0; d->prefix[0] = 0; }
            continue;
        }
        if (block || li) {
            char l[24];
            attr(ne, gt, "xml:lang", l, sizeof l - 1);
            if (lang_score(lang, l) < min) { if (!self) skip = 1; continue; }
        }
        if (block || list) { d->brk = 2; d->prefix[0] = 0; last_li = 0; if (list) { ordered = name[0] == 'o'; item = 0; } }
        else if (li) {
            d->brk = last_li ? 1 : 2;
            last_li = 1;
            if (ordered) snprintf(d->prefix, sizeof d->prefix, "%d. ", ++item < 1000 ? item : 999);
            else snprintf(d->prefix, sizeof d->prefix, "\xe2\x80\xa2 ");
        }
    }
}

/* the description [s, e) as plain text in out (FPAKAS_DESC_MAX bytes at most): paragraphs apart by an empty line,
 * items on their own line after "• " (or their number), spaces collapsed, cut at a word with "…" when longer */
static void desc_of(const char *s, const char *e, const char *lang, int score, char *out) {
    Desc d = { .out = out, .max = FPAKAS_DESC_MAX - 3 };
    /* an untranslated description that carries its translations per paragraph: the person's paragraphs if any */
    if (score == 1 && find(s, e, "xml:lang")) desc_walk(s, e, lang, 2, &d);
    if (!d.o) { memset(&d, 0, sizeof d); d.out = out; d.max = FPAKAS_DESC_MAX - 3; desc_walk(s, e, lang, 1, &d); }
    if (d.cut) {
        size_t o = d.o;
        while (o > d.o / 2 && out[o - 1] != ' ' && out[o - 1] != '\n') o--;
        if (o > d.o / 2) d.o = o;
        for (;;) {
            while (d.o && (out[d.o - 1] == ' ' || out[d.o - 1] == '\n')) d.o--;
            if (d.o >= 3 && !memcmp(out + d.o - 3, "\xe2\x80\xa2", 3)) { d.o -= 3; continue; }
            break;
        }
        if (d.o) { memcpy(out + d.o, "\xe2\x80\xa6", 3); d.o += 3; }
    }
    out[d.o] = 0;
}

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
    if (out(ud, line, (size_t)n)) return -1;
    if (!c->desc[0]) return 0;
    /* its description on the next line: "#d", the id, the text with "\" and the line break written as "\\" and "\n" */
    char dl[FPAKAS_DESC_LINE_MAX];
    size_t o = (size_t)snprintf(dl, sizeof dl, "#d\t%s\t", c->a.id);
    for (const char *q = c->desc; *q && o + 3 < sizeof dl; q++) {
        if (*q == '\n') { dl[o++] = '\\'; dl[o++] = 'n'; }
        else if (*q == '\\') { dl[o++] = '\\'; dl[o++] = '\\'; }
        else if ((unsigned char)*q >= 0x20 && *q != 0x7f) dl[o++] = *q;
    }
    dl[o++] = '\n';
    return out(ud, dl, o) ? -1 : 0;
}

int fpakas_desc_line(const char *line, size_t n, char id[64], char *text) {
    if (!line || !id || !text || n < 4 || n > FPAKAS_DESC_LINE_MAX || memcmp(line, "#d\t", 3)) return -1;
    if (line[n - 1] == '\n') n--;
    const char *s = line + 3, *e = line + n;
    const char *tab = memchr(s, '\t', (size_t)(e - s));
    if (!tab || (size_t)(tab - s) >= 64) return -1;
    memcpy(id, s, (size_t)(tab - s));
    id[tab - s] = 0;
    if (!fpak_id_ok(id)) return -1;
    size_t o = 0;
    for (s = tab + 1; s < e; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7f) return -1;                /* a tab, a raw line break, any control character */
        if (c == '\\') {
            if (++s >= e || (*s != 'n' && *s != '\\')) return -1;
            c = *s == 'n' ? '\n' : '\\';
        }
        if (o >= FPAKAS_DESC_MAX) return -1;
        text[o++] = (char)c;
    }
    text[o] = 0;
    if (!o || text[0] == '\n' || text[o - 1] == '\n' || strstr(text, "\n\n\n")) return -1;
    for (size_t i = 0; i < o;) {                                /* valid UTF-8, its only control the line break */
        unsigned char c = (unsigned char)text[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + (size_t)len > o) return -1;
        for (int k = 1; k < len; k++) if (((unsigned char)text[i + k] >> 6) != 2) return -1;
        i += (size_t)len;
    }
    return (int)o;
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
    const char *desc_at = NULL;                             /* where the component's <description> being read starts */
    int desc_sc = 0;
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
                } else if (!strcmp(name, "description") && !strcmp(grand, "component") && desc_at && desc_at <= lt) {
                    if (desc_sc > c.desc_score) { desc_of(desc_at, lt, lang, desc_sc, c.desc); c.desc_score = desc_sc; }
                    desc_at = NULL;
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
            desc_at = NULL;
        } else if (in_comp) {
            if (!strcmp(name, "description") && !strcmp(parent, "component") && !self) {
                char l[24];
                attr(ne, gt, "xml:lang", l, sizeof l - 1);
                desc_sc = lang_score(lang, l);
                desc_at = gt + 1;
            } else if (!strcmp(name, "name") || !strcmp(name, "summary") || !strcmp(name, "developer_name")) attr(ne, gt, "xml:lang", xml_lang, sizeof xml_lang - 1);
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
