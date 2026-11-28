/* fswatch_test.c — lord.fs.watch / unwatch (fswatch.c) on a temporary home folder.
 *
 * Checks (docs/drafts/DESKTOP-ICONS.md 5.1 and 7):
 *   - create, delete, rename within the folder, rename into and out of it, a file written: each makes a call;
 *   - coalescing: at most one call per watch every 150 ms, whatever the number of events; the poll's timeout says when;
 *   - the calls carry no file names (the callback only gets the id and two flags);
 *   - a folder deleted, or moved away and replaced by a link: overflow and gone, and nothing more from it;
 *   - the kernel's queue overflowing: every watch is told (overflow);
 *   - the limit: 16 watches, the 17th is fs.tooManyWatches; a removed one frees its place; two watches of one folder;
 *   - refusals: /proc (fs.systemFolder), a file (fs.notFolder), a missing path (fs.outsideHome), a FUSE or network mount
 *     from the mount table (fs.watchRemote);
 *   - clearing (the theme's context ended): no watch left, the inotify fd closed. */
#include "fswatch.h"
#include "fsops.h"
#include "errcode.h"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char H[256];

typedef struct { int n, id[64], overflow[64], gone[64]; } Calls;
static void record(void *ud, int id, int overflow, int gone) {
    Calls *c = ud;
    if (c->n < 64) { c->id[c->n] = id; c->overflow[c->n] = overflow; c->gone[c->n] = gone; }
    c->n++;
}
static int has_call(const Calls *c, int id) {
    for (int i = 0; i < c->n && i < 64; i++) if (c->id[i] == id) return 1;
    return 0;
}
static int call_of(const Calls *c, int id) {
    for (int i = 0; i < c->n && i < 64; i++) if (c->id[i] == id) return i;
    return -1;
}

/* waits (up to 1 s) for the inotify fd and reads it at the test's clock t */
static int pump(double t) {
    struct pollfd p = { fswatch_fd(), POLLIN, 0 };
    int r = poll(&p, 1, 1000);
    if (r > 0) fswatch_read(t);
    return r > 0;
}
/* reads whatever is queued without waiting */
static void drain(double t) {
    struct pollfd p = { fswatch_fd(), POLLIN, 0 };
    while (poll(&p, 1, 50) > 0) fswatch_read(t);
}

static void touch(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) { if (write(fd, "x", 1) < 0) perror("write"); close(fd); }
}

static void rm_rf(const char *p) {
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", p);
    if (system(cmd)) fprintf(stderr, "cleanup of %s failed\n", p);
}

