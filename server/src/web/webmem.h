/* webmem.h — how much memory a running web app takes (docs/WEBAPPS.md, "The Store's page of a web app"): the
 * proportional set size (Pss) of a lord-web process and of every process under it (WebKit's web and network
 * processes, the bubblewrap that starts them), from /proc/<pid>/smaps_rollup. Read-only and bounded: one pass over
 * /proc for the parents, at most WEBMEM_MAX_PROCS processes summed. Used by lord.webapps.memory (ui/js_webapps.c);
 * tests/webmem_test.c runs it on a made-up /proc tree. */
#ifndef LORD_WEB_WEBMEM_H
#define LORD_WEB_WEBMEM_H

#include <sys/types.h>

#define WEBMEM_MAX_PROCS   64       /* lord-web and its descendants counted (one app and profile uses about 5) */
#define WEBMEM_MAX_SCAN    8192     /* /proc entries read for their parent */

typedef struct {
    long long pss_kb;               /* the sum of the processes that could be read */
    int procs;                      /* how many were summed */
    int unreadable;                 /* descendants whose smaps_rollup could not be read (left out of pss_kb) */
} WebMem;

/* pid and its descendants under proc (normally "/proc"). 0 with *out filled; -1 when pid itself is not there or its
 * smaps_rollup cannot be read. */
int webmem_tree(const char *proc, pid_t pid, WebMem *out);

/* pid's descendants under proc (not pid itself), breadth first, at most max (and WEBMEM_MAX_PROCS): their count. For
 * ending a lord-web with every WebKit process under it (web/webmgr.c), whatever session or group they are in. */
int webmem_descendants(const char *proc, pid_t pid, pid_t *out, int max);
/* pid's start time (field 22 of its stat: clock ticks since boot), 0 when it can't be read: with the pid it names one
 * process, never another that got the same pid later. */
unsigned long long webmem_start_time(const char *proc, pid_t pid);

/* The "Pss:" line of a smaps_rollup text, in kB; -1 when there is none. */
long long webmem_pss_line(const char *text);

#endif
