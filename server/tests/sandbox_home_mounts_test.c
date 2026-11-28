/* sandbox_home_mounts_test.c — the launcher's view of a home with mounts under it (sandbox_files_isolate, keep_home and
 * home_ro: Linux and Flathub apps): it is built, the home and a mount under it are read-only in it, and a mount the view
 * can't reach by its path (covered by a later mount on a folder above it) doesn't stop it. Before 2026-11-20 that last one
 * made every such app fail with "could not build the app's view". Needs unprivileged user namespaces (exit 77 without). */
#include "../src/sec/sandbox.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int put(const char *path, const char *s) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int r = write(fd, s, strlen(s)) == (ssize_t)strlen(s) ? 0 : -1;
    close(fd);
    return r;
}

static int writable(const char *dir) {
    char p[4200];
    snprintf(p, sizeof p, "%s/probe", dir);
    int fd = open(p, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return errno == EROFS ? 0 : -1;
    close(fd);
    unlink(p);
    return 1;
}

int main(void) {
    char base[4096];                                      /* not under /tmp: the view mounts its own there */
    if (!getcwd(base, sizeof base - 32)) return 1;
    /* the view puts its own empty /tmp, /var/tmp and /dev/shm over the real ones: a tree there can't be checked */
    if (!strncmp(base, "/tmp/", 5) || !strcmp(base, "/tmp") || !strncmp(base, "/var/tmp/", 9) || !strncmp(base, "/dev/shm/", 9)) {
        printf("sandbox-home-mounts-test: skipped: the tree is under %s, which the sandbox's view replaces (build it elsewhere)\n", base);
        return 77;
    }
    strcat(base, "/build/sbx-home-test.XXXXXX");
    if (!mkdtemp(base)) { perror("mkdtemp"); return 1; }
    char home[4200], a[4300], ab[4400], c[4300];
    snprintf(home, sizeof home, "%s/home", base);
    snprintf(a, sizeof a, "%s/a", home);
    snprintf(ab, sizeof ab, "%s/a/b", home);
    snprintf(c, sizeof c, "%s/c", home);
    mkdir(home, 0700); mkdir(a, 0700); mkdir(ab, 0700); mkdir(c, 0700);
    int res[2];
    if (pipe(res)) return 1;
    pid_t t = fork();
    if (t == 0) {
        close(res[0]);
        /* the "computer": a user and mount namespace of its own where the home has mounts under it, as root there */
        unsigned uid = (unsigned)getuid(), gid = (unsigned)getgid();
        char m[64];
        if (unshare(CLONE_NEWUSER | CLONE_NEWNS)) { if (write(res[1], "S", 1) < 0) {} _exit(0); }
        put("/proc/self/setgroups", "deny");
        snprintf(m, sizeof m, "0 %u 1", uid); put("/proc/self/uid_map", m);
        snprintf(m, sizeof m, "0 %u 1", gid); put("/proc/self/gid_map", m);
        if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) ||
            mount("tmpfs", ab, "tmpfs", 0, "size=1m") ||       /* a cloud drive under ~/a ... */
            mount("tmpfs", a, "tmpfs", 0, "size=1m") ||        /* ... covered by a later mount on ~/a */
            mount("tmpfs", c, "tmpfs", 0, "size=1m") ||        /* and one the view sees */
            chown(a, 0, 0) || chown(c, 0, 0)) { if (write(res[1], "M", 1) < 0) {} _exit(0); }
        SandboxFilesCfg cfg = { .home = home, .keep_home = 1, .home_ro = 1 };
        if (sandbox_files_isolate(&cfg)) _exit(0);              /* the caller's side: it already said why */
        /* the app, in the view */
        char out[3] = { 'V', (char)('0' + (writable(home) + 1)), (char)('0' + (writable(c) + 1)) };
        if (write(res[1], out, 3) < 0) {}
        _exit(0);
    }
    close(res[1]);
    char r[3] = { 0 };
    ssize_t n = 0, k;
    while (n < 3 && (k = read(res[0], r + n, (size_t)(3 - n))) > 0) n += k;
    int st;
    waitpid(t, &st, 0);
    rmdir(ab); rmdir(a); rmdir(c); rmdir(home); rmdir(base);
    if (n >= 1 && r[0] == 'S') { printf("sandbox-home-mounts-test: skipped: needs unprivileged user namespaces\n"); return 77; }
    if (n >= 1 && r[0] == 'M') { printf("sandbox-home-mounts-test: skipped: couldn't mount in a user namespace\n"); return 77; }
    int fails = 0;
    if (n != 3 || r[0] != 'V') { printf("FAIL: the view wasn't built with a covered mount under the home\n"); fails++; }
    else {
        if (r[1] != '1') { printf("FAIL: the home is writable in the view (%c)\n", r[1]); fails++; }
        if (r[2] != '1') { printf("FAIL: the mount under the home is writable in the view (%c)\n", r[2]); fails++; }
    }
    printf("sandbox-home-mounts-test: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
