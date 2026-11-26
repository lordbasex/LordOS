/* websearch.c — see websearch.h. */
#include "websearch.h"
#include "../sec/json.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *const NAMES[WEBSEARCH_N] = { "google", "duckduckgo", "bing", "startpage" };
/* each engine's fixed search address; the query is its last parameter */
static const char *const BASES[WEBSEARCH_N] = {
    "https://www.google.com/search?q=", "https://duckduckgo.com/?q=", "https://www.bing.com/search?q=", "https://www.startpage.com/do/search?q=",
};

int websearch_engine(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < WEBSEARCH_N; i++) if (!strcmp(NAMES[i], name)) return i;
    return -1;
}

const char *websearch_engine_name(int engine) { return engine >= 0 && engine < WEBSEARCH_N ? NAMES[engine] : NULL; }

int websearch_parse(const char *text, size_t len) {
    if (!text || !len || len > WEBSEARCH_FILE_MAX) return WEBSEARCH_GOOGLE;
    JDoc d;
    if (json_parse(text, len, &d, NULL, 0)) return WEBSEARCH_GOOGLE;
    const char *e = d.root && d.root->type == J_OBJ ? json_str(d.root, "engine") : NULL;
    int k = websearch_engine(e);
    json_free(&d);
    return k < 0 ? WEBSEARCH_GOOGLE : k;
}

size_t websearch_write(int engine, char *out, size_t n) {
    const char *name = websearch_engine_name(engine);
    if (!name || !out) return 0;
    int w = snprintf(out, n, "{\"engine\":\"%s\"}\n", name);
    return w < 0 || (size_t)w >= n ? 0 : (size_t)w;
}

/* ---- the page's text, cleaned ---- */

/* the code point at s (len bytes left) and its length in *k; -1 for invalid UTF-8 (one byte to skip) */
static int32_t utf8_next(const unsigned char *s, size_t len, size_t *k) {
    unsigned c = s[0];
    *k = 1;
    if (c < 0x80) return (int32_t)c;
    size_t need = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (!need || need > len) return -1;
    uint32_t cp = c & (need == 2 ? 0x1F : need == 3 ? 0x0F : 0x07);
    for (size_t i = 1; i < need; i++) {
        if ((s[i] & 0xC0) != 0x80) return -1;
        cp = cp << 6 | (s[i] & 0x3F);
    }
    if ((need == 3 && cp < 0x800) || (need == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) return -1;
    *k = need;
    return (int32_t)cp;
}

/* what a character becomes: 1 kept, 0 left out, 2 a space */
static int keep(int32_t c) {
    if (c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' || c == 0x85 || c == 0xA0 || c == 0x2028 || c == 0x2029 ||
        c == 0x3000 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == ' ') return 2;
    if (c < 0x20 || (c >= 0x7F && c <= 0x9F)) return 0;                 /* C0, DEL, C1 */
    if (c == 0x061C || c == 0x200E || c == 0x200F || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069)) return 0;  /* bidi */
    if (c == 0x200B || c == 0xFEFF || (c >= 0xFFF9 && c <= 0xFFFB) || (c & 0xFFFE) == 0xFFFE || (c >= 0xFDD0 && c <= 0xFDEF)) return 0;
    return 1;
}

/* text cleaned into out (n bytes): at most max bytes of it (whole characters) and at most chars characters (0: no
 * limit); *cut 1 when something was left at the end. The length. */
static size_t clean(const char *text, size_t max, size_t chars, char *out, size_t n, int *cut) {
    size_t o = 0, count = 0;
    int space = 0;
    *cut = 0;
    if (!n) return 0;
    const unsigned char *s = (const unsigned char *)(text ? text : "");
    size_t len = strlen((const char *)s);
    for (size_t i = 0; i < len;) {
        size_t k;
        int32_t c = utf8_next(s + i, len - i, &k);
        int what = c < 0 ? 0 : keep(c);
        if (what == 2) { if (o) space = 1; i += k; continue; }
        if (what == 0) { i += k; continue; }
        size_t need = k + (size_t)space;
        if (o + need > max || o + need + 1 > n || (chars && count + (size_t)space + 1 > chars)) { *cut = 1; break; }
        if (space) { out[o++] = ' '; count++; space = 0; }
        memcpy(out + o, s + i, k);
        o += k;
        count++;
        i += k;
    }
    out[o] = 0;
    return o;
}

