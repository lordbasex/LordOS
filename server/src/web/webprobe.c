/* webprobe.c — adding an app from an address (docs/WEBAPPS.md, "Adding an app from an address"). Headless, in a
 * throwaway profile (an ephemeral network session: nothing is written to disk), with WebKit's sandbox as always:
 *
 *   lord-web --probe        fd 3 gives one line, the address (webargs_probe_line). lord-web loads it without a
 *                           window, reads the page's web app manifest from a script in an isolated world (the page's
 *                           own scripts can't see or change it), and answers one JSON line on fd 3
 *                           (webmanifest_answer, checked against the address WebKit really loaded), then exits.
 *   lord-web --probe-icon   fd 3 gives one line, an icon address the first step listed. lord-web downloads it into a
 *                           temporary folder (removed after) and answers on fd 3 one JSON line,
 *                           {"ok":true,"type":"image/png"|"image/svg+xml","size":N}, then exactly N bytes; or
 *                           {"ok":false,"error":"<code>"}. At most WEBICON_MAX bytes, a PNG or an SVG only.
 *
 * Errors: web.probe.request (no valid line), web.probe.load, web.probe.tls, web.probe.notHttps (the page ended up on
 * an address that isn't https), web.probe.page (the script gave nothing usable), web.probe.timeout,
 * web.probe.iconTooBig, web.probe.iconType. lord-server gives each step 20 s and kills it after that; lord-web gives
 * up on its own a little before. */
#include "web.h"
#include "webmanifest.h"
#include "weburl.h"

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wpe/unstable/fdo-shm.h>

#define CHAN 3
#define PROBE_DEADLINE_S 18
#define SETTLE_MS 1200                  /* after the load: time for a page that adds its manifest link from a script */

static const char PROBE_JS[] =
    "const abs = (u, base) => { try { const x = new URL(u, base); return x.protocol === 'https:' || x.protocol === 'http:' ? x.href : null; } catch (e) { return null; } };\n"
    "const str = (v) => typeof v === 'string' ? v.slice(0, 512) : '';\n"
    "const icons = (list, base) => Array.isArray(list) ? list.slice(0, 32).filter(i => i && typeof i === 'object')\n"
    "  .map(i => ({ src: abs(str(i.src), base), sizes: str(i.sizes), type: str(i.type) })).filter(i => i.src) : [];\n"
    "const out = { title: str(document.title), metaThemeColor: null, manifest: null, linkIcons: [] };\n"
    "const meta = document.querySelector('meta[name=\"theme-color\"]');\n"
    "if (meta) out.metaThemeColor = str(meta.getAttribute('content'));\n"
    "for (const l of document.querySelectorAll('link[rel~=\"icon\"], link[rel=\"apple-touch-icon\"]'))\n"
    "  if (out.linkIcons.length < 32) out.linkIcons.push({ src: abs(l.href, document.baseURI), sizes: str(l.getAttribute('sizes')), type: str(l.getAttribute('type')) });\n"
    "out.linkIcons = out.linkIcons.filter(i => i.src);\n"
    "const link = document.querySelector('link[rel~=\"manifest\"]');\n"
    "if (link && link.href) {\n"
    "  try {\n"
    "    const r = await fetch(link.href, { credentials: link.crossOrigin === 'use-credentials' ? 'include' : 'same-origin' });\n"
    "    const text = r.ok ? await r.text() : '';\n"
    "    if (text && text.length <= 131072) {\n"
    "      const m = JSON.parse(text);\n"
    "      const base = r.url || link.href;\n"
    "      if (m && typeof m === 'object' && !Array.isArray(m)) out.manifest = {\n"
    "        name: str(m.name), short_name: str(m.short_name),\n"
    "        start_url: typeof m.start_url === 'string' ? abs(m.start_url, base) : null,\n"
    "        scope: typeof m.scope === 'string' ? abs(m.scope, base) : null,\n"
    "        theme_color: typeof m.theme_color === 'string' ? str(m.theme_color) : null,\n"
    "        icons: icons(m.icons, base) };\n"
    "    }\n"
    "  } catch (e) { }\n"
    "}\n"
    "return JSON.stringify(out);\n";

