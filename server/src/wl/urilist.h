/* urilist.h — lists of files on the clipboard: text/uri-list and x-special/gnome-copied-files (see urilist.c). */
#ifndef LORD_URILIST_H
#define LORD_URILIST_H

#include <stddef.h>

#define URIL_MAX_FILES 65536            /* a longer list is refused whole, never cut short */
#define URIL_MAX_BYTES (16u << 20)      /* what the clipboard reads at most (wl.c CLIP_MAX) */
#define URIL_PATH_MAX 4096              /* bytes of one path, its NUL included */

enum { URIL_URI_LIST, URIL_GNOME, URIL_PLAIN };

/* A path LordOS may put on the clipboard: absolute, shorter than URIL_PATH_MAX, valid UTF-8, without
 * NUL, CR or LF (n is its length as the caller got it, so a NUL inside shows as n != strlen). */
int uril_path_ok(const char *p, size_t n);

/* A list of paths ("plist": each one followed by its NUL, len bytes in all) as fmt:
 *   URIL_URI_LIST  "file:///a%20b\r\n" per path
 *   URIL_GNOME     "copy\n" or "cut\n", then the URIs separated by "\n"
 *   URIL_PLAIN     the paths separated by "\n"
 * Returns malloc'd text (NUL-terminated, *out_len without it) or NULL (a bad path, an empty list, no memory). */
char *uril_encode(const char *plist, size_t len, int fmt, int cut, size_t *out_len);

/* Reads what an app offered (gnome = 1: x-special/gnome-copied-files, else text/uri-list) into a plist.
 * Only file:// URIs with an empty or "localhost" host count; each is decoded and must pass uril_path_ok;
 * comments and blank lines are skipped, any other URI is left out. *cut gets the gnome list's "cut".
 * NULL if no file is left, the text is not one of those lists, or it has too many files or bytes. */
char *uril_parse(const char *buf, size_t n, int gnome, int *cut, size_t *plist_len, int *count);

/* Where an app's file list came from: an app outside any sandbox, an isolated LordOS app (lord-server --app),
 * a Flatpak app, or one that couldn't be told apart (an X11 owner, a client whose root can't be looked at). */
enum { URIL_FROM_APP, URIL_FROM_ISOLATED, URIL_FROM_FLATPAK, URIL_FROM_UNKNOWN };

/* Whether text (n bytes) names exactly the paths of plist, in the same order: one per line (LF or CRLF, one
 * line end after the last allowed), each the path itself or a file:// URI of this machine that decodes to it. */
int uril_text_names(const char *text, size_t n, const char *plist, size_t len);

/* Whether LordOS keeps an app's file list (pasted by Files as a real copy of those files): only from an app
 * outside a sandbox (a sandboxed one could name files it can't read, for Files to copy where it can), and only
 * when the selection has no text (text NULL) or its text names exactly those paths, so what the person sees
 * as the clipboard's text is what gets copied. Otherwise the selection is text only. */
int uril_accept(int from, const char *text, size_t n, const char *plist, size_t len);

#endif
