/* websearch.h — the pure parts of a web app's context menu that touch the page's own text (docs/WEBAPPS.md,
 * "Editing"), shared by lord-web (webengine.c) and lord-server (ui/webask.c, web/webmgr.c). Tests: tests/webapp_test.c.
 *
 *   - "Search the web for “…”": the engine is one of a closed list (Settings → Web apps → Search the web with),
 *     kept in ~/.config/lordos/web/search.json as {"engine":"duckduckgo"} (the folder is in fsops_protected: only
 *     lord.webapps writes it). A missing file, or one that isn't valid, is Google, the default, as in Chrome. The
 *     address is that engine's fixed search address with the selected text percent-encoded as its one parameter:
 *     nothing else of the page (no address, no title) goes with it.
 *   - The few words of the selection that the menu shows in that item: the page's text, so it is cleaned before
 *     anything shows it (control characters, bidi embeddings, overrides and isolates, and invalid UTF-8 removed;
 *     line breaks and runs of spaces made one space) and cut to WEBSEARCH_SNIPPET_CHARS characters with an ellipsis.
 *     lord-web cleans it before it sends it and lord-server cleans it again before the shell gets it.
 *   - "Copy Link to Highlight": the page's address without its fragment and a text fragment (#:~:text=) that points
 *     at the selection: the whole text when it is short, else its first and last words (textStart,textEnd). */
#ifndef LORD_WEB_WEBSEARCH_H
#define LORD_WEB_WEBSEARCH_H

#include <stddef.h>

enum { WEBSEARCH_GOOGLE, WEBSEARCH_DUCKDUCKGO, WEBSEARCH_BING, WEBSEARCH_STARTPAGE, WEBSEARCH_N };

#define WEBSEARCH_FILE       "search.json"         /* in ~/.config/lordos/web */
#define WEBSEARCH_FILE_MAX   256
#define WEBSEARCH_QUERY_MAX  512                    /* bytes of the selection that go to the engine (UTF-8, whole characters) */
#define WEBSEARCH_URL_MAX    (64 + 3 * WEBSEARCH_QUERY_MAX)
#define WEBSEARCH_SNIPPET_CHARS 30                  /* characters of the selection in the menu's item */
#define WEBSEARCH_SNIPPET_MAX   (4 * WEBSEARCH_SNIPPET_CHARS + 8)   /* its bytes, with the ellipsis and the NUL */
#define WEBSEARCH_SELECTION_MAX (16 * 1024)        /* the selection lord-web keeps for a menu (search, highlight) */

int         websearch_engine(const char *name);      /* the engine named so (google, duckduckgo, bing, startpage), or -1 */
const char *websearch_engine_name(int engine);       /* its name, or NULL */
/* the engine the file's text names; WEBSEARCH_GOOGLE for anything else */
int         websearch_parse(const char *text, size_t len);
/* the file's text for engine; its length, or 0 for an engine that isn't on the list or a buffer too small */
size_t      websearch_write(int engine, char *out, size_t n);

/* The engine's search address for the selected text (its whitespace made single spaces and trimmed, cut to
 * WEBSEARCH_QUERY_MAX bytes at a character's start, invalid UTF-8 and control characters left out, percent-encoded).
 * 0, or -1 when nothing is left to search for, the engine isn't on the list or out is too small. */
int         websearch_url(int engine, const char *text, char *out, size_t n);

/* The selection as the menu may show it (see above), at most max characters and an ellipsis; out has n bytes.
 * The length in bytes ("" when nothing is left). */
size_t      websearch_snippet(const char *text, size_t max, char *out, size_t n);

/* The page's address (http or https only; its fragment dropped) with a text fragment for the selected text. 0, or -1
 * when the address isn't http(s), the text is empty once cleaned, or out is too small. */
int         websearch_highlight(const char *page, const char *text, char *out, size_t n);

#endif
