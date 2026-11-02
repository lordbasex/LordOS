/* backup.h — the session's backups (see backup.c, engine.h and docs/SYSTEM.md).
 *
 * A axia-server thread, running as the user at the lowest priority (nice 19, "idle" I/O), takes
 * the scheduled snapshots and does restores. It notifies JS through an eventfd watched by the main
 * loop (backup_fd → backup_dispatch → __lordBackup(status)). */
#ifndef LORD_BACKUP_H
#define LORD_BACKUP_H

#include <stddef.h>

int   backup_start(const char *home, const char *user);   /* after dropping root; once */
int   backup_fd(void);                                    /* -1 if not running */
void  backup_dispatch(void (*fn)(const char *json));      /* something changed: fn(status JSON) */

/* Everything returns JSON text in new memory (free it) or NULL; err receives the reason. */
char *backup_status_json(void);
char *backup_config_json(void);
int   backup_configure(const char *json, char *err, size_t n);
int   backup_run_now(char *err, size_t n);
void  backup_pause(int on);
void  backup_cancel(void);
char *backup_list_json(void);
char *backup_ls_json(const char *id, const char *rel, char *err, size_t n);
int   backup_restore(const char *id, const char *rel, int replace, const char *suffix, const char *to, char *err, size_t n);
int   backup_delete(const char *id, char *err, size_t n);
/* Encryption: unlock (or, with create, set up on a destination without backups) with the
 * passphrase; the result arrives in the status (unlock). The key stays only in memory; lock()
 * wipes it. */
int   backup_unlock(const char *pass, int create, char *err, size_t n);
void  backup_lock(void);
/* Real path of a snapshot (to open it in Files), or -1. */
int   backup_snapshot_path(const char *id, char *out, size_t n);

#endif
