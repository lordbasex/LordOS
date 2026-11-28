/* webcalls_check.c — lord-web's Chrome identity and call log pieces on the real engine, headless (WPEPlatform), driven
 * by tests/webcalls_check.py. Run from server/:
 *
 *   build/webcalls-check <url> [seconds]
 *
 * It loads url in a WebKit view set up as lord-web sets up a profile whose Browser identity is Chrome: Chrome's
 * User-Agent string (webpage_user_agent), lord-web-ext.so from build/lord-web-ext/ with the same data lord-web gives it
 * (webhints_variant), each main-frame response's Accept-CH remembered and sent to it (as webhints_response does), the
 * navigator.userAgentData script (webhints_script_new) and the call log's scripts and bridge (webcalls_add_scripts),
 * whose lines it prints as lord-web writes them ("webcall: ..."). WebRTC and getUserMedia on, a camera or microphone
 * request allowed (the pages use no real device: a canvas and WebAudio; WEBCALLS_MOCK_CAPTURE=1 gives WebKit's mock camera
 * and microphone instead, for tests/webcalls_jitsi.py; tests/webcam_check.py opens a real camera). The cameras' udev
 * entries in WebKit's sandbox as lord-web adds them (webcalls_sandbox_cameras). With a throwaway HOME and WebKit's sandbox on.
 * WEBCALLS_SWNOTIFY=1 (tests/webcalls_check.py --swnotify): a service worker's notifications. Notifications allowed for
 * the url's origin, a profile on disk (lord-web's are), each show-notification printed ("show-notification: ...") and
 * taken as lord-web takes it, the one titled "LordOS second" clicked a second later (webkit_notification_clicked, as
 * lord-web on a click), a closed one printed, the view's lordos-focus-from-service-worker printed; and a notification
 * server of its own on the session bus (org.freedesktop.Notifications, in a thread: WebKit's D-Bus path calls it
 * synchronously) that prints each Notify ("dbus Notify: ...") and clicks it a second later (ActionInvoked "default").
 * It ends when the page's title becomes "done" (exit 0, the title printed) or after seconds (exit 1). */
#include "../src/web/web.h"
#include "../src/web/webcall.h"
#include "lord_version.h"

#include <wpe/headless/wpe-headless.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

/* webhints.c's web_state_save: nothing to save here */
void web_state_save(Web *w) { (void)w; }

static WebKitWebContext *ctx;
static WebUaSites sites;
static char machine[65];
static GMainLoop *loop;
static int ok;

static void on_msg(WebKitUserContentManager *m, JSCValue *v, gpointer d) {
    (void)m; (void)d;
    if (!jsc_value_is_string(v)) return;
    char *s = jsc_value_to_string(v), line[600];
    if (s && webcall_line(s, strlen(s), line, sizeof line)) { printf("webcall: %s\n", line); fflush(stdout); }
    g_free(s);
}

static void on_init_ext(WebKitWebContext *c, gpointer d) {
    (void)d;
    webkit_web_context_set_web_process_extensions_initialization_user_data(c, webhints_variant(1, machine, &sites));
}

static gboolean on_decide(WebKitWebView *v, WebKitPolicyDecision *dec, WebKitPolicyDecisionType type, gpointer d) {
    (void)v; (void)d;
    if (type != WEBKIT_POLICY_DECISION_TYPE_RESPONSE) return FALSE;
    WebKitResponsePolicyDecision *r = WEBKIT_RESPONSE_POLICY_DECISION(dec);
    if (!webkit_response_policy_decision_is_main_frame_main_resource(r)) return FALSE;
    WebKitURIResponse *resp = webkit_response_policy_decision_get_response(r);
    char origin[256];
    SoupMessageHeaders *h = webkit_uri_response_get_http_headers(resp);
    webcall_set_dip(h ? soup_message_headers_get_one(h, "Document-Isolation-Policy") : NULL);     /* as webengine.c does */
    const char *ach = h ? soup_message_headers_get_list(h, "Accept-CH") : NULL;
    if (ach && !webua_origin(webkit_uri_response_get_uri(resp), origin, sizeof origin) && webua_sites_set(&sites, origin, webua_accept_ch(ach))) {
        char line[700];
        if (webcall_hints_line(webua_sites_get(&sites, origin), line, sizeof line)) printf("webcall: %s\n", line);
        webkit_web_context_send_message_to_all_extensions(ctx, webkit_user_message_new("lordos-ua", webhints_variant(1, machine, &sites)));
    }
    return FALSE;
}

