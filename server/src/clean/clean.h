/* clean.h — Cleaner's lord-server side (docs/CLEANER-ARCHITECTURE.md): one job at a time on a thread of
 * its own (a scan by the lord-clean helper, a plan or its execution), polled by lord.clean. The
 * compositor never waits for it.
 *
 * JS gets candidate ids and display paths; authority stays here. A plan is built from ids of the
 * current scan, lives only in this process and expires. Execution reopens each object from its rule's
 * root and compares device, inode, size and modification time before moving it to the Trash. There is
 * no real delete: nothing here unlinks a file. */
#ifndef LORD_CLEAN_H
#define LORD_CLEAN_H

#include <stddef.h>
#include <stdint.h>

#define CLEAN_PLAN_TTL_S 600
#define CLEAN_PAGE_MAX   500

/* every call returns 0 or a positive id, or a negative CLEAN_E_*, translated by js_clean.c */
enum { CLEAN_E_BUSY = -1, CLEAN_E_UNAVAILABLE = -2, CLEAN_E_NOSCAN = -3, CLEAN_E_BADID = -4, CLEAN_E_NOPLAN = -5, CLEAN_E_EXPIRED = -6 };

int   clean_available(void);                /* 1 if the helper is there and safe to start */
int   clean_start(void);                    /* a new scan of every rule: its id */
void  clean_cancel(int forget);             /* stops the job (a scan's helper is killed); forget: also drops the results */
char *clean_poll(void);                     /* the job, the scan and the plan, as JSON (malloc) */
char *clean_status(void);                   /* availability and free space, as JSON (malloc) */
char *clean_results(int plan, long offset, int limit);      /* a page of candidates or of plan items, as JSON (malloc) */
int   clean_plan(int scan, const uint32_t *ids, size_t n);  /* its id; the checks run on the thread */
int   clean_execute(int plan);

#ifdef CLEAN_TEST
/* tests only: not in lord-server */
extern long clean_test_max_files;
extern int  clean_test_plan_ttl;
extern int  clean_test_stop_helper;
extern void (*clean_test_before_trash)(const char *real);
int clean_test_helper_pid(void);
int clean_test_record(const char *json, int nrules);        /* 0 kept, 1 dropped, -1 protocol error */
#endif

#endif
