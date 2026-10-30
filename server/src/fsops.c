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
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
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

static char home[PATH_MAX], media1[PATH_MAX], media2[PATH_MAX];

void fsops_init(const char *h, const char *user) {
    pathf(home, sizeof home, "%s", h);
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
                                           NULL };

int fsops_protected(const char *real) {
    for (int i = 0; protect[i]; i++) {
        char p[PATH_MAX + 64];
        pathf(p, sizeof p, "%s%s", home, protect[i]);
        if (under(real, p)) return 1;
    }
    return 0;
}

/* Is a protected path strictly below real? (Cleaner's rule table: a rule's root must not hold one.) */
int fsops_protected_below(const char *real) {
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
    if (renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE)) {
        if (errno == EEXIST) seterr(err, errlen, "fs.exists");
        else seterrno(err, errlen, errno);
        return -1;
    }
    return 0;
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
    if (move && rename(src, dst) == 0) { j->done = 1; return j->id; }   /* same disk: instant */
    if (move && errno != EXDEV) { int e = errno; j->used = 0; seterrno(err, errlen, e); return -1; }
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
    (void)fs; (void)total; (void)avail;
    TrashDirs *t = ud;
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
    struct stat st;
    if (depth > 32 || lstat(p, &st)) return 0;
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
    out->complete = nftw(real, du_cb, 32, FTW_PHYS) == 0;
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

typedef struct { const char *needle; int limit, depth, hits, visited; FsHitFn fn; void *ud; } Search;
static void search_dir(const char *p, Search *s, int depth) {
    if (depth > s->depth || s->hits >= s->limit || s->visited > 100000) return;
    DIR *d = opendir(p);
    struct dirent *e;
    while (d && (e = readdir(d)) && s->hits < s->limit) {
        if (e->d_name[0] == '.') continue;                  /* hidden ones and . .. */
        s->visited++;
        char c[PATH_MAX];
        if ((size_t)snprintf(c, sizeof c, "%s/%s", p, e->d_name) >= sizeof c) continue;
        struct stat st;
        if (lstat(c, &st)) continue;
        if (has_ci(e->d_name, s->needle)) { s->hits++; s->fn(s->ud, c, S_ISDIR(st.st_mode), (double)st.st_size, (double)st.st_mtime * 1000); }
        if (S_ISDIR(st.st_mode)) search_dir(c, s, depth + 1);
    }
    if (d) closedir(d);
}

int fs_search(const char *real, const char *needle, int limit, int depth, FsHitFn fn, void *ud) {
    Search s = { needle, limit, depth, 0, 0, fn, ud };
    if (needle[0]) search_dir(real, &s, 0);
    return s.hits;
}

typedef struct { char path[PATH_MAX]; double mtime, size; } Recent;
typedef struct { Recent *r; int n, cap, visited; time_t since; } RecentAcc;
static void recent_dir(const char *p, RecentAcc *a, int depth) {
    if (depth > 5 || a->visited > 60000) return;
    DIR *d = opendir(p);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "node_modules")) continue;
        a->visited++;
        char c[PATH_MAX];
        if ((size_t)snprintf(c, sizeof c, "%s/%s", p, e->d_name) >= sizeof c) continue;
        struct stat st;
        if (lstat(c, &st)) continue;
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
    RecentAcc a = { calloc((size_t)limit, sizeof(Recent)), 0, limit, 0, time(NULL) - (time_t)days * 86400 };
    if (!a.r) return 0;
    recent_dir(home, &a, 0);
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

void fs_volumes(FsVolFn fn, void *ud) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        char dev[1024], mnt[2048], fstype[64];
        if (sscanf(line, "%1023s %2047s %63s", dev, mnt, fstype) != 3) continue;
        unescape_mount(mnt);
        size_t l1 = strlen(media1), l2 = strlen(media2);
        int ok = (!strncmp(mnt, media1, l1) && mnt[l1] == '/') || (!strncmp(mnt, media2, l2) && mnt[l2] == '/');
        if (!ok) continue;
        struct statvfs vs;
        double total = 0, avail = 0;
        if (!statvfs(mnt, &vs)) { total = (double)vs.f_blocks * vs.f_frsize; avail = (double)vs.f_bavail * vs.f_frsize; }
        fn(ud, mnt, fstype, total, avail);
    }
    fclose(f);
}
