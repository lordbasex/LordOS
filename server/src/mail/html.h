/* html.h — mail HTML, without a browser (see docs/MAIL.md, "HTML without executing anything").
 *
 * LordOS has no web engine: a message's HTML is never executed or "loaded". This
 * sanitizer reads it with an in-house, lenient tokenizer and builds a small JSON tree that
 * Mail renders with its native views (View/Text/Image):
 *
 *   {"v":1,"bg":"#fff","blocks":[node…],"remote":["https://…"],"links":[{"h":"…","w":"deceptive"}],
 *    "stats":{"remote":3,"trackers":1,"warn":1,"cid":2,"data":0}}
 *   node: {"t":"blk","s":{…},"c":[…]}  block (div, p, h1-6, blockquote, pre, center, li)
 *         {"t":"tbl","s":{…},"c":[{"t":"tr","c":[{"t":"td","s":{…},"c":[…]}]}]}
 *         {"t":"img","src":"cid:x"|"r:N"|"d:N","w":600,"h":200,"alt":"…","l":N}
 *         {"t":"hr"}  {"t":"br"}
 *         {"t":"x","x":"text","s":{b,i,u,st,mono,c,bg,fs},"l":N}   styled text; l = link index
 *
 * Always stripped: script, style, iframe, object, embed, form and its controls, svg,
 * math, audio/video, comments, <!DOCTYPE>, events (on*), CSS url(), javascript:/vbscript:/
 * data: in links (links may only be http, https and mailto) and everything under display:none.
 * Remote images are not fetched here: they go into "remote" and Mail decides (blocked by default).
 * 1×1 or 2×2 ones (tracking pixels) are counted in "trackers" and never shown.
 * Deceptive links: if the visible text looks like a web address and its domain is not the href's
 * (or the href contains user@, an IP or an odd scheme), the link is emitted with "w". */
#ifndef LORD_MAIL_HTML_H
#define LORD_MAIL_HTML_H

#include <stddef.h>

#define HTML_MAX_INPUT (8u << 20)
#define HTML_MAX_NODES 40000
#define HTML_MAX_DEPTH 48
/* Deepest JSON nesting of a message view that carries this tree: each block frame adds an object
 * and its "c" array, plus the message, "html", "blocks" and a text node with its "s". Whoever
 * validates that JSON (mailc.c) must accept at least this many levels. */
#define HTML_JSON_DEPTH (2 * HTML_MAX_DEPTH + 8)
#define HTML_MAX_REMOTE 200
#define HTML_MAX_LINKS 1000

typedef struct { int remote, trackers, warn, cid, data, nodes, truncated; } HtmlStats;

/* data: (base64) images found: they point into the input; "d:N" is the Nth one. */
typedef struct { const char *p; size_t n; char type[16]; } HtmlData;
#define HTML_MAX_DATA 64

/* JSON of the tree (malloc, always valid) or NULL when out of memory. data (optional) receives up to
 * HTML_MAX_DATA data: images and ndata how many. */
char *html_sanitize(const char *html, size_t len, size_t *olen, HtmlStats *st, HtmlData *data, int *ndata);
/* Plain text of the HTML (for the preview, search and the text/plain part when replying). */
char *html_to_text(const char *html, size_t len, size_t *olen);
/* Domain (lowercase host, without "www.") of an http(s) URL; "" if none. */
void  html_url_host(const char *url, char *out, size_t cap);
/* Does a link's visible text mislead about its destination? Returns a reason or NULL. */
const char *html_link_warning(const char *text, const char *href);

#endif
