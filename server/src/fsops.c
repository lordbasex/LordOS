/* fsops.c — file operations for lord.fs (the Files app).
 *
 * Roots: everything works only inside the home folder and the user's mounts
 * (/media/$USER/… and /run/media/$USER/…: USB sticks and disks), and never writes in the
 * protected folders (fsops_protected: the launcher's, the installed apps and the settings).
 *
 * - Copy and move: in a dedicated thread per job (FsJob), so a large copy does not freeze the
 *   interface; JS polls the progress with lord.fs.jobs(). Recursive, keeps permissions and
 *   dates, never overwrites anything (O_EXCL) and copies symbolic links as links. Moving is a
 *   rename; across different disks it copies and then deletes the original.
 * - Trash: the freedesktop.org specification (the same as GNOME and KDE): the home folder's
 *   in ~/.local/share/Trash and, for mounted disks, $mount/.Trash-$uid. Each
 *   item goes in files/ with its .trashinfo (Path, DeletionDate) in info/. Deleting for real
 *   is only possible inside a trash: there is no general "delete".
 * - Walks (folder size, search, recents) bounded by time, count and
 *   depth, and without following links. */

#include "fsops.h"
#include "errcode.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <stdarg.h>
#include <unistd.h>

/* snprintf for paths: if it does not fit, leaves the string empty (and returns -1). */
__attribute__((format(printf, 3, 4)))
static int pathf(char *out, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= n) { if (n) out[0] = 0; return -1; }
    return r;
}

static char home[PATH_MAX], media1[PATH_MAX], media2[PATH_MAX], remote_root[PATH_MAX];

void fsops_init(const char *h, const char *user) {
    pathf(home, sizeof home, "%s", h);
    pathf(remote_root, sizeof remote_root, "%s/.local/share/lordos/remote", home);
    pathf(media1, sizeof media1, "/media/%s", user && *user ? user : "-");
    pathf(media2, sizeof media2, "/run/media/%s", user && *user ? user : "-");
}

static int under(const char *p, const char *root) {
    size_t n = strlen(root);
    return n > 1 && !strncmp(p, root, n) && (p[n] == 0 || p[n] == '/');
}

int fsops_allowed(const char *real) { return under(real, home) || under(real, media1) || under(real, media2); }
const char *fsops_home(void) { return home; }

/* Reading only (0.8.5, Files → This computer): the whole tree as the session user, so the system's
 * own permissions decide, except the kernel's and the runtime's pseudo-filesystems: /proc (this
 * process's memory, environment and open files), /sys, /dev and /run (the session's sockets and
 * runtime files; /run/media/$USER stays a root). Writing keeps fsops_allowed. */
int fsops_readable(const char *real) {
    if (fsops_allowed(real)) return 1;
    if (real[0] != '/') return 0;
    static const char *const pseudo[] = { "/proc", "/sys", "/dev", "/run", NULL };
    for (int i = 0; pseudo[i]; i++) if (under(real, pseudo[i])) return 0;
    return 1;
}

static const char *const protect[] = { "/.local/share/applications", "/.config/autostart",
                                           "/.local/share/lordos/apps", "/.config/lordos/settings.json",
                                           "/.config/lordos/permisos.json",
                                           "/.config/lordos/firewall.json", /* network permissions: an app does not write its own rules */
                                           "/.config/lordos/fwlists.json",  /* (0.8.5) subscribed blocklists: only lord.fw */
                                           "/.config/lordos/fwlists.txt",   /* their compiled entries, read by lord-netperm */
                                           "/.cache/lordos/fwlists",        /* their downloads: only lord-server writes them */
                                           "/.config/lordos/backup.json",   /* Backups: decides where the data goes */
                                           "/.config/lordos/mail",          /* Mail accounts and trusted certificates: only lord.mail.saveConfig (docs/MAIL.md) */
                                           "/.local/share/lordos/mail",     /* Mail cache: only lord-maild writes it */
                                           "/.local/share/lordos/vault",    /* Passwords vaults: only lord-vault writes them (docs/PASSWORDS.md) */
                                           "/.config/lordos/databases",     /* Databases favorites (hosts, trusted certificates): only lord.sql.saveFavorites (docs/DATABASES.md) */
                                           NULL };

static char pfile[FSOPS_PF_COUNT][PATH_MAX];

void fsops_protect_file(int slot, const char *real) {
    if (slot < 0 || slot >= FSOPS_PF_COUNT) return;
    if (!real || snprintf(pfile[slot], sizeof pfile[slot], "%s", real) >= (int)sizeof pfile[slot]) pfile[slot][0] = 0;
}

/* real is one of the protected files or of its family */
static int pfile_match(const char *real) {
    for (int i = 0; i < FSOPS_PF_COUNT; i++) {
        size_t n = strlen(pfile[i]);
        if (n && !strncmp(real, pfile[i], n) && (real[n] == 0 || real[n] == '.')) return 1;
    }
    return 0;
}

int fsops_protected(const char *real) {
    if (pfile_match(real)) return 1;
    for (int i = 0; protect[i]; i++) {
        char p[PATH_MAX + 64];
        pathf(p, sizeof p, "%s%s", home, protect[i]);
        if (under(real, p)) return 1;
    }
    return 0;
}

/* The same two questions with every protected path resolved first (a link anywhere on its way followed), for a
 * folder handed outside lord.fs, such as a machine's writable share: with ~/.config a link to another disk, the real
 * folder holding settings.json is not under home/.config, and a lexical check would miss it. A protected path that
 * doesn't exist yet is taken as written. */
/* p with its longest existing prefix resolved and the rest added back as written: a protected file that doesn't exist
 * yet is still where its folder really is (~/.config a link elsewhere, terminal.json not written yet). 0, or -1. */
static int resolve_prefix(const char *p, char *out, size_t n) {
    char head[PATH_MAX], r[PATH_MAX];
    if (snprintf(head, sizeof head, "%s", p) >= (int)sizeof head) return -1;
    size_t cut = strlen(head);
    for (;;) {
        head[cut] = 0;
        if (realpath(cut ? head : "/", r)) break;
        char *s = strrchr(head, '/');
        if (!s) return -1;
        cut = (size_t)(s - head);
    }
    const char *tail = p + cut;                     /* "" or "/rest/of/path" */
    if (!strcmp(r, "/") && tail[0] == '/') tail++;
    return pathf(out, n, "%s%s", r, tail) < 0 ? -1 : 0;
}

