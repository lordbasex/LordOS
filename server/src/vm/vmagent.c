/* vmagent.c — org.lordos.agent frames, UTF-8, base64, and what lord-vm checks of a clipboard the other side sent (see
 * vmagent.h).
 *
 * HTML (6.8): the guest is untrusted, and the HTML goes on to apps on this computer that render it. What stays:
 *   - text, as written (a '<' that doesn't start a tag becomes &lt;);
 *   - the tags of TAG_OK, with the attributes of attr_ok: href on <a> only when it starts with http://, https://,
 *     mailto: or '#'; title; colspan, rowspan, start, size as digits; align, valign, color and face as plain words;
 *     style only with the properties of STYLE_OK and values of letters, digits, '#', '%', '.', ',', '(', ')', '-'
 *     and spaces, never "url", "expression", "import" or a backslash. Every value is re-quoted and escaped;
 *   - nothing of the tags of DROP_ALL (script, style, iframe, object, svg, math, template, …): tag and content go;
 *   - other tags (html, body, meta, link, img, form, input, …) go, their content stays; comments,
 *     <!DOCTYPE> and <?…> go. */
#include "vmagent.h"

#include <stdio.h>
#include <string.h>

int ag_frame_next(const uint8_t *b, size_t n, AgFrame *f) {
    if (n < AG_HEAD) return 0;
    uint32_t len = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    if (len > AG_FRAME_MAX) return -1;
    if (n < AG_HEAD + (size_t)len) return 0;
    f->kind = (uint16_t)(b[4] | b[5] << 8);
    f->chan = (uint16_t)(b[6] | b[7] << 8);
    f->data = b + AG_HEAD;
    f->len = len;
    return AG_HEAD + (int)len;
}

void ag_frame_head(uint8_t o[AG_HEAD], uint16_t kind, uint16_t chan, uint32_t len) {
    o[0] = (uint8_t)len; o[1] = (uint8_t)(len >> 8); o[2] = (uint8_t)(len >> 16); o[3] = (uint8_t)(len >> 24);
    o[4] = (uint8_t)kind; o[5] = (uint8_t)(kind >> 8); o[6] = (uint8_t)chan; o[7] = (uint8_t)(chan >> 8);
}

int ag_utf8_ok(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t c = p[i];
        if (!c) return 0;
        int k = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        if (k < 0 || (k == 1 && c < 0xc2) || (k == 3 && c > 0xf4) || i + (size_t)k >= n) return 0;
        uint32_t cp = k == 0 ? c : k == 1 ? c & 0x1f : k == 2 ? c & 0x0f : c & 0x07;
        for (int j = 1; j <= k; j++) {
            if (i + (size_t)j >= n || (p[i + j] & 0xc0) != 0x80) return 0;
            cp = cp << 6 | (p[i + j] & 0x3f);
        }
        if ((k == 2 && cp < 0x800) || (k == 3 && cp < 0x10000) || (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) return 0;
        i += (size_t)k + 1;
    }
    return 1;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

long ag_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    size_t need = (n + 2) / 3 * 4;
    if (need + 1 > cap) return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = B64[v >> 18 & 63];
        out[o++] = B64[v >> 12 & 63];
        out[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = 0;
    return (long)o;
}

long ag_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap) {
    uint32_t v = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == '=' ) break;
        if (c == '\n' || c == '\r') continue;
        const char *p = c ? strchr(B64, c) : NULL;
        if (!p) return -1;
        v = v << 6 | (uint32_t)(p - B64);
        bits += 6;
        if (bits >= 8) { bits -= 8; if (o >= cap) return -1; out[o++] = (uint8_t)(v >> bits); }
    }
    return (long)o;
}

/* ---- AGK_CLIPX ---- */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int ag_clipx_parse(const uint8_t *p, size_t n, AgClip *c) {
    memset(c, 0, sizeof *c);
    size_t o = 0;
    int parts = 0;
    while (o < n) {
        if (n - o < 5) return -1;
        uint8_t form = p[o];
        uint32_t len = le32(p + o + 1);
        o += 5;
        if (len > n - o) return -1;
        const uint8_t *d = p + o;
        if (form == AGF_TEXT) { if (c->text || len > AG_TEXT_MAX) return -1; c->text = d; c->text_n = len; }
        else if (form == AGF_HTML) { if (c->html || len > AG_TEXT_MAX) return -1; c->html = d; c->html_n = len; }
        else if (form == AGF_PNG) { if (c->png || len > AG_PNG_MAX) return -1; c->png = d; c->png_n = len; }
        else return -1;
        if (!len) return -1;
        o += len;
        parts++;
    }
    return parts ? 0 : -1;
}

