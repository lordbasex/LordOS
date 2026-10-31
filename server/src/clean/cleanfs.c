/* cleanfs.c — see cleanfs.h. */
#include "cleanfs.h"
#include "cleanproto.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int clean_open(int dirfd, const char *rel, int flags) {
    struct open_how how = { .flags = (uint64_t)(flags | O_CLOEXEC | O_NOFOLLOW),
                            .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV };
    for (int i = 0; i < 8; i++) {                   /* EAGAIN: the kernel saw a rename during the walk */
        int fd = (int)syscall(SYS_openat2, dirfd, rel, &how, sizeof how);
        if (fd >= 0 || errno != EAGAIN) return fd;
    }
    return -1;
}

/* s..s+n is valid UTF-8 without C0, DEL or C1 controls */
static int text_ok(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i];
        if (c < 0x20 || c == 0x7f) return 0;
        if (c < 0x80) { i++; continue; }
        int k = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 ? 1 : 0;
        if (!k || c > 0xf4 || i + (size_t)k >= n) return 0;
        unsigned cp = c & (0x3fu >> k);
        for (int j = 1; j <= k; j++) {
            if ((s[i + (size_t)j] & 0xc0) != 0x80) return 0;
            cp = cp << 6 | (s[i + (size_t)j] & 0x3f);
        }
        if ((k == 2 && cp < 0x800) || (k == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            (cp >= 0x80 && cp <= 0x9f)) return 0;
        i += (size_t)k + 1;
    }
    return 1;
}

static int comp_ok(const char *s, size_t n) {
    if (!n || n > 255 || memchr(s, '/', n)) return 0;
    if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
    return text_ok((const unsigned char *)s, n);
}

int clean_name_ok(const char *name) { return comp_ok(name, strlen(name)); }

int clean_relpath_ok(const char *rel, int depth) {
    size_t len = strlen(rel);
    if (!len || len > CLEAN_PATH_MAX) return 0;
    int levels = 0;
    for (const char *p = rel;;) {
        const char *sl = strchr(p, '/');
        size_t n = sl ? (size_t)(sl - p) : strlen(p);
        if (!comp_ok(p, n)) return 0;
        if (!sl) break;
        if (++levels > depth) return 0;
        p = sl + 1;
    }
    return 1;
}

int clean_glob(const char *pattern, const char *name) {
    const char *star = strchr(pattern, '*');
    if (!star) return !strcmp(pattern, name);
    size_t head = (size_t)(star - pattern), tail = strlen(star + 1), n = strlen(name);
    return n >= head + tail && !strncmp(name, pattern, head) && !strcmp(name + n - tail, star + 1);
}

int clean_type(const char *name) {
    static const struct { int type; const char *ext; } t[] = {
        { CLEAN_TYPE_IMAGE, "png jpg jpeg gif webp bmp tif tiff svg heic avif raw cr2 nef" },
        { CLEAN_TYPE_VIDEO, "mp4 mkv webm avi mov m4v mpg mpeg ts wmv" },
        { CLEAN_TYPE_AUDIO, "mp3 flac ogg opus wav m4a aac wma" },
        { CLEAN_TYPE_DOCUMENT, "pdf doc docx odt ods odp xls xlsx ppt pptx txt md rtf epub csv" },
        { CLEAN_TYPE_ARCHIVE, "zip tar gz bz2 xz zst 7z rar deb iso img lpk" },
        { CLEAN_TYPE_CODE, "c h cc cpp hpp js jsx ts py rs go java sh json xml html css o so a" },
    };
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1]) return CLEAN_TYPE_OTHER;
    char ext[8];
    size_t n = strlen(dot + 1);
    if (n >= sizeof ext) return CLEAN_TYPE_OTHER;
    for (size_t i = 0; i <= n; i++) ext[i] = dot[1 + i] >= 'A' && dot[1 + i] <= 'Z' ? (char)(dot[1 + i] + 32) : dot[1 + i];
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) {
        for (const char *p = t[i].ext; (p = strstr(p, ext)); p += n)
            if ((p == t[i].ext || p[-1] == ' ') && (p[n] == ' ' || !p[n])) return t[i].type;
    }
    return CLEAN_TYPE_OTHER;
}

const char *clean_type_name(int type) {
    static const char *const names[CLEAN_TYPES] = { "other", "image", "video", "audio", "document", "archive", "code" };
    return type >= 0 && type < CLEAN_TYPES ? names[type] : "other";
}

int clean_under(const char *path, const char *ex) {
    size_t n = strlen(ex);
    return n > 1 && !strncmp(path, ex, n) && (!path[n] || path[n] == '/');
}

int clean_similar(uint64_t a, uint64_t b, unsigned aw, unsigned ah, unsigned bw, unsigned bh) {
    if (!aw || !ah || !bw || !bh || __builtin_popcountll(a ^ b) > CLEAN_SIMILAR_BITS) return 0;
    double x = (double)aw / ah, y = (double)bw / bh;
    return (x > y ? x / y : y / x) <= 1.0 + CLEAN_SIMILAR_ASPECT;
}
