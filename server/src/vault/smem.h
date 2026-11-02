/* smem.h — memory for axia-vault secrets (see docs/PASSWORDS.md, "Memory").
 *
 * Everything decrypted (the vault, fields, requests and replies that carry secrets) lives
 * in blocks of this arena and not in the libc heap:
 *   - own anonymous mmap, with mlock (not swapped out) if RLIMIT_MEMLOCK allows;
 *   - MADV_DONTDUMP (never in a core) and MADV_WIPEONFORK;
 *   - sm_free wipes the block before returning it; sm_wipe_all (on lock) wipes EVERYTHING.
 * sm_harden_process() also disables the process core dumps (RLIMIT_CORE 0) and marks it not
 * "dumpable" (PR_SET_DUMPABLE 0: it cannot be ptraced even by the same user).
 *
 * Small blocks (up to 4 KB) use size classes with free lists, O(1); large ones get
 * their own mmap. Thread-safe (one mutex). */
#ifndef LORD_VAULT_SMEM_H
#define LORD_VAULT_SMEM_H

#include <stddef.h>

void  sm_harden_process(void);
void *sm_alloc(size_t n);                 /* zeroed; NULL if out of memory */
void *sm_realloc(void *p, size_t n);
void  sm_free(void *p);                   /* wipes and frees (NULL does nothing) */
char *sm_strdup(const char *s);           /* NULL → NULL */
char *sm_strndup(const char *s, size_t n);
size_t sm_size(const void *p);            /* usable capacity of the block */
void  sm_wipe_all(void);                  /* on lock: wipes and releases the whole arena */

typedef struct { size_t in_use, mapped, locked; int all_locked; } SmStats;
void  sm_stats(SmStats *st);

/* wipe memory without the compiler optimizing it away (explicit_bzero) */
void  sm_wipe(void *p, size_t n);

#endif