size_t ag_clipx_size(const AgClip *c) {
    return (c->text ? 5 + (size_t)c->text_n : 0) + (c->html ? 5 + (size_t)c->html_n : 0) + (c->png ? 5 + (size_t)c->png_n : 0);
}

static uint8_t *put_part(uint8_t *o, uint8_t form, const uint8_t *d, uint32_t n) {
    o[0] = form; o[1] = (uint8_t)n; o[2] = (uint8_t)(n >> 8); o[3] = (uint8_t)(n >> 16); o[4] = (uint8_t)(n >> 24);
    memcpy(o + 5, d, n);
    return o + 5 + n;
}

void ag_clipx_write(const AgClip *c, uint8_t *out) {
    if (c->text) out = put_part(out, AGF_TEXT, c->text, c->text_n);
    if (c->html) out = put_part(out, AGF_HTML, c->html, c->html_n);
    if (c->png) out = put_part(out, AGF_PNG, c->png, c->png_n);
}

int ag_png_check(const uint8_t *p, size_t n, uint32_t *w, uint32_t *h) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    if (n < 33 || n > AG_PNG_MAX || memcmp(p, sig, 8)) return -1;
    uint32_t len = (uint32_t)p[8] << 24 | (uint32_t)p[9] << 16 | (uint32_t)p[10] << 8 | p[11];
    if (len != 13 || memcmp(p + 12, "IHDR", 4)) return -1;
    uint32_t W = (uint32_t)p[16] << 24 | (uint32_t)p[17] << 16 | (uint32_t)p[18] << 8 | p[19];
    uint32_t H = (uint32_t)p[20] << 24 | (uint32_t)p[21] << 16 | (uint32_t)p[22] << 8 | p[23];
    uint8_t depth = p[24], color = p[25];
    if (!W || !H || W > AG_PNG_DIM || H > AG_PNG_DIM) return -1;
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return -1;
    if (color != 0 && color != 2 && color != 3 && color != 4 && color != 6) return -1;
    if (p[26] || p[27] || p[28] > 1) return -1;       /* compression, filter, interlace */
    if (w) *w = W;
    if (h) *h = H;
    return 0;
}

/* ---- HTML ---- */

static const char *const TAG_OK[] = {
    "a", "b", "i", "u", "s", "em", "strong", "small", "big", "mark", "sub", "sup", "br", "hr", "p", "div", "span",
    "blockquote", "pre", "code", "kbd", "samp", "var", "q", "cite", "abbr", "dfn", "h1", "h2", "h3", "h4", "h5", "h6",
    "ul", "ol", "li", "dl", "dt", "dd", "table", "thead", "tbody", "tfoot", "tr", "td", "th", "caption", "colgroup", "col",
    "font", "center", "del", "ins", "strike", "tt", "address", "figure", "figcaption", "section", "article", "header",
    "footer", "main", "nav", "aside", "details", "summary", "time", "wbr", NULL };
static const char *const DROP_ALL[] = {
    "script", "style", "iframe", "frame", "frameset", "object", "embed", "applet", "noscript", "noembed", "noframes",
    "template", "svg", "math", "head", "title", "textarea", "select", "option", "xmp", "plaintext", "audio", "video",
    "canvas", "map", "base", "button", NULL };
static const char *const STYLE_OK[] = {
    "color", "background-color", "font-weight", "font-style", "font-size", "font-family", "text-decoration",
    "text-align", "vertical-align", "white-space", "text-indent", NULL };

static int in_list(const char *const *l, const char *s) { for (int i = 0; l[i]; i++) if (!strcmp(l[i], s)) return 1; return 0; }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }
static int name_ch(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':'; }
static int space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