static int write_all(int fd, const void *p, size_t n) {
    const char *b = p;
    while (n) {
        ssize_t k = write(fd, b, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

static int answered;
/* the address asked for, and the last main-frame address allowed on the way (redirects and navigations) */
static char typed[WEBARGS_LINE_MAX + 1], current[WEBURL_MAX + 1];
static int typed_public = 1;            /* what the request was: until shown otherwise, public (the strict side) */

/* ---- where a name leads: resolved here and every address classified (weburl_addrs_class) ---- */

#define RESOLVE_MS 3000
#define RESOLVE_ADDRS 32

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int refs, done, ok, n;
    char host[WEBURL_HOST_MAX + 1];
    WebAddr a[RESOLVE_ADDRS];
} Lookup;

static void lookup_unref(Lookup *l) {
    pthread_mutex_lock(&l->mu);
    int last = --l->refs == 0;
    pthread_mutex_unlock(&l->mu);
    if (last) { pthread_mutex_destroy(&l->mu); pthread_cond_destroy(&l->cv); free(l); }
}

static void *lookup_thread(void *d) {
    Lookup *l = d;
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res = NULL;
    int ok = !getaddrinfo(l->host, NULL, &hints, &res), n = 0;
    for (struct addrinfo *r = ok ? res : NULL; r && n < RESOLVE_ADDRS; r = r->ai_next) {
        if (r->ai_family == AF_INET) { l->a[n].family = 4; memcpy(l->a[n].a, &((struct sockaddr_in *)r->ai_addr)->sin_addr, 4); n++; }
        else if (r->ai_family == AF_INET6) { l->a[n].family = 6; memcpy(l->a[n].a, &((struct sockaddr_in6 *)r->ai_addr)->sin6_addr, 16); n++; }
    }
    if (res) freeaddrinfo(res);
    pthread_mutex_lock(&l->mu);
    l->ok = ok;
    l->n = n;
    l->done = 1;
    pthread_cond_signal(&l->cv);
    pthread_mutex_unlock(&l->mu);
    lookup_unref(l);
    return NULL;
}

/* Where url's host leads: 0 and *cls, or -1 when it could not be resolved within RESOLVE_MS. An address is its own
 * class; a name the static rules call private is private without asking. */
static int resolve_class(const char *url, int *cls) {
    static struct { char host[WEBURL_HOST_MAX + 1]; int cls; } cache[16];
    static int next;
    WebUrl u;
    if (weburl_parse(url, &u)) return -1;
    *cls = weburl_host_class(&u);
    if (*cls == WEBURL_PRIVATE || weburl_host_is_literal(&u)) return 0;
    for (int i = 0; i < 16; i++) if (cache[i].host[0] && !strcmp(cache[i].host, u.host)) { *cls = cache[i].cls; return 0; }
    Lookup *l = calloc(1, sizeof *l);
    if (!l) return -1;
    pthread_mutex_init(&l->mu, NULL);
    pthread_cond_init(&l->cv, NULL);
    l->refs = 2;
    snprintf(l->host, sizeof l->host, "%s", u.host);
    size_t hn = strlen(l->host);
    if (hn && l->host[hn - 1] == '.') l->host[hn - 1] = 0;
    pthread_t th;
    if (pthread_create(&th, NULL, lookup_thread, l)) { l->refs = 1; lookup_unref(l); return -1; }
    pthread_detach(th);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += RESOLVE_MS / 1000;
    pthread_mutex_lock(&l->mu);
    while (!l->done) if (pthread_cond_timedwait(&l->cv, &l->mu, &until)) break;
    int done = l->done, ok = l->ok, n = l->n;
    WebAddr a[RESOLVE_ADDRS];
    memcpy(a, l->a, sizeof a);
    pthread_mutex_unlock(&l->mu);
    lookup_unref(l);                    /* a lookup still running frees itself when it ends */
    if (!done || !ok) return -1;
    *cls = weburl_addrs_class(a, n);
    snprintf(cache[next].host, sizeof cache[next].host, "%s", u.host);
    cache[next].cls = *cls;
    next = (next + 1) % 16;
    return 0;
}

/* a typed public address may not lead to a private one: to must resolve, and only to public addresses */
static int place_ok(const char *to) {
    if (!typed_public) return 1;
    int c;
    return !resolve_class(to, &c) && c == WEBURL_PUBLIC;
}

/* what the request itself is (failing to resolve it keeps it public: the strict side) */
static void typed_set(const char *url) {
    snprintf(typed, sizeof typed, "%s", url);
    int c;
    typed_public = resolve_class(url, &c) || c == WEBURL_PUBLIC;
}

static void answer_line(Web *w, const char *line) {
    if (answered) return;
    answered = 1;
    if (write_all(CHAN, line, strlen(line)) || write_all(CHAN, "\n", 1)) fprintf(stderr, "lord-web: the probe's answer was not read\n");
    web_quit(w, 0);
}

static void answer_error(Web *w, const char *code) {
    char line[128];
    webmanifest_error(code, line, sizeof line);
    answer_line(w, line);
}

/* the request: one line on fd 3, read within a few seconds */
static int read_request(char *url, size_t n) {
    char buf[WEBARGS_LINE_MAX + 2];
    size_t len = 0;
    gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (len < sizeof buf && (!len || buf[len - 1] != '\n')) {
        int left = (int)((until - g_get_monotonic_time()) / 1000);
        struct pollfd p = { CHAN, POLLIN, 0 };
        if (left <= 0 || poll(&p, 1, left) <= 0) return -1;
        ssize_t k = read(CHAN, buf + len, sizeof buf - len);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        len += (size_t)k;
    }
    return webargs_probe_line(buf, len, url, n);
}

static gboolean deadline(gpointer d) { answer_error(d, "web.probe.timeout"); return G_SOURCE_REMOVE; }

/* ---- --probe ---- */

static void on_js(GObject *src, GAsyncResult *res, gpointer d) {
    Web *w = d;
    GError *e = NULL;
    JSCValue *v = webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(src), res, &e);
    char *page = v && jsc_value_is_string(v) ? jsc_value_to_string(v) : NULL;
    if (e) g_clear_error(&e);
    if (v) g_object_unref(v);
    const char *uri = webkit_web_view_get_uri(w->view);
    /* https (or http on this computer, asked for as such), and this computer or a local network only when asked for */
    if (!uri || !weburl_probe_final_ok(typed, uri)) { g_free(page); answer_error(w, "web.probe.notHttps"); return; }
    if (!place_ok(uri)) { g_free(page); answer_error(w, "web.probe.load"); return; }
    char *line = g_malloc(WEBMANIFEST_LINE_MAX);
    if (page && !webmanifest_answer(uri, page, strlen(page), line, WEBMANIFEST_LINE_MAX)) answer_line(w, line);
    else answer_error(w, "web.probe.page");
    g_free(line);
    g_free(page);
}

static gboolean run_script(gpointer d) {
    Web *w = d;
    webkit_web_view_call_async_javascript_function(w->view, PROBE_JS, -1, NULL, "lordos-probe", NULL, NULL, on_js, w);
    return G_SOURCE_REMOVE;
}

static void on_load(WebKitWebView *v, WebKitLoadEvent ev, gpointer d) {
    (void)v;
    static int once;
    if (ev == WEBKIT_LOAD_FINISHED && !once) { once = 1; g_timeout_add(SETTLE_MS, run_script, d); }
}

static gboolean on_load_failed(WebKitWebView *v, WebKitLoadEvent ev, const char *uri, GError *e, gpointer d) {
    (void)v; (void)ev; (void)uri;
    if (g_error_matches(e, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED) || g_error_matches(e, WEBKIT_POLICY_ERROR, WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE))
        return FALSE;
    answer_error(d, "web.probe.load");
    return TRUE;
}

static gboolean on_tls(WebKitWebView *v, const char *uri, GTlsCertificate *c, GTlsCertificateFlags f, gpointer d) {
    (void)v; (void)uri; (void)c; (void)f;
    answer_error(d, "web.probe.tls");
    return TRUE;
}

static gboolean on_decide(WebKitWebView *v, WebKitPolicyDecision *dec, WebKitPolicyDecisionType type, gpointer d) {
    (void)v;
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) { webkit_policy_decision_ignore(dec); return TRUE; }
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        /* a redirect or a navigation of the page itself: never from https to http, never from a public host to this
         * computer or a local network (a public page must not make the probe read what only this computer can reach) */
        WebKitNavigationAction *a = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(dec));
        const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(a));
        if (!uri || !strncmp(uri, "about:", 6)) return FALSE;
        if (!weburl_probe_hop_ok(current, uri) || !weburl_probe_hop_ok(typed, uri) || !place_ok(uri)) {
            webkit_policy_decision_ignore(dec);
            WebUrl f, t;
            answer_error(d, !weburl_parse(current, &f) && !weburl_parse(uri, &t) && f.https && !t.https ? "web.probe.notHttps" : "web.probe.load");
            return TRUE;
        }
        snprintf(current, sizeof current, "%s", uri);
        return FALSE;
    }
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        WebKitResponsePolicyDecision *r = WEBKIT_RESPONSE_POLICY_DECISION(dec);
        if (!webkit_response_policy_decision_is_mime_type_supported(r)) { webkit_policy_decision_ignore(dec); return TRUE; }
    }
    return FALSE;
}