static int resolved_hit(const char *real, const char *p) {
    char q[PATH_MAX];
    if (resolve_prefix(p, q, sizeof q)) return 1;   /* can't tell where it is: treated as a hit */
    return under(real, q) || (strcmp(q, real) && under(q, real));
}
int fsops_protected_resolved(const char *real) {
    if (fsops_protected(real) || fsops_protected_below(real)) return 1;
    for (int i = 0; protect[i]; i++) {
        char p[PATH_MAX + 64];
        pathf(p, sizeof p, "%s%s", home, protect[i]);
        if (p[0] && resolved_hit(real, p)) return 1;
    }
    for (int i = 0; i < FSOPS_PF_COUNT; i++) if (pfile[i][0] && resolved_hit(real, pfile[i])) return 1;
    return 0;
}

/* Is a protected path strictly below real? (Cleaner's rule table: a rule's root must not hold one.) */
int fsops_protected_below(const char *real) {
    for (int i = 0; i < FSOPS_PF_COUNT; i++)
        if (pfile[i][0] && strcmp(pfile[i], real) && under(pfile[i], real)) return 1;
    for (int i = 0; protect[i]; i++) {
        char p[PATH_MAX + 64];
        pathf(p, sizeof p, "%s%s", home, protect[i]);
        if (strcmp(p, real) && under(p, real)) return 1;
    }
    return 0;
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* err buffers carry code lines (errcode.h): lord.fs throws them translated */
static void seterr(char *e, size_t n, const char *code) { if (e && n) lerr_set(e, n, code, NULL); }
static void seterrno(char *e, size_t n, int no) { if (e && n) lerr_set(e, n, "sys.errno", "errno", lerr_errno(no), NULL); }

/* Moving a parent of a protected path takes the protected path with it, and a folder arriving under such a
 * name brings its own: neither may happen through rename, move, Trash or restore. RENAME_NOREPLACE: the
 * kernel refuses an existing target, with no gap between a check and the rename. */
int fsops_rename(const char *from, const char *to, char *err, size_t errlen) {
    if (fsops_protected(from) || fsops_protected(to) || fsops_protected_below(from) || fsops_protected_below(to)) {
        seterr(err, errlen, "fs.protected");
        return -1;
    }
    if (!renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE)) return 0;
    if (errno == EINVAL || errno == ENOSYS) {
        /* FUSE filesystems (rclone's cloud and Connect to Server mounts) don't take RENAME_NOREPLACE: check that nothing
         * is at the target, then a plain rename. Between the check and the rename another program could create the
         * target, which would then be replaced: a gap of microseconds, only on those filesystems. */
        struct stat st;
        if (!lstat(to, &st)) { seterr(err, errlen, "fs.exists"); return -1; }
        if (errno != ENOENT) { seterrno(err, errlen, errno); return -1; }
        if (!renameat(AT_FDCWD, from, AT_FDCWD, to)) return 0;
    }
    if (errno == EEXIST) seterr(err, errlen, "fs.exists");
    else seterrno(err, errlen, errno);
    return -1;
}

/* ---- deleting a tree (only used by cross-disk move and the trash) ---- */

