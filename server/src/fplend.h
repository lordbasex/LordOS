/* fplend.h — lending the file the person chose to a running Flatpak app (docs/FLATPAK.md, "Lending a chosen file";
 * AGENTS.md, Flatpak entries; approved by the maintainer 2026-11-22). LordOS's equivalent of Flatpak's document portal,
 * without a FUSE file system: when a Flatpak app gets a file from LordOS's file dialog (org.freedesktop.portal.FileChooser,
 * ui/portal.c), exactly that file, that folder, or for a save only the chosen file (writable, in a folder of its own that
 * holds nothing else), is bind-mounted into that app's running view by the view's own PID 1 (sec/sandbox.c, SbLendMsg), and the app
 * gets /run/flatpak/app/<id>/doc/<grant>/<name> (where Flatpak 1.16 binds $XDG_RUNTIME_DIR/app/<id> in the sandbox)
 * instead of the real path. Never the rest of the home. Revoked when the app's
 * view ends (its last process leaves).
 *
 * lord-server's side: each Flatpak view with the session bus gets a socket pair from the launcher (apps.c, FPLEND_FD in
 * the view); the view's PID 1 says hello with its pidfd. A request names the app by the D-Bus caller's pid: its
 * root-owned app-lordos-fp-<id>.scope (appid_of_pid), and the view whose PID 1 is that process's topmost ancestor in the
 * scope. Nothing here is reachable from an app: the socket's other end is PID 1's alone. */
#ifndef LORD_FPLEND_H
#define LORD_FPLEND_H

#include <stddef.h>

#define FPLEND_FD 5                     /* the view's end in the launcher's --launch-view process (FPSW_FD + 1) */

/* What to lend (SB_LEND_* of sec/sandbox.h): FPLEND_FILE an existing file, FPLEND_DIR a folder, FPLEND_SAVE a save
 * dialog's target (made empty when missing; that file alone, writable: its folder is never lent) */
enum { FPLEND_FILE = 1, FPLEND_DIR, FPLEND_SAVE };

/* The launcher (apps_set_lend_hook): lord-server's end of a new view's socket, and the app id. fplend owns fd now. */
void fplend_add_view(int fd, const char *id);

/* Lends path (absolute, the real one the person chose) to the Flatpak app whose process caller_pid is (the portal's
 * D-Bus caller), as kind. out gets the path the app must use. 0 lent; 1 the caller isn't a Flatpak app LordOS started
 * (nothing to do: the real path is the answer); -1 refused or failed (logged; the caller answers without that path). */
int fplend_lend(unsigned caller_pid, const char *path, int kind, char *out, size_t n);

/* lord-server's checks for one grant to app id, before the view is asked (fplend_lend; tests): path must be canonical
 * (sec/pathcanon.h) and name, without a link on the way, the very object the kernel opens (its /proc/self/fd link is
 * the same string), and the policy (fsops_protected_resolved, apps_grant_ok) is decided on that path only. For a save,
 * the target is made empty when missing. q is filled (kind, rw, path, name, dev/ino of what PID 1 must find there).
 * 0, or -1 refused. */
struct SbLendMsg;
int fplend_check(const char *id, const char *path, int kind, struct SbLendMsg *q);

/* Policy hooks, apps.c's (lord-server): may a lent file be writable, may it be lent at all. Tests replace them. */
int apps_grant_ok(const char *path);

#endif
