/* webengine.c — WebKit for one app and profile (see web.h).
 *
 *   - the profile's own WebKitNetworkSession: data/ and cache/ under ~/.local/share/lordos/web/<app>/<profile>/,
 *     cookies in cookies.sqlite there, no credential storage of WebKit's own;
 *   - WebKit's sandbox (bubblewrap, seccomp, xdg-dbus-proxy) is always on in 2.48: webmain.c refuses to start when the
 *     variable that turns it off is set;
 *   - memory pressure settings so a web process gives memory back early, no developer extras, no windows opened
 *     without a click;
 *   - navigation: an address inside the app's scope stays in the window; a link clicked to an address outside it, and
 *     a new window outside it, open through the OpenURI portal in the person's browser. A redirect or a script that
 *     takes the page outside (a sign-in on another site) stays, and the title then starts with that site's host;
 *   - permissions: answered from the registry, asking lord-server when it says "ask" (webbus.c);
 *   - downloads: only to the XDG Downloads folder, under a unique, cleaned name (webdl.c), with a notification. */
#include "web.h"
#include "webdl.h"
#include "webwin.h"
#include "weburl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WEB_MEMORY_LIMIT_MB 1536         /* a web process past this frees its caches (conservative), then more (strict) */
#define CRASH_WINDOW_US (30 * G_USEC_PER_SEC)

WebKitWebContext *web_context_new(void) {
    WebKitMemoryPressureSettings *mp = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(mp, WEB_MEMORY_LIMIT_MB);
    webkit_memory_pressure_settings_set_conservative_threshold(mp, 0.33);
    webkit_memory_pressure_settings_set_strict_threshold(mp, 0.5);
    webkit_memory_pressure_settings_set_poll_interval(mp, 10);
    /* the network process: the same settings, set once before any session exists */
    webkit_network_session_set_memory_pressure_settings(mp);
    WebKitWebContext *c = g_object_new(WEBKIT_TYPE_WEB_CONTEXT, "memory-pressure-settings", mp, NULL);
    webkit_memory_pressure_settings_free(mp);
    /* one app, one page: the document browser model keeps smaller caches than a browser's, and with no
     * back-forward cache WebKit has no reason to keep a second, prewarmed web process */
    webkit_web_context_set_cache_model(c, WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);
    webkit_web_context_set_spell_checking_enabled(c, FALSE);
    return c;
}

WebKitSettings *web_settings_new(void) {
    return webkit_settings_new_with_settings(
        "enable-developer-extras", FALSE,
        "enable-write-console-messages-to-stdout", FALSE,
        "javascript-can-open-windows-automatically", FALSE,
        "javascript-can-access-clipboard", FALSE,
        "allow-file-access-from-file-urls", FALSE,
        "allow-universal-access-from-file-urls", FALSE,
        "allow-top-navigation-to-data-urls", FALSE,
        "enable-page-cache", FALSE,
        "enable-media-stream", TRUE,
        "enable-webrtc", TRUE,
        "enable-mediasource", TRUE,
        "enable-encrypted-media", FALSE,
        "enable-html5-local-storage", TRUE,
        "enable-html5-database", TRUE,
        "enable-site-specific-quirks", TRUE,
        NULL);
}

static int mkdirs(const char *path, mode_t mode) {
    char p[PATH_MAX];
    if (snprintf(p, sizeof p, "%s", path) >= (int)sizeof p) return -1;
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(p, mode) && errno != EEXIST) return -1;
        *s = '/';
    }
    return mkdir(p, mode) && errno != EEXIST ? -1 : 0;
}

