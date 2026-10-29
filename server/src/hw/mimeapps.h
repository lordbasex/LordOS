/* mimeapps.h — default applications (freedesktop "MIME Applications Associations" spec):
 * reading and editing mimeapps.list text and guessing a file's MIME type from shared-mime-info's
 * globs2. Pure functions over text; the files are read and written by ui/js_hw.c. Used by
 * Settings → Default Apps, Files' "Open With" and lord.apps.launchWith. See docs/SETTINGS.md.
 * Unit-tested in tests/mimeapps_test.c. */
#ifndef LORD_HW_MIMEAPPS_H
#define LORD_HW_MIMEAPPS_H

#include <stddef.h>

#define MIME_MAX 128
#define DESKTOP_ID_MAX 160

/* "type/subtype" (also x-scheme-handler/https): [a-z0-9] start, then [A-Za-z0-9.+_-]. */
int  mime_valid(const char *mime);
/* A desktop file id: "org.gnome.Evince.desktop", "lordos-photos.desktop". No '/', no "..". */
int  desktop_id_valid(const char *id);

/* First application in [Default Applications] for mime (the "mime=a.desktop;b.desktop;" list).
 * 0 if found (out gets the id). */
int  mimeapps_default(const char *text, const char *mime, char *out, size_t n);
/* The idx-th valid desktop id (0 = first) of mime's list in group: "Default Applications" or
 * "Added Associations" of a mimeapps.list, "MIME Cache" of a mimeinfo.cache. 0 if there is one. */
int  mimeapps_entry(const char *text, const char *group, const char *mime, int idx, char *out, size_t n);
/* Is id listed in [Removed Associations] for mime? */
int  mimeapps_removed(const char *text, const char *mime, const char *id);
/* New text with id as the default for mime: the [Default Applications] line is replaced (or
 * added, creating the group if needed) and id goes first in [Added Associations] (as GLib does).
 * Every other line is kept byte for byte. Returns a malloc'd string, or NULL. */
char *mimeapps_set_default(const char *text, const char *mime, const char *id);

/* New text without mime's lines in [Default Applications] and [Added Associations]: back to the
 * automatic choice, as GLib's reset does ([Removed Associations] is kept). Every other line is kept
 * byte for byte. Returns a malloc'd string, or NULL. */
char *mimeapps_unset_default(const char *text, const char *mime);

/* MIME type of a file name from a globs2 text ("weight:type:glob[:flags]"): highest weight,
 * then longest pattern; case-insensitive unless the "cs" flag is set. 0 if found. */
int  mime_guess(const char *globs2, const char *name, char *out, size_t n);

#endif
