/* lpk.c — LordOS native app packages (.lpk).
 *
 * An .lpk is a tar (ustar), gzip-compressed or not, with:
 *   manifest.json   id, name, version, permissions… (see docs/NATIVE-APPS.md)
 *   app.js          the already bundled app (tools/lpk.mjs), which uses React and the theme SDK
 *   icon.png        optional
 *   locales/<language>.json, *.wasm, images…   optional
 *
 * It is installed without root, in ~/.local/share/axia/apps/<id>/ (user-only). The rules:
 * only regular files and folders (no links or devices), relative paths with letters,
 * digits, '.', '-' and '_' (no "..", no components starting with '.'), at most
 * LPK_MAX_FILES files and LPK_MAX_TOTAL bytes. It is decompressed in memory with stb_image's
 * inflate, written to a new folder and swapped with the previous one via rename. */
#include "lpk.h"
#include "stb_image.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LPK_MAX_ARCHIVE (96u * 1024 * 1024)   /* the compressed file */

static void seterr(char *err, size_t n, const char *msg) { if (err && n) snprintf(err, n, "%s", msg); }

int lpk_valid_id(const char *id) {
    size_t n = strlen(id);
    if (n < 2 || n > 40 || id[0] < 'a' || id[0] > 'z') return 0;
    for (const char *p = id; *p; p++) if (!(*p >= 'a' && *p <= 'z') && !isdigit((unsigned char)*p) && *p != '-') return 0;
    return 1;
}

int lpk_valid_path(const char *rel) {
    size_t n = strlen(rel);
    if (!n || n >= sizeof ((LpkFile *)0)->path || rel[0] == '/') return 0;
    int start = 1;
    for (const char *p = rel; *p; p++) {
        if (*p == '/') { if (start) return 0; start = 1; continue; }
        if (start && *p == '.') return 0;                       /* ".", "..", hidden */
        if (!isalnum((unsigned char)*p) && !strchr("._-", *p)) return 0;
        start = 0;
    }
    return !start;
}

static unsigned long octal(const char *s, size_t n) {
    unsigned long v = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        if (s[i] == ' ') continue;
        if (s[i] < '0' || s[i] > '7') break;
        v = v * 8 + (unsigned long)(s[i] - '0');
    }
    return v;
}

/* gzip (RFC 1952) → raw bytes; NULL if not valid gzip. */
static unsigned char *gunzip(const unsigned char *in, size_t len, size_t *outlen) {
    if (len < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) return NULL;
    int flg = in[3];
    size_t p = 10;
    if (flg & 4) { if (p + 2 > len) return NULL; p += 2 + (size_t)(in[p] | in[p + 1] << 8); }
    if (flg & 8) { while (p < len && in[p]) p++; p++; }
    if (flg & 16) { while (p < len && in[p]) p++; p++; }
    if (flg & 2) p += 2;
    if (p >= len - 8) return NULL;
    uint32_t isize = (uint32_t)in[len - 4] | (uint32_t)in[len - 3] << 8 | (uint32_t)in[len - 2] << 16 | (uint32_t)in[len - 1] << 24;
    if (isize > LPK_MAX_TOTAL + (uint32_t)LPK_MAX_FILES * 1024u) return NULL;
    int out = 0;
    char *r = stbi_zlib_decode_malloc_guesssize_headerflag((const char *)in + p, (int)(len - 8 - p), (int)isize + 1, &out, 0);
    if (!r) return NULL;
    if ((uint32_t)out != isize) { free(r); return NULL; }
    *outlen = (size_t)out;
    return (unsigned char *)r;
}

int lpk_open(const char *path, Lpk *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (unsigned long long)st.st_size > LPK_MAX_ARCHIVE) {
        if (fd >= 0) close(fd);
        seterr(err, errlen, "lpk.readFailed");
        return -1;
    }
    size_t len = (size_t)st.st_size;
    unsigned char *buf = malloc(len);
    size_t got = 0;
    while (buf && got < len) {
        ssize_t r = read(fd, buf + got, len - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!buf || got != len) { free(buf); seterr(err, errlen, "lpk.readFailed"); return -1; }
    if (buf[0] == 0x1f && buf[1] == 0x8b) {
        size_t n = 0;
        unsigned char *raw = gunzip(buf, len, &n);
        free(buf);
        if (!raw) { seterr(err, errlen, "lpk.corruptGzip"); return -1; }
        buf = raw;
        len = n;
    }
    out->raw = buf;
    out->files = calloc(LPK_MAX_FILES, sizeof *out->files);
    if (!out->files) { lpk_free(out); seterr(err, errlen, "sys.outOfMemory"); return -1; }
    size_t total = 0;
    for (size_t off = 0; off + 512 <= len;) {
        const char *h = (const char *)buf + off;
        int empty = 1;
        for (int i = 0; i < 512 && empty; i++) if (h[i]) empty = 0;
        if (empty) break;                                       /* end of the tar */
        unsigned long sum = octal(h + 148, 8), calc = 0;
        for (int i = 0; i < 512; i++) calc += (i >= 148 && i < 156) ? ' ' : (unsigned char)h[i];
        if (sum != calc) { lpk_free(out); seterr(err, errlen, "lpk.corruptTar"); return -1; }
        unsigned long size = octal(h + 124, 12);
        char type = h[156];
        char name[260];
        if (!memcmp(h + 257, "ustar", 5) && h[345])
            snprintf(name, sizeof name, "%.155s/%.100s", h + 345, h);
        else snprintf(name, sizeof name, "%.100s", h);
        size_t data = off + 512;
        size_t next = data + ((size + 511) & ~511ul);
        if (next > len || size > LPK_MAX_TOTAL) { lpk_free(out); seterr(err, errlen, "lpk.corruptSize"); return -1; }
        size_t nl = strlen(name);
        while (nl && name[nl - 1] == '/') name[--nl] = 0;
        if (nl > 2 && name[0] == '.' && name[1] == '/') memmove(name, name + 2, (nl -= 2) + 1);
        if (type == '5' || (type == 0 && nl == 0)) { off = next; continue; }    /* folders: created on their own */
        if (type == 'x' || type == 'g') { off = next; continue; }               /* pax headers: ignored */
        if (type != '0' && type != 0) { lpk_free(out); seterr(err, errlen, "lpk.specialFiles"); return -1; }
        if (!lpk_valid_path(name)) { lpk_free(out); seterr(err, errlen, "lpk.badPath"); return -1; }
        if (out->nfiles >= LPK_MAX_FILES || (total += size) > LPK_MAX_TOTAL) { lpk_free(out); seterr(err, errlen, "lpk.tooLarge"); return -1; }
        if (lpk_find(out, name)) { lpk_free(out); seterr(err, errlen, "lpk.duplicateFiles"); return -1; }
        LpkFile *f = &out->files[out->nfiles++];
        snprintf(f->path, sizeof f->path, "%s", name);
        f->data = buf + data;
        f->size = size;
        off = next;
    }
    if (!out->nfiles) { lpk_free(out); seterr(err, errlen, "lpk.empty"); return -1; }
    return 0;
}

