/* termconf.c — the Terminal's configuration file (termconf.h). */
#include "termconf.h"
#include "fsops.h"
#include "errcode.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char home[PATH_MAX], def[PATH_MAX], custom[PATH_MAX];

static void seterr(char *e, size_t n, const char *code) { if (e && n) lerr_set(e, n, code, NULL); }

void termconf_init(const char *h) {
    snprintf(home, sizeof home, "%s", h ? h : "");
    if (snprintf(def, sizeof def, "%s/.config/lordos/terminal.json", home) >= (int)sizeof def) def[0] = 0;
    custom[0] = 0;
    char s[PATH_MAX];
    fsops_protect_file(FSOPS_PF_TERMINAL, def);
    if (snprintf(s, sizeof s, "%s/.config/lordos/settings.json", home) < (int)sizeof s)
        fsops_protect_file(FSOPS_PF_SETTINGS, s);       /* its .tmp and the migration's .bak */
    fsops_protect_file(FSOPS_PF_TERMINAL_CUSTOM, "");
}

const char *termconf_path(void) { return custom[0] ? custom : def; }
const char *termconf_cache(void) { return def; }
int termconf_is_custom(void) { return custom[0] != 0; }

/* lexically clean: absolute, no empty, "." or ".." components */
static int clean_path(const char *p) {
    if (p[0] != '/') return 0;
    for (const char *s = p; *s;) {
        while (*s == '/') s++;
        const char *e = strchr(s, '/');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if ((l == 1 && s[0] == '.') || (l == 2 && s[0] == '.' && s[1] == '.') || (e && l == 0)) return 0;
        if (!e) break;
        if (e[1] == '/' || e[1] == 0) return 0;
        s = e + 1;
    }
    return 1;
}

int termconf_set_custom(const char *path, char *err, size_t n) {
    if (!path || !path[0]) {
        custom[0] = 0;
        fsops_protect_file(FSOPS_PF_TERMINAL_CUSTOM, "");
        return 0;
    }
    size_t l = strlen(path);
    if (l >= PATH_MAX - 32 || l < 7 || strcmp(path + l - 5, ".json") || !clean_path(path)) { seterr(err, n, "termconf.badPath"); return -1; }
    const char *base = strrchr(path, '/') + 1;
    if (base[0] == '.') { seterr(err, n, "termconf.badPath"); return -1; }
    /* the folder as it really is (a link resolved) when it's there; as written when it isn't (not mounted yet) */
    char dir[PATH_MAX], real[PATH_MAX], full[PATH_MAX];
    snprintf(dir, sizeof dir, "%.*s", (int)(base - 1 - path), path);
    if (!dir[0]) { seterr(err, n, "termconf.badPath"); return -1; }
    if (realpath(dir, real)) {
        if (snprintf(full, sizeof full, "%s/%s", real, base) >= (int)sizeof full) { seterr(err, n, "termconf.badPath"); return -1; }
    } else snprintf(full, sizeof full, "%s", path);
    if (!fsops_allowed(full)) { seterr(err, n, "fs.outsideHome"); return -1; }
    if (!strcmp(full, def)) { custom[0] = 0; fsops_protect_file(FSOPS_PF_TERMINAL_CUSTOM, ""); return 0; }
    /* not a place that is protected for another reason (settings.json, the default file's family…); the current
     * choice doesn't count against itself */
    char was[PATH_MAX];
    snprintf(was, sizeof was, "%s", custom);
    fsops_protect_file(FSOPS_PF_TERMINAL_CUSTOM, "");
    if (fsops_protected(full) || fsops_protected_below(full)) {
        fsops_protect_file(FSOPS_PF_TERMINAL_CUSTOM, was);
        seterr(err, n, "fs.protected");
        return -1;
    }
    snprintf(custom, sizeof custom, "%s", full);
    fsops_protect_file(FSOPS_PF_TERMINAL_CUSTOM, custom);
    return 0;
}

static int dir_of(const char *path, char *out, size_t n) {
    const char *s = strrchr(path, '/');
    if (!s || s == path) return -1;
    return snprintf(out, n, "%.*s", (int)(s - path), path) < (int)n ? 0 : -1;
}

char *termconf_read(const char *path, size_t *len, double *mtime, char *err, size_t n) {
    if (!path || !path[0]) { seterr(err, n, "termconf.badPath"); return NULL; }
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        int e = errno;
        char d[PATH_MAX];
        struct stat st;
        if (e == ENOENT) seterr(err, n, !dir_of(path, d, sizeof d) && !stat(d, &st) && S_ISDIR(st.st_mode) ? "termconf.missing" : "termconf.unavailable");
        else if (e == EACCES || e == EPERM) seterr(err, n, "termconf.denied");
        else if (e == ELOOP) seterr(err, n, "termconf.notRegular");
        else seterr(err, n, "termconf.unavailable");
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); seterr(err, n, "termconf.notRegular"); return NULL; }
    if (st.st_size > TERMCONF_MAX) { close(fd); seterr(err, n, "termconf.tooLarge"); return NULL; }
    char *buf = malloc((size_t)st.st_size + 1);
    size_t got = 0;
    while (buf && got < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!buf || got != (size_t)st.st_size) { free(buf); seterr(err, n, "termconf.unavailable"); return NULL; }
    buf[got] = 0;
    if (len) *len = got;
    if (mtime) *mtime = st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9;
    return buf;
}

