/* clean.h — Cleaner's axia-server side (docs/CLEANER-ARCHITECTURE.md): one job at a time on a thread of
 * its own (a scan by the axia-clean helper, a plan or its execution), polled by lord.clean. The
 * compositor never waits for it.
 *
 * JS gets candidate ids and display paths; authority stays here. A plan is built from ids of a scan,
 * lives only in this process and expires. Execution reopens each object from its root and compares
 * device, inode, size and modification time before acting. A real delete exists only here, as the
 * session user, for regenerable rules, for a file beneath a rule's fixed root that came from a plan
 * (approved by the maintainer on 2026-10-30). Nothing in lord.fs can reach it. */
#ifndef LORD_CLEAN_H
#define LORD_CLEAN_H

#include <stddef.h>
#include <stdint.h>

#define CLEAN_PLAN_TTL_S 600
#define CLEAN_PAGE_MAX   500
#define CLEAN_MAX_FOLDERS 8                 /* folders one duplicates or images scan takes */

enum { CLEAN_KIND_RULES, CLEAN_KIND_TREE, CLEAN_KIND_DUPS, CLEAN_KIND_IMAGES, CLEAN_KINDS };

/* every call returns 0 or a positive id, or a negative CLEAN_E_*, translated by js_clean.c */
enum { CLEAN_E_BUSY = -1, CLEAN_E_UNAVAILABLE = -2, CLEAN_E_NOSCAN = -3, CLEAN_E_BADID = -4, CLEAN_E_NOPLAN = -5, CLEAN_E_EXPIRED = -6,
       CLEAN_E_BADFOLDER = -7 };

typedef struct {
    const char *volume;                     /* tree: "home", "root" or "mount:<n>" from clean_volumes */
    const char *folders[CLEAN_MAX_FOLDERS]; /* dups, images: relative to the home folder; "" is the home folder */
    int nfolders;
    const char *exclude[32];                /* relative to the home folder: never walked, never offered */
    int nexclude;
    double min_size;                        /* tree: files of at least this many bytes are kept */
    int remember;                           /* tree of "home" or "root": keep its folder totals for the next session */
    int saved;                              /* tree: the kept map instead of a scan; nothing can be acted on from it */
} CleanOpts;

typedef struct {
    const char *rule;                       /* NULL or a rule id */
    double min_size, older_than;            /* bytes; a modification time in seconds, 0 = any */
    int type;                               /* CLEAN_TYPE_*, -1 = any */
    const char *under;                      /* NULL, or a folder relative to the scan's root */
} CleanFilter;

int   clean_available(void);                /* 1 if the helper is there and safe to start */
int   clean_start(void);                    /* a new scan of every rule: its id */
int   clean_start_kind(int kind, const CleanOpts *o);
void  clean_cancel(int forget);             /* stops the job (a scan's helper is killed); forget: also drops the results */
char *clean_poll(int kind);                 /* the job, a scan (kind, or the latest with -1) and the plan, as JSON (malloc) */
char *clean_status(void);                   /* availability and free space, as JSON (malloc) */
char *clean_volumes(void);                  /* the root, the home folder and the user's mounts, as JSON (malloc) */
char *clean_results(int plan, long offset, int limit);      /* a page of the latest scan or of the plan, as JSON (malloc) */
char *clean_results_of(int scan, long offset, int limit, const CleanFilter *f);
char *clean_tree(int scan, long node, int limit);           /* a folder of a tree and its largest children */
char *clean_groups(int scan, long offset, int limit);       /* a page of groups of duplicates or similar images */
void  clean_set_sysapps(const char *assets);                /* where the system's own apps are, to tell a leftover */
int   clean_plan(int scan, const uint32_t *ids, size_t n);  /* its id; the checks run on the thread */
int   clean_plan_rules(int scan, const char *const *rules, int n);   /* every candidate of those rules */
int   clean_execute(int plan);
int   clean_restore(int plan, uint32_t id); /* back from the Trash, for an item that plan moved there */
int   clean_empty_trash(void);              /* the real emptyTrash, on the thread; can't be undone */
char *clean_history(void);                  /* what ran, without paths, as JSON (malloc) */
void  clean_history_system(const char *name, double before);   /* name: journal, apt, vartmp or kernels */
void  clean_history_clear(void);           /* and the kept map */
char *clean_saved(void);                    /* the kept map: { at, volume } or null, as JSON (malloc) */
void  clean_forget_saved(void);

#ifdef CLEAN_TEST
/* tests only: not in axia-server */
extern long clean_test_max_files;
extern int  clean_test_plan_ttl;
extern int  clean_test_stop_helper;
extern void (*clean_test_before_trash)(const char *real);   /* right before the action, Trash or delete */
extern const char *clean_test_tmp;                          /* stands for /tmp */
int clean_test_helper_pid(void);
int clean_test_record(const char *json, int nrules);        /* 0 kept, 1 dropped, -1 protocol error */
int clean_test_group(const char *a, const char *b);          /* two image records closed as a group */
#endif

#endif