static gboolean on_permission(WebKitWebView *v, WebKitPermissionRequest *req, gpointer d) {
    (void)v; (void)d;
    if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(req) || WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST(req) ||
        (getenv("WEBCALLS_SWNOTIFY") && WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(req))) webkit_permission_request_allow(req);
    else webkit_permission_request_deny(req);
    return TRUE;
}

static void on_title(WebKitWebView *v, GParamSpec *p, gpointer d) {
    (void)p; (void)d;
    const char *t = webkit_web_view_get_title(v);
    if (t && !strncmp(t, "done", 4)) { printf("title: %s\n", t); ok = 1; g_main_loop_quit(loop); }
}

/* WEBCALLS_EVAL_AFTER (tests/webcalls_jitsi.py, test only): a script run in the page 8 s after it starts loading */
static gboolean eval_later(gpointer d) {
    webkit_web_view_evaluate_javascript(d, getenv("WEBCALLS_EVAL_AFTER"), -1, NULL, NULL, NULL, NULL, NULL);
    return G_SOURCE_REMOVE;
}

/* ---- WEBCALLS_SWNOTIFY: a service worker's notifications ---- */

static void put_line(const char *what, WebKitNotification *n) {
    printf("%s: title=%s body=%s tag=%s\n", what, webkit_notification_get_title(n), webkit_notification_get_body(n),
           webkit_notification_get_tag(n) ? webkit_notification_get_tag(n) : "");
    fflush(stdout);
}

static void on_note_closed(WebKitNotification *n, gpointer d) { (void)d; put_line("notification closed", n); }

static gboolean click_note(gpointer d) {
    put_line("notification clicked", d);
    webkit_notification_clicked(d);
    g_object_unref(d);
    return G_SOURCE_REMOVE;
}

static gboolean on_show_notification(WebKitWebView *v, WebKitNotification *n, gpointer d) {
    (void)v; (void)d;
    put_line("show-notification", n);
    g_signal_connect(g_object_ref(n), "closed", G_CALLBACK(on_note_closed), NULL);      /* kept, as lord-web keeps it */
    if (!g_strcmp0(webkit_notification_get_title(n), "LordOS second")) g_timeout_add(1000, click_note, g_object_ref(n));
    return TRUE;
}

static void on_sw_focus(WebKitWebView *v, gpointer d) { (void)v; (void)d; printf("focus-from-service-worker\n"); fflush(stdout); }

static void on_notify_permissions(WebKitWebContext *c, gpointer d) {
    GList *allowed = g_list_append(NULL, d);
    webkit_web_context_initialize_notification_permissions(c, allowed, NULL);
    g_list_free(allowed);
}

static const char notifyd_xml[] =
    "<node><interface name='org.freedesktop.Notifications'>"
    "<method name='GetCapabilities'><arg type='as' direction='out'/></method>"
    "<method name='Notify'><arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='as' direction='in'/><arg type='a{sv}' direction='in'/>"
    "<arg type='i' direction='in'/><arg type='u' direction='out'/></method>"
    "<method name='CloseNotification'><arg type='u' direction='in'/></method>"
    "<method name='GetServerInformation'><arg type='s' direction='out'/><arg type='s' direction='out'/><arg type='s' direction='out'/>"
    "<arg type='s' direction='out'/></method>"
    "<signal name='NotificationClosed'><arg type='u'/><arg type='u'/></signal>"
    "<signal name='ActionInvoked'><arg type='u'/><arg type='s'/></signal>"
    "</interface></node>";