static int write_all(int fd, const char *p, size_t len) {
    for (size_t off = 0; off < len;) {
        ssize_t w = write(fd, p + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

static void fsync_dir(const char *dir) {
    int d = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (d >= 0) { fsync(d); close(d); }
}

/* a copy of src at dst (0600), atomically; 0 or -1 */
static int copy_file(const char *src, const char *dst) {
    size_t len;
    char e[64];
    char *txt = termconf_read(src, &len, NULL, e, sizeof e);
    if (!txt) return -1;
    char tmp[PATH_MAX + 32];
    snprintf(tmp, sizeof tmp, "%s.tmp-XXXXXX", dst);
    int fd = mkstemp(tmp);
    int ok = fd >= 0 && !fchmod(fd, 0600) && !write_all(fd, txt, len) && !fsync(fd);
    if (fd >= 0) ok = !close(fd) && ok;
    if (ok) ok = !rename(tmp, dst);
    if (!ok && fd >= 0) unlink(tmp);
    free(txt);
    return ok ? 0 : -1;
}

/* path.1 … path.5: the oldest goes, the others move up, the current file is copied to .1 (the file itself is never
 * missing) */
static void rotate(const char *path) {
    struct stat st;
    if (lstat(path, &st) || !S_ISREG(st.st_mode)) return;
    char b[TERMCONF_BACKUPS + 1][PATH_MAX + 8];
    for (int i = 1; i <= TERMCONF_BACKUPS; i++) snprintf(b[i], sizeof b[i], "%s.%d", path, i);
    struct stat s1;
    if (!lstat(b[1], &s1)) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        if (t.tv_sec - s1.st_mtim.tv_sec < TERMCONF_BACKUP_EVERY) return;
    }
    unlink(b[TERMCONF_BACKUPS]);
    for (int i = TERMCONF_BACKUPS - 1; i >= 1; i--) rename(b[i], b[i + 1]);
    copy_file(path, b[1]);
}

static int do_write(const char *path, const char *txt, size_t len, int backups, double *mtime, char *err, size_t n) {
    if (!path || !path[0]) { seterr(err, n, "termconf.badPath"); return -1; }
    if (len > TERMCONF_MAX) { seterr(err, n, "termconf.tooLarge"); return -1; }
    char dir[PATH_MAX];
    if (dir_of(path, dir, sizeof dir)) { seterr(err, n, "termconf.badPath"); return -1; }
    if (!strcmp(path, def)) {               /* the default file's folders are made; a chosen file's never are */
        char d[PATH_MAX + 16];
        snprintf(d, sizeof d, "%s/.config", home);
        mkdir(d, 0700);
        mkdir(dir, 0700);
    }
    struct stat st;
    if (stat(dir, &st) || !S_ISDIR(st.st_mode)) { seterr(err, n, "termconf.unavailable"); return -1; }
    if (!lstat(path, &st) && !S_ISREG(st.st_mode)) { seterr(err, n, "termconf.notRegular"); return -1; }
    if (backups) rotate(path);
    char tmp[PATH_MAX + 32];
    snprintf(tmp, sizeof tmp, "%s.tmp-XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) { seterr(err, n, errno == EACCES || errno == EPERM || errno == EROFS ? "termconf.denied" : "termconf.unavailable"); return -1; }
    int ok = !fchmod(fd, 0600) && !write_all(fd, txt, len) && !fsync(fd);
    ok = !close(fd) && ok;
    if (ok) ok = !rename(tmp, path);
    if (!ok) { unlink(tmp); seterr(err, n, "termconf.writeFailed"); return -1; }
    fsync_dir(dir);
    if (mtime && !stat(path, &st)) *mtime = st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9;
    return 0;
}

int termconf_write(const char *path, const char *txt, size_t len, double *mtime, char *err, size_t n) {
    return do_write(path, txt, len, 1, mtime, err, n);
}

int termconf_export(const char *path, const char *txt, size_t len, char *err, size_t n) {
    size_t l = path ? strlen(path) : 0;
    if (!l || l >= PATH_MAX - 32 || !clean_path(path)) { seterr(err, n, "termconf.badPath"); return -1; }
    char dir[PATH_MAX], real[PATH_MAX], full[PATH_MAX];
    const char *base = strrchr(path, '/') + 1;
    if (!base[0] || dir_of(path, dir, sizeof dir) || !realpath(dir, real)
        || snprintf(full, sizeof full, "%s/%s", real, base) >= (int)sizeof full) { seterr(err, n, "termconf.unavailable"); return -1; }
    if (!fsops_allowed(full)) { seterr(err, n, "fs.outsideHome"); return -1; }
    if (fsops_protected(full) || fsops_protected_below(full)) { seterr(err, n, "fs.protected"); return -1; }
    return do_write(full, txt, len, 0, NULL, err, n);
}

int termconf_migrate(const char *txt, size_t len, char *err, size_t n) {
    struct stat st;
    if (!def[0]) { seterr(err, n, "termconf.badPath"); return -1; }
    if (!lstat(def, &st)) { seterr(err, n, "termconf.exists"); return -1; }
    char s[PATH_MAX + 40], bak[PATH_MAX + 80];
    snprintf(s, sizeof s, "%s/.config/lordos/settings.json", home);
    snprintf(bak, sizeof bak, "%s.pre-terminal-migration.bak", s);
    if (lstat(bak, &st) && !lstat(s, &st) && copy_file(s, bak)) { seterr(err, n, "termconf.backupFailed"); return -1; }
    double mt;
    if (termconf_write(def, txt, len, &mt, err, n)) return -1;
    size_t got;
    char *back = termconf_read(def, &got, NULL, err, n);
    int same = back && got == len && !memcmp(back, txt, len);
    free(back);
    if (!same) { unlink(def); seterr(err, n, "termconf.verifyFailed"); return -1; }
    return 0;
}
