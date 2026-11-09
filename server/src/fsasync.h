/* fsasync.h — lord.fs calls that may wait on a slow file system, off the interface's thread (see fsasync.c).
 *
 *   int req = fsasync_submit(FSA_LIST, path, err, sizeof err);   // > 0, or -1 with a code line in err
 *   ... poll fsasync_fd() with the main loop; when readable:
 *   fsasync_dispatch(fn, ud);                                     // fn(ud, req, json) once per finished request
 *
 * The answer is JSON: { "entries": [...] } (list), { "stat": {...} } (stat), { "ok": true } (trash, mkdir, rename),
 * { "total", "free" } (free), or { "error": "<code line>" } (errcode.h). Paths are resolved and checked on the worker
 * by the same fsops.c functions lord.fs's synchronous calls use: nothing is checked less because it runs later. */
#ifndef LORD_FSASYNC_H
#define LORD_FSASYNC_H

#include <stddef.h>

enum { FSA_LIST = 1, FSA_STAT, FSA_TRASH, FSA_FREE, FSA_MKDIR, FSA_RENAME };

#define FSA_THREADS 6            /* at most this many requests wait on file systems at once */
#define FSA_QUEUE 256            /* queued plus running; more is refused at once with fs.busy */
#define FSA_LIST_MAX 5000        /* as lord.fs.list (apps/files/locations.js LIST_CAP) */

int  fsasync_submit(int op, const char *path, char *err, size_t errlen);
/* FSA_RENAME takes a second path (the new name), as lord.fs.rename(from, to) */
int  fsasync_submit2(int op, const char *path, const char *path2, char *err, size_t errlen);
int  fsasync_fd(void);           /* -1 until the first request */
typedef void (*FsaReplyFn)(void *ud, int req, const char *json);
int  fsasync_dispatch(FsaReplyFn fn, void *ud);   /* answers delivered */

/* tests only (tests/fsasync_test.c builds with -DFSASYNC_TEST): runs on the worker before each request */
#ifdef FSASYNC_TEST
extern void (*fsasync_test_hook)(int op, const char *path);
#endif

#endif
