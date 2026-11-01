/* sharegrant.h — the grants of the screen-sharing API: which app may share the screen (screen-share) and
 * take input (remote-input). Root writes the file, /var/lib/lordos/share/<uid>.json, through the share-grant
 * action; lord-server only reads it: permisos.json can be edited by any unsandboxed process of the user, and
 * this permission is the one worth stealing (docs/SCREEN-SHARING.md). No I/O here but the two functions at
 * the end, so tests/share_test.c and tests/share_fuzz.c cover the parser. */
#ifndef LORD_REMOTE_SHAREGRANT_H
#define LORD_REMOTE_SHAREGRANT_H

#include <stddef.h>
#include <sys/types.h>

#define SG_DIR "/var/lib/lordos/share"
#define SG_MAX 128                      /* entries in a file */
#define SG_ID_MAX 128                   /* FW_APP_MAX */
#define SG_EXE_MAX 256
#define SG_FILE_MAX (64 * 1024)

typedef enum { SG_SCREEN = 0, SG_INPUT = 1, SG_PERMS } SgPerm;
typedef enum { SG_ASK = 0, SG_ALLOW = 1, SG_DENY = 2 } SgState;

typedef struct { char id[SG_ID_MAX], exe[SG_EXE_MAX]; int perm, state; } SgEntry;
typedef struct { SgEntry e[SG_MAX]; int n; } SgStore;

/* "desktop:<name>" or "lpk:<name>" with the characters of an app id: anything else is refused. */
int sg_id_valid(const char *id);
/* An absolute path of letters, digits and /._-+@~, with no "..". */
int sg_exe_valid(const char *exe);
int sg_perm_of(const char *name);       /* "screen-share", "remote-input"; -1 else */
int sg_state_of(const char *name);      /* "allow", "deny", "ask"; -1 else */
const char *sg_perm_name(int perm);
const char *sg_state_name(int state);

/* The file's text → store. 0, or -1 with the store empty: a damaged file grants nothing. */
int sg_parse(const char *text, size_t len, SgStore *s);
/* The store → the file's text. Bytes written, or -1 if it doesn't fit. */
int sg_write(const SgStore *s, char *out, size_t n);
/* What holds for this app. A Linux app's grant is tied to its executable: another path means asking again. */
SgState sg_get(const SgStore *s, const char *id, const char *exe, int perm);
/* "ask" removes the entry. 0, or -1 if the store is full. */
int sg_set(SgStore *s, const char *id, const char *exe, int perm, int state);
void sg_forget(SgStore *s, const char *id);

/* <SG_DIR>/<uid>.json */
void sg_path(uid_t uid, char *out, size_t n);
/* Reads the store: only a regular file owned by root, not writable by group or others. 0, or -1 (empty store). */
int sg_load(const char *path, SgStore *s);

#endif
