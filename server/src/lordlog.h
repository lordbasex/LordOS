/* lordlog.h — where LordOS writes its logs, and in what shape (docs/SYSTEM.md, "Log files").
 *
 *   /var/log/lordos/                         root-owned, 0755
 *       lord-sessiond.log lord-netd.log       the root daemons (0640; each also still goes to the journal)
 *       lord-fand.log admin.log
 *       users/                                root-owned, 0711: nobody can list the user names
 *           <user>/                           the user's own, 0700, made by lord-sessiond at login
 *               compositor.log  shell.log     lord-server's own lines and the shell's (React/QuickJS) ones;
 *                                             the previous session's are kept as <name>.log.1
 *               xwayland.log  cloud.log
 *               apps/<app>.log                each isolated native app and each Linux app
 *               web/<app>-<profile>.log       each web app (lord-web)
 *
 * Without a usable /var/log/lordos/users/<user> (a nested or development lord-server, no lord-sessiond)
 * the same names go under ~/.cache/lordos, so development keeps working.
 *
 * Lines that LordOS writes itself carry a timestamp, a level and a subsystem:
 *     2026-11-11T12:43:56.123-03:00 INFO  xwayland: started on :0
 * They are stamped by a small "lord-log" process that reads the program's stderr through a pipe, so a
 * crash loses nothing that was already written. Output of other programs (a Linux app, Xwayland,
 * rclone, WebKit) is theirs, written as is, between stamped start lines.
 *
 * Rotation: /etc/logrotate.d/lordos (daily or 10 MB, 7 copies, compressed, copytruncate), and here a
 * fallback cap of LORDLOG_CAP per file: above it the file is copied to <name>.1 and emptied, so the
 * disk never fills even without logrotate. libc only. */
#ifndef LORDLOG_H
#define LORDLOG_H

#include <stddef.h>
#include <sys/types.h>

#define LORDLOG_ROOT     "/var/log/lordos"
#define LORDLOG_CAP      ((off_t)16 << 20)
#define LORDLOG_NAME_MAX 96

/* A file name component that is safe as is: 1..LORDLOG_NAME_MAX of [A-Za-z0-9._+-], not starting
 * with '.' or '-'. */
int lordlog_name_ok(const char *s);
/* Any id (a desktop id, "app-profile") as a safe component: other bytes become '_', a leading '.'
 * or '-' becomes '_', cut at LORDLOG_NAME_MAX; "_" if nothing is left. n must be > 1. */
void lordlog_sanitize(const char *in, char *out, size_t n);

/* Root side (lord-sessiond, at login): makes <root>/users/<user> a directory owned by uid:gid with
 * mode 0700, and <root>, <root>/users when missing. Every step is relative to an fd opened with
 * O_NOFOLLOW|O_DIRECTORY; <root> and <root>/users must be owned by `trusted` and writable by nobody
 * else, so the user can't swap anything on the way; the user's own directory is fixed with
 * fchown/fchmod on its fd. A <user> that is a symlink, not a directory, or a directory of another
 * user is refused (-1, errno set) and never changed. Nothing inside it is touched. */
int lordlog_prepare_user_at(const char *root, const char *user, uid_t uid, gid_t gid, uid_t trusted);
int lordlog_prepare_user(const char *user, uid_t uid, gid_t gid);           /* LORDLOG_ROOT, root */

/* Which directory a user's logs go to: <root>/users/<user> when it is a real directory (not a
 * symlink) owned by uid with no group/other access (returns 1), else <home>/.cache/lordos, made
 * if needed (returns 0); -1 when neither works. */
int lordlog_user_dir_at(const char *root, const char *user, uid_t uid, const char *home, char *out, size_t n);
/* The same for this process (getuid(), its name and $HOME), worked out once. "" if none. Call it
 * before forking children that open logs. /var/log/lordos is used only under lord-sessiond
 * (LORDOS_SESSIOND) with $HOME the account's own home, so a nested or test lord-server keeps to
 * its own ~/.cache/lordos. */
const char *lordlog_dir(void);
extern const char *lordlog_root;                 /* LORDLOG_ROOT; a test points it elsewhere before lordlog_dir() */
int lordlog_dir_is_system(void);                                             /* 1: under /var/log/lordos */
/* <dir>[/<sub>]/<sanitized name>.log; sub is NULL, "apps" or "web" (made 0700 when missing).
 * 0, or -1 when there is no log directory or the result doesn't fit. */
int lordlog_path(const char *sub, const char *name, char *out, size_t n);

/* Opens a log for appending (O_APPEND|O_NOFOLLOW, 0600), capping it first; -1 if it isn't a
 * regular file. Only open() and the cap's read/write/ftruncate: fine after fork. */
int lordlog_open(const char *path);
/* If path is a regular file bigger than cap: its contents go to <path>.1 (replaced) and the file is
 * emptied in place, so writers holding it with O_APPEND go on at its start. 1 if it did, 0 if it
 * didn't need to, -1 on error. */
int lordlog_cap(const char *path, off_t cap);
/* At a session's start: <path> becomes <path>.1 if it has anything (the previous session). */
void lordlog_rotate(const char *path);

/* ---- the line format ---- */

/* "ERROR", "WARN" or "INFO" for a message, from its words. */
const char *lordlog_level(const char *msg);
/* The subsystem of a line written as "name: text" (a name of [A-Za-z0-9_.-] and an optional
 * [pid], up to 40 bytes): copied to sub and *msg set after ": "; otherwise sub = dflt, *msg = line.
 * 1 if the line had one. */
int lordlog_subsystem(const char *line, const char *dflt, char *sub, size_t n, const char **msg);
/* One stamped line, "<local ISO 8601 time with ms and offset> <LEVEL> <subsystem>: <msg>\n", in out;
 * its length (cut to fit), never more than n - 1. */
size_t lordlog_format(char *out, size_t n, double unix_s, const char *level, const char *sub, const char *msg);
/* Writes one stamped line to fd (a log opened with lordlog_open): "apps: started …". */
void lordlog_write(int fd, const char *level, const char *sub, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

/* ---- stamping a program's stderr ---- */

/* lord-server's session: stdout and stderr go through a pipe to a "lord-log" child that stamps each
 * line into <dir>/compositor.log, and the shell's ("js: …" and the stack lines after them) into
 * <dir>/shell.log. Both are rotated to .1 first. Every minute the child also caps every log under
 * <dir>. Call it once, early, before any thread starts. 0, or -1 (stderr left as it was). */
int lordlog_session_start(void);
/* A root daemon: its stderr is stamped into /var/log/lordos/<name>.log and still copied, as it was,
 * to the original stderr (the journal). 0, or -1 (stderr left as it was). */
int lordlog_system_start(const char *name);
/* The original stderr kept by lordlog_system_start (close-on-exec), or -1: a child that has its own
 * log points its stderr back there, so its lines don't reach this daemon's file too. */
int lordlog_journal_fd(void);

#endif
