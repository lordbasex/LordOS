/* fsasync.h — lord.fs calls that may wait on a slow file system, off the interface's thread (see fsasync.c).
 *
 *   int req = fsasync_submit(FSA_LIST, path, err, sizeof err);   // > 0, or -1 with a code line in err
 *   ... poll fsasync_fd() with the main loop; when readable:
 *   fsasync_dispatch(fn, ud);                                     // fn(ud, req, json) once per finished request
 *
 * The answer is JSON: { "entries": [...] } (list), { "stat": {...} } (stat), { "ok": true } (trash, mkdir, rename),
 * { "total", "free" } (free),
 * { "size", "files", "dirs", "complete", "remote" } (du, as lord.fs.du), or { "error": "<code line>" } (errcode.h). Paths are resolved and checked on the worker
 * by the same fsops.c functions lord.fs's synchronous calls use: nothing is checked less because it runs later. */
#ifndef LORD_FSASYNC_H
#define LORD_FSASYNC_H

#include <stddef.h>

enum { FSA_LIST = 1, FSA_STAT, FSA_TRASH, FSA_FREE, FSA_MKDIR, FSA_RENAME, FSA_DU, FSA_FN, FSA_WRITE };

#define FSA_THREADS 6            /* at most this many requests wait on file systems at once */
#define FSA_QUEUE 256            /* queued plus running; more is refused at once with fs.busy */
#define FSA_LIST_MAX 5000        /* as lord.fs.list (apps/files/locations.js LIST_CAP) */
#define FSA_DU_MAX_MS 10000      /* a size walk stops after this long (lord.fs.du: 3000, on the interface's thread) */

int  fsasync_submit(int op, const char *path, char *err, size_t errlen);
/* FSA_RENAME takes a second path (the new name), as lord.fs.rename(from, to) */
int  fsasync_submit2(int op, const char *path, const char *path2, char *err, size_t errlen);
/* FSA_DU: the folder's size, walked for at most ms (FSA_DU_MAX_MS) */
int  fsasync_submit_du(const char *path, double ms, char *err, size_t errlen);
/* FSA_WRITE: lord.fs.write on a worker (lord.fs.writeAsync, Notes' saves): the same resolution and checks
 * (fsops_resolve_target), a temporary file next to it, fsync and rename. data is copied (at most FSA_WRITE_MAX). */
#define FSA_WRITE_MAX (32 * 1024 * 1024)
int  fsasync_submit_write(const char *path, const char *data, size_t len, char *err, size_t errlen);
/* waits until no write to this resolved file is queued or running (lord.fs.write calls it first, so a synchronous
 * save never lands before an older asynchronous one that is still on a worker) */
void fsasync_wait_write(const char *target);
/* FSA_FN: one of lord-server's own functions (never anything JS names) run on a worker: fn(arg) returns the answer as
 * malloc'd JSON (NULL: out of memory). For a native call that may wait on a device and touches no lord-server state
 * (media.c: lord.media.camerasAsync, which opens every /dev/video*). arg is copied; it may be "". */
typedef char *(*FsaFn)(const char *arg);
int  fsasync_submit_fn(FsaFn fn, const char *arg, char *err, size_t errlen);
int  fsasync_fd(void);           /* -1 until the first request */
typedef void (*FsaReplyFn)(void *ud, int req, const char *json);
int  fsasync_dispatch(FsaReplyFn fn, void *ud);   /* answers delivered */

/* tests only (tests/fsasync_test.c builds with -DFSASYNC_TEST): runs on the worker before each request */
#ifdef FSASYNC_TEST
extern void (*fsasync_test_hook)(int op, const char *path);
#endif

#endif
