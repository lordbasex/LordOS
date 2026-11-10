/* dndgate_test.c — who may drop files on LordOS (src/wl/dndgate.c): a sandboxed root, and a drop claimed only
 * during the release that made it. */
#include "../src/wl/dndgate.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

int main(void) {
    /* a root: plain, with .flatpak-info (a file, a symlink to nowhere, a directory) */
    char dir[] = "/tmp/dndgate-XXXXXX", p[256];
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    int fd = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(!dndg_root_sandboxed(fd));
    snprintf(p, sizeof p, "%s/.flatpak-info", dir);
    CHECK(symlink("/nonexistent/flatpak-info", p) == 0);
    CHECK(dndg_root_sandboxed(fd));             /* not followed: still there */
    unlink(p);
    CHECK(!dndg_root_sandboxed(fd));
    int f = open(p, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    CHECK(f >= 0);
    if (f >= 0) close(f);
    CHECK(dndg_root_sandboxed(fd));
    unlink(p);
    CHECK(mkdir(p, 0700) == 0);
    CHECK(dndg_root_sandboxed(fd));
    rmdir(p);
    /* .flatpak-info lower down does not count: only the root's */
    snprintf(p, sizeof p, "%s/sub", dir);
    CHECK(mkdir(p, 0700) == 0);
    char q[300];
    snprintf(q, sizeof q, "%s/.flatpak-info", p);
    f = open(q, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (f >= 0) close(f);
    CHECK(!dndg_root_sandboxed(fd));
    unlink(q);
    rmdir(p);
    close(fd);
    rmdir(dir);
    CHECK(dndg_root_sandboxed(-1));             /* what can't be looked at counts as sandboxed */

    /* a process: this one is not sandboxed (unless the tests themselves run in a Flatpak); none and gone are */
    struct stat st;
    if (stat("/.flatpak-info", &st) != 0) CHECK(!dndg_pid_sandboxed((int)getpid()));
    CHECK(dndg_pid_sandboxed(0));
    CHECK(dndg_pid_sandboxed(-5));
    CHECK(dndg_pid_sandboxed(0x7ffffff0));

    /* the gate: claimed during its release, once */
    DropGate g = { 0 };
    CHECK(!dndg_claim(&g));                     /* no drop */
    CHECK(!dndg_release_done(&g));
    dndg_drop(&g);
    CHECK(dndg_claim(&g));
    CHECK(!dndg_claim(&g));                     /* once */
    CHECK(!dndg_release_done(&g));              /* claimed: not cancelled */
    CHECK(g.pending && g.claimed);
    dndg_reset(&g);
    CHECK(!g.pending && !g.claimable && !g.claimed);

    /* nobody claims it during the release: cancelled, and no later claim takes it */
    dndg_drop(&g);
    CHECK(dndg_release_done(&g));
    CHECK(!dndg_claim(&g));
    CHECK(!dndg_release_done(&g));              /* told once */
    dndg_reset(&g);

    /* a later release (another click) can't claim a drop left pending */
    dndg_drop(&g);
    g.claimable = 0;                            /* as after its own release */
    CHECK(!dndg_claim(&g));
    CHECK(!dndg_release_done(&g));
    dndg_reset(&g);

    /* a new drop replaces the old one: claimable again, not claimed */
    dndg_drop(&g);
    CHECK(dndg_claim(&g));
    dndg_drop(&g);
    CHECK(!g.claimed && g.claimable);
    CHECK(dndg_release_done(&g));

    if (fails) { printf("dndgate-test: %d failed\n", fails); return 1; }
    printf("dndgate-test: ok\n");
    return 0;
}