static gboolean deny_all(WebKitWebView *v, WebKitPermissionRequest *r, gpointer d) { (void)v; (void)d; webkit_permission_request_deny(r); return TRUE; }

/* no window: every frame is given back at once */
static gboolean complete(gpointer d) {
    Web *w = d;
    if (w->exportable) wpe_view_backend_exportable_fdo_dispatch_frame_complete(w->exportable);
    return G_SOURCE_REMOVE;
}
static void headless_shm(void *d, struct wpe_fdo_shm_exported_buffer *b) {
    Web *w = d;
    wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(w->exportable, b);
    g_idle_add(complete, w);
}
static void headless_res(void *d, struct wl_resource *r) {
    Web *w = d;
    wpe_view_backend_exportable_fdo_dispatch_release_buffer(w->exportable, r);
    g_idle_add(complete, w);
}
static void headless_dmabuf(void *d, struct wpe_view_backend_exportable_fdo_dmabuf_resource *r) { headless_res(d, r->buffer_resource); }
static const struct wpe_view_backend_exportable_fdo_client headless = { headless_res, headless_dmabuf, headless_shm, NULL, NULL };

int webprobe_run(Web *w) {
    char url[WEBARGS_LINE_MAX + 1];
    w->loop = g_main_loop_new(NULL, FALSE);
    if (read_request(url, sizeof url)) { answer_error(w, "web.probe.request"); return 0; }
    typed_set(url);
    snprintf(current, sizeof current, "%s", url);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    if (!wpe_fdo_initialize_shm()) { answer_error(w, "web.probe.load"); return 1; }
    w->exportable = wpe_view_backend_exportable_fdo_create(&headless, w, 1280, 800);
    w->context = web_context_new();
    w->session = webkit_network_session_new_ephemeral();
    WebKitSettings *settings = web_settings_new();
    webkit_settings_set_enable_media_stream(settings, FALSE);
    webkit_settings_set_enable_webrtc(settings, FALSE);
    webkit_settings_set_media_playback_requires_user_gesture(settings, TRUE);
    WebKitWebViewBackend *backend = webkit_web_view_backend_new(wpe_view_backend_exportable_fdo_get_view_backend(w->exportable), NULL, NULL);
    w->view = g_object_new(WEBKIT_TYPE_WEB_VIEW, "backend", backend, "web-context", w->context, "network-session", w->session, "settings", settings, NULL);
    g_object_unref(settings);
    wpe_view_backend_add_activity_state(wpe_view_backend_exportable_fdo_get_view_backend(w->exportable), wpe_view_activity_state_visible | wpe_view_activity_state_in_window);
    g_signal_connect(w->view, "load-changed", G_CALLBACK(on_load), w);
    g_signal_connect(w->view, "load-failed", G_CALLBACK(on_load_failed), w);
    g_signal_connect(w->view, "load-failed-with-tls-errors", G_CALLBACK(on_tls), w);
    g_signal_connect(w->view, "decide-policy", G_CALLBACK(on_decide), w);
    g_signal_connect(w->view, "permission-request", G_CALLBACK(deny_all), w);
    g_timeout_add_seconds(PROBE_DEADLINE_S, deadline, w);
    webkit_web_view_load_uri(w->view, url);
    g_main_loop_run(w->loop);
    return w->exit_code;
}

