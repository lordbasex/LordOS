/* engine.c — the backup engine: hard-linked snapshots, retention and restore (see engine.h and
 * docs/SYSTEM.md, "Backups").
 *
 * Rules:
 *   - a symbolic link is never followed: the walk uses lstat and opens with O_NOFOLLOW; links are
 *     stored as links (their text is copied, not their target);
 *   - never crosses into another filesystem (a disk mounted inside the home folder);
 *   - fifos, sockets and devices are skipped;
 *   - a file is copied to "<name>.bkpart~" and renamed when done, so a final name is always a
 *     complete file (which is what makes resuming an interrupted run possible);
 *   - deletion only happens inside the machine folder and with validated names. */
#include "engine.h"
#include "../errcode.h"
#include "../fsops.h"
#include "crypt.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define CHUNK (8u << 20)
#define MAX_DEPTH 128

__attribute__((format(printf, 3, 4)))
static int pathf(char *out, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= n) { if (n) out[0] = 0; return -1; }
    return 0;
}

static int under(const char *p, const char *root) {
    size_t n = strlen(root);
    return n > 0 && !strncmp(p, root, n) && (p[n] == 0 || p[n] == '/' || root[n - 1] == '/');
}

static int mkdir_p(const char *path, mode_t mode) {
    char p[PATH_MAX];
    if (pathf(p, sizeof p, "%s", path)) return -1;
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(p, mode) && errno != EEXIST) return -1;
        *s = '/';
    }
    return mkdir(p, mode) && errno != EEXIST ? -1 : 0;
}

static int rm_cb(const char *p, const struct stat *st, int flag, struct FTW *f) {
    (void)st; (void)f;
    if (flag == FTW_DP) return rmdir(p) && errno != ENOENT ? -1 : 0;
    if (flag == FTW_DNR) return -1;
    return unlink(p) && errno != ENOENT ? -1 : 0;
}