static int unreserved(unsigned char c, int dash) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '~' || (dash && c == '-');
}

/* s percent-encoded at out + *o (n bytes in all); -1 when it doesn't fit */
static int encode(const char *s, size_t len, int dash, char *out, size_t n, size_t *o) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (unreserved(c, dash)) { if (*o + 2 > n) return -1; out[(*o)++] = (char)c; continue; }
        if (*o + 4 > n) return -1;
        out[(*o)++] = '%';
        out[(*o)++] = hex[c >> 4];
        out[(*o)++] = hex[c & 15];
    }
    out[*o] = 0;
    return 0;
}

int websearch_url(int engine, const char *text, char *out, size_t n) {
    const char *base = engine >= 0 && engine < WEBSEARCH_N ? BASES[engine] : NULL;
    char q[WEBSEARCH_QUERY_MAX + 1];
    int cut;
    if (!base || !out || !n) return -1;
    out[0] = 0;
    size_t ql = clean(text, WEBSEARCH_QUERY_MAX, 0, q, sizeof q, &cut);
    size_t o = strlen(base);
    if (!ql || o + 1 > n) return -1;
    memcpy(out, base, o + 1);
    if (encode(q, ql, 1, out, n, &o)) { out[0] = 0; return -1; }
    return 0;
}

size_t websearch_snippet(const char *text, size_t max, char *out, size_t n) {
    static const char ell[] = "\xE2\x80\xA6";        /* … */
    int cut;
    if (!out || n < sizeof ell) { if (out && n) out[0] = 0; return 0; }
    size_t o = clean(text, n - sizeof ell, max, out, n - (sizeof ell - 1), &cut);
    if (cut && o) {
        while (o && out[o - 1] == ' ') o--;
        memcpy(out + o, ell, sizeof ell);
        o += sizeof ell - 1;
    }
    out[o] = 0;
    return o;
}

/* the number of characters in s, and where the i-th starts (i < count; else the end) */
static size_t chars_in(const char *s) {
    size_t c = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) c++;
    return c;
}
static const char *char_at(const char *s, size_t i) {
    size_t c = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80 && c++ == i) return s;
    return s;
}

#define HL_SHORT 80                 /* characters: up to here, the whole text */
#define HL_WORDS 4                  /* else the first and last words … */
#define HL_CHARS 20                 /* … or characters, for a text with few spaces */

int websearch_highlight(const char *page, const char *text, char *out, size_t n) {
    if (!out || !n) return -1;
    out[0] = 0;
    if (!page || (strncasecmp(page, "https://", 8) && strncasecmp(page, "http://", 7))) return -1;
    size_t pl = strcspn(page, "#");
    for (size_t i = 0; i < pl; i++) if ((unsigned char)page[i] <= ' ' || page[i] == 0x7F) return -1;
    static char t[WEBSEARCH_SELECTION_MAX + 1];
    int cut;
    size_t tl = clean(text, WEBSEARCH_SELECTION_MAX, 0, t, sizeof t, &cut);
    if (!tl) return -1;
    static const char frag[] = "#:~:text=";
    if (pl + sizeof frag > n) return -1;
    memcpy(out, page, pl);
    memcpy(out + pl, frag, sizeof frag);
    size_t o = pl + sizeof frag - 1;
    size_t cc = chars_in(t);
    int bad;
    if (cc <= HL_SHORT && !cut) bad = encode(t, tl, 0, out, n, &o);
    else {
        /* textStart,textEnd: the first and last HL_WORDS words, or HL_CHARS characters */
        size_t words = 1;
        for (size_t i = 0; i < tl; i++) if (t[i] == ' ') words++;
        const char *a_end, *b;
        if (words >= 2 * HL_WORDS) {
            size_t sp = 0;
            a_end = t;
            while (*a_end && !(*a_end == ' ' && ++sp == HL_WORDS)) a_end++;
            sp = 0;
            b = t + tl;
            while (b > t && !(b[-1] == ' ' && ++sp == HL_WORDS)) b--;
        } else {
            a_end = char_at(t, HL_CHARS);
            b = char_at(t, cc - HL_CHARS);
        }
        bad = encode(t, (size_t)(a_end - t), 0, out, n, &o) || o + 2 > n;
        if (!bad) { out[o++] = ','; out[o] = 0; bad = encode(b, tl - (size_t)(b - t), 0, out, n, &o); }
    }
    if (bad) { out[0] = 0; return -1; }
    return 0;
}
