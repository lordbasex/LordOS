/* webdrop.c — the files of a drop's text/uri-list (see webdrop.h). */
#include "webdrop.h"

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int webdrop_uri_path(const char *uri, size_t len, char *out, size_t n) {
    const char *p = uri, *end = uri + len;
    if (len < 8 || strncmp(p, "file://", 7)) return -1;
    p += 7;
    if (end - p >= 9 && !strncmp(p, "localhost", 9) && (end - p == 9 || p[9] == '/')) p += 9;
    if (p >= end || *p != '/') return -1;                    /* a host: another computer's file */
    size_t o = 0;
    for (; p < end; p++) {
        char c = *p;
        if (c == '%') {
            int a = p + 2 < end ? hex(p[1]) : -1, b = a >= 0 ? hex(p[2]) : -1;
            if (a < 0 || b < 0) return -1;
            c = (char)(a * 16 + b);
            p += 2;
        } else if (c == '?' || c == '#') return -1;           /* not a plain file address */
        if (!c || c == '\n' || c == '\r') return -1;
        if (o + 1 >= n) return -1;
        out[o++] = c;
    }
    out[o] = 0;
    return o ? 0 : -1;
}

int webdrop_place_ok(const char *real, const char *home) {
    static const char *const roots[] = { "/media/", "/mnt/", "/run/media/", "/tmp/", NULL };
    size_t hl = home ? strlen(home) : 0;
    while (hl > 1 && home[hl - 1] == '/') hl--;
    int ok = hl > 1 && home[0] == '/' && !strncmp(real, home, hl) && real[hl] == '/';
    for (int i = 0; !ok && roots[i]; i++) ok = !strncmp(real, roots[i], strlen(roots[i]));
    if (!ok || real[0] != '/') return 0;
    /* no hidden folder or file anywhere on the way (~/.ssh, ~/.config, ~/.local/share/lordos …): Files doesn't show them */
    for (const char *c = real; (c = strchr(c, '/')); c++) if (c[1] == '.' || c[1] == '/' || !c[1]) return 0;
    return 1;
}

int webdrop_files(const char *list, size_t len, const char *home, WebDropFile *out, int max) {
    int n = 0;
    if (!list || len > WEBDROP_LIST_MAX) return 0;
    if (max > WEBDROP_FILES_MAX) max = WEBDROP_FILES_MAX;
    const char *p = list, *end = list + len;
    while (p < end && n < max) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        const char *le = e ? e : end;
        size_t l = (size_t)(le - p);
        if (l && p[l - 1] == '\r') l--;
        char path[PATH_MAX], real[PATH_MAX];
        struct stat st;
        /* the list comes from whichever app started the drag: only a file of the person's own places (where it really
         * is, links resolved), a regular file before opening it (opening a device or a pipe may do something), and
         * again once open, without following a link put there in between */
        if (l && p[0] != '#' && !webdrop_uri_path(p, l, path, sizeof path) && realpath(path, real) && webdrop_place_ok(real, home) &&
            !stat(real, &st) && S_ISREG(st.st_mode)) {
            int fd = open(real, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK | O_NOFOLLOW);
            if (fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size >= 0 && (unsigned long long)st.st_size <= WEBDROP_FILE_MAX) {
                const char *base = strrchr(path, '/');
                base = base && base[1] ? base + 1 : "file";
                WebDropFile *f = &out[n++];
                f->fd = fd;
                f->size = (size_t)st.st_size;
                f->mtime_ms = (long long)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000;
                size_t bl = strlen(base);
                if (bl > WEBDROP_NAME_MAX) bl = WEBDROP_NAME_MAX;
                memcpy(f->name, base, bl);
                f->name[bl] = 0;
                fd = -1;
            }
            if (fd >= 0) close(fd);
        }
        p = e ? e + 1 : end;
    }
    return n;
}

void webdrop_close(WebDropFile *f, int n) {
    for (int i = 0; i < n; i++) if (f[i].fd >= 0) { close(f[i].fd); f[i].fd = -1; }
}
