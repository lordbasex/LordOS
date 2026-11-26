/* webext.c — lord-web's WebKit web process extension, lord-web-ext.so (docs/WEBAPPS.md, "Browser identity"; webua.h).
 *
 * It does one thing: when the profile's Browser identity is Chrome, it adds Chrome's User-Agent Client Hints to every
 * request the page makes (WebKitWebPage::send-request), which no UI-process API of WebKit can do:
 *
 *   - Sec-CH-UA, Sec-CH-UA-Mobile and Sec-CH-UA-Platform on every request to a secure origin (https, or http on
 *     localhost), as Chrome;
 *   - the high-entropy hints only to a site that asked for them with Accept-CH (lord-web reads that header on the main
 *     frame's responses and tells this extension), and only on requests to that site from its own pages.
 *
 * It runs inside WebKit's sandboxed web process, loaded by WebKit from lord-web's own fixed folder (never one named by
 * the environment: lord-web drops WEBKIT_*), and has no other code path: no access to the page's JavaScript, no file,
 * no socket, no message to lord-web. What it gets from lord-web (the identity's bit, the machine's name for
 * Sec-CH-UA-Arch, and each site's bits) is checked again here; every header value comes from webua.c's table. */
#include "webua.h"

#include <wpe/webkit-web-process-extension.h>
#include <libsoup/soup.h>
#include <stdio.h>
#include <string.h>

static int chrome;                      /* the profile's identity is Chrome's */
static char machine[65];
static WebUaSites sites;

/* lord-web's "(bsa(su))": Chrome or not, the machine, each site's origin and hint bits */
static void apply(GVariant *v) {
    if (!v || !g_variant_is_of_type(v, G_VARIANT_TYPE("(bsa(su))"))) return;
    gboolean c;
    const char *m;
    GVariantIter *it;
    g_variant_get(v, "(b&sa(su))", &c, &m, &it);
    chrome = c != 0;
    snprintf(machine, sizeof machine, "%s", strlen(m) < sizeof machine ? m : "");
    WebUaSites s = { .n = 0 };
    const char *origin;
    guint32 mask;
    while (g_variant_iter_next(it, "(&su)", &origin, &mask)) {
        char o[256];
        if (!webua_origin(origin, o, sizeof o) && !strcmp(o, origin)) webua_sites_set(&s, o, mask);
    }
    g_variant_iter_free(it);
    sites = s;
}

static gboolean on_send_request(WebKitWebPage *page, WebKitURIRequest *req, WebKitURIResponse *redirect, gpointer d) {
    (void)redirect; (void)d;
    char origin[256], page_origin[256];
    if (!chrome || webua_origin(webkit_uri_request_get_uri(req), origin, sizeof origin)) return FALSE;
    SoupMessageHeaders *h = webkit_uri_request_get_http_headers(req);
    if (!h) return FALSE;
    /* the high-entropy hints go to the site that asked, from its own pages (or to the first load of a page, before it
     * has an address of its own); a third party's requests carry only the low-entropy three, as in Chrome without a
     * Permissions-Policy delegation */
    const char *puri = webkit_web_page_get_uri(page);
    unsigned mask = 0;
    if (!puri || webua_origin(puri, page_origin, sizeof page_origin) || !strcmp(page_origin, origin)) mask = webua_sites_get(&sites, origin);
    for (int i = 0; i < WEBUA_LOW_N + WEBUA_HINT_N; i++) {
        const char *name;
        char value[256];
        if (webua_header(i, mask, machine, &name, value, sizeof value) == 1) soup_message_headers_replace(h, name, value);
    }
    return FALSE;
}

static void on_page(WebKitWebProcessExtension *ext, WebKitWebPage *page, gpointer d) {
    (void)ext; (void)d;
    g_signal_connect(page, "send-request", G_CALLBACK(on_send_request), NULL);
}

static gboolean on_message(WebKitWebProcessExtension *ext, WebKitUserMessage *msg, gpointer d) {
    (void)ext; (void)d;
    if (!g_strcmp0(webkit_user_message_get_name(msg), "lordos-ua")) apply(webkit_user_message_get_parameters(msg));
    return TRUE;
}

G_MODULE_EXPORT void webkit_web_process_extension_initialize_with_user_data(WebKitWebProcessExtension *ext, const GVariant *data);
G_MODULE_EXPORT void webkit_web_process_extension_initialize_with_user_data(WebKitWebProcessExtension *ext, const GVariant *data) {
    apply((GVariant *)data);
    g_signal_connect(ext, "page-created", G_CALLBACK(on_page), NULL);
    g_signal_connect(ext, "user-message-received", G_CALLBACK(on_message), NULL);
}