typedef struct { char *o; size_t n, cap; int over; } Out;
static void put(Out *o, const char *s, size_t n) {
    if (o->over || o->n + n + 1 > o->cap) { o->over = 1; return; }
    memcpy(o->o + o->n, s, n);
    o->n += n;
}
static void puts_(Out *o, const char *s) { put(o, s, strlen(s)); }
static void put_escaped(Out *o, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '"') puts_(o, "&quot;");
        else if (c == '<') puts_(o, "&lt;");
        else if (c == '>') puts_(o, "&gt;");
        else put(o, &c, 1);
    }
}

static int digits(const char *v, size_t n) { if (!n || n > 6) return 0; for (size_t i = 0; i < n; i++) if (v[i] < '0' || v[i] > '9') return 0; return 1; }
static int word(const char *v, size_t n) {
    if (!n || n > 64) return 0;
    for (size_t i = 0; i < n; i++) { char c = v[i]; if (!(name_ch(c) || c == '#' || c == ' ' || c == ',' || c == '.')) return 0; }
    return 1;
}
static int ci_prefix(const char *v, size_t n, const char *p) {
    size_t l = strlen(p);
    if (n < l) return 0;
    for (size_t i = 0; i < l; i++) if (lower(v[i]) != p[i]) return 0;
    return 1;
}
static int href_ok(const char *v, size_t n) {
    if (!n || n > 2048) return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)v[i] < 0x20 || v[i] == 0x7f || v[i] == '\\') return 0;
    if (v[0] == '#') return 1;
    return ci_prefix(v, n, "http://") || ci_prefix(v, n, "https://") || ci_prefix(v, n, "mailto:");
}
/* style="a: b; c: d": only STYLE_OK properties, plain values; rebuilt */
static void style_put(Out *o, const char *v, size_t n) {
    char buf[1024];
    size_t bn = 0;
    size_t i = 0;
    while (i < n) {
        size_t s0 = i;
        while (i < n && v[i] != ';') i++;
        const char *d = v + s0;
        size_t dn = i - s0;
        if (i < n) i++;
        const char *colon = memchr(d, ':', dn);
        if (!colon) continue;
        char prop[32];
        size_t pn = 0;
        for (const char *q = d; q < colon && pn + 1 < sizeof prop; q++) if (!space(*q)) prop[pn++] = lower(*q);
        prop[pn] = 0;
        if (!in_list(STYLE_OK, prop)) continue;
        const char *val = colon + 1, *ve = d + dn;
        while (val < ve && space(*val)) val++;
        while (ve > val && space(ve[-1])) ve--;
        size_t vn = (size_t)(ve - val);
        if (!vn || vn > 128) continue;
        int good = 1;
        for (size_t k = 0; k < vn && good; k++) {
            char c = val[k];
            good = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '#' || c == '%' || c == '.' ||
                   c == ',' || c == '(' || c == ')' || c == '-' || c == ' ' || c == '\'';
        }
        char low[130];
        for (size_t k = 0; k < vn; k++) low[k] = lower(val[k]);
        low[vn] = 0;
        if (!good || strstr(low, "url") || strstr(low, "expression") || strstr(low, "import")) continue;
        int w = snprintf(buf + bn, sizeof buf - bn, "%s%s: %.*s", bn ? "; " : "", prop, (int)vn, val);
        if (w < 0 || (size_t)w >= sizeof buf - bn) break;
        bn += (size_t)w;
    }
    if (!bn) return;
    puts_(o, " style=\"");
    put_escaped(o, buf, bn);
    puts_(o, "\"");
}

static void attr_put(Out *o, const char *tag, const char *name, const char *v, size_t n) {
    int ok = 0;
    if (!strcmp(name, "href")) ok = !strcmp(tag, "a") && href_ok(v, n);
    else if (!strcmp(name, "title")) ok = n <= 512;
    else if (!strcmp(name, "colspan") || !strcmp(name, "rowspan") || !strcmp(name, "start") || !strcmp(name, "size")) ok = digits(v, n);
    else if (!strcmp(name, "align") || !strcmp(name, "valign") || !strcmp(name, "color") || !strcmp(name, "face")) ok = word(v, n);
    else if (!strcmp(name, "style")) { style_put(o, v, n); return; }
    if (!ok) return;
    put(o, " ", 1);
    puts_(o, name);
    puts_(o, "=\"");
    put_escaped(o, v, n);
    puts_(o, "\"");
}