static int rm_cb(const char *p, const struct stat *st, int flag, struct FTW *f) {
    (void)st; (void)f;
    return flag == FTW_DP ? rmdir(p) : unlink(p);
}
static int rm_tree(const char *p) {
    struct stat st;
    if (lstat(p, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(p);
    return nftw(p, rm_cb, 32, FTW_DEPTH | FTW_PHYS);
}

/* ---- copy / move ---- */

static FsJob jobs[FSJOB_MAX];
static int next_job = 1;

static void walk_total(const char *p, FsJob *j, int depth) {
    struct stat st;
    if (depth > 64 || lstat(p, &st) || j->cancel) return;
    if (S_ISREG(st.st_mode)) { j->total += (uint64_t)st.st_size; return; }
    if (!S_ISDIR(st.st_mode)) return;
    DIR *d = opendir(p);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[PATH_MAX];
        if ((size_t)snprintf(c, sizeof c, "%s/%s", p, e->d_name) < sizeof c) walk_total(c, j, depth + 1);
    }
    if (d) closedir(d);
}

static int copy_file(const char *s, const char *d, const struct stat *st, FsJob *j) {
    int in = open(s, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (in < 0) return -1;
    int out = open(d, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, st->st_mode & 07777);
    if (out < 0) { close(in); return -1; }
    char *buf = malloc(1 << 20);
    int ok = buf != NULL;
    while (ok && !j->cancel) {
        ssize_t r = read(in, buf, 1 << 20);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { ok = 0; break; }
        if (r == 0) break;
        for (ssize_t o = 0; o < r;) {
            ssize_t w = write(out, buf + o, (size_t)(r - o));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { ok = 0; break; }
            o += w;
        }
        j->bytes += (uint64_t)r;
    }
    free(buf);
    if (j->cancel) ok = 0;
    if (ok) {
        fchmod(out, st->st_mode & 07777);
        struct timespec ts[2] = { st->st_atim, st->st_mtim };
        futimens(out, ts);
    }
    if (close(out)) ok = 0;
    close(in);
    if (!ok) { int e = errno; unlink(d); errno = e ? e : ECANCELED; }
    return ok ? 0 : -1;
}

static int copy_tree(const char *s, const char *d, FsJob *j, int depth) {
    struct stat st;
    if (depth > 64) { errno = ELOOP; return -1; }
    if (j->cancel) { errno = ECANCELED; return -1; }
    if (lstat(s, &st)) return -1;
    if (S_ISLNK(st.st_mode)) {
        char t[PATH_MAX];
        ssize_t n = readlink(s, t, sizeof t - 1);
        if (n < 0) return -1;
        t[n] = 0;
        j->files++;
        return symlink(t, d);
    }
    if (S_ISREG(st.st_mode)) { j->files++; return copy_file(s, d, &st, j); }
    if (!S_ISDIR(st.st_mode)) return 0;                 /* fifos, devices: skipped */
    if (mkdir(d, 0700)) return -1;
    DIR *dir = opendir(s);
    if (!dir) return -1;
    struct dirent *e;
    int r = 0;
    while (!r && (e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char cs[PATH_MAX], cd[PATH_MAX];
        if ((size_t)snprintf(cs, sizeof cs, "%s/%s", s, e->d_name) >= sizeof cs ||
            (size_t)snprintf(cd, sizeof cd, "%s/%s", d, e->d_name) >= sizeof cd) { errno = ENAMETOOLONG; r = -1; break; }
        r = copy_tree(cs, cd, j, depth + 1);
    }
    closedir(dir);
    chmod(d, st.st_mode & 07777);
    struct timespec ts[2] = { st.st_atim, st.st_mtim };
    utimensat(AT_FDCWD, d, ts, AT_SYMLINK_NOFOLLOW);
    return r;
}

static void *job_main(void *arg) {
    FsJob *j = arg;
    if (j->try_rename) {
        if (!rename(j->src, j->dst)) { __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE); return NULL; }   /* same disk */
        if (errno != EXDEV) {
            seterrno(j->error, sizeof j->error, errno);
            j->failed = 1;
            __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
            return NULL;
        }
    }
    walk_total(j->src, j, 0);
    int r = copy_tree(j->src, j->dst, j, 0);
    if (r) {
        seterrno(j->error, sizeof j->error, errno);
        rm_tree(j->dst);                                /* no half-done copies */
        j->failed = 1;
    } else if (j->move && rm_tree(j->src)) {
        lerr_set(j->error, sizeof j->error, "fs.moveRemoveFailed", "errno", lerr_errno(errno), NULL);
        j->failed = 1;
    }
    __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

int fsjob_start(const char *src, const char *dst, int move, char *err, size_t errlen) {
    struct stat st;
    if ((move && fsops_protected_below(src)) || fsops_protected_below(dst)) { seterr(err, errlen, "fs.protected"); return -1; }
    if (lstat(src, &st)) { seterrno(err, errlen, errno); return -1; }
    if (!lstat(dst, &st)) { seterr(err, errlen, "fs.exists"); return -1; }
    size_t n = strlen(src);
    if (!strncmp(dst, src, n) && dst[n] == '/') { seterr(err, errlen, "fs.copyIntoItself"); return -1; }
    FsJob *j = NULL;
    for (int i = 0; i < FSJOB_MAX; i++) if (!jobs[i].used) { j = &jobs[i]; break; }
    if (!j) { seterr(err, errlen, "fs.tooManyCopies"); return -1; }
    memset(j, 0, sizeof *j);
    j->used = 1;
    j->id = next_job++;
    j->move = move;
    pathf(j->src, sizeof j->src, "%s", src);
    pathf(j->dst, sizeof j->dst, "%s", dst);
    /* a move inside a Cloud account or a network share is a call to its server (rclone moves the file there): the
     * rename goes to the job's thread, so the interface's never waits for it (0.8.8.7) */
    j->try_rename = move && (fsops_remote_path(src) || fsops_remote_path(dst));
    if (move && !j->try_rename && rename(src, dst) == 0) { j->done = 1; return j->id; }   /* same disk: instant */
    if (move && !j->try_rename && errno != EXDEV) { int e = errno; j->used = 0; seterrno(err, errlen, e); return -1; }
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, job_main, j)) { j->used = 0; seterr(err, errlen, "fs.copyStartFailed"); pthread_attr_destroy(&a); return -1; }
    pthread_attr_destroy(&a);
    return j->id;
}

const FsJob *fsjob_get(int i) { return i >= 0 && i < FSJOB_MAX && jobs[i].used ? &jobs[i] : NULL; }
void fsjob_cancel(int id) { for (int i = 0; i < FSJOB_MAX; i++) if (jobs[i].used && jobs[i].id == id) jobs[i].cancel = 1; }
void fsjob_forget(int id) {
    for (int i = 0; i < FSJOB_MAX; i++)
        if (jobs[i].used && jobs[i].id == id && __atomic_load_n(&jobs[i].done, __ATOMIC_ACQUIRE)) jobs[i].used = 0;
}

/* ---- Trash (freedesktop.org Trash 1.0) ---- */

/* Mount point of a path: going up while the device does not change. */
static int mount_top(const char *real, char *out, size_t n) {
    struct stat st, up;
    if (lstat(real, &st)) return -1;
    char cur[PATH_MAX];
    pathf(cur, sizeof cur, "%s", real);
    for (;;) {
        char *sl = strrchr(cur, '/');
        if (!sl || sl == cur) { pathf(out, n, "/"); return 0; }
        char parent[PATH_MAX];
        pathf(parent, sizeof parent, "%.*s", (int)(sl - cur), cur);
        if (lstat(parent, &up) || up.st_dev != st.st_dev) { pathf(out, n, "%s", cur); return 0; }
        pathf(cur, sizeof cur, "%s", parent);
    }
}

/* Trash folder for a path: the home one or $mount/.Trash-$uid. rel = 1 if Path is relative. */
static int trash_dir_for(const char *real, char *dir, size_t n, char *top, size_t tn, int *rel) {
    struct stat a, b;
    if (under(real, home) && !lstat(home, &a) && !lstat(real, &b) && a.st_dev == b.st_dev) {
        pathf(dir, n, "%s/.local/share/Trash", home);
        top[0] = 0;
        *rel = 0;
        return 0;
    }
    if (mount_top(real, top, tn) || !fsops_allowed(top)) return -1;
    pathf(dir, n, "%s/.Trash-%u", top, (unsigned)getuid());
    *rel = 1;
    return 0;
}

static int mkdirs_trash(const char *dir) {
    char p[PATH_MAX];
    pathf(p, sizeof p, "%s", dir);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0700); *s = '/'; }
    mkdir(p, 0700);
    char f[PATH_MAX + 8], i[PATH_MAX + 8];
    pathf(f, sizeof f, "%s/files", dir);
    pathf(i, sizeof i, "%s/info", dir);
    mkdir(f, 0700);
    mkdir(i, 0700);
    struct stat st;
    return lstat(f, &st) || !S_ISDIR(st.st_mode) || lstat(i, &st) || !S_ISDIR(st.st_mode) ? -1 : 0;
}

static void url_encode(const char *s, char *out, size_t n) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 4 < n; p++) {
        if (isalnum(*p) || strchr("/-_.~", *p)) out[o++] = (char)*p;
        else o += (size_t)snprintf(out + o, n - o, "%%%02X", *p);
    }
    out[o] = 0;
}

static void url_decode(const char *s, char *out, size_t n) {
    size_t o = 0;
    for (; *s && o + 1 < n; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            out[o++] = (char)strtol(h, NULL, 16);
            s += 2;
        } else out[o++] = *s;
    }
    out[o] = 0;
}

/* The object moved is `from` relative to dirfd (or a path with AT_FDCWD); real names it for the
 * .trashinfo and for choosing the trash. dest (optional) gets the item's path inside the Trash. On
 * failure errno says why. */
