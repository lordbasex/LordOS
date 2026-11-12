/* xauth.c — the Xauthority file of the session's X cookie (see xauth.h). */
#include "xauth.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned char *put16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
    return p + 2;
}

static unsigned char *put_field(unsigned char *p, const void *s, size_t n) {
    p = put16(p, (unsigned)n);
    if (n) memcpy(p, s, n);
    return p + n;
}

size_t xauth_encode(unsigned char *out, size_t cap, const char *host, int display, const unsigned char *cookie) {
    if (!out || !cookie || display < 0) return 0;
    char num[16];
    int nl = snprintf(num, sizeof num, "%d", display);
    if (nl <= 0 || nl >= (int)sizeof num) return 0;
    size_t hl = host ? strlen(host) : 0;
    if (hl > 255) return 0;                                 /* a host name is at most 64; anything longer is wrong */
    size_t pl = sizeof XAUTH_PROTO - 1;
    size_t entry = 2 + 2 + 2 + (size_t)nl + 2 + pl + 2 + XAUTH_COOKIE_LEN;   /* without the address */
    size_t need = (hl ? entry + hl : 0) + entry;
    if (need > cap) return 0;
    unsigned char *p = out;
    if (hl) {
        p = put16(p, XAUTH_FAMILY_LOCAL);
        p = put_field(p, host, hl);
        p = put_field(p, num, (size_t)nl);
        p = put_field(p, XAUTH_PROTO, pl);
        p = put_field(p, cookie, XAUTH_COOKIE_LEN);
    }
    p = put16(p, XAUTH_FAMILY_WILD);
    p = put_field(p, "", 0);
    p = put_field(p, num, (size_t)nl);
    p = put_field(p, XAUTH_PROTO, pl);
    p = put_field(p, cookie, XAUTH_COOKIE_LEN);
    return (size_t)(p - out);
}

int xauth_create_at(int dfd, const char *tmp, const unsigned char *data, size_t len) {
    if (dfd < 0 || !tmp || !tmp[0] || strchr(tmp, '/') || (!data && len)) { errno = EINVAL; return -1; }
    int fd = openat(dfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int ok = fchmod(fd, 0600) == 0;                         /* the umask only takes bits away; this says it */
    for (size_t o = 0; ok && o < len;) {
        ssize_t w = write(fd, data + o, len - o);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { ok = 0; break; }
        o += (size_t)w;
    }
    if (ok && fsync(fd)) ok = 0;
    int e = errno;
    close(fd);
    if (!ok) { unlinkat(dfd, tmp, 0); errno = e ? e : EIO; return -1; }
    return 0;
}

int xauth_write(const char *dir, const char *name, const unsigned char *data, size_t len, char *path, size_t pathlen) {
    if (!dir || dir[0] != '/' || !name || !name[0] || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..") ||
        strlen(name) > 64 || !path || !pathlen) { errno = EINVAL; return -1; }
    if ((size_t)snprintf(path, pathlen, "%s/%s", dir, name) >= pathlen) { path[0] = 0; errno = ENAMETOOLONG; return -1; }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) { path[0] = 0; return -1; }
    struct stat st;
    if (fstat(dfd, &st) || st.st_uid != getuid() || (st.st_mode & 077)) {
        close(dfd);
        path[0] = 0;
        errno = EPERM;                                      /* not a private folder of this user */
        return -1;
    }
    unsigned char rnd[6];
    char tmp[96];
    int r = -1;
    for (int tries = 0; tries < 4 && r; tries++) {
        if (getrandom(rnd, sizeof rnd, 0) != (ssize_t)sizeof rnd) { r = -1; break; }
        snprintf(tmp, sizeof tmp, "%s.tmp-%02x%02x%02x%02x%02x%02x", name, rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5]);
        r = xauth_create_at(dfd, tmp, data, len);
        if (r && errno != EEXIST) break;
    }
    if (!r && renameat(dfd, tmp, dfd, name)) { int e = errno; unlinkat(dfd, tmp, 0); errno = e; r = -1; }
    int e = errno;
    close(dfd);
    if (r) { path[0] = 0; errno = e; }
    return r;
}