/* ---- --probe-icon ---- */

static char tmpdir[PATH_MAX];

static void tmp_clean(void) {
    if (!tmpdir[0]) return;
    char p[PATH_MAX + 16];
    snprintf(p, sizeof p, "%s/icon", tmpdir);
    unlink(p);
    snprintf(p, sizeof p, "%s/icon.wkdownload", tmpdir);
    unlink(p);
    rmdir(tmpdir);
    tmpdir[0] = 0;
}

static gboolean icon_destination(WebKitDownload *dl, const char *suggested, gpointer d) {
    (void)suggested; (void)d;
    char p[PATH_MAX + 16];
    snprintf(p, sizeof p, "%s/icon", tmpdir);
    webkit_download_set_allow_overwrite(dl, TRUE);
    webkit_download_set_destination(dl, p);
    return TRUE;
}

static void icon_data(WebKitDownload *dl, guint64 len, gpointer d) {
    (void)len; (void)d;
    if (webkit_download_get_received_data_length(dl) > WEBICON_MAX) {
        g_object_set_data(G_OBJECT(dl), "lord-web-big", GINT_TO_POINTER(1));
        webkit_download_cancel(dl);
    }
}

static void icon_failed(WebKitDownload *dl, GError *e, gpointer d) {
    (void)e;
    g_object_set_data(G_OBJECT(dl), "lord-web-failed", GINT_TO_POINTER(1));
    answer_error(d, g_object_get_data(G_OBJECT(dl), "lord-web-big") ? "web.probe.iconTooBig" : "web.probe.load");
}