WebKitNetworkSession *web_session_new(Web *w) {
    char data[PATH_MAX + 8], cache[PATH_MAX + 8], cookies[PATH_MAX + 16];
    snprintf(data, sizeof data, "%s/data", w->profile_dir);
    snprintf(cache, sizeof cache, "%s/cache", w->profile_dir);
    snprintf(cookies, sizeof cookies, "%s/cookies.sqlite", w->profile_dir);
    if (mkdirs(data, 0700) || mkdirs(cache, 0700)) { fprintf(stderr, "lord-web: %s: %s\n", w->profile_dir, strerror(errno)); return NULL; }
    chmod(w->profile_dir, 0700);
    WebKitNetworkSession *s = webkit_network_session_new(data, cache);
    WebKitCookieManager *cm = webkit_network_session_get_cookie_manager(s);
    webkit_cookie_manager_set_persistent_storage(cm, cookies, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    webkit_network_session_set_persistent_credential_storage_enabled(s, FALSE);
    webkit_network_session_set_tls_errors_policy(s, WEBKIT_TLS_ERRORS_POLICY_FAIL);
    return s;
}

void web_quit(Web *w, int code) {
    if (w->quitting) return;
    w->quitting = 1;
    w->exit_code = code;
    if (w->loop) g_main_loop_quit(w->loop);
}

void web_show(Web *w, const char *token) {
    if (w->win) webwin_show(w->win, token);
}

void web_close_requested(Web *w) {
    if (w->app.background && w->tray && w->win) { webwin_hide(w->win); return; }
    web_quit(w, 0);
}

/* ---- title ---- */

static void update_title(Web *w) {
    if (!w->win) return;
    const char *t = webkit_web_view_get_title(w->view);
    const char *uri = webkit_web_view_get_uri(w->view);
    WebUrl u;
    if (w->outside && uri && !weburl_parse(uri, &u)) {
        char line[512];
        snprintf(line, sizeof line, "%s \xe2\x80\x94 %s", u.host, t && t[0] ? t : w->app.name);
        webwin_set_title(w->win, line);
    } else webwin_set_title(w->win, t);
}

static void on_title(WebKitWebView *v, GParamSpec *ps, gpointer d) { (void)v; (void)ps; update_title(d); }

static void on_uri(WebKitWebView *v, GParamSpec *ps, gpointer d) {
    (void)ps;
    Web *w = d;
    const char *uri = webkit_web_view_get_uri(v);
    w->outside = uri && strncmp(uri, "about:", 6) && !weburl_in_scope(uri, w->app.scope);
    update_title(w);
}

/* ---- navigation ---- */

static int is_web_address(const char *uri) { return uri && (!g_ascii_strncasecmp(uri, "https://", 8) || !g_ascii_strncasecmp(uri, "http://", 7)); }

static gboolean on_decide(WebKitWebView *v, WebKitPolicyDecision *dec, WebKitPolicyDecisionType type, gpointer d) {
    Web *w = d;
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        WebKitResponsePolicyDecision *r = WEBKIT_RESPONSE_POLICY_DECISION(dec);
        if (webkit_response_policy_decision_is_main_frame_main_resource(r) && !webkit_response_policy_decision_is_mime_type_supported(r)) {
            webkit_policy_decision_download(dec);
            return TRUE;
        }
        return FALSE;
    }
    WebKitNavigationAction *a = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(dec));
    WebKitURIRequest *req = webkit_navigation_action_get_request(a);
    const char *uri = webkit_uri_request_get_uri(req);
    if (!uri) { webkit_policy_decision_ignore(dec); return TRUE; }
    int inside = weburl_in_scope(uri, w->app.scope);
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        /* one window per app: a new window inside the scope loads here, outside it opens in the browser */
        webkit_policy_decision_ignore(dec);
        if (inside) webkit_web_view_load_request(v, req);
        else if (webkit_navigation_action_is_user_gesture(a)) webbus_open_uri(w, uri);
        return TRUE;
    }
    if (!inside && is_web_address(uri) && webkit_navigation_action_get_navigation_type(a) == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED
        && webkit_navigation_action_is_user_gesture(a)) {
        webkit_policy_decision_ignore(dec);
        webbus_open_uri(w, uri);
        return TRUE;
    }
    if (!is_web_address(uri) && g_ascii_strncasecmp(uri, "about:", 6) && g_ascii_strncasecmp(uri, "blob:", 5) && g_ascii_strncasecmp(uri, "data:", 5)) {
        /* mailto:, tel: and the like belong to the person's own apps */
        webkit_policy_decision_ignore(dec);
        if (webkit_navigation_action_is_user_gesture(a)) webbus_open_uri(w, uri);
        return TRUE;
    }
    return FALSE;
}

