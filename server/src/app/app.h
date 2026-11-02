/* app.h — lord-server's "app mode": running an .lpk app as its own isolated process and a
 * Wayland client of the compositor (see appclient.c, docs/PRIVACY.md). */
#ifndef LORD_APP_H
#define LORD_APP_H

#include <stddef.h>

/* `lord-server --app <id>`: runs the installed app <id>. Returns the exit code. */
int app_main(const char *id, const char *theme_bundle, const char *fonts);

/* From lord-server (the compositor): launches app <id> as an isolated process and registers its
 * broker. 0 if it started. The id must be an installed package (it is validated). */
int app_spawn(const char *id);

/* Package folder of app <id>. A system app shipped with LordOS (<assets>/sysapps/<id>, owned by
 * root or the user and not writable by others) wins over a user-installed .lpk
 * (lpk_root()/<id>), so an installed package can never shadow it. out receives the real path.
 * A packaged app (sec/pkgapp.h) comes second, so a home .lpk can't shadow it either.
 * Returns 1 for a system app, 2 for a packaged app, 0 for a user package, -1 if there is none. */
int app_package_dir(const char *id, const char *assets, char *out, size_t n);

/* Starts the companion of packaged app <id>: forks `lord-server --companion <id> <net>`, which checks the signed
 * package and only then runs the binary in its sandbox (companion.c). Never waits. share_fd: the share connection (fd 4 there) or
 * -1; taken over. *app_fd: the app's end of the channel (fd 3 there). The pid, for the caller to kill and reap, or -1. */
int app_companion_spawn(const char *id, int network, int share_fd, int *app_fd);

/* `lord-server --companion <id> <0|1>`, with fds 3 and 4 from app_companion_spawn. Returns only if it didn't start. */
int companion_main(int argc, char **argv);

#endif
