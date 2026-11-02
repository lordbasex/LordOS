/* clean_admin.c — see clean_admin.h. Runs as root: folders are opened with no symbolic link on the way, must
 * be root's and not writable by others, and every removal is unlinkat on that folder, of an entry checked
 * with lstat semantics. It never recurses and never looks at /var/lib/dpkg. */
#include "clean_admin.h"
#include "cleanfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int regular_entry(int dirfd, const struct dirent *e, struct stat *st) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) return 0;
    return !fstatat(dirfd, e->d_name, st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st->st_mode);
}

static DIR *list(int dirfd) {
    int fd = fcntl(dirfd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return NULL;
    DIR *d = fdopendir(fd);
    if (!d) { close(fd); return NULL; }
    rewinddir(d);
    return d;
}

static long purge(int dirfd, int debs_only, double *freed) {
    DIR *d = list(dirfd);
    long n = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        struct stat st;
        if (!regular_entry(dirfd, e, &st)) continue;
        if (debs_only && !clean_glob("*.deb", e->d_name)) continue;
        if (!unlinkat(dirfd, e->d_name, 0)) { n++; *freed += (double)st.st_blocks * 512; }
    }
    if (d) closedir(d);
    return n;
}

long clean_purge_debs(int dirfd, double *freed) { return purge(dirfd, 1, freed); }
long clean_purge_partial(int dirfd, double *freed) { return purge(dirfd, 0, freed); }

long clean_purge_old(int dirfd, uid_t owner, time_t now, double *freed) {
    DIR *d = list(dirfd);
    long n = 0;
    time_t limit = now - CLEAN_VAR_TMP_DAYS * 86400;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        struct stat st, again;
        if (!regular_entry(dirfd, e, &st) || st.st_uid != owner || st.st_nlink != 1 || st.st_mtime > limit || st.st_ctime > limit) continue;
        /* a write lease is refused while anyone else has the file open, and holds others off until it's gone */
        int fd = openat(dirfd, e->d_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (!fcntl(fd, F_SETLEASE, F_WRLCK) && !fstat(fd, &again) && again.st_ino == st.st_ino && again.st_dev == st.st_dev &&
            !unlinkat(dirfd, e->d_name, 0)) { n++; *freed += (double)st.st_blocks * 512; }
        close(fd);
    }
    if (d) closedir(d);
    return n;
}

double clean_folder_bytes(int dirfd, int depth) {
    DIR *d = list(dirfd);
    double sum = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || fstatat(dirfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW)) continue;
        if (S_ISREG(st.st_mode)) sum += (double)st.st_blocks * 512;
        else if (S_ISDIR(st.st_mode) && depth > 0) {
            int sub = openat(dirfd, e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (sub >= 0) { sum += clean_folder_bytes(sub, depth - 1); close(sub); }
        }
    }
    if (d) closedir(d);
    return sum;
}

/* a system folder: no link on the way, root's, not writable by others unless sticky (/var/tmp) */
static int open_system(const char *path) {
    struct open_how how = { .flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, .resolve = RESOLVE_NO_SYMLINKS };
    int fd = (int)syscall(SYS_openat2, AT_FDCWD, path, &how, sizeof how);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || st.st_uid != 0 || ((st.st_mode & 022) && !(st.st_mode & S_ISVTX))) { close(fd); errno = EPERM; return -1; }
    return fd;
}

static double system_bytes(const char *path, int depth) {
    int fd = open_system(path);
    if (fd < 0) return 0;
    double b = clean_folder_bytes(fd, depth);
    close(fd);
    return b;
}

void clean_act_status(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)arg;
    if (len) { admin_err("admin.invalidArgument", NULL); return; }
    admin_say("OK", "journal=%.0f apt=%.0f vartmp=%.0f", system_bytes("/var/log/journal", 1),   /* i18n-ok */
              system_bytes("/var/cache/apt/archives", 1), system_bytes("/var/tmp", 0));
}

void clean_act_journal(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)arg;
    if (len) { admin_err("admin.invalidArgument", NULL); return; }
    const char *const argv[] = { "/usr/bin/journalctl", "--vacuum-size=200M", NULL };
    int code = admin_run(argv);
    if (code) admin_err("clean.systemFailed", "code", lerr_int(code), NULL);
    else admin_say("OK", "done");                       /* i18n-ok */
}

void clean_act_apt(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)arg;
    if (len) { admin_err("admin.invalidArgument", NULL); return; }
    int fd = open_system("/var/cache/apt/archives");
    if (fd < 0) { admin_err("clean.systemFailed", "code", lerr_int(errno), NULL); return; }
    double freed = 0;
    long n = clean_purge_debs(fd, &freed);
    close(fd);
    /* partial/ belongs to the download user, under a root-owned parent */
    struct open_how how = { .flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, .resolve = RESOLVE_NO_SYMLINKS };
    int p = (int)syscall(SYS_openat2, AT_FDCWD, "/var/cache/apt/archives/partial", &how, sizeof how);
    if (p >= 0) { n += clean_purge_partial(p, &freed); close(p); }
    admin_say("OK", "files=%ld bytes=%.0f", n, freed);  /* i18n-ok */
}

void clean_act_var_tmp(const AdminReq *r, const char *arg, size_t len) {
    (void)r; (void)arg;
    if (len) { admin_err("admin.invalidArgument", NULL); return; }
    int fd = open_system("/var/tmp");
    if (fd < 0) { admin_err("clean.systemFailed", "code", lerr_int(errno), NULL); return; }
    double freed = 0;
    long n = clean_purge_old(fd, 0, time(NULL), &freed);
    close(fd);
    admin_say("OK", "files=%ld bytes=%.0f", n, freed);  /* i18n-ok */
}
