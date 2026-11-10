/* urilist.c — lists of files on the clipboard and in drag and drop, the way Linux file managers put them there.
 *
 * Drag and drop (wl.c, the dnd_* code) uses the same lists: Files' drag to a Linux app offers its paths as
 * text/uri-list and plain text, and a Linux app's drop onto Files is read back with uril_parse.
 *
 * Copying in Files offers the selected paths as text/uri-list (RFC 2483: one URI per line, CRLF),
 * x-special/gnome-copied-files ("copy" or "cut", then the URIs, LF) and plain text; pasting in Files reads
 * the first two back from an app. What an app sends is untrusted: only file:// URIs of this machine (an
 * empty host or "localhost") are kept, decoded to an absolute path without NUL or line breaks, and the copy
 * itself goes through lord.fs and its checks like any other. And only an app outside a sandbox may put files
 * there, with a text that names the same paths (uril_accept). No Wayland types here, so the test links it alone. */
#include "urilist.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int utf8_ok(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i];
        size_t k = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 && c >= 0xc2 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!k || i + k > n) return 0;
        for (size_t j = 1; j < k; j++) if ((s[i + j] & 0xc0) != 0x80) return 0;
        if (k == 3 && ((c == 0xe0 && s[i + 1] < 0xa0) || (c == 0xed && s[i + 1] >= 0xa0))) return 0;  /* overlong, surrogate */
        if (k == 4 && ((c == 0xf0 && s[i + 1] < 0x90) || (c == 0xf4 && s[i + 1] >= 0x90))) return 0;  /* overlong, > U+10FFFF */
        i += k;
    }
    return 1;
}

int uril_path_ok(const char *p, size_t n) {
    if (!p || !n || n >= URIL_PATH_MAX || p[0] != '/' || strlen(p) != n) return 0;
    if (memchr(p, '\r', n) || memchr(p, '\n', n)) return 0;
    return utf8_ok((const unsigned char *)p, n);
}

char *uril_join(const char *const *paths, int n, size_t *len) {
    size_t need = 0;
    *len = 0;
    if (!paths || n < 1 || n > URIL_MAX_FILES) return NULL;
    for (int i = 0; i < n; i++) {
        size_t k = paths[i] ? strlen(paths[i]) : 0;
        if (!uril_path_ok(paths[i], k)) return NULL;
        need += k + 1;
    }
    char *out = malloc(need), *o = out;
    if (!out) return NULL;
    for (int i = 0; i < n; i++) {
        size_t k = strlen(paths[i]) + 1;
        memcpy(o, paths[i], k);
        o += k;
    }
    *len = need;
    return out;
}

/* kept as it is in a URI path; everything else becomes %XX */
static int plain_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '.' || c == '_' || c == '~';
}

char *uril_encode(const char *plist, size_t len, int fmt, int cut, size_t *out_len) {
    static const char hex[] = "0123456789ABCDEF";
    if (!plist || !len || plist[len - 1] || len > URIL_MAX_BYTES) return NULL;
    /* first pass: check every path and count the bytes */
    size_t need = fmt == URIL_GNOME ? 5 : 0, files = 0;
    for (size_t off = 0; off < len; files++) {
        size_t n = strlen(plist + off);
        if (files >= URIL_MAX_FILES || !uril_path_ok(plist + off, n)) return NULL;
        if (fmt == URIL_PLAIN) need += n + 1;
        else {
            need += 7 + 2;                                  /* "file://" and the line end */
            for (size_t i = 0; i < n; i++) need += plain_byte((unsigned char)plist[off + i]) ? 1 : 3;
        }
        off += n + 1;
    }
    char *out = malloc(need + 1), *o = out;
    if (!out) return NULL;
    if (fmt == URIL_GNOME) { const char *h = cut ? "cut\n" : "copy\n"; memcpy(o, h, strlen(h)); o += strlen(h); }
    for (size_t off = 0, k = 0; off < len; k++) {
        const char *p = plist + off;
        size_t n = strlen(p);
        if (fmt == URIL_GNOME && k) *o++ = '\n';
        if (fmt == URIL_PLAIN) {
            if (k) *o++ = '\n';
            memcpy(o, p, n);
            o += n;
        } else {
            memcpy(o, "file://", 7);
            o += 7;
            for (size_t i = 0; i < n; i++) {
                unsigned char c = (unsigned char)p[i];
                if (plain_byte(c)) *o++ = (char)c;
                else { *o++ = '%'; *o++ = hex[c >> 4]; *o++ = hex[c & 15]; }
            }
            if (fmt == URIL_URI_LIST) { *o++ = '\r'; *o++ = '\n'; }
        }
        off += n + 1;
    }
    *o = 0;
    *out_len = (size_t)(o - out);
    return out;
}

