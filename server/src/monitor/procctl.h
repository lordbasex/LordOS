/* procctl.h — one process of the Monitor's Processes tab (see docs/MONITOR.md, "Process
 * actions"): the protected list, a signal that cannot reach a reused PID, and the details that
 * Get Info shows. Used by axia-server (the user's own processes) and by the root helper's
 * proc-signal / proc-info actions (proc_admin.c, administrators only). Read-only /proc access and
 * one syscall (pidfd_send_signal); nothing is executed. */
#ifndef LORD_PROCCTL_H
#define LORD_PROCCTL_H

#include "monpriv.h"

#define PROC_PID_MAX 4194304            /* the kernel's PID_MAX_LIMIT */

typedef struct {
    int pid, ppid, pgid, sid;
    char state;
    unsigned flags;
    unsigned long long minflt, majflt, utime, stime, start, vsize, rss;   /* start: ticks since boot; rss: pages */
    long prio, nice, threads;
    char comm[64];
} ProcStat;

/* Parses the text of /proc/<pid>/stat. 0 if it is well formed. */
int proc_stat_parse(const char *txt, ProcStat *s);
int proc_stat_read(int pid, ProcStat *s);

/* Why a process may not be stopped from the Monitor, or NULL if it may:
 *   "init"     PID 1 (and anything below it)
 *   "kernel"   a kernel thread
 *   "session"  the session's own axia-server (session_pid)
 *   "system"   another part of LordOS or the desktop: lord-sessiond, axia-netperm, lord-netd,
 *              axia-fand, the helpers (axia-vault, axia-maild, axia-doc, axia-media), the root
 *              helper, any axia-server that is not an isolated app (`--app`), PipeWire,
 *              WirePlumber and the systemd managers.
 * exe may be "" (unreadable); argv1 is the process's second argument or NULL. */
const char *proc_protect_reason(int pid, unsigned flags, const char *comm, const char *exe, const char *argv1, int session_pid);
/* Same, reading comm, flags, exe and the command line from /proc. */
const char *proc_protected(int pid, int session_pid);

/* Sends SIGTERM or SIGKILL to pid only if it is still the process that started at `start`
 * (clock ticks after boot, field 22 of stat): pidfd_open, compare the start time, then
 * pidfd_send_signal on that pidfd, so a PID reused in between is never hit. Returns NULL on
 * success or an error code (errcode.h): proc.gone, proc.protected<Reason>, proc.denied,
 * proc.failed. */
const char *proc_signal(int pid, unsigned long long start, int sig, int session_pid);

/* Get Info as JSON for pid, if it still started at `start` ({"pid":…,"gone":true} otherwise).
 * full: also what only the owner or root can read (smaps_rollup, exe, cwd, open files and
 * sockets); without it those fields are null. */
void proc_detail_json(Buf *b, int pid, unsigned long long start, int full, int session_pid);

#endif
