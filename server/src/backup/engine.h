/* engine.h — the backup engine (Backups app, docs/SYSTEM.md).
 *
 * Our own code, no rsync: a snapshot is a complete tree of the home folder in
 *   <dest>/LordOS-Backups/<host>-<user>/<YYYY-MM-DD-HHMMSS>/
 * Files that did not change (same size, modification time and mode as in the previous snapshot)
 * are hard links to the previous one: every snapshot looks complete but only takes the space of
 * what changed. While copying, the tree lives in .inprogress-<stamp>; it is renamed at the end
 * (atomic). An interrupted run is resumed: the next one uses that partial tree as another link
 * source and then deletes it.
 *
 * This file knows nothing about threads or JS (backup.c uses it, tests/backup_test.c tests it). */
#ifndef LORD_BACKUP_ENGINE_H
#define LORD_BACKUP_ENGINE_H

#include <stdint.h>
#include <time.h>

#define BK_DIRNAME "LordOS-Backups"
#define BK_MAX_EXCL 64
#define BK_MAX_SNAPS 4096
#define BK_ID_LEN 18                    /* "2026-10-17-235959" + 0 */

typedef struct BkRun BkRun;
struct BkRun {
    /* input */
    const char *home;                   /* source folder (already resolved with realpath) */
    const char *machine;                /* <dest>/LordOS-Backups/<host>-<user> (created if missing) */
    const char *excl[BK_MAX_EXCL];      /* prefixes relative to home ("Videos/large", ".cache") */
    int nexcl;
    const char *skip_real;              /* real path that is never copied (the destination, if inside home) */
    /* A path (relative to home) the caller holds back this run, != 0 to skip it and all below it: a running
     * machine's disks (backup.c, docs/VIRTUALIZATION.md 6.14). Counted in `skipped`. May be NULL. */
    int  (*hold)(BkRun *r, const char *rel);
    volatile int *cancel;               /* != 0: stop as soon as possible (left to resume) */
    /* Called between files and every ~8 MB: may block (pause); returns != 0 to stop. */
    int  (*checkpoint)(BkRun *r);
    void (*progress)(BkRun *r, const char *rel);   /* when moving to a new file (relative to home) */
    void *ud;
    uint64_t min_free;                  /* free bytes to keep on the destination (0: 512 MiB) */
    const uint8_t *key;                 /* 32-byte key if the destination is encrypted (crypt.h), or NULL */
    /* output */
    uint64_t total_files, total_bytes;  /* from the initial count */
    uint64_t files, bytes;              /* processed */
    uint64_t copied, linked, copied_bytes, errors, skipped;
    char id[BK_ID_LEN];                 /* snapshot created */
    char error[256];
};

/* 0: done; 1: interrupted (.inprogress left to resume); -1: error (r->error). */
int  bk_snapshot(BkRun *r, time_t now);

/* Complete snapshots of this machine, oldest first. Returns the count. */
int  bk_list(const char *machine, char (*ids)[BK_ID_LEN], int max);
int  bk_id_valid(const char *id);
time_t bk_id_time(const char *id);      /* local time encoded in the name, or -1 */
void bk_id_make(time_t t, char out[BK_ID_LEN]);

/* Retention (pure function): sets keep[i] for the snapshots that stay. t[] is oldest first.
 * Everything from the last 24 h; the newest of each day up to 30 days; the newest of each week
 * after that. The newest snapshot always stays. tz_off: seconds to add for local time. */
void bk_retention(const time_t *t, int n, time_t now, long tz_off, int *keep);
/* Applies retention and, if space is short (need bytes), deletes the oldest (never the newest). */
int  bk_prune(const char *machine, time_t now, uint64_t need);
int  bk_delete(const char *machine, const char *id);

/* Restores rel ("" = everything) from a snapshot into base/rel (base: normally home).
 * replace = 0: if the target exists and differs, the restored copy goes next to it as
 * "name<suffix>" (suffix comes from the UI, no '/'); replace = 1: it is replaced. Never writes
 * where lord.fs refuses to (fsops_protected) nor outside its roots (fsops_allowed). */
typedef struct {
    uint64_t files, bytes, kept_both, replaced, skipped, errors;
    volatile int *cancel;
    const uint8_t *key;                 /* to decrypt (encrypted destination), or NULL */
    char error[256];
} BkRestore;
int  bk_restore(const char *machine, const char *id, const char *rel, const char *base,
                int replace, const char *suffix, BkRestore *out);

/* Safe rel: relative, no "..", ".", "//" or control characters. "" is valid. */
int  bk_rel_valid(const char *rel);

#endif
