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
#include <strings.h>

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

/* ---- files dropped on the machine's window ---- */

static int dos_device(const char *s, size_t n) {
    static const char *const DEV[] = { "con", "prn", "aux", "nul", NULL };
    size_t b = 0;
    while (b < n && s[b] != '.') b++;
    while (b > 0 && s[b - 1] == ' ') b--;                    /* "CON .txt" is CON too */
    char l[5];
    if (b != 3 && b != 4) return 0;
    for (size_t i = 0; i < b; i++) l[i] = lower(s[i]);
    l[b] = 0;
    if (b == 3) return in_list(DEV, l);
    return (!memcmp(l, "com", 3) || !memcmp(l, "lpt", 3)) && l[3] >= '1' && l[3] <= '9';
}

/* the start of the UTF-8 character that holds byte i (or i itself) */
static size_t char_start(const char *s, size_t i) {
    while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80) i--;
    return i;
}

int ag_drop_name(const char *in, char *out, size_t cap) {
    if (!in || !out || cap < 2) return -1;
    const char *s = strrchr(in, '/');
    s = s ? s + 1 : in;
    size_t n = strlen(s);
    if (!n || n > 4096 || !ag_utf8_ok((const uint8_t *)s, n)) return -1;
    char t[4097];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        t[k++] = c < 0x20 || c == 0x7f || strchr("\\:*?\"<>|", c) ? '_' : (char)c;
    }
    size_t a = 0;
    while (a < k && t[a] == ' ') a++;
    while (k > a && (t[k - 1] == ' ' || t[k - 1] == '.')) k--;
    if (k == a) return -1;                                  /* nothing, ".", "..", "  " */
    const char *u = t + a;
    size_t un = k - a;
    int dev = dos_device(u, un);
    size_t lim = AG_NAME_MAX < cap - 1 ? AG_NAME_MAX : cap - 1;
    if (dev) lim--;
    size_t o = 0;
    if (dev) out[o++] = '_';
    if (un <= lim) { memcpy(out + o, u, un); o += un; }
    else {
        /* too long: the start, cut on a character, then the extension when it's short */
        const char *dot = NULL;
        for (size_t i = un; i > 1; i--) if (u[i - 1] == '.') { dot = u + i - 1; break; }
        size_t en = dot && (size_t)(u + un - dot) <= 16 ? (size_t)(u + un - dot) : 0;
        size_t head = char_start(u, lim - en);
        while (head > 0 && u[head - 1] == ' ') head--;
        if (!head) return -1;
        memcpy(out + o, u, head); o += head;
        memcpy(out + o, u + un - en, en); o += en;
    }
    out[o] = 0;
    return 0;
}

void ag_exec_text(const char *b64, char *out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    if (!b64) return;
    uint8_t buf[2048];
    size_t bl = strnlen(b64, 2700);
    long n = ag_b64_decode(b64, bl, buf, sizeof buf);
    if (n <= 0) return;
    for (long i = 0; i < n; i++) if (buf[i] < 0x20 || buf[i] == 0x7f) buf[i] = ' ';
    if (!ag_utf8_ok(buf, (size_t)n)) for (long i = 0; i < n; i++) if (buf[i] >= 0x80) buf[i] = '?';
    long s = 0, e = n;
    while (s < e && buf[s] == ' ') s++;
    while (e > s && buf[e - 1] == ' ') e--;
    size_t l = (size_t)(e - s);
    if (l > cap - 1) l = char_start((const char *)buf + s, cap - 1);
    memcpy(out, buf + s, l);
    out[l] = 0;
}