static int rm_tree(const char *p) {
    struct stat st;
    if (lstat(p, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(p);
    /* folders without write permission (copied as they were) could not be emptied */
    return nftw(p, rm_cb, 32, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
}

static void rm_cb_chmod_dirs(const char *p) {
    /* before deleting: folders stored with mode 0500 do not let their contents be removed */
    DIR *d = opendir(p);
    if (!d) { chmod(p, 0700); d = opendir(p); if (!d) return; }
    chmod(p, 0700);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (e->d_type != DT_DIR && e->d_type != DT_UNKNOWN) continue;
        char c[PATH_MAX];
        struct stat st;
        if (pathf(c, sizeof c, "%s/%s", p, e->d_name) || lstat(c, &st) || !S_ISDIR(st.st_mode)) continue;
        rm_cb_chmod_dirs(c);
    }
    closedir(d);
}

static int rm_tree_force(const char *p) {
    if (rm_tree(p) == 0) return 0;
    rm_cb_chmod_dirs(p);
    return rm_tree(p);
}

/* ---- snapshot names ---- */

int bk_id_valid(const char *id) {
    static const char pat[] = "dddd-dd-dd-dddddd";
    if (strlen(id) != sizeof pat - 1) return 0;
    for (size_t i = 0; pat[i]; i++) {
        if (pat[i] == 'd' ? (id[i] < '0' || id[i] > '9') : id[i] != pat[i]) return 0;
    }
    return 1;
}

time_t bk_id_time(const char *id) {
    if (!bk_id_valid(id)) return -1;
    struct tm tm = { 0 };
    if (sscanf(id, "%4d-%2d-%2d-%2d%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return -1;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

void bk_id_make(time_t t, char out[BK_ID_LEN]) {
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, BK_ID_LEN, "%04u-%02u-%02u-%02u%02u%02u", (unsigned)(tm.tm_year + 1900) % 10000u, (unsigned)(tm.tm_mon + 1) % 100u,
             (unsigned)tm.tm_mday % 100u, (unsigned)tm.tm_hour % 100u, (unsigned)tm.tm_min % 100u, (unsigned)tm.tm_sec % 100u);
}

static int cmp_id(const void *a, const void *b) { return strcmp(a, b); }

int bk_list(const char *machine, char (*ids)[BK_ID_LEN], int max) {
    DIR *d = opendir(machine);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (!bk_id_valid(e->d_name)) continue;
        char p[PATH_MAX];
        struct stat st;
        if (pathf(p, sizeof p, "%s/%s", machine, e->d_name) || lstat(p, &st) || !S_ISDIR(st.st_mode)) continue;
        memcpy(ids[n++], e->d_name, BK_ID_LEN);
    }
    closedir(d);
    qsort(ids, (size_t)n, BK_ID_LEN, cmp_id);
    return n;
}

int bk_rel_valid(const char *rel) {
    size_t n = strlen(rel);
    if (n >= PATH_MAX / 2 || rel[0] == '/') return 0;
    if (!n) return 1;
    if (rel[n - 1] == '/') return 0;
    const char *s = rel;
    while (*s) {
        const char *e = strchr(s, '/');
        size_t k = e ? (size_t)(e - s) : strlen(s);
        if (!k || (k == 1 && s[0] == '.') || (k == 2 && s[0] == '.' && s[1] == '.')) return 0;
        for (size_t i = 0; i < k; i++) if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f) return 0;
        s += k;
        if (*s == '/') s++;
    }
    return 1;
}

/* ---- retention ---- */

static long floordiv(long a, long b) { long q = a / b; return (a % b && (a < 0) != (b < 0)) ? q - 1 : q; }

void bk_retention(const time_t *t, int n, time_t now, long tz_off, int *keep) {
    for (int i = 0; i < n; i++) keep[i] = 0;
    if (n <= 0) return;
    keep[n - 1] = 1;
    long lastday = LONG_MIN, lastweek = LONG_MIN;
    for (int i = n - 1; i >= 0; i--) {
        long age = (long)(now - t[i]);
        if (age < 86400) { keep[i] = 1; continue; }
        long lt = (long)t[i] + tz_off;
        if (age < 30L * 86400) {
            long day = floordiv(lt, 86400);
            if (day != lastday) { keep[i] = 1; lastday = day; }
        } else {
            long week = floordiv(lt - 4L * 86400, 7L * 86400);      /* weeks start on Monday (1970-01-05) */
            if (week != lastweek) { keep[i] = 1; lastweek = week; }
        }
    }
}

static uint64_t free_bytes(const char *p) {
    struct statvfs v;
    if (statvfs(p, &v)) return UINT64_MAX;
    return (uint64_t)v.f_bavail * v.f_frsize;
}

int bk_delete(const char *machine, const char *id) {
    if (!bk_id_valid(id)) return -1;
    char p[PATH_MAX];
    if (pathf(p, sizeof p, "%s/%s", machine, id)) return -1;
    /* first out of the list (rename is atomic), then deleted at leisure */
    char gone[PATH_MAX];
    if (pathf(gone, sizeof gone, "%s/.deleting-%s", machine, id) || rename(p, gone)) return -1;
    return rm_tree_force(gone);
}

int bk_prune(const char *machine, time_t now, uint64_t need) {
    static char ids[BK_MAX_SNAPS][BK_ID_LEN];
    static time_t t[BK_MAX_SNAPS];
    static int keep[BK_MAX_SNAPS];
    int n = bk_list(machine, ids, BK_MAX_SNAPS), gone = 0;
    for (int i = 0; i < n; i++) t[i] = bk_id_time(ids[i]);
    struct tm tm;
    localtime_r(&now, &tm);
    bk_retention(t, n, now, tm.tm_gmtoff, keep);
    for (int i = 0; i < n; i++) if (!keep[i] && !bk_delete(machine, ids[i])) { gone++; ids[i][0] = 0; }
    /* leftovers of interrupted deletions */
    DIR *d = opendir(machine);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, ".deleting-", 10)) continue;
        char p[PATH_MAX];
        if (!pathf(p, sizeof p, "%s/%s", machine, e->d_name)) rm_tree_force(p);
    }
    if (d) closedir(d);
    /* short on space: the oldest go, never the newest */
    if (need) {
        int left = 0;
        for (int i = 0; i < n; i++) if (ids[i][0]) left++;
        for (int i = 0; i < n && left > 1 && free_bytes(machine) < need; i++) {
            if (!ids[i][0]) continue;
            if (!bk_delete(machine, ids[i])) { gone++; left--; ids[i][0] = 0; }
        }
    }
    return gone;
}

