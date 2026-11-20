/* fpspawn.h — org.freedesktop.portal.Flatpak served by lord-server: a Flatpak app's Spawn (a sub-sandbox, or a new
 * process of itself) runs inside that same app's scope and view, under the same restrictions (docs/FLATPAK.md,
 * "Sub-sandboxes"; AGENTS.md E9). Flatpak's own flatpak-portal stays masked: it runs under the user's service
 * manager, so what it starts leaves the app's scope, its firewall rules and its view. */
#ifndef LORD_FPSPAWN_H
#define LORD_FPSPAWN_H

/* Connects to the session bus and takes the name. 0, or -1 (no bus, or another program has the name). */
int  fpspawn_start(void);
int  fpspawn_fd(void);                  /* -1 when not running */
int  fpspawn_wants_write(void);
void fpspawn_dispatch(void);
void fpspawn_tick(void);                /* children that exited, callers that left the bus, instance ids */

#endif