static GDBusConnection *notifyd_bus;
static GMainContext *notifyd_ctx;
static gint notifyd_ready;

static gboolean notifyd_click(gpointer d) {
    guint32 id = GPOINTER_TO_UINT(d);
    printf("dbus ActionInvoked: %u default\n", id);
    fflush(stdout);
    g_dbus_connection_emit_signal(notifyd_bus, NULL, "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "ActionInvoked",
                                  g_variant_new("(us)", id, "default"), NULL);
    return G_SOURCE_REMOVE;
}

static void notifyd_call(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *method,
                         GVariant *p, GDBusMethodInvocation *inv, gpointer d) {
    (void)c; (void)sender; (void)path; (void)iface; (void)d;
    static guint32 next = 1;
    if (!strcmp(method, "GetCapabilities")) {
        const char *caps[] = { "body", "actions", NULL };
        g_dbus_method_invocation_return_value(inv, g_variant_new("(^as)", caps));
    } else if (!strcmp(method, "Notify")) {
        const char *app, *icon, *title, *body;
        guint32 replaces;
        g_variant_get(p, "(&su&s&s&s@as@a{sv}i)", &app, &replaces, &icon, &title, &body, NULL, NULL, NULL);
        guint32 id = next++;
        printf("dbus Notify: app=%s title=%s body=%s id=%u\n", app, title, body, id);
        fflush(stdout);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", id));
        GSource *s = g_timeout_source_new(1000);
        g_source_set_callback(s, notifyd_click, GUINT_TO_POINTER(id), NULL);
        g_source_attach(s, notifyd_ctx);
        g_source_unref(s);
    } else if (!strcmp(method, "GetServerInformation")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(ssss)", "webcalls-check", "LordOS", "1", "1.2"));
    } else g_dbus_method_invocation_return_value(inv, NULL);
}

/* the server in a thread of its own: WebKit asks for its capabilities synchronously from the main thread */
static gpointer notifyd_thread(gpointer d) {
    (void)d;
    notifyd_ctx = g_main_context_new();
    g_main_context_push_thread_default(notifyd_ctx);
    GError *e = NULL;
    char *addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, NULL, &e);
    if (addr) notifyd_bus = g_dbus_connection_new_for_address_sync(addr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                                    G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, &e);
    g_free(addr);
    if (!notifyd_bus) { fprintf(stderr, "notifyd: no session bus (%s)\n", e ? e->message : "?"); g_atomic_int_set(&notifyd_ready, -1); return NULL; }
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(notifyd_xml, NULL);
    static const GDBusInterfaceVTable vt = { notifyd_call, NULL, NULL, { 0 } };
    g_dbus_connection_register_object(notifyd_bus, "/org/freedesktop/Notifications", node->interfaces[0], &vt, NULL, NULL, NULL);
    GVariant *r = g_dbus_connection_call_sync(notifyd_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                                              g_variant_new("(su)", "org.freedesktop.Notifications", 4u), G_VARIANT_TYPE("(u)"),
                                              G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    guint32 got = 0;
    if (r) { g_variant_get(r, "(u)", &got); g_variant_unref(r); }
    g_atomic_int_set(&notifyd_ready, got == 1 ? 1 : -1);
    GMainLoop *l = g_main_loop_new(notifyd_ctx, FALSE);
    g_main_loop_run(l);
    return NULL;
}

