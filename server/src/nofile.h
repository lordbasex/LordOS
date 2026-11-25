/* nofile.h — how many files lord-server may keep open (GitHub #251).
 *
 * Every Linux app's GPU buffer (a dmabuf, one file per plane), every explicit-sync point it waits on, every Wayland
 * connection and every helper's socket is an open file of lord-server. The session starts with the usual soft limit
 * of 1024; past it the kernel silently drops the files apps send (SCM_RIGHTS is cut short), so libwayland disconnects
 * the next app that sends one ("file descriptor expected ... import_timeline") and Xwayland can't be started: Brave,
 * Chromium and Vulkan apps closed and would not open again until the next login, while LordOS's own apps went on.
 *
 * As other compositors do, lord-server raises its own soft limit towards the hard one (at most NOFILE_WANT), and the
 * apps, Xwayland and terminal shells it starts get the usual 1024 back (nofile_restore, in the child just before
 * execve), the FD_SETSIZE that select() expects. Not at fork(): the child still holds every one of lord-server's files
 * then, so with more than 1024 open every number under 1024 is taken and the child could not open /dev/null or move a
 * socket (Xwayland's exit 126 again). Only after the child closed lord-server's files (close_range) is there room under
 * 1024. LordOS's own helpers keep the raised one; nothing of theirs uses select(). No privilege: a process may raise
 * its soft limit up to its hard one. */
#ifndef LORD_NOFILE_H
#define LORD_NOFILE_H

#include <stdarg.h>
#include <sys/resource.h>
#include <sys/select.h>

#define NOFILE_WANT 65536           /* plenty for every app's buffers; still a bound */

/* The soft limit to set: soft raised to min(hard, want), never lowered (pure) */
static inline rlim_t nofile_target(rlim_t soft, rlim_t hard, rlim_t want) {
    rlim_t t = hard == RLIM_INFINITY || hard > want ? want : hard;
    return soft == RLIM_INFINITY || soft >= t ? soft : t;
}

/* Raises the soft limit; 0 = raised or already enough (a second call changes nothing). before/after (may be NULL):
 * the soft limit before and after. */
int nofile_raise(rlim_t *before, rlim_t *after);

/* In a forked child that will exec an app, Xwayland or a shell, after it closed lord-server's files (close_range) and
 * before execve: the soft limit back to FD_SETSIZE (1024, the session's usual one) if it is higher; the hard one stays.
 * Async-signal-safe (two syscalls), and inline: apps.c, pty.c and xwayland.c are built into tests and tools without
 * nofile.c. */
static inline void nofile_restore(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) || (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur <= FD_SETSIZE)) return;
    rl.rlim_cur = FD_SETSIZE;
    setrlimit(RLIMIT_NOFILE, &rl);
}

/* The files open now (from /proc/self/fd, else by probing up to the soft limit), and the soft limit; -1 if unknown */
int nofile_count(rlim_t *limit);

/* libwayland's log handler (wl_log_set_handler_server): its lines go to stderr, as before; after "file descriptor
 * expected" it adds how many files are open of how many allowed, at most once every 10 s, so a report shows when the
 * limit was the cause */
void nofile_wl_log(const char *fmt, va_list ap) __attribute__((format(printf, 1, 0)));

#endif