static WebKitWebView *on_create(WebKitWebView *v, WebKitNavigationAction *a, gpointer d);

/* ---- permissions ---- */

typedef struct { WebKitPermissionRequest *req; int kinds[2], n, i, allowed; } Pending;

static void ask_next(Web *w, Pending *p);

static void asked(Web *w, int kind, const char *answer, void *data) {
    (void)kind;
    Pending *p = data;
    if (strcmp(answer, "allow") && strcmp(answer, "once")) p->allowed = 0;
    p->i++;
    ask_next(w, p);
}

/* the page in the window is the app's own (weburl_perm_allowed): WebKit says no origin for a permission request,
 * so it is the main frame's address, inside the scope (a frame of another origin can't ask without the page's
 * Permissions Policy, and then it asks as the page) */
static int page_is_app(Web *w) {
    return !w->outside && weburl_perm_allowed(webkit_web_view_get_uri(w->view), NULL, w->app.scope, w->app.origin);
}

static void ask_next(Web *w, Pending *p) {
    /* again before each question and the answer: the page may have left the scope while lord-server asked */
    if (p->allowed && !page_is_app(w)) p->allowed = 0;
    if (p->allowed && p->i < p->n) { webbus_ask(w, p->kinds[p->i], asked, p); return; }
    if (p->allowed) webkit_permission_request_allow(p->req);
    else webkit_permission_request_deny(p->req);
    g_object_unref(p->req);
    g_free(p);
}

static gboolean on_permission(WebKitWebView *v, WebKitPermissionRequest *req, gpointer d) {
    (void)v;
    Web *w = d;
    Pending *p = g_new0(Pending, 1);
    p->allowed = 1;
    if (!page_is_app(w)) {
        /* another site in the window (a sign-in, a redirect): the app's permissions are not its own, and nobody is asked */
        fprintf(stderr, "lord-web: a permission request from outside %s: denied\n", w->app.scope);
        webkit_permission_request_deny(req);
        g_free(p);
        return TRUE;
    }
    if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(req)) p->kinds[p->n++] = WEBREG_PERM_NOTIFICATIONS;
    else if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(req)) p->kinds[p->n++] = WEBREG_PERM_LOCATION;
    else if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(req)) {
        WebKitUserMediaPermissionRequest *m = WEBKIT_USER_MEDIA_PERMISSION_REQUEST(req);
        if (webkit_user_media_permission_is_for_display_device(m)) p->kinds[p->n++] = WEBREG_PERM_SCREEN;
        else {
            if (webkit_user_media_permission_is_for_video_device(m)) p->kinds[p->n++] = WEBREG_PERM_CAMERA;
            if (webkit_user_media_permission_is_for_audio_device(m)) p->kinds[p->n++] = WEBREG_PERM_MICROPHONE;
        }
    } else if (WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST(req)) {
        /* the names of cameras and microphones: only when one of them is already allowed, never a dialog of its own */
        p->allowed = w->app.perms[WEBREG_PERM_CAMERA] == WEBREG_ALLOW || w->app.perms[WEBREG_PERM_MICROPHONE] == WEBREG_ALLOW;
    } else p->allowed = 0;      /* storage access, media keys, plugins: no */
    if (!p->n) p->allowed = p->allowed && WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST(req);
    p->req = g_object_ref(req);
    ask_next(w, p);
    return TRUE;
}

static int query_kind(const char *name) {
    if (!name) return -1;
    if (!strcmp(name, "notifications")) return WEBREG_PERM_NOTIFICATIONS;
    if (!strcmp(name, "camera")) return WEBREG_PERM_CAMERA;
    if (!strcmp(name, "microphone")) return WEBREG_PERM_MICROPHONE;
    if (!strcmp(name, "geolocation")) return WEBREG_PERM_LOCATION;
    if (!strcmp(name, "display-capture")) return WEBREG_PERM_SCREEN;
    return -1;
}