static int trash_put(int dirfd, const char *from, const char *real, char *destout, size_t destn, char *err, size_t errlen) {
    if (fsops_protected_below(real)) { seterr(err, errlen, "fs.protected"); errno = EPERM; return -1; }
    char dir[PATH_MAX], top[PATH_MAX];
    int rel;
    if (trash_dir_for(real, dir, sizeof dir, top, sizeof top, &rel) || mkdirs_trash(dir)) { seterr(err, errlen, "fs.noTrash"); errno = EIO; return -1; }
    if (!strncmp(real, dir, strlen(dir))) { seterr(err, errlen, "fs.alreadyInTrash"); errno = EINVAL; return -1; }
    const char *base = strrchr(real, '/');
    base = base ? base + 1 : real;
    char name[NAME_MAX + 16], info[PATH_MAX + 300], dest[PATH_MAX + 300];
    int fd = -1;
    for (int k = 1; k < 10000 && fd < 0; k++) {
        if (k == 1) pathf(name, sizeof name, "%.240s", base);
        else pathf(name, sizeof name, "%.230s.%d", base, k);
        pathf(info, sizeof info, "%s/info/%s.trashinfo", dir, name);
        pathf(dest, sizeof dest, "%s/files/%s", dir, name);
        struct stat st;
        if (!lstat(dest, &st)) continue;
        fd = open(info, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST) break;
    }
    if (fd < 0) { seterr(err, errlen, "fs.trashEntryFailed"); errno = EIO; return -1; }
    char path[PATH_MAX * 3], enc[PATH_MAX * 3], when[32];
    pathf(path, sizeof path, "%s", rel ? real + strlen(top) + 1 : real);
    url_encode(path, enc, sizeof enc);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &tm);
    dprintf(fd, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", enc, when);
    close(fd);
    if (renameat(dirfd, from, AT_FDCWD, dest)) {
        int e = errno;
        unlink(info);
        seterr(err, errlen, e == EXDEV ? "fs.trashOtherDisk" : "sys.errno");
        if (e != EXDEV) seterrno(err, errlen, e);
        errno = e;
        return -1;
    }
    if (destout) pathf(destout, destn, "%s", dest);
    return 0;
}

int fstrash_put(const char *real, char *err, size_t errlen) { return trash_put(AT_FDCWD, real, real, NULL, 0, err, errlen); }

/* (Cleaner) The same Trash, for a name in an already open folder: a folder swapped for a link after
 * the caller checked it can't redirect the move. */
int fstrash_put_at(int dirfd, const char *name, const char *real, char *dest, size_t destn, char *err, size_t errlen) {
    if (strchr(name, '/')) { seterr(err, errlen, "sys.badArgument"); errno = EINVAL; return -1; }
    return trash_put(dirfd, name, real, dest, destn, err, errlen);
}

/* Known trash folders: the home one and those of the user's mounts. */
typedef struct { char dirs[17][PATH_MAX]; int n; } TrashDirs;
static void vol_dir_cb(void *ud, const char *path, const char *fs, double total, double avail) {
    (void)total; (void)avail;
    TrashDirs *t = ud;
    /* the Dock asks for the Trash on the interface's thread: a share that doesn't answer is left out */
    if (fsops_share_fs(fs) && fsops_share_down(path)) return;
    if (t->n < 17) pathf(t->dirs[t->n++], PATH_MAX, "%s/.Trash-%u", path, (unsigned)getuid());
}
static TrashDirs *trash_dirs(void) {
    static TrashDirs t;
    t.n = 0;
    pathf(t.dirs[t.n++], PATH_MAX, "%s/.local/share/Trash", home);
    fs_volumes(vol_dir_cb, &t);
    return &t;
}

static double tree_size(const char *p, int depth) {
    static __thread dev_t top;
    struct stat st;
    if (depth > 32 || lstat(p, &st)) return 0;
    if (!depth) top = st.st_dev;
    else if (st.st_dev != top) return 0;            /* a mount inside the trash is not the trash's size */
    if (!S_ISDIR(st.st_mode)) return (double)st.st_size;
    double sum = 0;
    DIR *d = opendir(p);
    struct dirent *e;
    int count = 0;
    while (d && (e = readdir(d)) && count++ < 20000) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[PATH_MAX];
        if ((size_t)snprintf(c, sizeof c, "%s/%s", p, e->d_name) < sizeof c) sum += tree_size(c, depth + 1);
    }
    if (d) closedir(d);
    return sum;
}

static int read_info(const char *dir, const char *name, char *orig, size_t on, char *date, size_t dn) {
    char path[PATH_MAX * 2];
    pathf(path, sizeof path, "%s/info/%s.trashinfo", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[PATH_MAX * 3], raw[PATH_MAX * 3] = "";
    orig[0] = date[0] = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "Path=", 5)) pathf(raw, sizeof raw, "%s", line + 5);
        else if (!strncmp(line, "DeletionDate=", 13)) pathf(date, dn, "%s", line + 13);
    }
    fclose(f);
    char dec[PATH_MAX * 3];
    url_decode(raw, dec, sizeof dec);
    if (dec[0] == '/') pathf(orig, on, "%s", dec);
    else {
        /* relative to the mount: dir is $mount/.Trash-uid */
        char top[PATH_MAX];
        pathf(top, sizeof top, "%s", dir);
        char *sl = strrchr(top, '/');
        if (sl) *sl = 0;
        pathf(orig, on, "%s/%s", top, dec);
    }
    return 0;
}

void fstrash_list(FsTrashFn fn, void *ud) {
    TrashDirs *t = trash_dirs();
    for (int i = 0; i < t->n; i++) {
        char files[PATH_MAX + 8];
        pathf(files, sizeof files, "%s/files", t->dirs[i]);
        DIR *d = opendir(files);
        struct dirent *e;
        int count = 0;
        while (d && (e = readdir(d)) && count++ < 5000) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char id[PATH_MAX * 2], orig[PATH_MAX * 2], date[64];
            pathf(id, sizeof id, "%s/%s", files, e->d_name);
            if (read_info(t->dirs[i], e->d_name, orig, sizeof orig, date, sizeof date)) { orig[0] = 0; date[0] = 0; }
            struct stat st;
            int dir = !lstat(id, &st) && S_ISDIR(st.st_mode);
            fn(ud, id, e->d_name, orig, date, dir, dir ? tree_size(id, 0) : (double)st.st_size);
        }
        if (d) closedir(d);
    }
}

