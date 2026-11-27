/* bgsave.h — whole files lord-server saves often (settings.json, the clipboard's history and snippets), written on a
 * worker thread instead of the interface's (see bgsave.c; GitHub #250).
 *
 *   bgsave_put(dst, tmp, data, len, flags);   // queued; returns at once. The newest text for dst wins.
 *   bgsave_flush(dst);                        // waits until nothing for dst is queued or being written (NULL: all)
 *
 * Each save is written whole to tmp (BGS_UNIQUE: a mkstemp name next to dst instead), fsync'd when BGS_FSYNC asks,
 * and renamed over dst: a reader sees the old file or the new one, never half of one. The paths are lord-server's own
 * (js_sys.c, js_term.c), never one JS names. */
#ifndef LORD_BGSAVE_H
#define LORD_BGSAVE_H

#include <stddef.h>

#define BGS_FSYNC   1u          /* fsync the new file before the rename */
#define BGS_PRIVATE 2u          /* the new file is 0600 (else 0666 less the umask, as fopen makes it) */
#define BGS_UNIQUE  4u          /* tmp is a mkstemp template (…XXXXXX) instead of a fixed name */
#define BGS_SYNCDIR 8u          /* fsync dst's folder after the rename (the rename itself on the disk) */
/* A newer save of dst replacing one still queued keeps the older one's BGS_FSYNC and BGS_SYNCDIR: a durable save is
 * never made plain by the next one (session restore's marker, then a setting changed). */

#define BGS_SLOTS 8             /* files with a save queued at once; one more is written on the caller's thread */

/* 0: queued (or, without a worker, written); -1: not written (errno). data is copied. */
int  bgsave_put(const char *dst, const char *tmp, const char *data, size_t len, unsigned flags);
void bgsave_flush(const char *dst);
/* writes one file now, on this thread, the way the worker does: 0 or -1 (errno) */
int  bgsave_write_now(const char *dst, const char *tmp, const char *data, size_t len, unsigned flags);

/* lord.fs.write's durability off the interface's thread (QA 0.8.9.6.2 on a Raspberry Pi: a 1.4 GB copy to the SD card
 * held its fsync 5 to 23 s): the file is written and renamed on the caller's thread as before, and queued here; the
 * worker opens it (no link followed) and syncs it and its folder after the queued saves. 0 queued (or already), 1 synced
 * on this thread (no worker, or 32 already queued), -1 a bad path. bgsave_flush(NULL) waits for these too. */
int  bgsave_sync_later(const char *path);
void bgsave_sync_now(const char *path);

/* tests only (tests/bgsave_test.c builds with -DBGSAVE_TEST): runs on the worker before each write */
#ifdef BGSAVE_TEST
extern void (*bgsave_test_hook)(const char *dst);
extern int bgsave_writes;       /* files written by the worker, for the coalescing check */
extern unsigned bgsave_last_flags;  /* the flags of the worker's last write */
#endif

#endif