/* the end of a tag that started at p (after '<'): the index after its '>' (quotes respected), or 0 if there's none */
static size_t tag_end(const char *s, size_t i, size_t n) {
    char q = 0;
    for (; i < n; i++) {
        if (q) { if (s[i] == q) q = 0; }
        else if (s[i] == '"' || s[i] == '\'') q = s[i];
        else if (s[i] == '>') return i + 1;
    }
    return 0;
}

/* skips to after </name> (case-insensitive), or to the end */
static size_t skip_to_close(const char *s, size_t i, size_t n, const char *name) {
    size_t l = strlen(name);
    for (; i + 2 + l <= n; i++) {
        if (s[i] != '<' || s[i + 1] != '/') continue;
        size_t k = 0;
        while (k < l && lower(s[i + 2 + k]) == name[k]) k++;
        if (k == l && (i + 2 + l == n || !name_ch(s[i + 2 + l]))) { size_t e = tag_end(s, i + 2 + l, n); return e ? e : n; }
    }
    return n;
}

long ag_html_sanitize(const char *s, size_t n, char *out, size_t cap) {
    if (!cap) return -1;
    Out o = { out, 0, cap, 0 };
    size_t i = 0;
    while (i < n && !o.over) {
        char c = s[i];
        if (c != '<') {
            if (c == '>') puts_(&o, "&gt;");
            else if (c) put(&o, &c, 1);
            i++;
            continue;
        }
        /* comments, <!DOCTYPE …>, <?…>: gone */
        if (i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
            const char *e = NULL;
            for (size_t k = i + 4; k + 2 < n; k++) if (s[k] == '-' && s[k + 1] == '-' && s[k + 2] == '>') { e = s + k + 3; break; }
            i = e ? (size_t)(e - s) : n;
            continue;
        }
        if (i + 1 < n && (s[i + 1] == '!' || s[i + 1] == '?')) { size_t e = tag_end(s, i + 1, n); i = e ? e : n; continue; }
        int closing = i + 1 < n && s[i + 1] == '/';
        size_t k = i + 1 + (size_t)closing;
        char name[16];
        size_t nl = 0;
        int too_long = 0;
        while (k < n && name_ch(s[k])) { if (nl + 1 < sizeof name) name[nl++] = lower(s[k]); else too_long = 1; k++; }
        name[nl] = 0;
        size_t e = nl ? tag_end(s, k, n) : 0;
        if (!nl || !e || !((name[0] >= 'a' && name[0] <= 'z'))) { puts_(&o, "&lt;"); i++; continue; }   /* not a tag: text */
        if (too_long) { i = e; continue; }
        if (in_list(DROP_ALL, name)) {
            int self = e >= 2 && s[e - 2] == '/';
            i = closing || self ? e : skip_to_close(s, e, n, name);
            continue;
        }
        if (!in_list(TAG_OK, name)) { i = e; continue; }
        if (closing) { puts_(&o, "</"); puts_(&o, name); puts_(&o, ">"); i = e; continue; }
        puts_(&o, "<");
        puts_(&o, name);
        /* attributes: name, optionally = value (quoted or not) */
        size_t a = k, end = e - 1;
        while (a < end) {
            while (a < end && (space(s[a]) || s[a] == '/')) a++;
            if (a >= end) break;
            char an[16];
            size_t anl = 0;
            int bad = 0;
            while (a < end && !space(s[a]) && s[a] != '=' && s[a] != '/') { if (anl + 1 < sizeof an && name_ch(s[a])) an[anl++] = lower(s[a]); else bad = 1; a++; }
            an[anl] = 0;
            while (a < end && space(s[a])) a++;
            const char *v = "";
            size_t vn = 0;
            if (a < end && s[a] == '=') {
                a++;
                while (a < end && space(s[a])) a++;
                if (a < end && (s[a] == '"' || s[a] == '\'')) {
                    char q = s[a++];
                    v = s + a;
                    while (a < end && s[a] != q) a++;
                    vn = (size_t)(s + a - v);
                    if (a < end) a++;
                } else {
                    v = s + a;
                    while (a < end && !space(s[a])) a++;
                    vn = (size_t)(s + a - v);
                }
            }
            if (!bad && anl) attr_put(&o, name, an, v, vn);
        }
        puts_(&o, ">");
        i = e;
    }
    if (o.over) return -1;
    out[o.n] = 0;
    return (long)o.n;
}