/* valid id: a direct child of <trash>/files. Returns the trash and the name. */
static int trash_id(const char *id, char *dir, size_t dn, char *name, size_t nn) {
    TrashDirs *t = trash_dirs();
    for (int i = 0; i < t->n; i++) {
        char files[PATH_MAX + 8];
        pathf(files, sizeof files, "%s/files/", t->dirs[i]);
        size_t fl = strlen(files);
        if (!strncmp(id, files, fl) && id[fl] && !strchr(id + fl, '/') && strcmp(id + fl, ".") && strcmp(id + fl, "..")) {
            pathf(dir, dn, "%s", t->dirs[i]);
            pathf(name, nn, "%s", id + fl);
            return 0;
        }
    }
    return -1;
}

int fstrash_restore(const char *id, char *err, size_t errlen) {
    char dir[PATH_MAX], name[NAME_MAX + 16], orig[PATH_MAX * 2], date[64];
    if (trash_id(id, dir, sizeof dir, name, sizeof name) || read_info(dir, name, orig, sizeof orig, date, sizeof date) || !orig[0]) {
        seterr(err, errlen, "fs.notInTrash");
        return -1;
    }
    /* the original folder must still exist, inside the roots and unprotected */
    char parent[PATH_MAX * 2], rparent[PATH_MAX];
    pathf(parent, sizeof parent, "%s", orig);
    char *sl = strrchr(parent, '/');
    if (!sl) { seterr(err, errlen, "fs.trashBadOrigin"); return -1; }
    *sl = 0;
    if (!realpath(parent[0] ? parent : "/", rparent) || !fsops_allowed(rparent)) { seterr(err, errlen, "fs.trashOriginGone"); return -1; }
    char dest[PATH_MAX * 2];
    pathf(dest, sizeof dest, "%s/%s", rparent, sl + 1);
    if (fsops_protected(dest) || fsops_protected_below(dest)) { seterr(err, errlen, "fs.protected"); return -1; }
    struct stat st;
    if (!lstat(dest, &st)) { seterr(err, errlen, "fs.trashRestoreExists"); return -1; }
    if (rename(id, dest)) { seterrno(err, errlen, errno); return -1; }
    char info[PATH_MAX * 2];
    pathf(info, sizeof info, "%s/info/%s.trashinfo", dir, name);
    unlink(info);
    return 0;
}

int fstrash_remove(const char *id, char *err, size_t errlen) {
    char dir[PATH_MAX], name[NAME_MAX + 16];
    if (trash_id(id, dir, sizeof dir, name, sizeof name)) { seterr(err, errlen, "fs.notInTrash"); return -1; }
    if (rm_tree(id)) { seterrno(err, errlen, errno); return -1; }
    char info[PATH_MAX * 2];
    pathf(info, sizeof info, "%s/info/%s.trashinfo", dir, name);
    unlink(info);
    return 0;
}

int fstrash_empty(void) {
    TrashDirs *t = trash_dirs();
    int n = 0;
    for (int i = 0; i < t->n; i++) {
        char files[PATH_MAX + 8], infos[PATH_MAX + 8];
        pathf(files, sizeof files, "%s/files", t->dirs[i]);
        pathf(infos, sizeof infos, "%s/info", t->dirs[i]);
        DIR *d = opendir(files);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char p[PATH_MAX * 2];
            pathf(p, sizeof p, "%s/%s", files, e->d_name);
            if (!rm_tree(p)) n++;
        }
        if (d) closedir(d);
        d = opendir(infos);
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[PATH_MAX * 2];
            pathf(p, sizeof p, "%s/%s", infos, e->d_name);
            unlink(p);
        }
        if (d) closedir(d);
    }
    return n;
}

/* ---- bounded walks ---- */

static FsDu *du_out;
static double du_deadline;
static int du_cb(const char *p, const struct stat *st, int flag, struct FTW *f) {
    (void)p; (void)f;
    if (flag == FTW_D) du_out->dirs++;
    else if (flag == FTW_F) { du_out->files++; du_out->size += (double)st->st_size; }
    if (((du_out->files + du_out->dirs) & 1023) == 0 && now_ms() > du_deadline) return 1;   /* time is up */
    return 0;
}

void fs_du(const char *real, double max_ms, FsDu *out) {
    memset(out, 0, sizeof *out);
    du_out = out;
    du_deadline = now_ms() + max_ms;
    out->complete = nftw(real, du_cb, 32, FTW_PHYS | FTW_MOUNT) == 0;   /* never into another filesystem (a cloud mount) */
}

static int has_ci(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

/* The walks for Recents and search stay on the filesystem they start on (docs/CLOUD.md 17.2, the Pi): a mount inside the
 * home folder, above all a cloud or network one (~/Cloud/<account>, fuse.rclone, cifs, nfs, sshfs), is never entered.
 * Its folder is skipped by its path, from /proc/self/mounts, before any lstat or readdir, so a server that doesn't answer
 * can't block the interface's thread; st_dev catches the rest (a bind mount, one made during the walk). */
#define WALK_SKIP_MAX 64
typedef struct { char m[WALK_SKIP_MAX][PATH_MAX]; int n; dev_t dev; } WalkSkip;
static void unescape_mount(char *s);
static void walk_skip_init(WalkSkip *w, const char *root) {
    w->n = 0;
    struct stat st;
    w->dev = !stat(root, &st) ? st.st_dev : 0;
    FILE *f = fopen("/proc/self/mounts", "re");
    if (!f) return;
    char line[4096], dev[1024], mnt[2048];
    size_t rl = strlen(root);
    while (w->n < WALK_SKIP_MAX && fgets(line, sizeof line, f)) {
        if (sscanf(line, "%1023s %2047s", dev, mnt) != 2) continue;
        unescape_mount(mnt);
        if (!strncmp(mnt, root, rl) && mnt[rl] == '/') pathf(w->m[w->n++], PATH_MAX, "%s", mnt);
    }
    fclose(f);
}
static int walk_skip(const WalkSkip *w, const char *path) {
    for (int i = 0; i < w->n; i++) if (!strcmp(w->m[i], path)) return 1;
    return 0;
}

typedef struct { const char *needle; int limit, depth, hits, visited; FsHitFn fn; void *ud; WalkSkip *skip; } Search;
static void search_dir(const char *p, Search *s, int depth) {
    if (depth > s->depth || s->hits >= s->limit || s->visited > 100000) return;
    DIR *d = opendir(p);
    struct dirent *e;
    while (d && (e = readdir(d)) && s->hits < s->limit) {
        if (e->d_name[0] == '.') continue;                  /* hidden ones and . .. */
        s->visited++;
        char c[PATH_MAX];
        if ((size_t)snprintf(c, sizeof c, "%s/%s", p, e->d_name) >= sizeof c) continue;
        if (walk_skip(s->skip, c)) continue;
        struct stat st;
        if (lstat(c, &st) || (s->skip->dev && st.st_dev != s->skip->dev)) continue;
        if (has_ci(e->d_name, s->needle)) { s->hits++; s->fn(s->ud, c, S_ISDIR(st.st_mode), (double)st.st_size, (double)st.st_mtime * 1000); }
        if (S_ISDIR(st.st_mode)) search_dir(c, s, depth + 1);
    }
    if (d) closedir(d);
}

static int remote_type(const char *ty) {
    return !strncmp(ty, "fuse.", 5) || !strcmp(ty, "cifs") || !strcmp(ty, "smb3") || !strncmp(ty, "nfs", 3) ||
           !strcmp(ty, "9p") || !strcmp(ty, "ceph") || !strcmp(ty, "glusterfs") || !strcmp(ty, "davfs");
}

int fsops_remote_path(const char *real) {
    FILE *f = fopen("/proc/self/mounts", "re");
    if (!f) return 0;
    char line[4096], dev[1024], mnt[2048], ty[64];
    size_t best = 0;
    int remote = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%1023s %2047s %63s", dev, mnt, ty) != 3) continue;
        unescape_mount(mnt);
        size_t l = strlen(mnt);
        int in = !strcmp(mnt, "/") || (!strncmp(real, mnt, l) && (real[l] == 0 || real[l] == '/'));
        if (!in || l < best) continue;
        best = l;
        remote = remote_type(ty);
    }
    fclose(f);
    return remote;
}

