/* webdark.c — a web app follows the desktop's light or dark mode (docs/WEBAPPS.md, "Light and dark"; webscheme.h).
 *
 * WPE WebKit 2.48 has no API for the page's prefers-color-scheme: every page sees light. lord-web does two things:
 *
 *   - a script of its own (webscheme.js) at document start in every frame, in the page's world, so the page's
 *     matchMedia answers any query about prefers-color-scheme with the desktop's scheme and sends 'change' when it
 *     flips. A new scheme (SetColorScheme from lord-server) replaces the script for the next loads and calls its hook
 *     in the main frame, which passes it on to same-origin frames; a frame of another origin gets it with its next
 *     load. CSS @media (prefers-color-scheme) rules still follow WebKit's own answer (light): see docs/WEBAPPS.md;
 *   - in dark mode, the view's background is the desktop's dark window color from a load's start to its end, so no
 *     white shows before the page paints its own; then it is white again, WebKit's default, so a page that paints no
 *     background of its own reads as it would in a browser.
 *
 * The script has no privileges and no message handler: the page can call its hook, which changes only what the page
 * itself sees, and nothing goes from the page to lord-web or lord-server. */
#include "web.h"
#include "webscheme_js.h"                /* build/gen, from webscheme.js (mk/web.mk) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

static void set_background(Web *w) {
    WebKitColor c = { 1, 1, 1, 1 };
    if (w->scheme == WEBSCHEME_DARK && w->loading)
        c = (WebKitColor){ WEBSCHEME_DARK_BG_R / 255.0, WEBSCHEME_DARK_BG_G / 255.0, WEBSCHEME_DARK_BG_B / 255.0, 1 };
    webkit_web_view_set_background_color(w->view, &c);
}

/* the script for the scheme now, in place of the one before (it runs from the next load on) */
static void add_script(Web *w) {
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(w->view);
    if (!ucm || !w->scheme_key[0]) return;
    char *js = webscheme_script(WEBSCHEME_JS, w->scheme_key, w->scheme);
    if (!js) return;
    if (w->scheme_script) {
        webkit_user_content_manager_remove_script(ucm, w->scheme_script);
        webkit_user_script_unref(w->scheme_script);
    }
    w->scheme_script = webkit_user_script_new(js, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    free(js);
    webkit_user_content_manager_add_script(ucm, w->scheme_script);
}

void webdark_init(Web *w) {
    w->loading = 1;                     /* the first load starts right after this */
    set_background(w);
    guint8 r[WEBSCHEME_KEY_LEN / 2];
    if (getrandom(r, sizeof r, 0) != (ssize_t)sizeof r) return;      /* no key, no script: the page sees light */
    for (size_t i = 0; i < sizeof r; i++) snprintf(w->scheme_key + 2 * i, 3, "%02x", r[i]);
    add_script(w);
}

void web_set_scheme(Web *w, int scheme) {
    if (!webscheme_name(scheme) || scheme == w->scheme || !w->view) return;
    w->scheme = scheme;
    set_background(w);
    add_script(w);
    char *call = w->scheme_key[0] ? webscheme_call(w->scheme_key, scheme) : NULL;
    if (call) webkit_web_view_evaluate_javascript(w->view, call, -1, NULL, NULL, NULL, NULL, NULL);
    free(call);
}

void webdark_load(Web *w, WebKitLoadEvent ev) {
    int was = w->loading;
    if (ev == WEBKIT_LOAD_STARTED) w->loading = 1;
    else if (ev == WEBKIT_LOAD_FINISHED) w->loading = 0;
    if (was != w->loading) set_background(w);
}
