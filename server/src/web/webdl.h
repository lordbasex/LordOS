/* webdl.h — where a web app's download goes (docs/WEBAPPS.md, "What lord-web uses of the session"): only the XDG
 * Downloads folder, under a name taken from what the page suggested and made safe, never over an existing file.
 * Pure apart from the folder lookups (tests/webapp_test.c). */
#ifndef LORD_WEB_WEBDL_H
#define LORD_WEB_WEBDL_H

#include <stddef.h>

#define WEBDL_NAME_MAX 200      /* bytes, the extension kept */

/* The last part of suggested ('/' and '\' separate parts), without control characters, with '/', '\', ':', '*', '?',
 * '"', '<', '>' and '|' as '_', no leading dot or space, no trailing dot or space, cut to WEBDL_NAME_MAX bytes on a
 * character boundary keeping a short extension. "download" when nothing is left. Returns 0 (or -1 if n is too small). */
int webdl_name(const char *suggested, char *out, size_t n);
/* name, or "stem (2).ext", "stem (3).ext"… : the first that doesn't exist in the folder dirfd. 0, or -1 after 999. */
int webdl_unique(int dirfd, const char *name, char *out, size_t n);

/* "Save Image As…" and "Save Video As…" (the page's context menu): the file the person chose in LordOS's save dialog
 * (the FileChooser portal). 1 when path may be written: absolute, in home (not home itself), no "." or ".." part, no
 * part that starts with a dot (the hidden folders hold what starts programs and LordOS's settings), no control
 * character, and not a launcher (*.desktop); else 0. */
int webdl_target_ok(const char *home, const char *path);

#endif