const LpkFile *lpk_find(const Lpk *p, const char *path) {
    for (int i = 0; i < p->nfiles; i++) if (!strcmp(p->files[i].path, path)) return &p->files[i];
    return NULL;
}

void lpk_free(Lpk *p) {
    free(p->raw);
    free(p->files);
    memset(p, 0, sizeof *p);
}

const char *lpk_root(void) {
    static char root[PATH_MAX];
    const char *home = getenv("HOME");
    if (!home) return NULL;
    char tmp[PATH_MAX];
    static const char *const parts[] = { "/.local", "/.local/share", "/.local/share/axia", "/.local/share/axia/apps" };
    for (size_t i = 0; i < sizeof parts / sizeof *parts; i++) {
        snprintf(tmp, sizeof tmp, "%s%s", home, parts[i]);
        if (mkdir(tmp, 0755) && errno != EEXIST) return NULL;
    }
    snprintf(root, sizeof root, "%s", tmp);
    return root;
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)ftw;
    return flag == FTW_DP ? rmdir(path) : unlink(path);
}

static int rm_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    return nftw(path, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* Creates the intermediate folders of rel inside base. */
static int make_parents(const char *base, const char *rel) {
    char path[PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/%s", base, rel);
    if (n < 0 || (size_t)n >= sizeof path) return -1;
    for (char *p = path + strlen(base) + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(path, 0755) && errno != EEXIST) return -1;
        struct stat st;
        if (lstat(path, &st) || !S_ISDIR(st.st_mode)) return -1;
        *p = '/';
    }
    return 0;
}

int lpk_install(const Lpk *p, const char *id, char *err, size_t errlen) {
    const char *root = lpk_root();
    if (!root || !lpk_valid_id(id)) { seterr(err, errlen, "lpk.installHome"); return -1; }
    char dst[PATH_MAX], tmp[PATH_MAX], old[PATH_MAX];
    snprintf(dst, sizeof dst, "%s/%s", root, id);
    snprintf(tmp, sizeof tmp, "%s/.%s.new", root, id);
    snprintf(old, sizeof old, "%s/.%s.old", root, id);
    rm_tree(tmp);
    rm_tree(old);
    if (mkdir(tmp, 0755)) { seterr(err, errlen, "lpk.mkdirFailed"); return -1; }
    for (int i = 0; i < p->nfiles; i++) {
        const LpkFile *f = &p->files[i];
        char path[PATH_MAX];
        int n = snprintf(path, sizeof path, "%s/%s", tmp, f->path);
        int fd = n > 0 && (size_t)n < sizeof path && !make_parents(tmp, f->path)
                     ? open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644) : -1;
        size_t w = 0;
        while (fd >= 0 && w < f->size) {
            ssize_t r = write(fd, f->data + w, f->size - w);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            w += (size_t)r;
        }
        if (fd < 0 || close(fd) || w != f->size) { rm_tree(tmp); seterr(err, errlen, "lpk.writeFailed"); return -1; }
    }
    struct stat st;
    int had = !lstat(dst, &st);
    if (had && rename(dst, old)) { rm_tree(tmp); seterr(err, errlen, "lpk.replaceFailed"); return -1; }
    if (rename(tmp, dst)) {
        if (had) rename(old, dst);
        rm_tree(tmp);
        seterr(err, errlen, "lpk.installFailed");
        return -1;
    }
    if (had) rm_tree(old);
    return 0;
}

int lpk_remove(const char *id) {
    const char *root = lpk_root();
    if (!root || !lpk_valid_id(id)) return -1;
    char dst[PATH_MAX];
    snprintf(dst, sizeof dst, "%s/%s", root, id);
    struct stat st;
    if (lstat(dst, &st) || !S_ISDIR(st.st_mode)) return -1;
    return rm_tree(dst);
}
