/* urilist.h — file paths as text/uri-list (RFC 2483) for drag and drop between LordOS and Linux apps
 * (see urilist.c). */
#ifndef LORD_URILIST_H
#define LORD_URILIST_H

#include <stddef.h>

#define URIL_MAX_PATHS 256              /* the most paths one drag carries */
#define URIL_MAX_BYTES (1u << 20)       /* the most bytes of text/uri-list read from an app */

/* A path a drag may carry: absolute, shorter than PATH_MAX, without a NUL (len is the whole length),
 * a newline or a carriage return. It does not look at the disk. */
int uril_path_ok(const char *path, size_t len);

/* The paths as file:// URIs, percent-encoded, each one ended by CRLF. Returns the length written to out
 * (with a NUL after it), or 0 if a path is not uril_path_ok or the list does not fit in cap. */
size_t uril_encode(const char *const *paths, int n, char *out, size_t cap);

/* The paths as plain text, one per line (for a text field or a terminal). Same returns as uril_encode. */
size_t uril_plain(const char *const *paths, int n, char *out, size_t cap);

/* The local paths in a text/uri-list an app gave: one per file:// line with an empty host or
 * "localhost" (other hosts, other schemes, comments and blank lines are skipped). A %00, a newline
 * or a relative path drops that line. They go to out one after another, each ended by its NUL;
 * returns how many (at most max), or -1 if they do not fit in cap. */
int uril_decode(const char *data, size_t len, char *out, size_t cap, int max);

#endif