void fs_mounts(FsMountFn fn, void *ud) {
    FILE *f = fopen("/proc/self/mounts", "re");
    if (!f) return;
    char line[4096], dev[1024], mnt[2048], ty[64];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%1023s %2047s %63s", dev, mnt, ty) != 3) continue;
        unescape_mount(mnt);
        fn(ud, mnt, remote_type(ty));
    }
    fclose(f);
}

int fs_search(const char *real, const char *needle, int limit, int depth, FsHitFn fn, void *ud) {
    WalkSkip *w = malloc(sizeof *w);
    if (!w) return 0;
    walk_skip_init(w, real);
    Search s = { needle, limit, depth, 0, 0, fn, ud, w };
    if (needle[0]) search_dir(real, &s, 0);
    free(w);
    return s.hits;
}

typedef struct { char path[PATH_MAX]; double mtime, size; } Recent;
typedef struct { Recent *r; int n, cap, visited; time_t since; WalkSkip *skip; } RecentAcc;
static void recent_dir(const char *p, RecentAcc *a, int depth) {
    if (depth > 5 || a->visited > 60000) return;
    DIR *d = opendir(p);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "node_modules")) continue;
        a->visited++;
        char c[PATH_MAX];
        if ((size_t)snprintf(c, sizeof c, "%s/%s", p, e->d_name) >= sizeof c) continue;
        if (walk_skip(a->skip, c)) continue;
        struct stat st;
        if (lstat(c, &st) || (a->skip->dev && st.st_dev != a->skip->dev)) continue;
        if (S_ISDIR(st.st_mode)) { recent_dir(c, a, depth + 1); continue; }
        if (!S_ISREG(st.st_mode) || st.st_mtime < a->since) continue;
        double m = (double)st.st_mtime * 1000;
        if (a->n == a->cap && m <= a->r[a->n - 1].mtime) continue;
        int i = a->n < a->cap ? a->n++ : a->n - 1;
        while (i > 0 && a->r[i - 1].mtime < m) { a->r[i] = a->r[i - 1]; i--; }
        pathf(a->r[i].path, PATH_MAX, "%s", c);
        a->r[i].mtime = m;
        a->r[i].size = (double)st.st_size;
    }
    if (d) closedir(d);
}

int fs_recent(int limit, int days, FsHitFn fn, void *ud) {
    if (limit < 1) limit = 1;
    if (limit > 500) limit = 500;
    WalkSkip *w = malloc(sizeof *w);
    RecentAcc a = { calloc((size_t)limit, sizeof(Recent)), 0, limit, 0, time(NULL) - (time_t)days * 86400, w };
    if (!a.r || !w) { free(a.r); free(w); return 0; }
    walk_skip_init(w, home);
    recent_dir(home, &a, 0);
    free(w);
    for (int i = 0; i < a.n; i++) fn(ud, a.r[i].path, 0, a.r[i].size, a.r[i].mtime);
    free(a.r);
    return a.n;
}

/* /proc/mounts escapes spaces and others as \040. */
static void unescape_mount(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '7' && s[2] >= '0' && s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
            *o++ = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
            s += 3;
        } else *o++ = *s;
    }
    *o = 0;
}

int fsops_share_fs(const char *t) { return !strcmp(t, "fuse.rclone") || !strcmp(t, "cifs") || !strcmp(t, "smb3") || !strcmp(t, "nfs") || !strcmp(t, "nfs4"); }

/* ---- shares that stopped answering ----
 * lord.fs runs on the interface's thread, and a call on a network mount whose server went away
 * blocks in the kernel: tens of seconds on SMB, minutes on NFSv4 even when soft (measured, docs/FILES.md).
 * So before lord.fs touches a path under a share, a thread of its own asks the server (statvfs) and
 * the caller waits at most SHARE_WAIT_MS. While that question hangs the share counts as down, without
 * waiting. What it can't cover: a server that dies inside the SHARE_OK_MS an answer is trusted. */
#define SHARE_WAIT_MS 800
#define SHARE_OK_MS 3000
#define SHARE_MAX 8

typedef struct { char mnt[PATH_MAX]; int asking, ok; long long when; } ShareMount;
static ShareMount sh[SHARE_MAX];
static int nsh;
static long long sh_read;
static pthread_mutex_t sh_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sh_cv;
static pthread_once_t sh_once = PTHREAD_ONCE_INIT;

static long long mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static void sh_init(void) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&sh_cv, &a);
    pthread_condattr_destroy(&a);
}

static void *sh_ask(void *arg) {
    char mnt[PATH_MAX];
    pthread_mutex_lock(&sh_mu);
    snprintf(mnt, sizeof mnt, "%s", (char *)arg);
    pthread_mutex_unlock(&sh_mu);
    free(arg);
    struct statvfs vs;
    int ok = !statvfs(mnt, &vs);
    pthread_mutex_lock(&sh_mu);
    for (int i = 0; i < nsh; i++) {
        if (strcmp(sh[i].mnt, mnt)) continue;
        sh[i].asking = 0;
        sh[i].ok = ok;
        sh[i].when = mono_ms();
    }
    pthread_cond_broadcast(&sh_cv);
    pthread_mutex_unlock(&sh_mu);
    return NULL;
}

