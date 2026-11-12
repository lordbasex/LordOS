/* xauth.h — the session's X cookie (MIT-MAGIC-COOKIE-1) and its Xauthority file (xauth.c).
 *
 * Xwayland is started with `-auth <file>` (xwayland.c): a client must show the cookie, whatever its
 * uid. The X socket is still 0600, so in practice that is this user's programs (which get
 * XAUTHORITY from the session) and root's: a program run as root with pkexec or sudo (GParted,
 * GitHub #107) gets DISPLAY and XAUTHORITY passed on and connects with the cookie. Without it,
 * Xwayland let in only its own uid, and root was refused ("Authorization required").
 *
 * The file lives in $XDG_RUNTIME_DIR (the user's 0700 tmpfs), which restricted Linux apps don't see
 * (their own view mounts an empty one there, apps.c) and lord.fs doesn't read (/run, fsops.c).
 * Restricted Linux apps and isolated apps also lose XAUTHORITY with DISPLAY (xauth_env_drop).
 * See docs/WAYLAND.md, "X11 apps (Xwayland)". */
#ifndef LORD_XAUTH_H
#define LORD_XAUTH_H

#include <stddef.h>
#include <stdlib.h>

#define XAUTH_COOKIE_LEN 16
#define XAUTH_FAMILY_LOCAL 256          /* FamilyLocal: address = the host name */
#define XAUTH_FAMILY_WILD 65535         /* FamilyWild: any address (the host name may change in the session) */
#define XAUTH_PROTO "MIT-MAGIC-COOKIE-1"

/* The Xauthority entries for display `display` with `cookie` (XAUTH_COOKIE_LEN bytes): one
 * FamilyLocal entry for `host` (skipped when host is NULL or empty) and one FamilyWild entry. Each
 * entry is family (u16 big endian), then address, display number, protocol name and cookie, each
 * as a u16 big-endian length and the bytes. Returns the length written to out, 0 if it doesn't fit
 * or an argument is bad. */
size_t xauth_encode(unsigned char *out, size_t cap, const char *host, int display, const unsigned char *cookie);

/* Creates `tmp` in the folder dfd with O_CREAT | O_EXCL | O_NOFOLLOW, mode 0600, writes data and
 * fsyncs it. Never follows or reuses anything already at that name. 0, or -1 with errno set (the
 * file is removed on a failed write). */
int xauth_create_at(int dfd, const char *tmp, const unsigned char *data, size_t len);

/* Writes data to dir/name: dir must be a folder of this user with no access for group or others
 * (XDG_RUNTIME_DIR), name a plain name. It is written to a fresh temporary name (xauth_create_at)
 * and renamed over dir/name, so whatever was at dir/name (a link, an old file) is replaced, never
 * followed. The full path goes to path. 0, or -1 with errno set. */
int xauth_write(const char *dir, const char *name, const unsigned char *data, size_t len, char *path, size_t pathlen);

/* Before running a program that must not use the X display (a restricted Linux app, an isolated
 * app): neither the display nor the cookie's file. */
static inline void xauth_env_drop(void) {
    unsetenv("DISPLAY");
    unsetenv("XAUTHORITY");
}

#endif
