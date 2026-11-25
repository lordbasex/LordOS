/* fswatch.c — lord.fs.watch / unwatch (docs/drafts/DESKTOP-ICONS.md 5.1): tells JS that a folder changed, so the
 * desktop and Files list it again (asynchronously) instead of polling.
 *
 * Read-only and narrow by design:
 *   - the folder is resolved like a read (fsops_resolve_readable: realpath, never /proc, /sys, /dev, /run), must be a
 *     folder, and is refused on a FUSE or network mount (inotify doesn't see the server's changes; the caller lists
 *     again on focus instead);
 *   - one inotify fd in lord-server's poll loop, one non-recursive watch per folder (IN_ONLYDIR | IN_DONT_FOLLOW);
 *   - at most FSWATCH_MAX watches per session (inotify's per-user limit is shared with the person's own programs);
 *   - events are coalesced: at most one call per watch every FSWATCH_COALESCE_MS, carrying no file names, only that
 *     something changed (and whether the folder itself went away or the kernel's queue overflowed);
 *   - nothing is ever run or written. Isolated apps never get it (their lord.fs is the broker's, js_broker.c).
 * Every watch ends with the theme's context (js_shutdown → fswatch_clear). */
#include "fswatch.h"
#include "fsops.h"
#include "errcode.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#ifdef FSWATCH_TEST
const char *fswatch_test_mounts;
#endif

#define MASK (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF | \
              IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW | IN_EXCL_UNLINK)
#define SELF_GONE (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT | IN_IGNORED)

typedef struct {
    int used, id, wd, pending, overflow, gone;
    double last, due;                   /* the last call; when the waiting one is due */
} Watch;

static Watch w[FSWATCH_MAX];
static int ino = -1, next_id = 1;

int fswatch_fd(void) { return ino; }

int fswatch_count(void) {
    int n = 0;
    for (int i = 0; i < FSWATCH_MAX; i++) n += w[i].used;
    return n;
}

/* a cloud, network or other FUSE mount: from the mount table, and from the file system's own magic number in case the
 * table is not what it seems (a bind mount of a FUSE folder) */
static int remote_folder(const char *real) {
    char ty[64];
#ifdef FSWATCH_TEST
    const char *mounts = fswatch_test_mounts ? fswatch_test_mounts : "/proc/self/mounts";
#else
    const char *mounts = "/proc/self/mounts";
#endif
    if (!fsops_mount_type_in(mounts, real, ty, sizeof ty) && fsops_remote_type(ty)) return 1;
    struct statfs sf;
    if (statfs(real, &sf)) return 1;                       /* can't tell: not watched */
    switch ((unsigned long)sf.f_type) {
    case 0x65735546UL:                                     /* FUSE_SUPER_MAGIC */
    case 0x6969UL:                                         /* NFS_SUPER_MAGIC */
    case 0x517BUL:                                         /* SMB_SUPER_MAGIC */
    case 0xFE534D42UL:                                     /* SMB2_MAGIC_NUMBER */
    case 0xFF534D42UL:                                     /* CIFS_MAGIC_NUMBER */
    case 0x01021997UL:                                     /* V9FS_MAGIC */
    case 0x00C36400UL:                                     /* CEPH_SUPER_MAGIC */
        return 1;
    default:
        return 0;
    }
}