/* ---- snapshot ---- */

typedef struct {
    BkRun *r;
    char src[PATH_MAX], dst[PATH_MAX];          /* roots */
    char link1[PATH_MAX], link2[PATH_MAX];      /* link sources: interrupted run, previous snapshot */
    dev_t home_dev;
    dev_t skip_dev;
    ino_t skip_ino;
    int has_skip, counting, stop, fatal;
} Walk;

static int excluded(const Walk *w, const char *rel) {
    for (int i = 0; i < w->r->nexcl; i++) if (under(rel, w->r->excl[i])) return 1;
    return 0;
}

/* a: the original; b: the one in the snapshot (encrypted: its size is the encrypted size of a) */
static int same_file(const struct stat *a, const struct stat *b, const uint8_t *key) {
    uint64_t want = key ? bkc_enc_size((uint64_t)a->st_size) : (uint64_t)a->st_size;
    return S_ISREG(b->st_mode) && (uint64_t)b->st_size == want && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           (a->st_mtim.tv_nsec == b->st_mtim.tv_nsec || b->st_mtim.tv_nsec == 0) &&
           (a->st_mode & 07777) == (b->st_mode & 07777);
}

static int check(Walk *w) {
    if (w->stop) return 1;
    if ((w->r->cancel && *w->r->cancel) || (w->r->checkpoint && w->r->checkpoint(w->r))) w->stop = 1;
    return w->stop;
}

/* Hard-links dst to base/rel if it is the same file. 1: linked. */
static int try_link(const char *base, const char *rel, const struct stat *st, const char *dst, const uint8_t *key) {
    if (!base[0]) return 0;
    char p[PATH_MAX];
    struct stat o;
    if (pathf(p, sizeof p, "%s/%s", base, rel) || lstat(p, &o) || !same_file(st, &o, key)) return 0;
    if (link(p, dst) == 0) return 1;
    if (errno == EEXIST) { unlink(dst); return link(p, dst) == 0; }
    return 0;                           /* EMLINK, EPERM (FAT/exFAT): copy instead */
}

static int enc_tick(void *ud, size_t n) {
    Walk *w = ud;
    w->r->bytes += n;
    w->r->copied_bytes += n;
    return check(w);
}

/* -1 file error, -2 out of space (fatal), 1 interrupted, 0 ok */
static int copy_data(Walk *w, int in, int out) {
    char *buf = NULL;
    int use_cfr = 1;
    for (;;) {
        ssize_t k;
        if (use_cfr) {
            k = copy_file_range(in, NULL, out, NULL, CHUNK, 0);
            if (k < 0 && (errno == EXDEV || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP || errno == EBADF)) {
                use_cfr = 0;
                continue;
            }
        } else {
            if (!buf && !(buf = malloc(1 << 20))) return -1;
            k = read(in, buf, 1 << 20);
            for (ssize_t o = 0; k > 0 && o < k;) {
                ssize_t wr = write(out, buf + o, (size_t)(k - o));
                if (wr < 0 && errno == EINTR) continue;
                if (wr <= 0) { k = -1; break; }
                o += wr;
            }
        }
        if (k < 0 && errno == EINTR) continue;
        if (k < 0) { int e = errno; free(buf); return e == ENOSPC || e == EDQUOT ? -2 : -1; }
        if (k == 0) break;
        w->r->bytes += (uint64_t)k;
        w->r->copied_bytes += (uint64_t)k;
        if (check(w)) { free(buf); return 1; }
    }
    free(buf);
    return 0;
}