static void sh_reread(void) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return;
    ShareMount fresh[SHARE_MAX];
    int n = 0;
    char line[4096];
    size_t l1 = strlen(media1), l2 = strlen(media2);
    while (n < SHARE_MAX && fgets(line, sizeof line, f)) {
        char dev[1024], mnt[2048], fstype[64];
        if (sscanf(line, "%1023s %2047s %63s", dev, mnt, fstype) != 3 || !fsops_share_fs(fstype)) continue;
        unescape_mount(mnt);
        size_t l3 = strlen(remote_root);
        if (!((!strncmp(mnt, media1, l1) && mnt[l1] == '/') || (!strncmp(mnt, media2, l2) && mnt[l2] == '/') || (!strncmp(mnt, remote_root, l3) && mnt[l3] == '/'))) continue;
        memset(&fresh[n], 0, sizeof fresh[n]);
        snprintf(fresh[n].mnt, sizeof fresh[n].mnt, "%s", mnt);
        for (int i = 0; i < nsh; i++) if (!strcmp(sh[i].mnt, mnt)) fresh[n] = sh[i];    /* an answer on its way stays */
        n++;
    }
    fclose(f);
    memcpy(sh, fresh, (size_t)n * sizeof sh[0]);
    nsh = n;
}

int fsops_share_down(const char *path) {
    if (!under(path, media1) && !under(path, media2) && !under(path, remote_root)) return 0;
    pthread_once(&sh_once, sh_init);
    pthread_mutex_lock(&sh_mu);
    long long now = mono_ms();
    if (now - sh_read > 2000 || !sh_read) { sh_reread(); sh_read = now; }
    int k = -1;
    for (int i = 0; i < nsh; i++) if (under(path, sh[i].mnt)) k = i;
    if (k < 0) { pthread_mutex_unlock(&sh_mu); return 0; }
    ShareMount *m = &sh[k];
    int down = 0;
    if (m->asking) down = 1;
    else if (!m->when || now - m->when > SHARE_OK_MS) {
        char *arg = strdup(m->mnt);
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 128 * 1024);
        sigset_t all, old;
        sigfillset(&all);
        pthread_sigmask(SIG_BLOCK, &all, &old);
        int e = arg ? pthread_create(&th, &at, sh_ask, arg) : -1;
        pthread_sigmask(SIG_SETMASK, &old, NULL);
        pthread_attr_destroy(&at);
        if (e) { free(arg); down = 1; }
        else {
            m->asking = 1;
            char want[PATH_MAX];
            snprintf(want, sizeof want, "%s", m->mnt);
            struct timespec until;
            clock_gettime(CLOCK_MONOTONIC, &until);
            until.tv_nsec += SHARE_WAIT_MS * 1000000L;
            if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
            for (;;) {
                int r = pthread_cond_timedwait(&sh_cv, &sh_mu, &until);
                int still = 0;
                for (int i = 0; i < nsh; i++) if (!strcmp(sh[i].mnt, want)) { still = sh[i].asking; down = !still && !sh[i].ok; }
                if (!still) break;
                if (r == ETIMEDOUT) { down = 1; break; }
            }
        }
    } else down = !m->ok;
    pthread_mutex_unlock(&sh_mu);
    return down;
}

void fs_volumes(FsVolFn fn, void *ud) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        char dev[1024], mnt[2048], fstype[64];
        if (sscanf(line, "%1023s %2047s %63s", dev, mnt, fstype) != 3) continue;
        unescape_mount(mnt);
        size_t l1 = strlen(media1), l2 = strlen(media2);
        size_t l3 = strlen(remote_root);
        int ok = (!strncmp(mnt, media1, l1) && mnt[l1] == '/') || (!strncmp(mnt, media2, l2) && mnt[l2] == '/') || (!strncmp(mnt, remote_root, l3) && mnt[l3] == '/');
        if (!ok) continue;
        struct statvfs vs;
        double total = 0, avail = 0;
        /* never on a network mount: this runs on the interface's thread and a server that went away
         * would hold it for as long as the file system's timeout */
        if (!fsops_share_fs(fstype) && !statvfs(mnt, &vs)) { total = (double)vs.f_blocks * vs.f_frsize; avail = (double)vs.f_bavail * vs.f_frsize; }
        fn(ud, mnt, fstype, total, avail);
    }
    fclose(f);
}

/* ---- resolving lord.fs paths, listing and stat (shared by the interface's thread and fsasync.c) ----
 * The same functions answer lord.fs.list / stat / trash on the interface's thread and their Async variants on fsasync.c's
 * workers, so a path is checked by exactly the same rules wherever it runs. Thread-safe: no static buffers, getpwuid_r. */

static void full_of(const char *path, char *full, size_t n) {
    if (path[0] == '/') pathf(full, n, "%s", path);
    else pathf(full, n, "%s/%s", home, path);
}

int fsops_resolve_readable(const char *path, char *out, int *down) {
    char full[PATH_MAX * 2];
    if (down) *down = 0;
    full_of(path, full, sizeof full);
    if (!full[0]) return -1;
    int d = fsops_share_down(full);
    if (down) *down = d;
    if (d || !realpath(full, out)) return -1;
    return fsops_readable(out) ? 0 : -2;      /* -2: exists, but one of the folders not read */
}

int fsops_resolve_target(const char *path, char *out, size_t n, char *dir, size_t dn, int *down) {
    char full[PATH_MAX * 2];
    if (down) *down = 0;
    full_of(path, full, sizeof full);
    size_t len = strlen(full);
    while (len > 1 && full[len - 1] == '/') full[--len] = 0;
    char *slash = strrchr(full, '/');
    if (!slash) return -1;
    const char *base = slash + 1;
    if (!base[0] || !strcmp(base, ".") || !strcmp(base, "..") || strlen(base) > 255) return -1;
    *slash = 0;
    char rdir[PATH_MAX];
    int d = fsops_share_down(full);
    if (down) *down = d;
    if (d || !realpath(full[0] ? full : "/", rdir)) return -1;
    if (!fsops_allowed(rdir)) return -1;
    if ((size_t)snprintf(out, n, "%s/%s", rdir, base) >= n) return -1;
    if (dir && (size_t)snprintf(dir, dn, "%s", rdir) >= dn) return -1;
    if (fsops_protected(out)) return -2;
    return 0;
}