static int hexval(unsigned char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* One URI (n bytes, no line end) to a path in out (URIL_PATH_MAX bytes): its length, or -1 if it is not a
 * file of this machine or doesn't decode to a good path. */
static long uri_path(const char *u, size_t n, char *out) {
    if (n < 8 || strncasecmp(u, "file://", 7)) return -1;
    u += 7;
    n -= 7;
    const char *slash = memchr(u, '/', n);
    if (!slash) return -1;
    size_t host = (size_t)(slash - u);
    if (host && !(host == 9 && !strncasecmp(u, "localhost", 9))) return -1;     /* another machine's file */
    u += host;
    n -= host;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)u[i];
        if (c == '?' || c == '#' || c <= 0x20 || c == 0x7f) return -1;           /* a query, a fragment, a raw space */
        if (c == '%') {
            int hi = i + 2 < n ? hexval((unsigned char)u[i + 1]) : -1, lo = i + 2 < n ? hexval((unsigned char)u[i + 2]) : -1;
            if (hi < 0 || lo < 0) return -1;
            c = (unsigned char)(hi << 4 | lo);
            i += 2;
        }
        if (o + 1 >= URIL_PATH_MAX) return -1;
        out[o++] = (char)c;
    }
    out[o] = 0;
    return uril_path_ok(out, o) ? (long)o : -1;
}

char *uril_parse(const char *buf, size_t n, int gnome, int *cut, size_t *plist_len, int *count) {
    *cut = 0;
    *plist_len = 0;
    *count = 0;
    if (!buf || !n || n > URIL_MAX_BYTES) return NULL;
    /* a plist is never longer than the text (decoding only shortens; each line end becomes a NUL) */
    char *out = malloc(n + 1), path[URIL_PATH_MAX];
    if (!out) return NULL;
    size_t o = 0;
    int files = 0, first = 1;
    for (size_t at = 0; at < n;) {
        const char *line = buf + at, *nl = memchr(line, '\n', n - at);
        size_t len = nl ? (size_t)(nl - line) : n - at;
        at += len + (nl ? 1 : 0);
        if (len && line[len - 1] == '\r') len--;
        if (gnome && first) {                               /* "copy" or "cut", nothing else */
            first = 0;
            if (len == 3 && !memcmp(line, "cut", 3)) *cut = 1;
            else if (!(len == 4 && !memcmp(line, "copy", 4))) break;
            continue;
        }
        first = 0;
        if (!len || line[0] == '#') continue;               /* RFC 2483: comments */
        long pl = uri_path(line, len, path);
        if (pl < 0) continue;
        if (files >= URIL_MAX_FILES || o + (size_t)pl + 1 > n + 1) { files = 0; break; }
        memcpy(out + o, path, (size_t)pl + 1);
        o += (size_t)pl + 1;
        files++;
    }
    if (!files) { free(out); *cut = 0; return NULL; }
    *plist_len = o;
    *count = files;
    return out;
}

int uril_text_names(const char *text, size_t n, const char *plist, size_t len) {
    if (!text || !n || !plist || !len || plist[len - 1]) return 0;
    if (text[n - 1] == '\n') n -= n > 1 && text[n - 2] == '\r' ? 2 : 1;     /* one line end after the last */
    char path[URIL_PATH_MAX];
    size_t off = 0;
    for (size_t at = 0; at <= n;) {
        if (off >= len) return 0;                                   /* more lines than paths */
        const char *line = text + at, *nl = memchr(line, '\n', n - at);
        size_t ll = nl ? (size_t)(nl - line) : n - at;
        at += ll + 1;
        if (ll && line[ll - 1] == '\r') ll--;
        const char *p = plist + off;
        size_t pn = strlen(p);
        off += pn + 1;
        if (ll == pn && !memcmp(line, p, pn)) continue;
        long ul = uri_path(line, ll, path);
        if (ul < 0 || (size_t)ul != pn || memcmp(path, p, pn)) return 0;
    }
    return off == len;
}

int uril_accept(int from, const char *text, size_t n, const char *plist, size_t len) {
    if (from != URIL_FROM_APP || !plist || !len) return 0;
    return !text || uril_text_names(text, n, plist, len);
}