static int copy_file(Walk *w, const char *src, const char *dst, const struct stat *st) {
    char tmp[PATH_MAX];
    if (pathf(tmp, sizeof tmp, "%s.bkpart~", dst)) return -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        int in = open(src, O_RDONLY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
        if (in < 0) return -1;
        struct stat a;
        if (fstat(in, &a) || !S_ISREG(a.st_mode)) { close(in); return -1; }
        int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (out < 0) { int e = errno; close(in); return e == ENOSPC || e == EDQUOT ? -2 : -1; }
        posix_fadvise(in, 0, 0, POSIX_FADV_SEQUENTIAL);
        uint64_t before = w->r->bytes;
        int r = w->r->key ? bkc_encrypt_fd(in, out, w->r->key, enc_tick, w) : copy_data(w, in, out);
        struct stat b;
        int changed = !fstat(in, &b) && (b.st_size != a.st_size || b.st_mtim.tv_sec != a.st_mtim.tv_sec || b.st_mtim.tv_nsec != a.st_mtim.tv_nsec);
        /* do not pollute the desktop's page cache with the copy */
        posix_fadvise(in, 0, 0, POSIX_FADV_DONTNEED);
        if (r == 0) {
            fchmod(out, a.st_mode & 07777);
            struct timespec ts[2] = { a.st_atim, a.st_mtim };
            futimens(out, ts);
            /* no fsync per file (too slow with many small files): syncfs at the end, before the
             * snapshot is renamed */
            posix_fadvise(out, 0, 0, POSIX_FADV_DONTNEED);
        }
        close(in);
        if (close(out) && r == 0) r = -1;
        if (r != 0) { unlink(tmp); return r; }
        if (changed && attempt < 2) { w->r->bytes = before; continue; }   /* changed while being copied */
        if (rename(tmp, dst)) { unlink(tmp); return -1; }
        (void)st;
        return 0;
    }
    return -1;
}

static void walk(Walk *w, const char *rel, int depth);

static void entry(Walk *w, const char *rel, int depth) {
    if (check(w) || w->fatal) return;
    char src[PATH_MAX], dst[PATH_MAX];
    if (pathf(src, sizeof src, "%s/%s", w->src, rel) || pathf(dst, sizeof dst, "%s/%s", w->dst, rel)) { w->r->errors++; return; }
    struct stat st;
    if (lstat(src, &st)) { if (errno != ENOENT) w->r->errors++; return; }
    if (S_ISDIR(st.st_mode)) {
        if (st.st_dev != w->home_dev || (w->has_skip && st.st_dev == w->skip_dev && st.st_ino == w->skip_ino)) return;
        if (depth >= MAX_DEPTH) { w->r->errors++; return; }
        if (!w->counting && mkdir(dst, 0700) && errno != EEXIST) { w->r->errors++; return; }
        walk(w, rel, depth + 1);
        if (!w->counting) {
            chmod(dst, (st.st_mode & 07777) | 0700);            /* the owner can always get back in */
            struct timespec ts[2] = { st.st_atim, st.st_mtim };
            utimensat(AT_FDCWD, dst, ts, AT_SYMLINK_NOFOLLOW);
        }
        return;
    }
    if (S_ISREG(st.st_mode)) {
        if (w->counting) { w->r->total_files++; w->r->total_bytes += (uint64_t)st.st_size; return; }
        if (w->r->progress) w->r->progress(w->r, rel);
        w->r->files++;
        if (try_link(w->link1, rel, &st, dst, w->r->key) || try_link(w->link2, rel, &st, dst, w->r->key)) {
            w->r->linked++;
            w->r->bytes += (uint64_t)st.st_size;
            return;
        }
        int r = copy_file(w, src, dst, &st);
        if (r == -2) {
            /* out of space: delete old snapshots (never the newest) and retry once */
            if (bk_prune(w->r->machine, time(NULL), (uint64_t)st.st_size + (w->r->min_free ? w->r->min_free : (512u << 20))) > 0)
                r = copy_file(w, src, dst, &st);
            if (r == -2) { w->fatal = 1; lerr_set(w->r->error, sizeof w->r->error, "backup.noSpace", NULL); return; }
        }
        if (r == 0) w->r->copied++;
        else if (r < 0) w->r->errors++;
        return;
    }
    if (S_ISLNK(st.st_mode)) {
        if (w->counting) { w->r->total_files++; return; }
        w->r->files++;
        char target[PATH_MAX];
        ssize_t k = readlink(src, target, sizeof target - 1);
        if (k < 0) { w->r->errors++; return; }
        target[k] = 0;
        unlink(dst);
        if (symlink(target, dst)) { w->r->errors++; return; }   /* FAT has no symlinks */
        struct timespec ts[2] = { st.st_atim, st.st_mtim };
        utimensat(AT_FDCWD, dst, ts, AT_SYMLINK_NOFOLLOW);
        return;
    }
    if (!w->counting) w->r->skipped++;   /* fifos, sockets, devices */
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void walk(Walk *w, const char *rel, int depth) {
    char dir[PATH_MAX];
    if (pathf(dir, sizeof dir, "%s%s%s", w->src, rel[0] ? "/" : "", rel)) { w->r->errors++; return; }
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
    if (!d) { if (fd >= 0) close(fd); w->r->errors++; return; }
    /* names first (the folder is closed before descending: fds do not pile up) */
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 64;
            char **nn = realloc(names, nc * sizeof *nn);
            if (!nn) break;
            names = nn;
            cap = nc;
        }
        if (!(names[n] = strdup(e->d_name))) break;
        n++;
    }
    closedir(d);
    if (n) qsort(names, n, sizeof *names, cmp_str);
    for (size_t i = 0; i < n; i++) {
        char r[PATH_MAX];
        size_t len = strlen(names[i]);
        int skip = len > 8 && !strcmp(names[i] + len - 8, ".bkpart~");
        if (!skip && !pathf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", names[i]) && !excluded(w, r)) {
            if (w->r->hold && w->r->hold(w->r, r)) { if (!w->counting) w->r->skipped++; }
            else entry(w, r, depth);
        }
        free(names[i]);
    }
    free(names);
}

