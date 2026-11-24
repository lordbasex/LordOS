/* blockwatch.h — Sentinela's blocklist (docs/SENTINELA.md, "Blocklist"; approved by the maintainer on 2026-11-24): a
 * program the person chose to block, after "Investigate with Lumo" or from Get Info, is ended whenever it starts, as the
 * person's own user, with what LordOS already has: no new privilege, no root part.
 *
 * The list is ~/.config/lordos/sentinela/blocked.json, which only lord-server writes (fsops_protected: no app, no
 * lord.fs). Each entry is a program file by its SHA-256 (and its size, its path when it was blocked, a script's
 * interpreter, optionally its package): {"version":1,"entries":[{"id":"<sha256>","kind":"binary|script","path":"…",
 * "size":N,"interp":"shell"|null,"pkg":"…"|null,"name":"…","at":ms}]}.
 *
 * While the list has entries, a thread of lord-server's looks at the person's new processes every BLOCKWATCH_MS
 * (/proc, only processes of the session's uid, never lord-server, its parents or PID 1): a program whose file has
 * the size of an entry is hashed (cached by device, inode, size and modification time) and, when the hash matches
 * (anywhere it was copied to), ended with SIGKILL through a pidfd checked against its start time. A script entry
 * matches the script an interpreter runs (evidence.c's rules), never the interpreter. An entry with a package also
 * matches any program that package installed. What was ended is kept for Sentinela's notices (blockwatch_events).
 * It can't stop a program before it starts, nor another user's: that would need a root part (FAN_OPEN_EXEC_PERM),
 * proposed in docs/SENTINELA.md and not built. */
#ifndef LORD_LLM_BLOCKWATCH_H
#define LORD_LLM_BLOCKWATCH_H

#include <stddef.h>

#define BLOCKWATCH_MS 500
#define BLOCKWATCH_MAX 256                   /* entries */

/* Load the list (path: the file; NULL: ~/.config/lordos/sentinela/blocked.json) and run the thread while it has
 * entries. Called once by lord.sentinela's registration. */
void blockwatch_init(const char *path);
/* Block the program of process (pid, start) (with_pkg: its package too), or take an entry (its id, the SHA-256) off the
 * list: queued for the thread (hashing a program takes a while), a request id or -1 (EINVAL, EBUSY). Its result comes
 * once from blockwatch_take: {"ok":true,"entry":{…}} | {"ok":true} | {"ok":false,"error":"proc.gone" | "proc.notOwn" |
 * "sentinela.blockRefused" (LordOS's own programs, an interpreter itself, the system's core) | "sentinela.blockFull" |
 * "sentinela.notBlocked" | "sys.failed"}; NULL while it runs. */
int blockwatch_block(int pid, unsigned long long start, int with_pkg);
int blockwatch_unblock(const char *id);
char *blockwatch_take(int id);
/* The list as JSON (malloc'd). */
char *blockwatch_list_json(void);
/* What was ended since the last call: [{"id":"…","name":"…","pid":N,"at":ms}] (malloc'd). */
char *blockwatch_events_json(void);

/* evidence.c: a process's script by evidence.c's rules (fd at offset 0, its path as the command line names it), or -1;
 * an executable's interpreter family (0: not one); a file's SHA-256 (at most 512 MB). */
int llm_script_of(int pid, char *path, size_t n);
int llm_interpreter(const char *exe);
void llm_sha256_fd(int fd, char hex[65], long long *size);

#endif
