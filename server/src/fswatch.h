/* fswatch.h — lord.fs.watch / unwatch: "this folder changed", read-only, for the desktop icons and Files (see fswatch.c).
 *
 *   char err[256];
 *   int id = fswatch_add(path, err, sizeof err);   // > 0, or -1 with a code line in err
 *   ... poll fswatch_fd() with the main loop; when readable: fswatch_read(now);
 *   ... every pass: fswatch_dispatch(now, fn, ud);   // fn(ud, id, overflow, gone), at most once per 150 ms per watch
 *   ... the poll's timeout includes fswatch_timeout(now)
 *   fswatch_remove(id);  fswatch_clear();            // one watch; every watch (the theme's context ended)
 *
 * now is a monotonic time in milliseconds (the caller's clock, so the tests can drive it). */
#ifndef LORD_FSWATCH_H
#define LORD_FSWATCH_H

#include <stddef.h>

#define FSWATCH_MAX 16                 /* per session: inotify's per-user limit is shared with the person's own programs */
#define FSWATCH_COALESCE_MS 150.0      /* at most one call per watch in this time */

int    fswatch_add(const char *path, char *err, size_t errlen);
int    fswatch_remove(int id);         /* 0, or -1 when no such watch */
void   fswatch_clear(void);
int    fswatch_count(void);
int    fswatch_fd(void);               /* -1 until the first watch */
void   fswatch_read(double now);       /* drains the inotify queue: the watches it names become due */
double fswatch_timeout(double now);    /* ms until the next call is due, -1 when none is waiting */
/* overflow: the kernel's queue overflowed, or the folder itself was deleted, moved or unmounted (list it again, resolve
 * the folder again); gone: the watch no longer sees the folder (deleted, moved, unmounted) and stays until removed */
typedef void (*FsWatchFn)(void *ud, int id, int overflow, int gone);
int    fswatch_dispatch(double now, FsWatchFn fn, void *ud);   /* calls made */

#ifdef FSWATCH_TEST
extern const char *fswatch_test_mounts;   /* a mounts file instead of /proc/self/mounts */
#endif

#endif
