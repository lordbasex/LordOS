/* webicon.h — a web app's own icon, cached for the Dock, the launcher, the Store and Settings (docs/WEBAPPS.md,
 * "Icons"). The catalog ships only LordOS's letter tiles; with lord-web installed, lord.webapps.fetchIcon runs the
 * same probe as "Add from an address" on the app's own start address, and the icon it fetched from the site (a PNG or
 * an SVG of at most 1 MB, checked by webreg_icon_kind) is kept here:
 *
 *   ~/.cache/lordos/web/icons/<app>.png  or  <app>.svg        (the folder is in fsops_protected)
 *
 * The file name is always the app id (webreg_id_ok) and the kind's extension, nothing taken from the site. Files only;
 * tested by tests/webdock_test.c on a temporary home. */
#ifndef LORD_UI_WEBICON_H
#define LORD_UI_WEBICON_H

#include <stddef.h>

#define WEBICON_DIR ".cache/lordos/web/icons"

/* "<app>.png" (kind 1) or "<app>.svg" (kind 2). 0, or -1 for a bad id or kind or a short buffer. */
int webicon_name(const char *app, int kind, char *out, size_t n);
/* The cached icon of app under home (a regular file, a PNG preferred): 0 with its path, -1 when there is none. */
int webicon_cached(const char *home, const char *app, char *out, size_t n);
/* Keeps b (checked again: a PNG or an SVG of at most WEBREG_ICON_BYTES) as app's icon, the other kind removed.
 * 0 with the new file's path in out (optional), -1 refused or not written. */
int webicon_store(const char *home, const char *app, const unsigned char *b, size_t len, char *out, size_t n);
void webicon_remove(const char *home, const char *app);

/* ---- a profile's icon (its color's dot, with its initial, on the app's icon), for lord-web's tray icon and
 * notifications: <config>/icons/<app>.<profile>.png, config = ~/.config/lordos/web (docs/WEBAPPS.md, "Profiles") ---- */
#define WEBICON_SIDE 64
/* The tag color's 0xRRGGBB (red, orange, yellow, green, blue, purple, gray); -1 for none or anything else. */
long webicon_color_rgb(const char *color);
/* "<app>.<profile>.png". 0, or -1 for a bad id or a short buffer. */
int  webicon_profile_name(const char *app, const char *profile, char *out, size_t n);
/* The app's PNG (base; NULL or not a PNG: a gray tile) at WEBICON_SIDE px with a dot of rgb at the bottom end corner,
 * a white ring and letter (A-Z, 0-9; anything else: no letter) in it. RGBA (malloc'd, WEBICON_SIDE² × 4), or NULL. */
unsigned char *webicon_compose(const unsigned char *base, size_t len, long rgb, char letter);
/* Composes and writes <config>/icons/<app>.<profile>.png (0600, atomically); with rgb -1 removes it instead. 0 done. */
int  webicon_profile_write(const char *config, const char *app, const char *profile, const unsigned char *base, size_t len, long rgb, char letter);
void webicon_profile_remove(const char *config, const char *app, const char *profile);

#endif