int bk_snapshot(BkRun *r, time_t now) {
    r->files = r->bytes = r->copied = r->linked = r->copied_bytes = r->errors = r->skipped = 0;
    r->total_files = r->total_bytes = 0;
    r->error[0] = r->id[0] = 0;
    static Walk w;
    memset(&w, 0, sizeof w);
    w.r = r;
    struct stat hs;
    if (pathf(w.src, sizeof w.src, "%s", r->home) || lstat(w.src, &hs) || !S_ISDIR(hs.st_mode)) {
        lerr_set(r->error, sizeof r->error, "backup.sourceUnreadable", NULL);
        return -1;
    }
    w.home_dev = hs.st_dev;
    if (r->skip_real && r->skip_real[0]) {
        struct stat ss;
        if (!stat(r->skip_real, &ss)) { w.has_skip = 1; w.skip_dev = ss.st_dev; w.skip_ino = ss.st_ino; }
    }
    if (mkdir_p(r->machine, 0700)) { lerr_set(r->error, sizeof r->error, "backup.createFailed", "path", r->machine, "errno", lerr_errno(errno), NULL); return -1; }

    static char ids[BK_MAX_SNAPS][BK_ID_LEN];
    int n = bk_list(r->machine, ids, BK_MAX_SNAPS);
    bk_id_make(now, r->id);
    while (n && strcmp(r->id, ids[n - 1]) <= 0) bk_id_make(++now, r->id);   /* always later than the previous one */
    if (n) pathf(w.link2, sizeof w.link2, "%s/%s", r->machine, ids[n - 1]);

    /* interrupted runs: the newest is a link source; the others are dropped */
    char old[PATH_MAX] = "", oldname[256] = "";
    DIR *d = opendir(r->machine);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, ".inprogress-", 12) || !bk_id_valid(e->d_name + 12)) continue;
        if (strcmp(e->d_name, oldname) > 0) {
            if (oldname[0]) { char p[PATH_MAX]; if (!pathf(p, sizeof p, "%s/%s", r->machine, oldname)) rm_tree_force(p); }
            snprintf(oldname, sizeof oldname, "%s", e->d_name);
        } else {
            char p[PATH_MAX];
            if (!pathf(p, sizeof p, "%s/%s", r->machine, e->d_name)) rm_tree_force(p);
        }
    }
    if (d) closedir(d);
    if (oldname[0]) {
        /* renamed so it does not clash with the new one and is known to be the old one */
        char p[PATH_MAX];
        if (!pathf(p, sizeof p, "%s/%s", r->machine, oldname) && !pathf(old, sizeof old, "%s/.resume-%s", r->machine, oldname + 12))
            if (rename(p, old)) old[0] = 0;
    }
    {   /* also a .resume- left by an interruption during a resume */
        d = opendir(r->machine);
        while (d && (e = readdir(d))) {
            if (strncmp(e->d_name, ".resume-", 8)) continue;
            char p[PATH_MAX];
            if (pathf(p, sizeof p, "%s/%s", r->machine, e->d_name) || !strcmp(p, old)) continue;
            if (!old[0]) snprintf(old, sizeof old, "%s", p); else rm_tree_force(p);
        }
        if (d) closedir(d);
    }
    if (old[0]) snprintf(w.link1, sizeof w.link1, "%s", old);

    /* minimum free space before starting */
    uint64_t min_free = r->min_free ? r->min_free : (512u << 20);
    if (free_bytes(r->machine) < min_free) bk_prune(r->machine, now, min_free);

    /* count (for progress) */
    w.counting = 1;
    walk(&w, "", 0);
    w.counting = 0;
    if (w.stop) return 1;

    if (pathf(w.dst, sizeof w.dst, "%s/.inprogress-%s", r->machine, r->id) || mkdir(w.dst, 0700)) {
        lerr_set(r->error, sizeof r->error, "backup.destUnwritable", "errno", lerr_errno(errno), NULL);
        return -1;
    }
    walk(&w, "", 0);
    if (w.fatal) {
        /* left half done, to resume when there is space */
        return -1;
    }
    if (w.stop) return 1;
    chmod(w.dst, (hs.st_mode & 07777) | 0700);
    int dfd = open(w.dst, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) { syncfs(dfd); close(dfd); }
    char final[PATH_MAX];
    if (pathf(final, sizeof final, "%s/%s", r->machine, r->id) || rename(w.dst, final)) {
        lerr_set(r->error, sizeof r->error, "backup.snapshotFailed", "errno", lerr_errno(errno), NULL);
        return -1;
    }
    int mfd = open(r->machine, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (mfd >= 0) { fsync(mfd); close(mfd); }
    if (old[0]) rm_tree_force(old);
    bk_prune(r->machine, now, 0);
    return 0;
}