static gboolean on_query(WebKitWebView *v, WebKitPermissionStateQuery *q, gpointer d) {
    (void)v;
    Web *w = d;
    int k = query_kind(webkit_permission_state_query_get_name(q));
    WebKitSecurityOrigin *so = webkit_permission_state_query_get_security_origin(q);
    gchar *asker = so ? webkit_security_origin_to_string(so) : NULL;
    /* the origin that asks must be the app's, and the window inside its scope: else "denied", whatever the registry says */
    int mine = asker && !w->outside && weburl_perm_allowed(webkit_web_view_get_uri(w->view), asker, w->app.scope, w->app.origin);
    g_free(asker);
    int p = k >= 0 && mine ? w->app.perms[k] : WEBREG_DENY;
    webkit_permission_state_query_finish(q, p == WEBREG_ALLOW ? WEBKIT_PERMISSION_STATE_GRANTED : p == WEBREG_ASK ? WEBKIT_PERMISSION_STATE_PROMPT : WEBKIT_PERMISSION_STATE_DENIED);
    return TRUE;
}

/* ---- notifications, the file chooser, the pointer, crashes ---- */

static gboolean on_notification(WebKitWebView *v, WebKitNotification *n, gpointer d) { (void)v; webbus_notify(d, n); return TRUE; }
static gboolean on_file_chooser(WebKitWebView *v, WebKitFileChooserRequest *r, gpointer d) { (void)v; webbus_choose_files(d, r); return TRUE; }
static gboolean on_context_menu(WebKitWebView *v, WebKitContextMenu *m, WebKitHitTestResult *h, gpointer d) { (void)v; (void)m; (void)h; (void)d; return TRUE; }

static void on_target(WebKitWebView *v, WebKitHitTestResult *h, guint mods, gpointer d) {
    (void)v; (void)mods;
    Web *w = d;
    if (w->win) webwin_set_cursor_hint(w->win, webkit_hit_test_result_context_is_link(h), webkit_hit_test_result_context_is_editable(h));
}

static void on_close(WebKitWebView *v, gpointer d) { (void)v; web_close_requested(d); }

static void on_crash(WebKitWebView *v, WebKitWebProcessTerminationReason why, gpointer d) {
    Web *w = d;
    static gint64 last;
    gint64 now = g_get_monotonic_time();
    fprintf(stderr, "lord-web: the web process ended (%s)\n", why == WEBKIT_WEB_PROCESS_CRASHED ? "crashed" : why == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "memory limit" : "terminated");
    if (why == WEBKIT_WEB_PROCESS_TERMINATED_BY_API || (last && now - last < CRASH_WINDOW_US)) { web_quit(w, 1); return; }
    last = now;
    webkit_web_view_reload(v);
}

/* ---- downloads ---- */

static int downloads_dir(Web *w, char *out, size_t n) {
    const char *d = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    size_t hl = strlen(w->home);
    /* only a folder inside the home folder, and never the home folder itself */
    int k = !d || strncmp(d, w->home, hl) || d[hl] != '/' || !d[hl + 1] || strstr(d, "/../") ? snprintf(out, n, "%s/Downloads", w->home) : snprintf(out, n, "%s", d);
    if (k < 0 || (size_t)k >= n) return -1;
    return mkdir(out, 0755) && errno != EEXIST ? -1 : 0;
}

static gboolean on_destination(WebKitDownload *dl, const char *suggested, gpointer d) {
    Web *w = d;
    char dir[PATH_MAX], name[WEBDL_NAME_MAX + 16], uniq[WEBDL_NAME_MAX + 32], path[PATH_MAX + WEBDL_NAME_MAX + 40];
    if (downloads_dir(w, dir, sizeof dir) || webdl_name(suggested, name, sizeof name)) { webkit_download_cancel(dl); return TRUE; }
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int bad = fd < 0 || webdl_unique(fd, name, uniq, sizeof uniq);
    if (fd >= 0) close(fd);
    if (bad) { webkit_download_cancel(dl); return TRUE; }
    snprintf(path, sizeof path, "%s/%s", dir, uniq);
    webkit_download_set_allow_overwrite(dl, FALSE);
    webkit_download_set_destination(dl, path);
    return TRUE;
}

