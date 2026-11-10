/* dndgate.c — who may drop files on LordOS (wl.c): a Linux app's drag of text/uri-list released over LordOS
 * is copied by Files, so it must name files the app could reach itself. A Flatpak app (its root holds
 * .flatpak-info) sees only part of the person's files, so its drags never hand files to LordOS (nor do an
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

int dndg_pid_sandboxed(int pid) {
    char path[32];
    if (pid <= 0) return 1;
    snprintf(path, sizeof path, "/proc/%d/root", pid);
    /* the magic link opens the process's own root (ptrace read access: our own processes only) */
    int fd = open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);
    int r = dndg_root_sandboxed(fd);
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
