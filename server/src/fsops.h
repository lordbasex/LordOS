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
/* Network shares (see fsops.c): a kernel file system name of a share, and whether a path lies on a
 * share that stopped answering. Doesn't touch the path itself and waits under a second. */
int fsops_share_fs(const char *fstype);
int fsops_share_down(const char *path);
int  fsops_readable(const char *real);         /* (0.8.5) read-only: the whole tree but /proc, /sys, /dev, /run */
int  fsops_protected(const char *real);        /* folders JS does not write to (launcher, apps, settings) */
int  fsops_protected_below(const char *real);  /* a protected path lies strictly below real (rule tables) */
/* Files protected with their family (the file, and "<file>." followed by anything: .tmp-…, .1 … .5, .bak), set at
 * run time: the Terminal's configuration may be anywhere the user chose (termconf.h). "" clears a slot. */
enum { FSOPS_PF_TERMINAL, FSOPS_PF_TERMINAL_CUSTOM, FSOPS_PF_SETTINGS, FSOPS_PF_COUNT };
void fsops_protect_file(int slot, const char *real);
/* real is, lies under or holds a protected path, each protected path resolved first (links followed): for folders
 * handed to something outside lord.fs (a machine's writable share) */
int  fsops_protected_resolved(const char *real);
const char *fsops_home(void);                  /* the resolved home folder given to fsops_init */

/* rename for lord.fs: refuses a protected path, a parent of one (as source or destination) and an existing target */
int  fsops_rename(const char *from, const char *to, char *err, size_t errlen);

/* ---- copy / move in a thread ---- */
#define FSJOB_MAX 8
typedef struct {
    int id, used, move, done, failed, cancel;
    int try_rename;                 /* a move on a slow mount: the rename is tried on the job's thread, not the caller's */
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
/* real is on a cloud, network or other FUSE mount (fuse.*, cifs, smb3, nfs*, 9p…), from the mount table: the interface's
 * thread mustn't walk it (lord.fs.du and search answer at once there) */
int  fsops_remote_path(const char *real);
/* every mount point and whether it is one of those (the same test), from the mount table only */
typedef void (*FsMountFn)(void *ud, const char *path, int remote);
void fs_mounts(FsMountFn fn, void *ud);
int  fs_recent(int limit, int days, FsHitFn fn, void *ud);
typedef void (*FsVolFn)(void *ud, const char *path, const char *fstype, double total, double avail);
void fs_volumes(FsVolFn fn, void *ud);

/* ---- what lord.fs.list / stat / trash resolve and read (interface's thread and fsasync.c's workers alike) ----
 * path: absolute or relative to the home folder, as JS gives it. down (optional) is set when the path was refused
 * because the share it lies on doesn't answer (fsops_share_down). */
int fsops_resolve_readable(const char *path, char *out, int *down);    /* 0; -1 refused or missing; -2 exists but not read */
/* A destination inside the writable roots, even if it doesn't exist yet: the parent resolved, the last name kept.
 * 0; -1 outside the roots; -2 protected (fsops_protected). dir (optional) gets the resolved parent. */
int fsops_resolve_target(const char *path, char *out, size_t n, char *dir, size_t dn, int *down);
/* lord.fs.stat's: 0 in the writable roots, 1 read-only elsewhere where reading is allowed, -1 refused, -2 protected */
int fsops_resolve_stat(const char *path, char *target, size_t n, int *down);
typedef struct {
    const char *name;
    int link, dir, locked;
    double size, mtime, atime, ctime;
    unsigned mode;
    uid_t uid;
    gid_t gid;
} FsEntry;
typedef void (*FsListFn)(void *ud, const FsEntry *e);
int fsops_list(const char *dir, int max, FsListFn fn, void *ud);   /* entries given, or -1 (errno) */
typedef struct {
    char name[256], path[4096], target[4096], user[64], group[64];
    int dir, link, has_target, has_btime, writable;
    unsigned mode;
    double size, mtime, atime, ctime, btime;
} FsStat;
int fsops_stat(const char *target, int writable_root, FsStat *o);  /* 0, or -1 (errno) */
void fsops_user_name(uid_t u, char *out, size_t n);                /* "?" when unknown (thread-safe) */
void fsops_group_name(gid_t g, char *out, size_t n);

#endif
