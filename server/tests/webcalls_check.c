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
    if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(req) || WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST(req)) webkit_permission_request_allow(req);
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
    WebKitNetworkSession *session = webkit_network_session_new_ephemeral();
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
    printf("engine: WPE WebKit %u.%u.%u\n", webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());
    webkit_web_view_load_uri(v, argv[1]);
    if (getenv("WEBCALLS_EVAL_AFTER")) g_timeout_add_seconds(8, eval_later, v);
    loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add_seconds(argc > 2 ? (guint)atoi(argv[2]) : 30, on_timeout, NULL);
    g_main_loop_run(loop);
    return ok ? 0 : 1;
}
