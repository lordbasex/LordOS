/* urilist.c — file paths as text/uri-list (RFC 2483) and back, for drag and drop (wl.c): LordOS's Files
 * offers the paths it drags to a Linux app as file:// URIs, and a Linux app's drop onto Files arrives
 * the same way. Pure functions, no I/O (tests/urilist_test.c). */
#include "urilist.h"

#include <limits.h>
#include <string.h>

int uril_path_ok(const char *path, size_t len) {
    if (!path || !len || len >= PATH_MAX || path[0] != '/' || strlen(path) != len) return 0;
    return !memchr(path, '\n', len) && !memchr(path, '\r', len);
}

/* RFC 3986 unreserved characters and the separator stay as they are; every other byte is %XX */
static int keep(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~' || c == '/';
}

size_t uril_encode(const char *const *paths, int n, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    if (!out || !cap || n < 1 || n > URIL_MAX_PATHS) return 0;
    for (int i = 0; i < n; i++) {
        const char *p = paths[i];
        if (!p || !uril_path_ok(p, strlen(p))) return 0;
        if (o + 7 >= cap) return 0;
        memcpy(out + o, "file://", 7);
        o += 7;
        for (; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (keep(c)) { if (o + 1 >= cap) return 0; out[o++] = (char)c; continue; }
            if (o + 3 >= cap) return 0;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
        if (o + 2 >= cap) return 0;
        out[o++] = '\r';
        out[o++] = '\n';
    }
    out[o] = 0;
    return o;
}

size_t uril_plain(const char *const *paths, int n, char *out, size_t cap) {
    size_t o = 0;
    if (!out || !cap || n < 1 || n > URIL_MAX_PATHS) return 0;
    for (int i = 0; i < n; i++) {
        size_t l = paths[i] ? strlen(paths[i]) : 0;
        if (!uril_path_ok(paths[i], l) || o + l + (i + 1 < n) >= cap) return 0;   /* the line, its newline and the NUL */
        memcpy(out + o, paths[i], l);
        o += l;
        if (i + 1 < n) out[o++] = '\n';
    }
    out[o] = 0;
    return o;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* One line (without its end) → the path, percent-decoded, in out: its length, 0 to skip the line, or -1 if it
 * does not fit in cap. */
static long decode_line(const char *s, size_t n, char *out, size_t cap) {
    if (n < 7 || strncmp(s, "file://", 7)) return 0;          /* the scheme is case-insensitive, but every app writes it so */
    s += 7;
    n -= 7;
    if (n >= 9 && !strncmp(s, "localhost", 9)) { s += 9; n -= 9; }
    if (!n || s[0] != '/') return 0;                           /* another host, or no path */
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '?' || c == '#') break;                       /* a query or a fragment is not part of the path */
        if (c == '%') {
            int h = i + 2 < n ? hexval(s[i + 1]) : -1, l = i + 2 < n ? hexval(s[i + 2]) : -1;
            if (h < 0 || l < 0) return 0;
            c = (char)(h << 4 | l);
            i += 2;
        }
        if (!c || c == '\n' || c == '\r') return 0;
        if (o + 1 >= cap) return -1;
        out[o++] = c;
    }
    out[o] = 0;
    return o < PATH_MAX ? (long)o : 0;
}

int uril_decode(const char *data, size_t len, char *out, size_t cap, int max) {
    int n = 0;
    size_t o = 0;
    if (!data || !out || !cap) return -1;
    for (size_t i = 0; i < len && n < max;) {
        size_t e = i;
        while (e < len && data[e] != '\n') e++;
        size_t l = e - i;
        if (l && data[i + l - 1] == '\r') l--;
        if (l && data[i] != '#') {
            long got = decode_line(data + i, l, out + o, cap - o);
            if (got < 0) return -1;
            if (got) { o += (size_t)got + 1; n++; }
        }
        i = e + 1;
    }
    return n;
}
