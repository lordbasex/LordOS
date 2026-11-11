/* webdrop.h — files dropped on a web app's window (docs/WEBAPPS.md, "Drag and drop"): which ones of a text/uri-list
 * lord-web reads and hands to the page. Only local files: file:// URIs with no host (or "localhost"), percent-decoded
 * to an absolute path without a NUL, a regular file once opened (a link is followed: Files drags what the person
 * sees), at most WEBDROP_FILE_MAX bytes; at most WEBDROP_FILES_MAX of them. Any app may start a drag with any list,
 * so the file, where it really is (realpath), must be in the person's places (webdrop_place_ok). Everything else on the list (other
 * schemes, comments, folders, devices, files too big, the ones past the limit) is skipped, never an error.
 * Tests: tests/webapp_test.c. */
#ifndef LORD_WEB_WEBDROP_H
#define LORD_WEB_WEBDROP_H

#include <stddef.h>
#include <sys/types.h>

#define WEBDROP_FILES_MAX 20
#define WEBDROP_FILE_MAX  (64 * 1024 * 1024)
#define WEBDROP_LIST_MAX  (1024 * 1024)          /* the uri-list itself */
#define WEBDROP_NAME_MAX  255

typedef struct {
    int fd;                                      /* open read-only, close-on-exec */
    size_t size;
    long long mtime_ms;
    char name[WEBDROP_NAME_MAX + 1];             /* the file's own name, for the page's File */
} WebDropFile;

/* The local path of one URI of the list (without its line break) into out: 0, or -1 when it isn't one. */
int webdrop_uri_path(const char *uri, size_t len, char *out, size_t n);
/* 1 when real (a path without links) is in the person's places: under home, /media, /mnt, /run/media or /tmp, with no
 * hidden folder or file (a name starting with a dot) on the way; else 0. */
int webdrop_place_ok(const char *real, const char *home);
/* The files of a uri-list (CRLF or LF lines), opened, into out (at most max, ≤ WEBDROP_FILES_MAX): how many. */
int webdrop_files(const char *list, size_t len, const char *home, WebDropFile *out, int max);
void webdrop_close(WebDropFile *f, int n);

#endif
