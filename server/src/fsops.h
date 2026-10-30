/* fsops.h — file operations for lord.fs (Files): background copy/move,
 * Trash (freedesktop), information, bounded searches and mounted volumes (see fsops.c). */
#ifndef LORD_FSOPS_H
#define LORD_FSOPS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Allowed roots: the home folder and the user's mounts (/media/$USER, /run/media/$USER). */
void fsops_init(const char *home, const char *user);
int  fsops_allowed(const char *real);          /* already resolved path (realpath) inside a root */
int  fsops_readable(const char *real);         /* (0.8.5) read-only: the whole tree but /proc, /sys, /dev, /run */
int  fsops_protected(const char *real);        /* folders JS does not write to (launcher, apps, settings) */
int  fsops_protected_below(const char *real);  /* a protected path lies strictly below real (rule tables) */
const char *fsops_home(void);                  /* the resolved home folder given to fsops_init */

/* rename for lord.fs: refuses a protected path, a parent of one (as source or destination) and an existing target */
int  fsops_rename(const char *from, const char *to, char *err, size_t errlen);

/* ---- copy / move in a thread ---- */
#define FSJOB_MAX 8
typedef struct {
    int id, used, move, done, failed, cancel;
    char src[4096], dst[4096];
    volatile uint64_t bytes, total, files;
    char error[256];
} FsJob;
int  fsjob_start(const char *src, const char *dst, int move, char *err, size_t errlen);   /* id > 0 */
const FsJob *fsjob_get(int i);                 /* i = 0..FSJOB_MAX-1, for listing */
void fsjob_cancel(int id);
void fsjob_forget(int id);                      /* result already read: frees the slot */

/* ---- Trash ---- */
int  fstrash_put(const char *real, char *err, size_t errlen);          /* moves to the matching trash */
/* The same, for a name in an open folder (dirfd; real only names it). dest gets the item's id. */
int  fstrash_put_at(int dirfd, const char *name, const char *real, char *dest, size_t destn, char *err, size_t errlen);
/* List: calls fn for each item (id = real path inside .../files). */
typedef void (*FsTrashFn)(void *ud, const char *id, const char *name, const char *orig, const char *date, int dir, double size);
void fstrash_list(FsTrashFn fn, void *ud);
int  fstrash_restore(const char *id, char *err, size_t errlen);
int  fstrash_remove(const char *id, char *err, size_t errlen);          /* delete permanently */
int  fstrash_empty(void);

/* ---- bounded walks ---- */
typedef struct { double size; long files, dirs; int complete; } FsDu;
void fs_du(const char *real, double max_ms, FsDu *out);
typedef void (*FsHitFn)(void *ud, const char *path, int dir, double size, double mtime);
int  fs_search(const char *real, const char *needle, int limit, int depth, FsHitFn fn, void *ud);
int  fs_recent(int limit, int days, FsHitFn fn, void *ud);
typedef void (*FsVolFn)(void *ud, const char *path, const char *fstype, double total, double avail);
void fs_volumes(FsVolFn fn, void *ud);

#endif