static gboolean on_timeout(gpointer d) { (void)d; printf("timed out\n"); g_main_loop_quit(loop); return G_SOURCE_REMOVE; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: webcalls-check <url> [seconds]\n"); return 2; }
    if (getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS")) { fprintf(stderr, "WebKit's sandbox stays on\n"); return 2; }
    struct utsname u;
    if (!uname(&u)) snprintf(machine, sizeof machine, "%s", u.machine);
    char dir[PATH_MAX];
    if (webhints_ext_dir(dir, sizeof dir)) { fprintf(stderr, "no lord-web-ext/lord-web-ext.so next to this program\n"); return 2; }
    ctx = webkit_web_context_new();
    printf("cameras: %d in the web process's sandbox\n", webcalls_sandbox_cameras(ctx));   /* as lord-web, before any web process */
    webkit_web_context_set_web_process_extensions_directory(ctx, dir);
    g_signal_connect(ctx, "initialize-web-process-extensions", G_CALLBACK(on_init_ext), NULL);
    int swnotify = getenv("WEBCALLS_SWNOTIFY") && !strcmp(getenv("WEBCALLS_SWNOTIFY"), "1");
    WebKitNetworkSession *session;
    if (swnotify) {
        /* a profile on disk, as lord-web's, for the service worker; the notification server up before the page */
        char *data = g_build_filename(g_get_user_data_dir(), "webcalls-check", NULL), *cache = g_build_filename(g_get_user_cache_dir(), "webcalls-check", NULL);
        session = webkit_network_session_new(data, cache);
        g_free(data); g_free(cache);
        g_signal_connect(ctx, "initialize-notification-permissions", G_CALLBACK(on_notify_permissions), webkit_security_origin_new_for_uri(argv[1]));
        g_thread_unref(g_thread_new("notifyd", notifyd_thread, NULL));
        while (!g_atomic_int_get(&notifyd_ready)) g_usleep(10000);
        if (g_atomic_int_get(&notifyd_ready) < 0) { fprintf(stderr, "notifyd: could not own org.freedesktop.Notifications\n"); return 2; }
    } else session = webkit_network_session_new_ephemeral();
    char ua[256];
    webpage_user_agent(WEBREG_UA_CHROME, machine, LORD_VERSION, ua, sizeof ua);
    WebKitSettings *st = webkit_settings_new_with_settings("user-agent", ua, "enable-media-stream", TRUE, "enable-webrtc", TRUE,
                                                           "enable-mediasource", TRUE, "media-playback-requires-user-gesture", FALSE, NULL);
    /* WEBCALLS_MOCK_CAPTURE=1 (tests/webcalls_jitsi.py): WebKit's own mock camera and microphone, for a real call from a
     * machine without devices; never in lord-web */
    if (getenv("WEBCALLS_MOCK_CAPTURE") && !strcmp(getenv("WEBCALLS_MOCK_CAPTURE"), "1")) webkit_settings_set_enable_mock_capture_devices(st, TRUE);
    WPEDisplay *disp = wpe_display_headless_new();
    WebKitWebView *v = g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", disp, "web-context", ctx, "network-session", session, "settings", st, NULL);
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(v);
    WebKitUserScript *s = webhints_script_new(machine);
    if (s) { webkit_user_content_manager_add_script(ucm, s); webkit_user_script_unref(s); }
    webcalls_add_scripts(ucm, G_CALLBACK(on_msg), NULL);
    g_signal_connect(v, "decide-policy", G_CALLBACK(on_decide), NULL);
    g_signal_connect(v, "permission-request", G_CALLBACK(on_permission), NULL);
    g_signal_connect(v, "notify::title", G_CALLBACK(on_title), NULL);
    if (swnotify) {
        g_signal_connect(v, "show-notification", G_CALLBACK(on_show_notification), NULL);
        /* LordOS's WebKit since +4.2 */
        if (g_signal_lookup("lordos-focus-from-service-worker", WEBKIT_TYPE_WEB_VIEW)) g_signal_connect(v, "lordos-focus-from-service-worker", G_CALLBACK(on_sw_focus), NULL);
    }
    g_set_application_name("webcalls-check");       /* as lord-web names its app (WebKit sends it on D-Bus) */
    printf("engine: WPE WebKit %u.%u.%u\n", webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());
    webkit_web_view_load_uri(v, argv[1]);
    if (getenv("WEBCALLS_EVAL_AFTER")) g_timeout_add_seconds(8, eval_later, v);
    loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add_seconds(argc > 2 ? (guint)atoi(argv[2]) : 30, on_timeout, NULL);
    g_main_loop_run(loop);
    return ok ? 0 : 1;
}