int fswatch_add(const char *path, char *err, size_t errlen) {
    if (!path || !path[0] || strlen(path) >= PATH_MAX) { lerr_set(err, errlen, "sys.badArgument", NULL); return -1; }
    if (fswatch_count() >= FSWATCH_MAX) { lerr_set(err, errlen, "fs.tooManyWatches", "max", "16", NULL); return -1; }
    char real[PATH_MAX];
    int down = 0, r = fsops_resolve_readable(path, real, &down);
    if (r == -2) { lerr_set(err, errlen, "fs.systemFolder", NULL); return -1; }
    if (r) { lerr_set(err, errlen, down ? "share.notResponding" : "fs.outsideHome", NULL); return -1; }
    struct stat before, after;
    if (lstat(real, &before)) { lerr_set(err, errlen, "sys.errno", "errno", lerr_errno(errno), NULL); return -1; }
    if (!S_ISDIR(before.st_mode)) { lerr_set(err, errlen, "fs.notFolder", NULL); return -1; }
    if (remote_folder(real)) { lerr_set(err, errlen, "fs.watchRemote", NULL); return -1; }
    if (ino < 0) {
        ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (ino < 0) { lerr_set(err, errlen, "sys.errno", "errno", lerr_errno(errno), NULL); return -1; }
    }
    int wd = inotify_add_watch(ino, real, MASK);
    if (wd < 0) { lerr_set(err, errlen, "sys.errno", "errno", lerr_errno(errno), NULL); return -1; }
    int shared = 0, slot = -1;
    for (int i = 0; i < FSWATCH_MAX; i++) {
        if (w[i].used && w[i].wd == wd) shared = 1;
        if (!w[i].used && slot < 0) slot = i;
    }
    /* the folder checked is the folder watched: one swapped for another (or for a link) in between is refused */
    if (lstat(real, &after) || after.st_dev != before.st_dev || after.st_ino != before.st_ino || !S_ISDIR(after.st_mode)) {
        if (!shared) inotify_rm_watch(ino, wd);
        lerr_set(err, errlen, "sys.errno", "errno", lerr_errno(ESTALE), NULL);
        return -1;
    }
    if (next_id <= 0) next_id = 1;
    w[slot] = (Watch){ .used = 1, .id = next_id++, .wd = wd, .last = -1e18 };
    return w[slot].id;
}

/* drops the kernel's watch when no other slot uses it (two watches of the same folder share one wd) */
static void release(int i) {
    int wd = w[i].wd;
    w[i] = (Watch){ 0 };
    if (wd < 0 || ino < 0) return;
    for (int k = 0; k < FSWATCH_MAX; k++) if (w[k].used && w[k].wd == wd) return;
    inotify_rm_watch(ino, wd);
}

int fswatch_remove(int id) {
    if (id <= 0) return -1;
    for (int i = 0; i < FSWATCH_MAX; i++) if (w[i].used && w[i].id == id) { release(i); return 0; }
    return -1;
}

void fswatch_clear(void) {
    for (int i = 0; i < FSWATCH_MAX; i++) w[i] = (Watch){ 0 };
    if (ino >= 0) close(ino);
    ino = -1;
}

static void mark(Watch *x, double now, int overflow) {
    x->overflow |= overflow;
    if (x->pending) return;
    x->pending = 1;
    x->due = x->last + FSWATCH_COALESCE_MS > now ? x->last + FSWATCH_COALESCE_MS : now;
}

void fswatch_read(double now) {
    if (ino < 0) return;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t n = read(ino, buf, sizeof buf);
        if (n <= 0) break;
        for (char *p = buf; p + sizeof(struct inotify_event) <= buf + n;) {
            const struct inotify_event *e = (const struct inotify_event *)p;
            p += sizeof *e + e->len;
            if (e->mask & IN_Q_OVERFLOW) {
                for (int i = 0; i < FSWATCH_MAX; i++) if (w[i].used) mark(&w[i], now, 1);
                continue;
            }
            int self = (e->mask & SELF_GONE) != 0;
            for (int i = 0; i < FSWATCH_MAX; i++) {
                if (!w[i].used || w[i].wd < 0 || w[i].wd != e->wd) continue;
                mark(&w[i], now, self);
                if (self) { w[i].gone = 1; w[i].wd = -1; }
            }
            /* the folder went away, was moved or unmounted: the kernel's watch is dropped (it would follow the moved
             * folder wherever it went); the slots stay until JS removes them */
            if (self && !(e->mask & IN_IGNORED)) inotify_rm_watch(ino, e->wd);
        }
    }
}

double fswatch_timeout(double now) {
    double best = -1;
    for (int i = 0; i < FSWATCH_MAX; i++) {
        if (!w[i].used || !w[i].pending) continue;
        double left = w[i].due - now;
        if (left < 0) left = 0;
        if (best < 0 || left < best) best = left;
    }
    return best;
}

int fswatch_dispatch(double now, FsWatchFn fn, void *ud) {
    int calls = 0;
    for (int i = 0; i < FSWATCH_MAX; i++) {
        if (!w[i].used || !w[i].pending || w[i].due > now) continue;
        int id = w[i].id, overflow = w[i].overflow, gone = w[i].gone;
        w[i].pending = 0;
        w[i].overflow = 0;
        w[i].last = now;
        calls++;
        if (fn) fn(ud, id, overflow, gone);             /* may remove or add watches: the slots are read again */
    }
    return calls;
}