static void on_download_done(WebKitDownload *dl, gpointer d) {
    Web *w = d;
    const char *dest = webkit_download_get_destination(dl);
    if (!dest || g_object_get_data(G_OBJECT(dl), "lord-web-failed")) return;
    const char *base = strrchr(dest, '/');
    webbus_notify_text(w, weblang("download.done"), base ? base + 1 : dest);
}

static void on_download_failed(WebKitDownload *dl, GError *e, gpointer d) {
    g_object_set_data(G_OBJECT(dl), "lord-web-failed", GINT_TO_POINTER(1));
    if (g_error_matches(e, WEBKIT_DOWNLOAD_ERROR, WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER)) return;
    const char *dest = webkit_download_get_destination(dl);
    const char *base = dest ? strrchr(dest, '/') : NULL;
    webbus_notify_text(d, weblang("download.failed"), base ? base + 1 : "");
}

static void on_download(WebKitNetworkSession *s, WebKitDownload *dl, gpointer d) {
    (void)s;
    g_signal_connect(dl, "decide-destination", G_CALLBACK(on_destination), d);
    g_signal_connect(dl, "failed", G_CALLBACK(on_download_failed), d);
    g_signal_connect(dl, "finished", G_CALLBACK(on_download_done), d);
}

static WebKitWebView *on_create(WebKitWebView *v, WebKitNavigationAction *a, gpointer d) {
    Web *w = d;
    WebKitURIRequest *req = webkit_navigation_action_get_request(a);
    const char *uri = req ? webkit_uri_request_get_uri(req) : NULL;
    if (uri && weburl_in_scope(uri, w->app.scope)) webkit_web_view_load_request(v, req);
    else if (uri && webkit_navigation_action_is_user_gesture(a)) webbus_open_uri(w, uri);
    return NULL;
}

int web_engine_start(Web *w) {
    w->context = web_context_new();
    w->session = web_session_new(w);
    if (!w->session) return -1;
    WebKitSettings *settings = web_settings_new();
    WebKitWebViewBackend *backend = webkit_web_view_backend_new(wpe_view_backend_exportable_fdo_get_view_backend(w->exportable), NULL, NULL);
    w->view = g_object_new(WEBKIT_TYPE_WEB_VIEW, "backend", backend, "web-context", w->context, "network-session", w->session, "settings", settings, NULL);
    g_object_unref(settings);
    g_signal_connect(w->view, "decide-policy", G_CALLBACK(on_decide), w);
    g_signal_connect(w->view, "create", G_CALLBACK(on_create), w);
    g_signal_connect(w->view, "permission-request", G_CALLBACK(on_permission), w);
    g_signal_connect(w->view, "query-permission-state", G_CALLBACK(on_query), w);
    g_signal_connect(w->view, "show-notification", G_CALLBACK(on_notification), w);
    g_signal_connect(w->view, "run-file-chooser", G_CALLBACK(on_file_chooser), w);
    g_signal_connect(w->view, "context-menu", G_CALLBACK(on_context_menu), w);
    g_signal_connect(w->view, "mouse-target-changed", G_CALLBACK(on_target), w);
    g_signal_connect(w->view, "notify::title", G_CALLBACK(on_title), w);
    g_signal_connect(w->view, "notify::uri", G_CALLBACK(on_uri), w);
    g_signal_connect(w->view, "close", G_CALLBACK(on_close), w);
    g_signal_connect(w->view, "web-process-terminated", G_CALLBACK(on_crash), w);
    g_signal_connect(w->session, "download-started", G_CALLBACK(on_download), w);
    webkit_web_view_load_uri(w->view, w->app.start_url);
    return 0;
}