int fsops_resolve_stat(const char *path, char *target, size_t n, int *down) {
    int r = fsops_resolve_target(path, target, n, NULL, 0, down);
    if (r != -1) return r;
    /* outside the writable roots: the same answer, read-only, where reading is allowed
     * (its folder resolved, the last name kept, so a link is described, not followed) */
    char full[PATH_MAX * 2], rdir[PATH_MAX];
    full_of(path, full, sizeof full);
    size_t len = strlen(full);
    while (len > 1 && full[len - 1] == '/') full[--len] = 0;
    char *slash = strrchr(full, '/');
    const char *base = slash ? slash + 1 : "";
    if (slash && !strcmp(full, "/")) { if (fsops_readable("/")) { pathf(target, n, "/"); return 1; } }
    else if (slash && base[0] && strcmp(base, ".") && strcmp(base, "..")) {
        *slash = 0;
        if (realpath(full[0] ? full : "/", rdir) && fsops_readable(rdir) &&
            (size_t)snprintf(target, n, "%s%s%s", rdir, strcmp(rdir, "/") ? "/" : "", base) < n &&
            fsops_readable(target)) return 1;
    }
    return -1;
}

/* A folder Files shows with a lock: the system's permissions don't let this user open it, or it is one of the
 * pseudo-filesystems lord.fs doesn't read (fsops_readable). */
static int dir_locked(const char *path) {
    char real[PATH_MAX];
    if (!realpath(path, real) || !fsops_readable(real)) return 1;
    return access(real, R_OK | X_OK) != 0;
}

void fsops_user_name(uid_t u, char *out, size_t n) {
    struct passwd pw, *res = NULL;
    char buf[1024];
    if (!getpwuid_r(u, &pw, buf, sizeof buf, &res) && res) pathf(out, n, "%s", res->pw_name);
    else pathf(out, n, "?");
}

void fsops_group_name(gid_t g, char *out, size_t n) {
    struct group gr, *res = NULL;
    char buf[4096];
    if (!getgrgid_r(g, &gr, buf, sizeof buf, &res) && res) pathf(out, n, "%s", res->gr_name);
    else pathf(out, n, "?");
}

/* #85: the cloud and network mounts right inside dir (~/Cloud's accounts, a share mounted in the home folder), from
 * /proc/self/mounts: their root is the server's, and rclone holds a folder while it lists it from the provider, so a stat
 * of ~/Cloud/<account> waited for a provider that didn't answer, on the interface's thread when Files showed ~/Cloud.
 * They are listed as folders without touching them. names gets each one's last component; the count is returned. */
#define LIST_MOUNTS_MAX 32
static int remote_children(const char *dir, char names[][256]) {
    FILE *f = fopen("/proc/self/mounts", "re");
    if (!f) return 0;
    char line[4096], dev[1024], mnt[2048], ty[64];
    size_t dl = strlen(dir);
    while (dl > 0 && dir[dl - 1] == '/') dl--;          /* "/" is the empty prefix */
    int n = 0;
    while (n < LIST_MOUNTS_MAX && fgets(line, sizeof line, f)) {
        if (sscanf(line, "%1023s %2047s %63s", dev, mnt, ty) != 3 || !remote_type(ty)) continue;
        unescape_mount(mnt);
        if (strncmp(mnt, dir, dl) || mnt[dl] != '/') continue;
        const char *leaf = mnt + dl + 1;
        if (!leaf[0] || strchr(leaf, '/') || strlen(leaf) >= 256) continue;
        memcpy(names[n++], leaf, strlen(leaf) + 1);
    }
    fclose(f);
    return n;
}

int fsops_list(const char *dir, int max, FsListFn fn, void *ud) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    static __thread char mounts[LIST_MOUNTS_MAX][256];
    int nm = remote_children(dir, mounts);
    int i = 0;
    struct dirent *e;
    while (i < max && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        int mounted = 0;
        for (int k = 0; k < nm && !mounted; k++) mounted = !strcmp(mounts[k], e->d_name);
        if (mounted) {
            FsEntry x;
            memset(&x, 0, sizeof x);
            x.name = e->d_name;
            x.dir = 1;
            x.mode = 0700;
            x.uid = getuid();
            x.gid = getgid();
            fn(ud, &x);
            i++;
            continue;
        }
        char fp[PATH_MAX * 2];
        struct stat st, lst;
        if (pathf(fp, sizeof fp, "%s/%s", dir, e->d_name) < 0) continue;
        int link = !lstat(fp, &lst) && S_ISLNK(lst.st_mode);
        if (stat(fp, &st)) { if (!link) continue; st = lst; }     /* broken link: shown anyway */
        FsEntry x;
        memset(&x, 0, sizeof x);
        x.name = e->d_name;
        x.link = link;
        x.dir = S_ISDIR(st.st_mode);
        x.locked = x.dir && dir_locked(fp);      /* a folder this user can't open (shown with a lock in This computer) */
        x.size = (double)st.st_size;
        x.mtime = (double)st.st_mtime * 1000;
        x.atime = (double)st.st_atime * 1000;
        x.ctime = (double)st.st_ctime * 1000;
        x.mode = st.st_mode & 07777;
        x.uid = st.st_uid;
        x.gid = st.st_gid;
        fn(ud, &x);
        i++;
    }
    closedir(d);
    return i;
}

int fsops_stat(const char *target, int writable_root, FsStat *o) {
    struct statx sx;
    memset(o, 0, sizeof *o);
    if (statx(AT_FDCWD, target, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS | STATX_BTIME, &sx)) return -1;
    const char *base = strrchr(target, '/');
    pathf(o->name, sizeof o->name, "%s", base ? base + 1 : target);
    pathf(o->path, sizeof o->path, "%s", target);
    o->link = S_ISLNK(sx.stx_mode);
    struct stat st;
    o->dir = S_ISDIR(sx.stx_mode) || (o->link && !stat(target, &st) && S_ISDIR(st.st_mode));
    if (o->link) {
        ssize_t n = readlink(target, o->target, sizeof o->target - 1);
        if (n >= 0) { o->target[n] = 0; o->has_target = 1; }
    }
    o->mode = sx.stx_mode & 07777;
    o->size = (double)sx.stx_size;
    fsops_user_name(sx.stx_uid, o->user, sizeof o->user);
    fsops_group_name(sx.stx_gid, o->group, sizeof o->group);
#define TS(f) ((double)sx.f.tv_sec * 1000 + sx.f.tv_nsec / 1e6)
    o->mtime = TS(stx_mtime);
    o->atime = TS(stx_atime);
    o->ctime = TS(stx_ctime);
    o->has_btime = !!(sx.stx_mask & STATX_BTIME);
    if (o->has_btime) o->btime = TS(stx_btime);
#undef TS
    o->writable = writable_root && access(target, W_OK) == 0;
    return 0;
}
