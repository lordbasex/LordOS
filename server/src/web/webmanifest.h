/* webmanifest.h — what `lord-web --probe` answers (docs/WEBAPPS.md, "Adding an app from an address"), built from
 * what the page showed and checked against the address WebKit really loaded; and what `lord-web --probe-icon` may
 * hand back. Pure (tests/webapp_test.c).
 *
 * The page's side is read by a script in an isolated world (webprobe.c), which resolves every address against the
 * manifest's or the document's own and answers one JSON object:
 *   { "title": s, "metaThemeColor": s|null, "manifest": null | { "name", "short_name", "start_url", "scope",
 *     "theme_color": s|null, "icons": [{ "src", "sizes", "type" }] }, "linkIcons": [{ "src", "sizes", "type" }] }
 * Nothing in it is trusted: the origin is the loaded address's, a start address or a scope on another origin is
 * replaced by the page's own, names and colors are cleaned, and at most WEBMANIFEST_ICONS icons are listed. */
#ifndef LORD_WEB_WEBMANIFEST_H
#define LORD_WEB_WEBMANIFEST_H

#include "webreg.h"

#include <stddef.h>

#define WEBMANIFEST_ICONS  16
#define WEBMANIFEST_NAME_MAX 128               /* the probe's cleaned name (the registry keeps WEBREG_NAME_MAX) */
#define WEBMANIFEST_PAGE_MAX (256 * 1024)      /* the script's answer, the manifest's text included */
#define WEBMANIFEST_LINE_MAX (48 * 1024)       /* the answer on fd 3 */
#define WEBICON_MAX        WEBREG_ICON_BYTES   /* --probe-icon: the icon's bytes */

/* The answer line (no newline): {"ok":true,"name",...,"manifest":true|false}. doc_url is the address WebKit loaded
 * (after redirects). 0, or -1 when doc_url isn't an app address or page isn't the object above. */
int webmanifest_answer(const char *doc_url, const char *page, size_t len, char *out, size_t n);
/* {"ok":false,"error":"<code>"} */
int webmanifest_error(const char *code, char *out, size_t n);
/* "#rgb" or "#rrggbb" (any case) as "#rrggbb" in lower case; 0, or -1 for anything else */
int webmanifest_color(const char *s, char out[8]);

enum { WEBICON_NONE, WEBICON_PNG, WEBICON_SVG };
/* What the icon's bytes are (webreg_icon_kind): a PNG (its signature, an IHDR chunk, 1..4096 px) or an SVG (text whose first element is <svg>, no script,
 * after an XML declaration, comments or a doctype); WEBICON_NONE for anything else or over WEBICON_MAX. */
int webicon_kind(const unsigned char *b, size_t n);
/* MIME type of a kind: "image/png", "image/svg+xml", or NULL */
const char *webicon_type(int kind);

#endif
