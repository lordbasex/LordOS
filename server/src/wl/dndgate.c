/* dndgate.c — who may drop files on LordOS (wl.c): a Linux app's drag of text/uri-list released over LordOS
 * is copied by Files, so it must name files the app could reach itself. A Flatpak app (its root holds
 * .flatpak-info), or any process in another root or mount, pid or user namespace, sees only part of the
 * person's files, so its drags never hand files to LordOS (nor do an
 * isolated LordOS app's, which wl.c checks through the broker). And a drop is taken only by what the UI runs
 * for the release that made it (DropGate): nothing later can claim a drop left pending. Tests:
 * tests/dndgate_test.c. */
#include "dndgate.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int dndg_root_sandboxed(int rootfd) {
    struct stat st;
    if (rootfd < 0) return 1;
    return fstatat(rootfd, ".flatpak-info", &st, AT_SYMLINK_NOFOLLOW) == 0;
}

/* 1 if the process's namespace link (ns/mnt, ns/pid, ns/user) is the same one as ours */
static int same_ns(int pid, const char *ns) {
    char a[48], b[32];
    struct stat sa, sb;
    snprintf(a, sizeof a, "/proc/%d/ns/%s", pid, ns);
    snprintf(b, sizeof b, "/proc/self/ns/%s", ns);
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

int dndg_pid_sandboxed(int pid) {
    char path[32];
    if (pid <= 0) return 1;
    snprintf(path, sizeof path, "/proc/%d/root", pid);
    /* the magic link opens the process's own root (ptrace read access: our own processes only) */
    int fd = open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);
    int r = dndg_root_sandboxed(fd);
    /* fail closed: unsandboxed only on positive evidence, the same root and the same mount, pid and user
     * namespaces as lord-server (any other sandbox or container counts as one) */
    struct stat sr, sl;
    if (!r && (fstat(fd, &sr) != 0 || stat("/", &sl) != 0 || sr.st_dev != sl.st_dev || sr.st_ino != sl.st_ino)) r = 1;
    if (!r && (!same_ns(pid, "mnt") || !same_ns(pid, "pid") || !same_ns(pid, "user"))) r = 1;
    if (fd >= 0) close(fd);
    return r;
}

void dndg_drop(DropGate *g) {
    g->pending = g->claimable = 1;
    g->claimed = 0;
}

int dndg_claim(DropGate *g) {
    if (!g->pending || !g->claimable || g->claimed) return 0;
    g->claimed = 1;
    return 1;
}

int dndg_release_done(DropGate *g) {
    int was = g->claimable;
    g->claimable = 0;
    return was && g->pending && !g->claimed;
}

void dndg_reset(DropGate *g) {
    g->pending = g->claimable = g->claimed = 0;
}
