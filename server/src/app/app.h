/* app.h — axia-server's "app mode": running an .lpk app as its own isolated process and a
 * Wayland client of the compositor (see appclient.c, docs/PRIVACY.md). */
#ifndef LORD_APP_H
#define LORD_APP_H

#include <stddef.h>

/* `axia-server --app <id>`: runs the installed app <id>. Returns the exit code. */
int app_main(const char *id, const char *theme_bundle, const char *fonts);

/* From axia-server (the compositor): launches app <id> as an isolated process and registers its
 * broker. 0 if it started. The id must be an installed package (it is validated). */
int app_spawn(const char *id);

/* Package folder of app <id>. A system app shipped with LordOS (<assets>/sysapps/<id>, owned by
 * root or the user and not writable by others) wins over a user-installed .lpk
 * (lpk_root()/<id>), so an installed package can never shadow it. out receives the real path.
 * Returns 1 for a system app, 0 for a user package, -1 if there is none. */
int app_package_dir(const char *id, const char *assets, char *out, size_t n);

#endif