static void icon_done(WebKitDownload *dl, gpointer d) {
    Web *w = d;
    if (g_object_get_data(G_OBJECT(dl), "lord-web-failed")) return;
    WebKitURIResponse *resp = webkit_download_get_response(dl);
    const char *final = resp ? webkit_uri_response_get_uri(resp) : NULL;
    if (!final || !weburl_probe_final_ok(typed, final)) { answer_error(w, "web.probe.notHttps"); return; }
    if (!place_ok(final)) { answer_error(w, "web.probe.load"); return; }
    char p[PATH_MAX + 16];
    snprintf(p, sizeof p, "%s/icon", tmpdir);
    int fd = open(p, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) { if (fd >= 0) close(fd); answer_error(w, "web.probe.load"); return; }
    if (st.st_size > WEBICON_MAX) { close(fd); answer_error(w, "web.probe.iconTooBig"); return; }
    unsigned char *buf = g_malloc((gsize)st.st_size + 1);
    size_t len = 0;
    for (;;) {
        ssize_t k = read(fd, buf + len, (size_t)st.st_size + 1 - len);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        len += (size_t)k;
        if (len > (size_t)st.st_size) break;
    }
    close(fd);
    int kind = len <= (size_t)st.st_size ? webicon_kind(buf, len) : WEBICON_NONE;
    if (kind == WEBICON_NONE) { g_free(buf); answer_error(w, "web.probe.iconType"); return; }
    char head[128];
    snprintf(head, sizeof head, "{\"ok\":true,\"type\":\"%s\",\"size\":%zu}\n", webicon_type(kind), len);
    answered = 1;
    if (write_all(CHAN, head, strlen(head)) || write_all(CHAN, buf, len)) fprintf(stderr, "lord-web: the icon was not read\n");
    g_free(buf);
    web_quit(w, 0);
}

int webprobe_icon_run(Web *w) {
    char url[WEBARGS_LINE_MAX + 1];
    w->loop = g_main_loop_new(NULL, FALSE);
    if (read_request(url, sizeof url)) { answer_error(w, "web.probe.request"); return 0; }
    typed_set(url);
    const char *rt = getenv("XDG_RUNTIME_DIR");
    snprintf(tmpdir, sizeof tmpdir, "%s/lordos-web-icon.XXXXXX", rt && rt[0] == '/' ? rt : "/tmp");
    if (!mkdtemp(tmpdir)) { tmpdir[0] = 0; answer_error(w, "web.probe.load"); return 1; }
    w->context = web_context_new();
    w->session = webkit_network_session_new_ephemeral();
    WebKitDownload *dl = webkit_network_session_download_uri(w->session, url);
    g_signal_connect(dl, "decide-destination", G_CALLBACK(icon_destination), w);
    g_signal_connect(dl, "received-data", G_CALLBACK(icon_data), w);
    g_signal_connect(dl, "failed", G_CALLBACK(icon_failed), w);
    g_signal_connect(dl, "finished", G_CALLBACK(icon_done), w);
    g_timeout_add_seconds(PROBE_DEADLINE_S, deadline, w);
    g_main_loop_run(w->loop);
    g_object_unref(dl);
    tmp_clean();
    return w->exit_code;
}
