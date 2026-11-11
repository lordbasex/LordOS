/* webclip.c — the clipboard's types and text for a paste (see webclip.h). */
#include "webclip.h"

#include <stdlib.h>
#include <string.h>

int webclip_text_rank(const char *m) {
    if (!m) return 0;
    if (!strcmp(m, "text/plain;charset=utf-8")) return 4;
    if (!strcmp(m, "UTF8_STRING")) return 3;
    if (!strcmp(m, "text/plain")) return 2;
    if (!strcmp(m, "STRING")) return 1;
    return 0;
}

int webclip_utf8_valid(const char *str, size_t len) {
    const unsigned char *s = (const unsigned char *)str;
    size_t i = 0;
    while (i < len) {
        unsigned c = s[i];
        if (c < 0x80) { i++; continue; }
        size_t n;
        unsigned cp, min;
        if (c >= 0xc2 && c <= 0xdf) { n = 1; cp = c & 0x1f; min = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; cp = c & 0x0f; min = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; cp = c & 0x07; min = 0x10000; }
        else return 0;
        if (len - i <= n) return 0;
        for (size_t k = 1; k <= n; k++) {
            if ((s[i + k] & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (s[i + k] & 0x3f);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
        i += n + 1;
    }
    return 1;
}

char *webclip_text_utf8(const char *mime, const char *data, size_t len, size_t *out_len) {
    int r = webclip_text_rank(mime);
    if (!r || (!data && len)) return NULL;
    int utf8 = webclip_utf8_valid(data, len);
    if ((r >= 3 && !utf8) || len > ((size_t)-1) / 2 - 1) return NULL;
    char *out;
    size_t o = 0;
    if (utf8 && r >= 2) {
        if (!(out = malloc(len + 1))) return NULL;
        if (len) memcpy(out, data, len);
        o = len;
    } else {
        /* Latin-1: each byte is its own code point, two UTF-8 bytes from 0x80 up */
        if (!(out = malloc(len * 2 + 1))) return NULL;
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)data[i];
            if (c < 0x80) out[o++] = (char)c;
            else { out[o++] = (char)(0xc0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 0x3f)); }
        }
    }
    out[o] = 0;
    if (out_len) *out_len = o;
    return out;
}

int webclip_list_rank(const char *m) {
    if (!m) return 0;
    if (!strcmp(m, WEBCLIP_URIS)) return 2;
    if (!strcmp(m, WEBCLIP_GNOME)) return 1;
    return 0;
}

size_t webclip_list_body(const char *mime, const char *data, size_t len) {
    if (!data || webclip_list_rank(mime) != 1) return 0;
    /* GNOME's list: "copy" or "cut", then one URI a line */
    const char *e = memchr(data, '\n', len);
    size_t l = e ? (size_t)(e - data) : len;
    if (l && data[l - 1] == '\r') l--;
    if ((l == 4 && !memcmp(data, "copy", 4)) || (l == 3 && !memcmp(data, "cut", 3))) return e ? (size_t)(e - data) + 1 : len;
    return 0;
}

int webclip_list_count(const char *data, size_t len) {
    int n = 0;
    const char *p = data, *end = data ? data + len : NULL;
    while (p && p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        size_t l = (size_t)((e ? e : end) - p);
        if (l && p[l - 1] == '\r') l--;
        if (l && p[0] != '#') n++;
        p = e ? e + 1 : end;
    }
    return n;
}

int webclip_webkit_reads(const char *s, size_t len) {
    for (size_t i = 0; s && i < len; i++) if ((unsigned char)s[i] >= 0x80) return 0;
    return 1;
}
