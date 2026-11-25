/* seat.h — is this user the one on the monitor (the physical seat)? For root actions that must only serve the person at the
 * computer, never a session in the background (fast user switching) or one reached through the remote desktop.
 *
 * The answer is lord-sessiond's own record, never the request's: /run/lordos/status.json (root's, written by sessiond.c
 * write_status from rd_shared->monitor, "" while the login screen or a locked screen is shown), read only when the file is
 * owned by `owner` (root), is a regular file reached without a symlink, and the pid it names is a running lord-sessiond.
 * Anything else (no lord-sessiond, a file left by one that died, the login screen in front, another user in front) is "no".
 * Pure functions of paths, so tests/vmusb_test.c runs them on a made-up file and /proc. */
#ifndef LORD_SEAT_H
#define LORD_SEAT_H

#include <stddef.h>
#include <sys/types.h>

#define SEAT_STATUS "/run/lordos/status.json"

/* The user status.json says is on the monitor, into out: 0, or -1 when it can't be told (copied only when it is a
 * plain user name). The last "monitor" key counts: it comes after every list a stranger's text can reach. */
int seat_monitor_user(const char *json, char *out, size_t n);

/* 1 when user is the one on the monitor by status (owned by owner) and lord-sessiond under proc is running; 0 otherwise */
int seat_user_is_active(const char *status, const char *proc, uid_t owner, const char *user);

#endif
