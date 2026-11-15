/* browserprefs.h — LordOS's initial preferences for the Chromium-based browsers from the Store
 * (0.8.8.9; docs/WAYLAND.md "Frame and colors", docs/PACKAGES.md "Browser defaults").
 *
 * Debian's Chromium reads /etc/chromium/master_preferences, which the image writes
 * (tools/iso/browser-defaults.sh): new profiles get their own bar ("custom_chrome_frame") and the GTK
 * theme ("system_theme" 1), so the window buttons are LordOS's traffic lights from LordOS's GTK theme.
 * Google Chrome, Brave and Microsoft Edge read the same file format, but only next to their binary
 * (Chromium's InitialPreferences::Path: <dir of the executable>/initial_preferences, or
 * master_preferences when that one isn't readable); there is no /etc location and no policy for the
 * GTK theme. After the package engine installs or upgrades one of them, the root helper calls
 * browser_prefs_apply_pkg; migration 009 runs /usr/lib/lordos/bin/lord-browser-prefs (browserprefs_tool.c,
 * built from this file and sec/json.c only, no arguments), which calls browser_prefs_apply_all.
 *
 * The rules:
 * - a closed list in C maps a package name to its fixed directory; nothing comes from the caller but
 *   the package name, which only selects an entry;
 * - only that directory's initial_preferences (or master_preferences, if that's the file the browser
 *   reads) is written, and only if the directory already exists (the package is installed);
 * - an existing file is merged, never replaced: the vendor's keys stay, LordOS's two keys are added
 *   only where absent. A file that isn't a JSON object, a symlink or anything else is left alone;
 * - the text written is either a fixed generated JSON or the vendor's text with fixed keys inserted.
 * Home folders are never touched: only profiles created afterwards get the defaults. */
#ifndef LORD_PKG_BROWSERPREFS_H
#define LORD_PKG_BROWSERPREFS_H

#include <stddef.h>

/* The fixed directory (absolute, no root prefix) of a known browser package, or NULL. */
const char *browser_prefs_dir(const char *pkg);
/* Pure merge. text == NULL: no file yet. Returns 1 with *out (malloc'd, *outlen bytes) when there is
 * something to write, 0 when the text already has both keys (or keeps its own values for them),
 * -1 when the text must be left alone (not a JSON object, too large). */
int browser_prefs_merge(const char *text, size_t len, char **out, size_t *outlen);
/* Applies the merge in <root><dir>. root: "" or "/" for the real system, a fixture for tests.
 * 1 written, 0 nothing to do (or the browser isn't installed), -1 left alone or failed. */
int browser_prefs_apply_dir(const char *root, const char *dir);
int browser_prefs_apply_pkg(const char *root, const char *pkg);
/* Every known browser that is installed under root; returns how many files were written, -1 if any failed. */
/* One line per file written or left alone on standard output (lord-browser-prefs, migration 009). */
int browser_prefs_apply_all(const char *root);

#endif
