/* webclip.h — what lord-web takes from the session's clipboard when the person pastes (docs/WEBAPPS.md, "The
 * clipboard"): which text type it reads and how those bytes become UTF-8, which file list it reads and where its URIs
 * start, and whether WebKit's own paste reads a text right. Pure (no WebKit, no GLib).
 * Tests: tests/webapp_test.c. */
#ifndef LORD_WEB_WEBCLIP_H
#define LORD_WEB_WEBCLIP_H

#include <stddef.h>

#define WEBCLIP_URIS  "text/uri-list"
#define WEBCLIP_GNOME "x-special/gnome-copied-files"

/* How good a text type is: 4 text/plain;charset=utf-8, 3 UTF8_STRING, 2 text/plain (UTF-8 when it is valid UTF-8,
 * else Latin-1), 1 STRING (Latin-1, X11's rule). 0 for anything else: TEXT and COMPOUND_TEXT are never read. */
int webclip_text_rank(const char *mime);
/* 1 when len bytes are valid UTF-8 (no overlong forms, no surrogates, nothing past U+10FFFF) */
int webclip_utf8_valid(const char *s, size_t len);
/* The text of type mime as UTF-8, malloc'd and NUL-ended (*out_len its length): as it is for a UTF-8 type that is
 * valid UTF-8, converted from Latin-1 for STRING and for a text/plain that isn't UTF-8. NULL for a UTF-8 type that
 * isn't valid UTF-8, a type of rank 0, or no memory. */
char *webclip_text_utf8(const char *mime, const char *data, size_t len, size_t *out_len);
/* How good a file list type is: 2 text/uri-list, 1 x-special/gnome-copied-files, 0 anything else */
int webclip_list_rank(const char *mime);
/* Where the URI lines of a list of type mime start: after the "copy" or "cut" line of a gnome-copied-files list
 * (len when it has nothing else), 0 for a text/uri-list */
size_t webclip_list_body(const char *mime, const char *data, size_t len);
/* How many URI lines a list's body has (CRLF or LF; empty lines and # comments don't count) */
int webclip_list_count(const char *data, size_t len);
/* 1 when WebKit's own paste reads this text right: ASCII only. WPE 2.48's pasteboard (PlatformPasteboardLibWPE.cpp
 * readString) makes a String of the stored bytes as Latin-1 while it writes UTF-8, so any other text pasted by WebKit
 * comes out mojibake ("Imágenes" as "ImÃ¡genes"); lord-web pastes such a text itself. */
int webclip_webkit_reads(const char *s, size_t len);

#endif
