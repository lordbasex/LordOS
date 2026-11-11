/* webdl.c — see webdl.h. */
#include "webdl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* the length of s cut to at most max bytes, not inside a UTF-8 character */
static size_t cut(const char *s, size_t len, size_t max) {
    if (len <= max) return len;
    size_t k = max;
    while (k > 0 && ((unsigned char)s[k] & 0xc0) == 0x80) k--;
    return k;
}

int webdl_name(const char *suggested, char *out, size_t n) {
    if (!out || n < 16) return -1;
    const char *s = suggested ? suggested : "";
    for (const char *p = s; *p; p++) if (*p == '/' || *p == '\\') s = p + 1;
    char buf[1024];
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 1 < sizeof buf; p++) {
        unsigned c = *p;
        if (c < 0x20 || c == 0x7f) continue;
        if (c == 0xc2 && p[1] >= 0x80 && p[1] < 0xa0) { p++; continue; }     /* C1 controls */
        buf[o++] = strchr(":*?\"<>|", (int)c) ? '_' : (char)c;
    }
    buf[o] = 0;
    size_t a = 0;
    while (buf[a] == '.' || buf[a] == ' ') a++;
    size_t z = o;
    while (z > a && (buf[z - 1] == '.' || buf[z - 1] == ' ')) z--;
    const char *b = buf + a;
    size_t len = z - a;
    size_t room = n - 1 < WEBDL_NAME_MAX ? n - 1 : WEBDL_NAME_MAX;
    if (!len) { snprintf(out, n, "download"); return 0; }
    if (len <= room) { memcpy(out, b, len); out[len] = 0; return 0; }
    /* too long: keep an extension of up to 16 bytes and cut the stem */
    const char *dot = NULL;
    for (size_t i = len; i-- > 1;) if (b[i] == '.') { dot = b + i; break; }
    size_t ext = dot && (size_t)(b + len - dot) <= 16 ? (size_t)(b + len - dot) : 0;
    size_t stem = cut(b, len - ext, room - ext);
    memcpy(out, b, stem);
    memcpy(out + stem, b + len - ext, ext);
    out[stem + ext] = 0;
    return 0;
}

int webdl_unique(int dirfd, const char *name, char *out, size_t n) {
    struct stat st;
    if (!name || !name[0] || !out) return -1;
    if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) && errno == ENOENT) {
        if (snprintf(out, n, "%s", name) >= (int)n) return -1;
        return 0;
    }
    const char *dot = strrchr(name, '.');
    if (dot == name) dot = NULL;
    size_t stem = dot ? (size_t)(dot - name) : strlen(name);
    for (int k = 2; k < 1000; k++) {
        if (snprintf(out, n, "%.*s (%d)%s", (int)stem, name, k, dot ? dot : "") >= (int)n) return -1;
        if (fstatat(dirfd, out, &st, AT_SYMLINK_NOFOLLOW) && errno == ENOENT) return 0;
    }
    return -1;
}