/* ---- restore ---- */

typedef struct {
    BkRestore *o;
    const char *suffix;
    int replace;
    char snapreal[PATH_MAX];
} Rest;

static int rcancel(Rest *x) { return x->o->cancel && *x->o->cancel; }

/* May dst be written? Its (resolved) folder must be inside the lord.fs roots and the final name
 * must not fall inside a protected folder. */
static int writable_target(const char *dst) {
    char parent[PATH_MAX], real[PATH_MAX], full[PATH_MAX + 256];
    if (pathf(parent, sizeof parent, "%s", dst)) return 0;
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return 0;
    *slash = 0;
    if (!realpath(parent, real) || !fsops_allowed(real)) return 0;
    if (pathf(full, sizeof full, "%s/%s", real, slash + 1)) return 0;
    return !fsops_protected(full) && !fsops_protected(real);
}

/* "photo.jpg" + " (restored)" → "photo (restored).jpg", and if that exists "photo (restored) 2.jpg" */
static int alt_name(const char *dst, const char *suffix, char *out, size_t n) {
    const char *slash = strrchr(dst, '/');
    const char *name = slash ? slash + 1 : dst;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) dot = name + strlen(name);
    for (int k = 1; k < 1000; k++) {
        char num[16] = "";
        if (k > 1) snprintf(num, sizeof num, " %d", k);
        if (pathf(out, n, "%.*s%s%s%s", (int)(dot - dst), dst, suffix, num, dot)) return -1;
        struct stat st;
        if (lstat(out, &st) && errno == ENOENT) return 0;
    }
    return -1;
}

static int rest_tick(void *ud, size_t n) {
    Rest *x = ud;
    x->o->bytes += n;
    return rcancel(x);
}

