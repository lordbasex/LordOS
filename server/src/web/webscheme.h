/* webscheme.h — the desktop's color scheme for web apps (docs/WEBAPPS.md, "Light and dark"), pure (tests/webapp_test.c).
 *
 * WPE WebKit 2.48 has no way to tell a page the person's light or dark mode: every page's
 * matchMedia('(prefers-color-scheme: dark)') answers false. lord-server tells lord-web the desktop's scheme with two
 * closed words, "light" and "dark", never anything else:
 *
 *   - at the start, in lord-web's environment: LORDOS_WEB_SCHEME=light|dark (webmgr.c builds it; missing or any other
 *     value is light);
 *   - while it runs: org.lordos.WebApp1.SetColorScheme(s scheme) on its bus name, from lord-server's own process only
 *     (the bus says the caller's pid, the same check as Edit); any other word is refused with InvalidArgs.
 *
 * lord-web then answers the page's matchMedia queries about prefers-color-scheme with the desktop's scheme (a script
 * in every frame, webscheme.js) and paints the view's background dark while a page loads in dark mode. Nothing goes
 * the other way: the page can't tell lord-server or lord-web anything through it. */
#ifndef LORD_WEB_WEBSCHEME_H
#define LORD_WEB_WEBSCHEME_H

#include <stddef.h>

enum { WEBSCHEME_LIGHT = 0, WEBSCHEME_DARK = 1 };

#define WEBSCHEME_ENV     "LORDOS_WEB_SCHEME"
#define WEBSCHEME_KEY_LEN 32            /* the script's hook on window: "__lordosScheme" + 32 hex digits, random per process */

/* the view's background in dark mode while a page loads (the desktop's own window color, #1f1f23) */
#define WEBSCHEME_DARK_BG_R 0x1f
#define WEBSCHEME_DARK_BG_G 0x1f
#define WEBSCHEME_DARK_BG_B 0x23

int         webscheme_parse(const char *name);      /* WEBSCHEME_LIGHT or WEBSCHEME_DARK for exactly "light" or "dark", else -1 */
const char *webscheme_name(int scheme);             /* "light" or "dark", NULL for anything else */
int         webscheme_from_env(const char *value);  /* the environment's value: dark only for exactly "dark" */
/* the user script: js (webscheme.js, a function expression) called with the hook's name and the scheme. key must be
 * WEBSCHEME_KEY_LEN lowercase hex digits. Malloc'd, or NULL for a bad key or scheme. */
char       *webscheme_script(const char *js, const char *key, int scheme);
/* what lord-web runs in the main frame when the scheme changes: the hook with the new scheme (it passes it on to
 * same-origin frames). Malloc'd, or NULL for a bad key or scheme. */
char       *webscheme_call(const char *key, int scheme);

#endif