int ag_drop_staging_ok(const char *p, int windows) {
    if (!p) return 0;
    size_t l = strlen(p);
    if (!windows) {
        static const char pre[] = "/var/tmp/lordos-drop.";
        size_t pl = sizeof pre - 1;
        if (l != pl + 8 || strncmp(p, pre, pl)) return 0;
        for (size_t i = pl; i < l; i++) if (!((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9'))) return 0;
        return 1;
    }
    static const char tail[] = "\\Temp\\lordos-drop-";
    size_t tl = sizeof tail - 1;
    if (l < 3 + tl + 32 || l > 200 || !((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) || p[1] != ':' || p[2] != '\\') return 0;
    for (size_t i = 0; i < l; i++) if (p[i] < 0x20 || p[i] > 0x7e || p[i] == '/' || (p[i] == '.' && p[i + 1] == '.')) return 0;
    const char *t = p + l - 32 - tl;
    if (strncasecmp(t, tail, tl)) return 0;
    for (size_t i = l - 32; i < l; i++) if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return 0;
    return 1;
}

/* ---- the Windows guest tools: the display's size, apps mode (vmagent.h) ---- */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static int32_t lei32(const uint8_t *p) { return (int32_t)le32(p); }

int ag_display_parse(const uint8_t *p, size_t n, uint32_t *w, uint32_t *h) {
    if (!p || n != 8) return -1;
    uint32_t W = le32(p), H = le32(p + 4);
    if (W < AG_DISPLAY_MIN_W || H < AG_DISPLAY_MIN_H || W > AG_DISPLAY_MAX || H > AG_DISPLAY_MAX) return -1;
    if (w) *w = W;
    if (h) *h = H;
    return 0;
}

void ag_display_write(uint8_t o[8], uint32_t w, uint32_t h) {
    for (int i = 0; i < 4; i++) { o[i] = (uint8_t)(w >> (8 * i)); o[4 + i] = (uint8_t)(h >> (8 * i)); }
}

/* AGK_DISPLAYS: the guest's displays, for lord-vm's log */
static int ag_disp_ok(int32_t x, int32_t y, uint32_t w, uint32_t h, uint8_t flags, const uint8_t *name, size_t nl) {
    if (x < -LAG_DIM_MAX || x > LAG_DIM_MAX || y < -LAG_DIM_MAX || y > LAG_DIM_MAX) return 0;
    if (w < 1 || h < 1 || w > AG_DISPLAY_MAX || h > AG_DISPLAY_MAX || (flags & ~AGDS_ALL)) return 0;
    if (nl < 1 || nl > AG_DISPLAY_NAME_MAX) return 0;
    for (size_t i = 0; i < nl; i++) if (name[i] < 0x20 || name[i] > 0x7e) return 0;
    return 1;
}

int ag_displays_parse(const uint8_t *p, size_t n, AgDisplay out[AG_DISPLAYS_MAX], int *shown) {
    if (!p || !out || n < 2 || n > AG_DISPLAYS_BYTES) return -1;
    unsigned cnt = p[0], sh = p[1];
    if (cnt < 1 || cnt > AG_DISPLAYS_MAX || (sh != AG_DISPLAYS_NONE && sh >= cnt)) return -1;
    size_t o = 2;
    int primaries = 0;
    for (unsigned i = 0; i < cnt; i++) {
        if (n - o < 18) return -1;
        int32_t x = lei32(p + o), y = lei32(p + o + 4);
        uint32_t w = le32(p + o + 8), h = le32(p + o + 12);
        uint8_t fl = p[o + 16];
        size_t nl = p[o + 17];
        o += 18;
        if (n - o < nl || !ag_disp_ok(x, y, w, h, fl, p + o, nl)) return -1;
        if ((fl & AGDS_PRIMARY) && ++primaries > 1) return -1;
        out[i].x = x; out[i].y = y; out[i].w = w; out[i].h = h; out[i].flags = fl;
        memcpy(out[i].name, p + o, nl);
        out[i].name[nl] = 0;
        o += nl;
    }
    if (o != n) return -1;
    if (shown) *shown = sh == AG_DISPLAYS_NONE ? -1 : (int)sh;
    return (int)cnt;
}

static void ag_put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

size_t ag_displays_write(const AgDisplay *d, int n, int shown, uint8_t *out, size_t cap) {
    if (!d || !out || n < 1 || n > AG_DISPLAYS_MAX || shown < -1 || shown >= n || cap < 2) return 0;
    size_t o = 2;
    int primaries = 0;
    out[0] = (uint8_t)n;
    out[1] = shown < 0 ? (uint8_t)AG_DISPLAYS_NONE : (uint8_t)shown;
    for (int i = 0; i < n; i++) {
        size_t nl = strnlen(d[i].name, AG_DISPLAY_NAME_MAX + 1);
        if (!ag_disp_ok(d[i].x, d[i].y, d[i].w, d[i].h, d[i].flags, (const uint8_t *)d[i].name, nl)) return 0;
        if ((d[i].flags & AGDS_PRIMARY) && ++primaries > 1) return 0;
        if (cap - o < 18 + nl) return 0;
        ag_put32(out + o, (uint32_t)d[i].x); ag_put32(out + o + 4, (uint32_t)d[i].y);
        ag_put32(out + o + 8, d[i].w); ag_put32(out + o + 12, d[i].h);
        out[o + 16] = d[i].flags;
        out[o + 17] = (uint8_t)nl;
        memcpy(out + o + 18, d[i].name, nl);
        o += 18 + nl;
    }
    return o;
}

static int ag_size_is(const AgDisplay *d, uint32_t w, uint32_t h) {
    return w && h && d->w <= w && d->h <= h && w - d->w < 8 && h - d->h < 8;
}

int ag_shown_pick(const AgDisplay *d, int n, uint32_t want_w, uint32_t want_h, int locked, int *lock) {
    if (lock) *lock = -1;
    if (!d || n < 1) return -1;
    int anyhw = 0;
    for (int i = 0; i < n; i++) anyhw |= (d[i].flags & AGDS_HW) != 0;
#define AG_CAND(i) (!anyhw || (d[i].flags & AGDS_HW))
    if (locked >= 0 && locked < n && AG_CAND(locked)) { if (lock) *lock = locked; return locked; }
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (!AG_CAND(i) || !ag_size_is(&d[i], want_w, want_h)) continue;
        if (best < 0 || ((d[i].flags & AGDS_PRIMARY) && !(d[best].flags & AGDS_PRIMARY))) best = i;
    }
    if (best >= 0) { if (lock) *lock = best; return best; }
    for (int i = 0; i < n; i++) if (AG_CAND(i) && (d[i].flags & AGDS_PRIMARY)) return i;
    for (int i = 0; i < n; i++) if (AG_CAND(i)) return i;
#undef AG_CAND
    return 0;
}

int ag_displays_layout(const AgDisplay *d, int n, int shown, int L[6]) {
    for (int i = 0; i < 6; i++) L[i] = 0;
    if (!d || n < 2 || n > AG_DISPLAYS_MAX || shown < 0 || shown >= n) return 1;
    long long x0 = d[0].x, y0 = d[0].y, x1 = (long long)d[0].x + d[0].w, y1 = (long long)d[0].y + d[0].h;
    for (int i = 1; i < n; i++) {
        if (d[i].x < x0) x0 = d[i].x;
        if (d[i].y < y0) y0 = d[i].y;
        if ((long long)d[i].x + d[i].w > x1) x1 = (long long)d[i].x + d[i].w;
        if ((long long)d[i].y + d[i].h > y1) y1 = (long long)d[i].y + d[i].h;
    }
    long long w = x1 - x0, h = y1 - y0;
    if (w > 32767 || h > 32767 || (w == d[shown].w && h == d[shown].h)) return 1;
    L[0] = (int)(d[shown].x - x0); L[1] = (int)(d[shown].y - y0);
    L[2] = (int)d[shown].w; L[3] = (int)d[shown].h;
    L[4] = (int)w; L[5] = (int)h;
    return 0;
}

int ag_tablet_parse(const uint8_t *p, size_t n, int *mode) {
    if (!p || n != 1 || (p[0] != AG_TABLET_PRIMARY && p[0] != AG_TABLET_DESKTOP)) return -1;
    if (mode) *mode = p[0];
    return 0;
}

int ag_tablet_layout(const AgDisplay *d, int n, int shown, int mode, int L[6]) {
    if (mode == AG_TABLET_DESKTOP) return ag_displays_layout(d, n, shown, L);
    for (int i = 0; i < 6; i++) L[i] = 0;
    if (!d || n < 2 || n > AG_DISPLAYS_MAX || shown < 0 || shown >= n) return 1;
    int primary = -1;
    for (int i = 0; i < n; i++) if (d[i].flags & AGDS_PRIMARY) { primary = i; break; }
    /* no primary said (never from Windows): Windows' primary is the display at 0,0 */
    if (primary < 0) for (int i = 0; i < n; i++) if (!d[i].x && !d[i].y) { primary = i; break; }
    if (primary < 0 || primary == shown) return 1;
    return 2;
}

int ag_extra_parse(const uint8_t *p, size_t n, int *off) {
    if (!p || n != 1 || p[0] > 1) return -1;
    if (off) *off = p[0];
    return 0;
}

int ag_displays_extra(const AgDisplay *d, int n, int shown) {
    if (!d || n < 2 || shown < 0 || shown >= n || !(d[shown].flags & AGDS_HW)) return 0;
    for (int i = 0; i < n; i++) if (i != shown && !(d[i].flags & AGDS_HW)) return 1;
    return 0;
}

int lag_winrefused_parse(const uint8_t *p, size_t n, uint32_t *id, uint8_t *action, uint8_t *reason) {
    if (!p || n != LAG_WINREFUSED_BYTES) return -1;
    uint32_t i = le32(p);
    if (!i || p[4] < LAGA_FOCUS || p[4] > LAGA_MOVE || p[5] < LAGR_ELEVATED || p[5] > LAGR_LAST) return -1;
    if (id) *id = i;
    if (action) *action = p[4];
    if (reason) *reason = p[5];
    return 0;
}

int lag_winrefused_write(uint8_t out[LAG_WINREFUSED_BYTES], uint32_t id, uint8_t action, uint8_t reason) {
    if (!out || !id || action < LAGA_FOCUS || action > LAGA_MOVE || reason < LAGR_ELEVATED || reason > LAGR_LAST) return -1;
    ag_put32(out, id);
    out[4] = action;
    out[5] = reason;
    return 0;
}

void ag_displays_text(const AgDisplay *d, int n, int shown, char *out, size_t cap) {
    if (!out || !cap) return;
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; d && i < n && o < cap; i++) {
        int k = snprintf(out + o, cap - o, "%s%s %ux%u at %d,%d%s%s%s", i ? "; " : "", d[i].name, (unsigned)d[i].w, (unsigned)d[i].h,
                         (int)d[i].x, (int)d[i].y, d[i].flags & AGDS_PRIMARY ? " primary" : "", d[i].flags & AGDS_HW ? "" : " (not a PCI adapter)",
                         i == shown ? " (shown)" : "");
        if (k < 0) break;
        o += (size_t)k;
    }
    if (o >= cap) out[cap - 1] = 0;
}

int lag_apps_parse(const uint8_t *p, size_t n, int *on) {
    if (!p || n != 1 || p[0] > 1) return -1;
    if (on) *on = p[0];
    return 0;
}

/* a u16-length string at p[*o]: within n, at most max bytes (min 0 or 1), valid UTF-8 when not empty */
static int lstr(const uint8_t *p, size_t n, size_t *o, size_t min, size_t max, const uint8_t **s, uint16_t *sn) {
    if (n - *o < 2) return -1;
    uint16_t l = le16(p + *o);
    *o += 2;
    if (l < min || l > max || l > n - *o) return -1;
    if (l && !ag_utf8_ok(p + *o, l)) return -1;
    *s = p + *o;
    *sn = l;
    *o += l;
    return 0;
}

static int coord_ok(int32_t v) { return v >= -LAG_DIM_MAX && v <= LAG_DIM_MAX; }
static int size_ok(int32_t v) { return v >= 1 && v <= LAG_DIM_MAX; }

int lag_windows_parse(const uint8_t *p, size_t n, uint32_t *serial, LagWin *out, int max) {
    if (!p || n < 6) return -1;
    uint16_t count = le16(p + 4);
    if (count > LAG_WIN_MAX) return -1;
    size_t o = 6;
    int got = 0;
    for (uint16_t i = 0; i < count; i++) {
        LagWin w;
        memset(&w, 0, sizeof w);
        if (n - o < 24) return -1;
        w.id = le32(p + o); w.x = lei32(p + o + 4); w.y = lei32(p + o + 8); w.w = lei32(p + o + 12); w.h = lei32(p + o + 16);
        w.flags = le32(p + o + 20);
        o += 24;
        if (!w.id || !coord_ok(w.x) || !coord_ok(w.y) || !size_ok(w.w) || !size_ok(w.h) || (w.flags & ~(uint32_t)LAGW_ALL)) return -1;
        if (w.flags & LAGW_OWNED) {
            if (n - o < 4) return -1;
            w.owner = le32(p + o);
            o += 4;
            if (!w.owner || w.owner == w.id) return -1;
        }
        if (n - o < 4) return -1;
        w.pid = le32(p + o);
        o += 4;
        if (lstr(p, n, &o, 0, LAG_TITLE_MAX, &w.title, &w.title_n) || lstr(p, n, &o, 0, LAG_APP_MAX, &w.app, &w.app_n)) return -1;
        for (uint16_t k = 0; k < w.app_n; k++) if (w.app[k] == '/' || w.app[k] == '\\' || w.app[k] == ':') return -1;
        for (int k = 0; out && k < got && k < max; k++) if (out[k].id == w.id) return -1;       /* each window once */
        if (got < max && out) out[got] = w;
        got++;
    }
    if (o != n) return -1;
    if (serial) *serial = le32(p);
    return got < max ? got : max;
}

int lag_winicon_parse(const uint8_t *p, size_t n, uint32_t *id, uint16_t *w, uint16_t *h, const uint8_t **rgba) {
    if (!p || n < 8) return -1;
    uint32_t i = le32(p);
    uint16_t W = le16(p + 4), H = le16(p + 6);
    if (!i || !W || !H || W > LAG_ICON_MAX || H > LAG_ICON_MAX || n != 8 + (size_t)W * H * 4) return -1;
    if (id) *id = i;
    if (w) *w = W;
    if (h) *h = H;
    if (rgba) *rgba = p + 8;
    return 0;
}

int lag_winact_parse(const uint8_t *p, size_t n, LagAct *a) {
    if (!p || !a || n < 5) return -1;
    memset(a, 0, sizeof *a);
    a->id = le32(p);
    a->action = p[4];
    if (!a->id || a->action < LAGA_FOCUS || a->action > LAGA_MOVE) return -1;
    if (a->action != LAGA_MOVE) return n == 5 ? 0 : -1;
    if (n != 21) return -1;
    a->x = lei32(p + 5); a->y = lei32(p + 9); a->w = lei32(p + 13); a->h = lei32(p + 17);
    return coord_ok(a->x) && coord_ok(a->y) && size_ok(a->w) && size_ok(a->h) ? 0 : -1;
}

int lag_applist_parse(const uint8_t *p, size_t n, LagApp *out, int max) {
    if (!p || n < 2) return -1;
    uint16_t count = le16(p);
    if (count > LAG_APPS_MAX) return -1;
    size_t o = 2;
    int got = 0;
    for (uint16_t i = 0; i < count; i++) {
        LagApp a;
        if (lstr(p, n, &o, 1, LAG_NAME_MAX, &a.name, &a.name_n) || lstr(p, n, &o, 1, LAG_TOKEN_MAX, &a.token, &a.token_n)) return -1;
        if (got < max && out) out[got] = a;
        got++;
    }
    if (o != n) return -1;
    return got < max ? got : max;
}

int lag_launch_parse(const uint8_t *p, size_t n, const uint8_t **tok, uint16_t *tok_n) {
    size_t o = 0;
    const uint8_t *t;
    uint16_t tn;
    if (!p || lstr(p, n, &o, 1, LAG_TOKEN_MAX, &t, &tn) || o != n) return -1;
    if (tok) *tok = t;
    if (tok_n) *tok_n = tn;
    return 0;
}

/* ---- apps mode, copied out (vmagent.h: lord-vm's and lordos-guest's relays, lord-server) ---- */

static void lag_wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void lag_wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

int ag_offer_parse(const uint8_t *p, size_t n, uint32_t *gen, uint8_t *forms, uint8_t *flags) {
    if (!p || n != 6) return -1;
    uint32_t g = le32(p);
    if (!g || !p[4] || (p[4] & ~AGM_ALL) || (p[5] & ~AGO_ALL)) return -1;
    if (gen) *gen = g;
    if (forms) *forms = p[4];
    if (flags) *flags = p[5];
    return 0;
}

void ag_offer_write(uint8_t o[6], uint32_t gen, uint8_t forms, uint8_t flags) {
    lag_wr32(o, gen);
    o[4] = forms; o[5] = flags;
}

int ag_clipreq_parse(const uint8_t *p, size_t n, uint32_t *gen, uint8_t *form) {
    if (!p || n != 5) return -1;
    uint32_t g = le32(p);
    if (!g || p[4] < AGF_TEXT || p[4] > AGF_PNG) return -1;
    if (gen) *gen = g;
    if (form) *form = p[4];
    return 0;
}

void ag_clipreq_write(uint8_t o[5], uint32_t gen, uint8_t form) {
    lag_wr32(o, gen);
    o[4] = form;
}

int ag_clipdata_parse(const uint8_t *p, size_t n, uint32_t *gen, uint8_t *form, uint8_t *status, const uint8_t **data, uint32_t *dn) {
    if (!p || n < 6) return -1;
    uint32_t g = le32(p);
    uint8_t f = p[4], st = p[5];
    size_t l = n - 6;
    if (!g || f < AGF_TEXT || f > AGF_PNG || st > AGD_NONE) return -1;
    if (st != AGD_OK) { if (l) return -1; }
    else {
        if (!l || l > (f == AGF_PNG ? AG_PNG_MAX : AG_TEXT_MAX)) return -1;
        if (f == AGF_PNG ? ag_png_check(p + 6, l, NULL, NULL) != 0 : !ag_utf8_ok(p + 6, l)) return -1;
    }
    if (gen) *gen = g;
    if (form) *form = f;
    if (status) *status = st;
    if (data) *data = st == AGD_OK ? p + 6 : NULL;
    if (dn) *dn = (uint32_t)l;
    return 0;
}

void ag_clipdata_head(uint8_t o[6], uint32_t gen, uint8_t form, uint8_t status) {
    lag_wr32(o, gen);
    o[4] = form; o[5] = status;
}

/* ---- AGK_OPEN, AGK_OPENED ---- */
int ag_open_token_ok(const char *t, size_t n) {
    if (!t || n != AG_OPEN_TOKEN) return 0;
    for (size_t i = 0; i < n; i++) if (!((t[i] >= '0' && t[i] <= '9') || (t[i] >= 'a' && t[i] <= 'f'))) return 0;
    return 1;
}

int ag_open_parse(const uint8_t *p, size_t n, char token[AG_OPEN_TOKEN + 1], uint64_t *size, uint8_t sha[32]) {
    if (!p || n != AG_OPEN_BYTES || !ag_open_token_ok((const char *)p, AG_OPEN_TOKEN)) return -1;
    if (token) { memcpy(token, p, AG_OPEN_TOKEN); token[AG_OPEN_TOKEN] = 0; }
    if (size) *size = (uint64_t)le32(p + AG_OPEN_TOKEN) | (uint64_t)le32(p + AG_OPEN_TOKEN + 4) << 32;
    if (sha) memcpy(sha, p + AG_OPEN_TOKEN + 8, 32);
    return 0;
}

int ag_open_write(uint8_t out[AG_OPEN_BYTES], const char *token, uint64_t size, const uint8_t sha[32]) {
    if (!token || strlen(token) != AG_OPEN_TOKEN || !ag_open_token_ok(token, AG_OPEN_TOKEN) || !sha) return -1;
    memcpy(out, token, AG_OPEN_TOKEN);
    lag_wr32(out + AG_OPEN_TOKEN, (uint32_t)size);
    lag_wr32(out + AG_OPEN_TOKEN + 4, (uint32_t)(size >> 32));
    memcpy(out + AG_OPEN_TOKEN + 8, sha, 32);
    return 0;
}

int ag_opened_parse(const uint8_t *p, size_t n, char token[AG_OPEN_TOKEN + 1], uint8_t *status, uint32_t *code) {
    if (!p || n != AG_OPENED_BYTES || !ag_open_token_ok((const char *)p, AG_OPEN_TOKEN)) return -1;
    uint8_t st = p[AG_OPEN_TOKEN];
    uint32_t c = le32(p + AG_OPEN_TOKEN + 1);
    if (st > AGOS_LAST || (st == AGOS_OK && c)) return -1;
    if (token) { memcpy(token, p, AG_OPEN_TOKEN); token[AG_OPEN_TOKEN] = 0; }
    if (status) *status = st;
    if (code) *code = c;
    return 0;
}

int ag_opened_write(uint8_t out[AG_OPENED_BYTES], const char *token, uint8_t status, uint32_t code) {
    if (!token || strlen(token) != AG_OPEN_TOKEN || !ag_open_token_ok(token, AG_OPEN_TOKEN) || status > AGOS_LAST) return -1;
    memcpy(out, token, AG_OPEN_TOKEN);
    out[AG_OPEN_TOKEN] = status;
    lag_wr32(out + AG_OPEN_TOKEN + 1, status == AGOS_OK ? 0 : code);
    return 0;
}

int ag_place_parse(const uint8_t *p, size_t n, AgPlace *out) {
    AgPlace a;
    if (!p || n != AG_PLACE_BYTES || ag_open_parse(p, AG_OPEN_BYTES, a.token, &a.size, a.sha)) return -1;
    const uint8_t *q = p + AG_OPEN_BYTES;
    a.win = le32(q); a.x = (int32_t)le32(q + 4); a.y = (int32_t)le32(q + 8); a.batch = le32(q + 12); a.index = q[16]; a.count = q[17];
    if (!a.win || a.x < 0 || a.y < 0 || a.x > LAG_DIM_MAX || a.y > LAG_DIM_MAX || !a.batch || !a.count || a.count > AG_PLACE_MAX || a.index >= a.count) return -1;
    if (out) *out = a;
    return 0;
}

int ag_place_write(uint8_t out[AG_PLACE_BYTES], const AgPlace *a) {
    if (!a || ag_open_write(out, a->token, a->size, a->sha)) return -1;
    if (!a->win || a->x < 0 || a->y < 0 || a->x > LAG_DIM_MAX || a->y > LAG_DIM_MAX || !a->batch || !a->count || a->count > AG_PLACE_MAX || a->index >= a->count) return -1;
    uint8_t *q = out + AG_OPEN_BYTES;
    lag_wr32(q, a->win); lag_wr32(q + 4, (uint32_t)a->x); lag_wr32(q + 8, (uint32_t)a->y); lag_wr32(q + 12, a->batch);
    q[16] = a->index; q[17] = a->count;
    return 0;
}

int ag_placed_parse(const uint8_t *p, size_t n, char token[AG_OPEN_TOKEN + 1], uint8_t *status, uint8_t *how, uint32_t *code) {
    if (!p || n != AG_PLACED_BYTES || !ag_open_token_ok((const char *)p, AG_OPEN_TOKEN)) return -1;
    uint8_t st = p[AG_OPEN_TOKEN], hw = p[AG_OPEN_TOKEN + 1];
    uint32_t c = le32(p + AG_OPEN_TOKEN + 2);
    if (st > AGOS_LAST || hw > AGP_LAST || (st == AGOS_OK ? !hw || c : hw != AGP_NONE)) return -1;
    if (token) { memcpy(token, p, AG_OPEN_TOKEN); token[AG_OPEN_TOKEN] = 0; }
    if (status) *status = st;
    if (how) *how = hw;
    if (code) *code = c;
    return 0;
}

int ag_placed_write(uint8_t out[AG_PLACED_BYTES], const char *token, uint8_t status, uint8_t how, uint32_t code) {
    if (!token || strlen(token) != AG_OPEN_TOKEN || !ag_open_token_ok(token, AG_OPEN_TOKEN) || status > AGOS_LAST || how > AGP_LAST) return -1;
    if (status == AGOS_OK ? !how : how != AGP_NONE) return -1;
    memcpy(out, token, AG_OPEN_TOKEN);
    out[AG_OPEN_TOKEN] = status;
    out[AG_OPEN_TOKEN + 1] = how;
    lag_wr32(out + AG_OPEN_TOKEN + 2, status == AGOS_OK ? 0 : code);
    return 0;
}

unsigned ag_hello_caps(const uint8_t *p, size_t n) {
    unsigned caps = 0;
    size_t i = 0;
    while (i < n) {
        while (i < n && p[i] == ' ') i++;
        size_t s0 = i;
        while (i < n && p[i] != ' ') i++;
        size_t l = i - s0;
        if (l == 9 && !memcmp(p + s0, "clipboard", 9)) caps |= AG_HELLO_CLIPBOARD;
        else if (l == 5 && !memcmp(p + s0, "clipx", 5)) caps |= AG_HELLO_CLIPX;
        else if (l == 7 && !memcmp(p + s0, "windows", 7)) caps |= AG_HELLO_WINDOWS;
        else if (l == 6 && !memcmp(p + s0, "noclip", 6)) caps |= AG_HELLO_NOCLIP;
        else if (l == 7 && !memcmp(p + s0, "clipreq", 7)) caps |= AG_HELLO_CLIPREQ;
        else if (l == 4 && !memcmp(p + s0, "open", 4)) caps |= AG_HELLO_OPEN;
        else if (l == 5 && !memcmp(p + s0, "place", 5)) caps |= AG_HELLO_PLACE;
        else if (l == 7 && !memcmp(p + s0, "pointer", 7)) caps |= AG_HELLO_POINTER;
        else if (l == 3 && !memcmp(p + s0, "gvt", 3)) caps |= AG_HELLO_GVT;
        else if (l == 5 && !memcmp(p + s0, "extra", 5)) caps |= AG_HELLO_EXTRA;
        else if (l == 8 && !memcmp(p + s0, "extraoff", 8)) caps |= AG_HELLO_EXTRAOFF;
    }
    return caps;
}

int lag_text_ok(const uint8_t *p, size_t n) {
    if (!ag_utf8_ok(p, n)) return 0;
    for (size_t i = 0; i < n; i++) if (p[i] < 0x20 || p[i] == 0x7f) return 0;
    for (size_t i = 0; i + 1 < n; i++) if (p[i] == 0xc2 && p[i + 1] >= 0x80 && p[i + 1] <= 0x9f) return 0;     /* C1 */
    return 1;
}

int lag_rect_ok(int32_t x, int32_t y, int32_t w, int32_t h) {
    return w >= 1 && h >= 1 && w <= LAG_DIM_MAX && h <= LAG_DIM_MAX && x >= -LAG_DIM_MAX && x <= LAG_DIM_MAX && y >= -LAG_DIM_MAX && y <= LAG_DIM_MAX;
}

static void copy_text(char *out, const uint8_t *p, uint16_t n) { memcpy(out, p, n); out[n] = 0; }

int lag_winlist_read(const uint8_t *p, size_t n, LagWinList *out) {
    LagWin w[LAG_WIN_MAX];
    uint32_t serial = 0;
    int k = lag_windows_parse(p, n, &serial, w, LAG_WIN_MAX);
    if (k < 0) return -1;
    out->serial = serial;
    out->n = 0;
    for (int i = 0; i < k; i++) {
        if (!lag_text_ok(w[i].title, w[i].title_n) || !lag_text_ok(w[i].app, w[i].app_n)) return -1;
        LagWinEntry *e = &out->w[i];
        e->id = w[i].id; e->flags = w[i].flags; e->owner = w[i].owner; e->pid = w[i].pid;
        e->x = w[i].x; e->y = w[i].y; e->w = w[i].w; e->h = w[i].h;
        copy_text(e->title, w[i].title, w[i].title_n);
        copy_text(e->app, w[i].app, w[i].app_n);
        out->n = i + 1;
    }
    return 0;
}

static size_t lag_put_str(uint8_t *out, size_t o, const char *s) {
    size_t l = strlen(s);
    lag_wr16(out + o, (uint32_t)l);
    memcpy(out + o + 2, s, l);
    return o + 2 + l;
}

size_t lag_windows_write(const LagWinList *l, uint8_t *out, size_t cap) {
    if (l->n < 0 || l->n > LAG_WIN_MAX) return 0;
    size_t need = 6;
    for (int i = 0; i < l->n; i++) {
        const LagWinEntry *w = &l->w[i];
        if (!w->id || strlen(w->title) > LAG_TITLE_MAX || strlen(w->app) > LAG_APP_MAX) return 0;
        if (!(w->flags & LAGW_MINIMIZED) && !lag_rect_ok(w->x, w->y, w->w, w->h)) return 0;
        need += 28 + ((w->flags & LAGW_OWNED) ? 4 : 0) + 4 + strlen(w->title) + strlen(w->app);
    }
    if (need > cap) return 0;
    lag_wr32(out, l->serial);
    lag_wr16(out + 4, (uint32_t)l->n);
    size_t o = 6;
    for (int i = 0; i < l->n; i++) {
        const LagWinEntry *w = &l->w[i];
        int place = lag_rect_ok(w->x, w->y, w->w, w->h);         /* a minimized window without one: 0, 0, 1, 1 */
        lag_wr32(out + o, w->id);
        lag_wr32(out + o + 4, (uint32_t)(place ? w->x : 0)); lag_wr32(out + o + 8, (uint32_t)(place ? w->y : 0));
        lag_wr32(out + o + 12, (uint32_t)(place ? w->w : 1)); lag_wr32(out + o + 16, (uint32_t)(place ? w->h : 1));
        lag_wr32(out + o + 20, w->flags);
        o += 24;
        if (w->flags & LAGW_OWNED) { lag_wr32(out + o, w->owner); o += 4; }
        lag_wr32(out + o, w->pid);
        o += 4;
        o = lag_put_str(out, o, w->title);
        size_t a0 = o + 2;
        o = lag_put_str(out, o, w->app);
        for (size_t k = a0; k < o; k++) if (out[k] == '/' || out[k] == '\\' || out[k] == ':') out[k] = '_';   /* a file name */
    }
    return o;
}

size_t lag_icon_write(uint32_t id, int w, int h, const uint8_t *rgba, uint8_t *out, size_t cap) {
    if (!id || w < 1 || h < 1 || w > LAG_ICON_MAX || h > LAG_ICON_MAX) return 0;
    size_t need = 8 + (size_t)w * h * 4;
    if (need > cap) return 0;
    lag_wr32(out, id); lag_wr16(out + 4, (uint32_t)w); lag_wr16(out + 6, (uint32_t)h);
    memcpy(out + 8, rgba, need - 8);
    return need;
}

size_t lag_act_write(const LagAct *a, uint8_t *out, size_t cap) {
    if (!a->id || a->action < LAGA_FOCUS || a->action > LAGA_MOVE) return 0;
    size_t need = a->action == LAGA_MOVE ? 21 : 5;
    if (cap < need || (a->action == LAGA_MOVE && !lag_rect_ok(a->x, a->y, a->w, a->h))) return 0;
    lag_wr32(out, a->id);
    out[4] = a->action;
    if (a->action == LAGA_MOVE) { lag_wr32(out + 5, (uint32_t)a->x); lag_wr32(out + 9, (uint32_t)a->y); lag_wr32(out + 13, (uint32_t)a->w); lag_wr32(out + 17, (uint32_t)a->h); }
    return need;
}

int lag_launch_token(const uint8_t *p, size_t n, char *token, size_t cap) {
    const uint8_t *t;
    uint16_t tn;
    if (cap <= LAG_TOKEN_MAX || lag_launch_parse(p, n, &t, &tn) || !lag_text_ok(t, tn)) return -1;
    copy_text(token, t, tn);
    return 0;
}

size_t lag_launch_write(const char *token, uint8_t *out, size_t cap) {
    size_t l = strlen(token);
    if (!l || l > LAG_TOKEN_MAX || cap < l + 2 || !lag_text_ok((const uint8_t *)token, l)) return 0;
    return lag_put_str(out, 0, token);
}

int lag_applist_read(const uint8_t *p, size_t n, LagAppList *out) {
    static LagApp a[LAG_APPS_MAX];
    int k = lag_applist_parse(p, n, a, LAG_APPS_MAX);
    if (k < 0) return -1;
    out->n = 0;
    for (int i = 0; i < k; i++) {
        if (!lag_text_ok(a[i].name, a[i].name_n) || !lag_text_ok(a[i].token, a[i].token_n)) return -1;
        copy_text(out->a[i].name, a[i].name, a[i].name_n);
        copy_text(out->a[i].token, a[i].token, a[i].token_n);
        out->n = i + 1;
    }
    return 0;
}

size_t lag_applist_write(const LagAppList *l, uint8_t *out, size_t cap) {
    if (l->n < 0 || l->n > LAG_APPS_MAX) return 0;
    size_t need = 2;
    for (int i = 0; i < l->n; i++) {
        size_t a = strlen(l->a[i].name), b = strlen(l->a[i].token);
        if (!a || !b || a > LAG_NAME_MAX || b > LAG_TOKEN_MAX) return 0;
        need += 4 + a + b;
    }
    if (need > cap) return 0;
    lag_wr16(out, (uint32_t)l->n);
    size_t o = 2;
    for (int i = 0; i < l->n; i++) { o = lag_put_str(out, o, l->a[i].name); o = lag_put_str(out, o, l->a[i].token); }
    return o;
}

int lag_applist_has(const LagAppList *l, const char *token) {
    for (int i = 0; i < l->n; i++) if (!strcmp(l->a[i].token, token)) return 1;
    return 0;
}