static int restore_file(Rest *x, const char *src, const char *dst, const struct stat *st) {
    char tmp[PATH_MAX];
    if (pathf(tmp, sizeof tmp, "%s.bkpart~", dst)) return -1;
    int in = open(src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (in < 0) return -1;
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_EXCL | O_CLOEXEC, 0600);
    if (out < 0 && errno == EEXIST) { unlink(tmp); out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_EXCL | O_CLOEXEC, 0600); }
    if (out < 0) { close(in); return -1; }
    int r = 0;
    if (x->o->key) {
        r = bkc_decrypt_fd(in, out, x->o->key, rest_tick, x);
        if (r == -3) lerr_set(x->o->error, sizeof x->o->error, "backup.verifyFailed", NULL);
        if (r < 0) r = -1;
    } else {
        char *buf = malloc(1 << 20);
        r = buf ? 0 : -1;
        while (!r) {
            ssize_t k = read(in, buf, 1 << 20);
            if (k < 0 && errno == EINTR) continue;
            if (k < 0) { r = -1; break; }
            if (k == 0) break;
            for (ssize_t o = 0; o < k;) {
                ssize_t w = write(out, buf + o, (size_t)(k - o));
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) { r = -1; break; }
                o += w;
            }
            x->o->bytes += (uint64_t)k;
            if (rcancel(x)) r = 1;
        }
        free(buf);
    }
    fchmod(out, st->st_mode & 07777);
    struct timespec ts[2] = { st->st_atim, st->st_mtim };
    futimens(out, ts);
    close(in);
    if (close(out) && !r) r = -1;
    if (r || rename(tmp, dst)) { unlink(tmp); return r ? r : -1; }
    return 0;
}

static void restore_entry(Rest *x, const char *src, const char *dst, int depth) {
    if (rcancel(x) || depth > MAX_DEPTH) return;
    struct stat st, cur;
    if (lstat(src, &st)) { x->o->errors++; return; }
    if (!writable_target(dst)) { x->o->skipped++; return; }
    int exists = lstat(dst, &cur) == 0;
    char target[PATH_MAX];
    snprintf(target, sizeof target, "%s", dst);

    if (S_ISDIR(st.st_mode)) {
        if (exists && !S_ISDIR(cur.st_mode)) {
            if (x->replace) { if (unlink(dst)) { x->o->errors++; return; } x->o->replaced++; exists = 0; }
            else if (alt_name(dst, x->suffix, target, sizeof target)) { x->o->errors++; return; }
            else { x->o->kept_both++; exists = 0; }
        }
        if (!exists && mkdir(target, 0700)) { x->o->errors++; return; }
        DIR *d = opendir(src);
        if (!d) { x->o->errors++; return; }
        struct dirent *e;
        char **names = NULL;
        size_t n = 0, cap = 0;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (n == cap) { size_t nc = cap ? cap * 2 : 64; char **nn = realloc(names, nc * sizeof *nn); if (!nn) break; names = nn; cap = nc; }
            if (!(names[n] = strdup(e->d_name))) break;
            n++;
        }
        closedir(d);
        for (size_t i = 0; i < n; i++) {
            char s2[PATH_MAX], d2[PATH_MAX];
            if (!pathf(s2, sizeof s2, "%s/%s", src, names[i]) && !pathf(d2, sizeof d2, "%s/%s", target, names[i]))
                restore_entry(x, s2, d2, depth + 1);
            free(names[i]);
        }
        free(names);
        if (!exists) {
            chmod(target, (st.st_mode & 07777) | 0700);
            struct timespec ts[2] = { st.st_atim, st.st_mtim };
            utimensat(AT_FDCWD, target, ts, AT_SYMLINK_NOFOLLOW);
        }
        return;
    }
    if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) { x->o->skipped++; return; }
    if (exists) {
        uint64_t plain = (uint64_t)st.st_size;
        if (x->o->key && S_ISREG(st.st_mode) && bkc_plain_size((uint64_t)st.st_size, &plain)) plain = UINT64_MAX;
        if (S_ISREG(st.st_mode) && S_ISREG(cur.st_mode) && (uint64_t)cur.st_size == plain && cur.st_mtim.tv_sec == st.st_mtim.tv_sec) {
            x->o->skipped++;                    /* identical: nothing to do */
            return;
        }
        if (S_ISDIR(cur.st_mode) || !x->replace) {
            if (alt_name(dst, x->suffix, target, sizeof target)) { x->o->errors++; return; }
            x->o->kept_both++;
        } else x->o->replaced++;
    }
    int r;
    if (S_ISREG(st.st_mode)) r = restore_file(x, src, target, &st);
    else {
        char l[PATH_MAX], tmp[PATH_MAX];
        ssize_t k = readlink(src, l, sizeof l - 1);
        r = -1;
        if (k >= 0 && !pathf(tmp, sizeof tmp, "%s.bkpart~", target)) {
            l[k] = 0;
            unlink(tmp);
            r = symlink(l, tmp) || rename(tmp, target) ? -1 : 0;   /* rename replaces the link, it does not follow it */
            if (r) unlink(tmp);
        }
    }
    if (r < 0) x->o->errors++;
    else if (r == 0) x->o->files++;
}