int main(void) {
    snprintf(H, sizeof H, "/tmp/fswatch-test-%d", (int)getpid());
    mkdir(H, 0755);
    char real[PATH_MAX];
    if (!realpath(H, real)) { perror("realpath"); return 1; }
    if (strlen(real) >= sizeof H) return 1;
    memcpy(H, real, strlen(real) + 1);
    fsops_init(H, "nobody");
    char d[512], f[600], g[1024], out[512], err[256];
    snprintf(d, sizeof d, "%s/Desktop", H); mkdir(d, 0755);
    snprintf(out, sizeof out, "%s/Elsewhere", H); mkdir(out, 0755);

    /* ---- a watch and its calls ---- */
    CHECK(fswatch_fd() < 0);
    int id = fswatch_add("Desktop", err, sizeof err);       /* relative to the home folder, as lord.fs takes it */
    CHECK(id > 0);
    CHECK(fswatch_fd() >= 0);
    CHECK(fswatch_count() == 1);
    CHECK(fswatch_timeout(0) < 0);                         /* nothing waiting: no timeout */

    Calls c = { 0 };
    double t = 1000;
    snprintf(f, sizeof f, "%s/a.txt", d);
    touch(f);                                              /* create (+ close_write) */
    CHECK(pump(t));
    CHECK(fswatch_timeout(t) == 0);                        /* first change after a quiet time: due at once */
    CHECK(fswatch_dispatch(t, record, &c) == 1);
    CHECK(c.n == 1 && c.id[0] == id && !c.overflow[0] && !c.gone[0]);

    /* coalescing: more changes within 150 ms of the last call wait for it, and make a single call */
    memset(&c, 0, sizeof c);
    snprintf(g, sizeof g, "%s/b.txt", d);
    t = 1050;
    touch(g);
    CHECK(pump(t));
    unlink(f);                                             /* delete */
    drain(t);
    CHECK(fswatch_dispatch(t, record, &c) == 0);
    CHECK(fswatch_timeout(t) == 100);                      /* due 150 ms after the last call (1000 + 150) */
    CHECK(fswatch_dispatch(1149, record, &c) == 0);
    CHECK(fswatch_dispatch(1150, record, &c) == 1);
    CHECK(c.n == 1 && c.id[0] == id);
    CHECK(fswatch_dispatch(1200, record, &c) == 0);        /* nothing left */
    CHECK(fswatch_timeout(1200) < 0);

    /* a burst of 200 files: one call */
    memset(&c, 0, sizeof c);
    t = 2000;
    for (int i = 0; i < 200; i++) { char p[700]; snprintf(p, sizeof p, "%s/burst-%d", d, i); touch(p); }
    CHECK(pump(t));
    drain(t);
    CHECK(fswatch_dispatch(t, record, &c) == 1);
    t = 2100;
    for (int i = 0; i < 200; i++) { char p[700]; snprintf(p, sizeof p, "%s/burst-%d", d, i); unlink(p); }
    drain(t);
    CHECK(fswatch_dispatch(t, record, &c) == 0);
    CHECK(fswatch_dispatch(2150, record, &c) == 1);
    CHECK(c.n == 2);

    /* rename within, out of and into the folder; attributes */
    struct { const char *what; int step; } steps[] = { { "rename within", 0 }, { "rename out", 1 }, { "rename in", 2 }, { "chmod", 3 } };
    snprintf(f, sizeof f, "%s/b.txt", d);
    for (int s = 0; s < 4; s++) {
        memset(&c, 0, sizeof c);
        t = 3000 + s * 1000;
        char p1[700], p2[700];
        snprintf(p1, sizeof p1, "%s/b.txt", d);
        snprintf(p2, sizeof p2, "%s/c.txt", d);
        if (steps[s].step == 0) CHECK(!rename(p1, p2));
        if (steps[s].step == 1) { snprintf(p1, sizeof p1, "%s/c.txt", out); CHECK(!rename(p2, p1)); }
        if (steps[s].step == 2) { snprintf(p2, sizeof p2, "%s/c.txt", out); CHECK(!rename(p2, p1)); }
        if (steps[s].step == 3) CHECK(!chmod(p1, 0600));
        CHECK(pump(t));
        drain(t);
        CHECK(fswatch_dispatch(t, record, &c) == 1);
        if (c.n != 1) fprintf(stderr, "  (step: %s)\n", steps[s].what);
        CHECK(c.n == 1 && !c.overflow[0] && !c.gone[0]);
    }
    /* a change in another folder says nothing */
    memset(&c, 0, sizeof c);
    snprintf(g, sizeof g, "%s/other.txt", out);
    touch(g);
    drain(9000);
    CHECK(fswatch_dispatch(9000, record, &c) == 0);
    /* nor a change inside a subfolder (non-recursive) */
    char sub[700];
    snprintf(sub, sizeof sub, "%s/sub", d);
    mkdir(sub, 0755);
    drain(9500);
    fswatch_dispatch(9500, NULL, NULL);
    snprintf(g, sizeof g, "%s/deep.txt", sub);
    touch(g);
    drain(10000);
    CHECK(fswatch_dispatch(10000, record, &c) == 0);

    /* ---- refusals ---- */
    CHECK(fswatch_add("/proc", err, sizeof err) < 0 && lerr_has(err, "fs.systemFolder"));
    CHECK(fswatch_add("/proc/self", err, sizeof err) < 0 && lerr_has(err, "fs.systemFolder"));
    CHECK(fswatch_add(f, err, sizeof err) < 0 && lerr_has(err, "fs.notFolder"));
    CHECK(fswatch_add("Missing", err, sizeof err) < 0 && lerr_has(err, "fs.outsideHome"));
    CHECK(fswatch_add("", err, sizeof err) < 0 && lerr_has(err, "sys.badArgument"));
    CHECK(fswatch_add(NULL, err, sizeof err) < 0 && lerr_has(err, "sys.badArgument"));
    /* a link to a file is a file; a link to a folder is that folder, resolved */
    char lnk[700];
    snprintf(lnk, sizeof lnk, "%s/to-file", H);
    CHECK(!symlink(f, lnk));
    CHECK(fswatch_add("to-file", err, sizeof err) < 0 && lerr_has(err, "fs.notFolder"));
    /* a FUSE mount (a Cloud account) and a network share, from the mount table */
    char cloud[700], share[700], mounts[700];
    snprintf(cloud, sizeof cloud, "%s/Cloud", H); mkdir(cloud, 0755);
    snprintf(share, sizeof share, "%s/Share", H); mkdir(share, 0755);
    snprintf(mounts, sizeof mounts, "%s/mounts", H);
    FILE *mf = fopen(mounts, "w");
    if (mf) {
        fprintf(mf, "/dev/sda1 / ext4 rw 0 0\nrclone %s fuse.rclone rw,nosuid 0 0\n//srv/x %s cifs rw 0 0\n", cloud, share);
        fclose(mf);
    }
    fswatch_test_mounts = mounts;
    CHECK(fswatch_add("Cloud", err, sizeof err) < 0 && lerr_has(err, "fs.watchRemote"));
    CHECK(fswatch_add(share, err, sizeof err) < 0 && lerr_has(err, "fs.watchRemote"));
    char inside[800];
    snprintf(inside, sizeof inside, "%s/folder", cloud); mkdir(inside, 0755);
    CHECK(fswatch_add(inside, err, sizeof err) < 0 && lerr_has(err, "fs.watchRemote"));
    int okd = fswatch_add("Elsewhere", err, sizeof err);  /* a local folder in that same table is fine */
    CHECK(okd > 0);
    CHECK(fswatch_remove(okd) == 0);
    fswatch_test_mounts = NULL;
    CHECK(fswatch_count() == 1);                           /* the refusals left nothing behind */

    /* ---- the limit and two watches of one folder ---- */
    int ids[FSWATCH_MAX];
    ids[0] = id;
    for (int i = 1; i < FSWATCH_MAX; i++) {
        char p[700];
        snprintf(p, sizeof p, "%s/w%d", H, i);
        mkdir(p, 0755);
        ids[i] = fswatch_add(p, err, sizeof err);
        CHECK(ids[i] > 0);
    }
    CHECK(fswatch_count() == FSWATCH_MAX);
    CHECK(fswatch_add("Elsewhere", err, sizeof err) < 0 && lerr_has(err, "fs.tooManyWatches"));
    CHECK(strstr(err, "max=16") != NULL);
    CHECK(fswatch_remove(ids[5]) == 0);
    CHECK(fswatch_remove(ids[5]) == -1);                   /* already gone */
    CHECK(fswatch_remove(0) == -1 && fswatch_remove(-3) == -1 && fswatch_remove(99999) == -1);
    int twin = fswatch_add(d, err, sizeof err);            /* the same folder as `id`, by its full path */
    CHECK(twin > 0 && twin != id);
    memset(&c, 0, sizeof c);
    touch(f);
    CHECK(pump(20000));
    drain(20000);
    CHECK(fswatch_dispatch(20000, record, &c) == 2);
    CHECK(has_call(&c, id) && has_call(&c, twin));
    CHECK(fswatch_remove(twin) == 0);                      /* the other keeps the kernel's watch */
    memset(&c, 0, sizeof c);
    unlink(f);
    CHECK(pump(21000));
    drain(21000);
    CHECK(fswatch_dispatch(21000, record, &c) == 1 && c.id[0] == id);

    /* ---- the kernel's queue overflowing: every watch hears about it ---- */
    long maxq = 16384;
    FILE *q = fopen("/proc/sys/fs/inotify/max_queued_events", "r");
    if (q) { if (fscanf(q, "%ld", &maxq) != 1) maxq = 16384; fclose(q); }
    if (maxq <= 40000) {
        char many[700];
        snprintf(many, sizeof many, "%s/many", d);
        mkdir(many, 0755);
        int wm = fswatch_add(many, err, sizeof err);
        CHECK(wm > 0);
        drain(30000);
        fswatch_dispatch(30000, NULL, NULL);
        long n = maxq / 2 + 200;                           /* create + close_write per file */
        for (long i = 0; i < n; i++) {
            char p[800];
            snprintf(p, sizeof p, "%s/f%ld", many, i);
            touch(p);
        }
        memset(&c, 0, sizeof c);
        drain(31000);
        fswatch_dispatch(31000, record, &c);
        int k = call_of(&c, wm), k0 = call_of(&c, ids[1]);
        CHECK(k >= 0 && c.overflow[k]);
        CHECK(k0 >= 0 && c.overflow[k0] && !c.gone[k0]);   /* a quiet watch is told too: it may have missed something */
        CHECK(fswatch_remove(wm) == 0);
        rm_rf(many);
        drain(32000);
        fswatch_dispatch(32000, NULL, NULL);
    } else {
        printf("skipped: the queue-overflow case (max_queued_events %ld is large)\n", maxq);
    }

    /* ---- the folder deleted, or moved away and replaced by a link ---- */
    char gone[700];
    snprintf(gone, sizeof gone, "%s/w1", H);
    memset(&c, 0, sizeof c);
    CHECK(!rmdir(gone));
    CHECK(pump(40000));
    drain(40000);
    CHECK(fswatch_dispatch(40000, record, &c) == 1);
    CHECK(c.id[0] == ids[1] && c.overflow[0] && c.gone[0]);
    CHECK(fswatch_count() == FSWATCH_MAX - 1);             /* (it stays until JS removes it; ids[5] was removed) */
    CHECK(fswatch_remove(ids[1]) == 0);

    char moved[700], target[700];
    snprintf(moved, sizeof moved, "%s/w2", H);
    snprintf(target, sizeof target, "%s/w2-moved", H);
    memset(&c, 0, sizeof c);
    CHECK(!rename(moved, target));
    CHECK(!symlink(out, moved));                           /* the old name is now a link to another folder */
    CHECK(pump(41000));
    drain(41000);
    CHECK(fswatch_dispatch(41000, record, &c) == 1);
    CHECK(c.id[0] == ids[2] && c.overflow[0] && c.gone[0]);
    memset(&c, 0, sizeof c);
    snprintf(g, sizeof g, "%s/after-move", target);
    touch(g);                                              /* the moved folder is no longer followed */
    drain(42000);
    CHECK(fswatch_dispatch(42000, record, &c) == 0);
    CHECK(fswatch_remove(ids[2]) == 0);
    int relinked = fswatch_add(moved, err, sizeof err);    /* watching the name again watches what it resolves to */
    CHECK(relinked > 0);
    memset(&c, 0, sizeof c);
    snprintf(g, sizeof g, "%s/via-link", out);
    touch(g);
    CHECK(pump(43000));
    drain(43000);
    CHECK(fswatch_dispatch(43000, record, &c) >= 1 && has_call(&c, relinked));

    CHECK(fswatch_remove(relinked) == 0);

    /* ---- the theme's context ended ---- */
    fswatch_clear();
    CHECK(fswatch_count() == 0);
    CHECK(fswatch_fd() < 0);
    CHECK(fswatch_dispatch(50000, record, &c) == 0);
    CHECK(fswatch_timeout(50000) < 0);
    int again = fswatch_add("Desktop", err, sizeof err);   /* a new context starts from nothing */
    CHECK(again > 0 && fswatch_count() == 1);
    fswatch_clear();

    rm_rf(H);
    printf("fswatch-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
