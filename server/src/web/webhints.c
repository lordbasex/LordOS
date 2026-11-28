/* webhints.c — a profile that identifies as Chrome looks like Chrome beyond its User-Agent string (webua.h;
 * docs/WEBAPPS.md, "Browser identity"):
 *
 *   - navigator.userAgentData and navigator.vendor: webua.js at document start in every frame, in the page's world;
 *   - the Sec-CH-UA-* request headers: lord-web-ext.so (webext.c), WebKit's web process extension, loaded only from
 *     lord-web's own folder (<lord-web's folder>/lord-web-ext/), told the identity, the machine and the hints each site
 *     asked for (each new web process at its start, running ones with a message when something changes);
 *   - Accept-CH: read here on the main frame's responses of the app's own site only (inside its scope, on its origin),
 *     kept in the profile's state.json as Chrome keeps it and logged without the origin (webcall.h); another site's
 *     is ignored, so no trail of other sites is kept or written.
 *
 * Another identity gets none of it, and switching identity (SetUserAgent) adds or removes all of it at once. */
#include "web.h"
#include "webcall.h"
#include "webua_js.h"                   /* build/gen, from webua.js (mk/web.mk) */

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

static char machine[65];
static WebKitUserScript *ua_script;
static int ext_dir(char *out, size_t n);

static int is_chrome(Web *w) { return w->user_agent == WEBREG_UA_CHROME; }

GVariant *webhints_variant(int chrome, const char *mach, const WebUaSites *s) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a(su)"));
    for (int i = 0; s && i < s->n; i++) g_variant_builder_add(&b, "(su)", s->site[i].origin, (guint32)s->site[i].mask);
    return g_variant_new("(bsa(su))", chrome != 0, mach ? mach : "", &b);
}

static GVariant *ext_data(Web *w) { return webhints_variant(is_chrome(w), machine, &w->state.hints); }

WebKitUserScript *webhints_script_new(const char *mach) {
    char *js = webua_script(WEBUA_JS, mach);
    if (!js) return NULL;
    WebKitUserScript *s = webkit_user_script_new(js, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    free(js);
    return s;
}

int webhints_ext_dir(char *out, size_t n) { return ext_dir(out, n); }

/* each new web process starts with what is true now */
static void on_init_extensions(WebKitWebContext *c, gpointer d) {
    webkit_web_context_set_web_process_extensions_initialization_user_data(c, ext_data(d));
}

/* running web processes learn it at once */
static void tell_extensions(Web *w) {
    if (!w->context) return;
    webkit_web_context_send_message_to_all_extensions(w->context, webkit_user_message_new("lordos-ua", ext_data(w)));
}

/* the extension's folder: next to lord-web, never from the environment; a folder that isn't a real directory, or whose
 * file isn't a regular one, and no extension is loaded (the hints aren't sent; the rest works) */
static int ext_dir(char *out, size_t n) {
    char exe[PATH_MAX];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k <= 0) return -1;
    exe[k] = 0;
    int r = snprintf(out, n, "%s/lord-web-ext", dirname(exe));
    if (r < 0 || (size_t)r >= n) return -1;
    char so[PATH_MAX + 32];
    struct stat st;
    snprintf(so, sizeof so, "%s/lord-web-ext.so", out);
    if (lstat(out, &st) || !S_ISDIR(st.st_mode) || lstat(so, &st) || !S_ISREG(st.st_mode)) return -1;
    return 0;
}

void webhints_context(Web *w) {
    struct utsname u;
    if (!uname(&u)) snprintf(machine, sizeof machine, "%s", u.machine);
    char dir[PATH_MAX];
    if (ext_dir(dir, sizeof dir)) {
        fprintf(stderr, "lord-web: no lord-web-ext next to lord-web: Chrome's Client Hints are not sent\n");
        return;
    }
    webkit_web_context_set_web_process_extensions_directory(w->context, dir);
    g_signal_connect(w->context, "initialize-web-process-extensions", G_CALLBACK(on_init_extensions), w);
}

static void script_update(Web *w) {
    WebKitUserContentManager *ucm = w->view ? webkit_web_view_get_user_content_manager(w->view) : NULL;
    if (!ucm) return;
    if (ua_script) {
        webkit_user_content_manager_remove_script(ucm, ua_script);
        webkit_user_script_unref(ua_script);
        ua_script = NULL;
    }
    if (!is_chrome(w) || !(ua_script = webhints_script_new(machine))) return;
    webkit_user_content_manager_add_script(ucm, ua_script);
}

void webhints_view(Web *w) { script_update(w); }

void webhints_identity(Web *w) {
    script_update(w);
    tell_extensions(w);
}

void webhints_response(Web *w, WebKitURIResponse *r) {
    if (!r || !is_chrome(w)) return;
    /* only the app's own site: another site's Accept-CH (a sign-in, a redirect, anything outside the app's scope) is
     * neither kept nor logged, so neither state.json nor the log becomes a trail of where the window went. Such a site
     * still gets the low-entropy hints, as from Chrome, and no high-entropy ones */
    char origin[256];
    if (webcall_hints_site(webkit_uri_response_get_uri(r), w->app.scope, w->app.origin, origin, sizeof origin)) return;
    SoupMessageHeaders *h = webkit_uri_response_get_http_headers(r);
    const char *v = h ? soup_message_headers_get_list(h, "Accept-CH") : NULL;
    /* no Accept-CH keeps what the site asked before, as Chrome does; an empty one asks for nothing more */
    if (!v) return;
    unsigned mask = webua_accept_ch(v);
    if (!webua_sites_set(&w->state.hints, origin, mask)) return;
    webcall_log_hints(mask);
    web_state_save(w);
    tell_extensions(w);
}