int bk_restore(const char *machine, const char *id, const char *rel, const char *base,
               int replace, const char *suffix, BkRestore *out) {
    out->files = out->bytes = out->kept_both = out->replaced = out->skipped = out->errors = 0;
    out->error[0] = 0;
    if (!bk_id_valid(id) || !bk_rel_valid(rel)) { lerr_set(out->error, sizeof out->error, "backup.badPath", NULL); return -1; }
    size_t sl = strlen(suffix);
    if (!sl || sl > 64 || strchr(suffix, '/')) { lerr_set(out->error, sizeof out->error, "backup.badSuffix", NULL); return -1; }
    for (const char *p = suffix; *p; p++) if ((unsigned char)*p < 0x20) { lerr_set(out->error, sizeof out->error, "backup.badSuffix", NULL); return -1; }
    Rest x = { .o = out, .suffix = suffix, .replace = replace };
    char snap[PATH_MAX], src[PATH_MAX], breal[PATH_MAX], dst[PATH_MAX];
    if (pathf(snap, sizeof snap, "%s/%s", machine, id) || !realpath(snap, x.snapreal)) {
        lerr_set(out->error, sizeof out->error, "backup.noSuchBackup", "id", id, NULL);
        return -1;
    }
    if (pathf(src, sizeof src, "%s%s%s", x.snapreal, rel[0] ? "/" : "", rel)) return -1;
    /* no links along the path inside the snapshot: the resolved folder must stay inside */
    if (rel[0]) {
        char parent[PATH_MAX], preal[PATH_MAX];
        snprintf(parent, sizeof parent, "%s", src);
        *strrchr(parent, '/') = 0;
        if (!realpath(parent, preal) || !under(preal, x.snapreal)) { lerr_set(out->error, sizeof out->error, "backup.badPath", NULL); return -1; }
    }
    if (!realpath(base, breal) || !fsops_allowed(breal)) { lerr_set(out->error, sizeof out->error, "backup.destNotAllowed", NULL); return -1; }
    if (!rel[0]) {
        /* everything: the snapshot's contents, inside base */
        DIR *d = opendir(src);
        struct dirent *e;
        while (d && (e = readdir(d)) && !rcancel(&x)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char s2[PATH_MAX];
            if (!pathf(s2, sizeof s2, "%s/%s", src, e->d_name) && !pathf(dst, sizeof dst, "%s/%s", breal, e->d_name))
                restore_entry(&x, s2, dst, 0);
        }
        if (d) closedir(d);
    } else {
        /* create the missing folders up to the target, never through links to forbidden places */
        char cur[PATH_MAX];
        snprintf(cur, sizeof cur, "%s", breal);
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s", rel);
        char *save = NULL, *tok = strtok_r(tmp, "/", &save);
        while (tok) {
            char *next = strtok_r(NULL, "/", &save);
            if (!next) break;
            char p[PATH_MAX];
            if (pathf(p, sizeof p, "%s/%s", cur, tok)) return -1;
            struct stat st;
            if (lstat(p, &st) && mkdir(p, 0700)) { lerr_set(out->error, sizeof out->error, "backup.createFailed", "path", p, "errno", lerr_errno(errno), NULL); return -1; }
            char pr[PATH_MAX];
            if (!realpath(p, pr) || !fsops_allowed(pr) || fsops_protected(pr)) { lerr_set(out->error, sizeof out->error, "backup.destNotAllowed", NULL); return -1; }
            snprintf(cur, sizeof cur, "%s", pr);
            tok = next;
        }
        if (!tok || pathf(dst, sizeof dst, "%s/%s", cur, tok)) return -1;
        restore_entry(&x, src, dst, 0);
    }
    if (rcancel(&x)) return 1;
    return 0;
}
