/* webcalls.c — a web app's call log in lord-web (webcall.h; docs/WEBAPPS.md, "Calls").
 *
 *   - webcall.js at document start in the app's main frame, in the page's world, called with an event name random per
 *     process: it watches RTCPeerConnection and getUserMedia without changing them and dispatches that event with a
 *     JSON string;
 *   - a bridge in lord-web's own isolated world (lordos-web-call) listens for that event and hands the string to
 *     lord-web (the script message handler lordosCall, which exists only in that world: the page never sees a
 *     webkit.messageHandlers of lord-web's);
 *   - webcall_line checks every field against its closed list and writes the line, rate limited; while the window shows
 *     another site (a sign-in), nothing is written;
 *   - the permission answers for the camera, the microphone and the screen are written here too. */
#include "web.h"
#include "webcall.h"
#include "webcall_js.h"                 /* build/gen, from webcall.js (mk/web.mk) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#define CALL_WORLD "lordos-web-call"

static void on_call_msg(WebKitUserContentManager *m, JSCValue *v, gpointer d) {
    (void)m;
    Web *w = d;
    if (w->outside || !jsc_value_is_string(v)) return;
    char *s = jsc_value_to_string(v);
    char line[600];
    size_t n = s ? strlen(s) : 0;
    if (s && n <= WEBCALL_MSG_MAX && webcall_line(s, n, line, sizeof line)) webcall_log(line);
    g_free(s);
}

int webcalls_add_scripts(WebKitUserContentManager *ucm, GCallback on_message, gpointer data) {
    guint8 r[16];
    if (!ucm || getrandom(r, sizeof r, 0) != (ssize_t)sizeof r) return -1;
    char ev[48] = "lordos";
    for (size_t i = 0; i < sizeof r; i++) snprintf(ev + 6 + 2 * i, 3, "%02x", r[i]);
    if (!webkit_user_content_manager_register_script_message_handler(ucm, "lordosCall", CALL_WORLD)) return -1;
    g_signal_connect(ucm, "script-message-received::lordosCall", on_message, data);
    /* the bridge: only a string, only of the size webcall_line reads */
    char *bridge = g_strdup_printf("(function () { var h = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.lordosCall;"
                                   " if (!h || window.top !== window) return;"
                                   " document.addEventListener('%s', function (e) { var s = e.detail;"
                                   " if (typeof s === 'string' && s.length <= %d) h.postMessage(s); }, true); })();", ev, WEBCALL_MSG_MAX);
    WebKitUserScript *b = webkit_user_script_new_for_world(bridge, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                                                           CALL_WORLD, NULL, NULL);
    g_free(bridge);
    webkit_user_content_manager_add_script(ucm, b);
    webkit_user_script_unref(b);
    /* the page's side after the bridge, so the first report (what the page has) finds it listening */
    char *js = g_strdup_printf("%s(\"%s\");\n", WEBCALL_JS, ev);
    WebKitUserScript *p = webkit_user_script_new(js, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    g_free(js);
    webkit_user_content_manager_add_script(ucm, p);
    webkit_user_script_unref(p);
    return 0;
}

/* what WebKit itself says it captures (camera, microphone, screen): active, muted or none, each change */
static void on_capture(WebKitWebView *v, GParamSpec *ps, gpointer d) {
    (void)d;
    WebKitMediaCaptureState st;
    const char *what = ps->name;
    if (!strcmp(what, "camera-capture-state")) { st = webkit_web_view_get_camera_capture_state(v); what = "camera"; }
    else if (!strcmp(what, "microphone-capture-state")) { st = webkit_web_view_get_microphone_capture_state(v); what = "microphone"; }
    else if (!strcmp(what, "display-capture-state")) { st = webkit_web_view_get_display_capture_state(v); what = "screen"; }
    else return;
    char line[64];
    snprintf(line, sizeof line, "capture: %s %s", what, st == WEBKIT_MEDIA_CAPTURE_STATE_ACTIVE ? "on" : st == WEBKIT_MEDIA_CAPTURE_STATE_MUTED ? "muted" : "off");
    webcall_log(line);
}

void webcalls_init(Web *w) {
    webcalls_add_scripts(webkit_web_view_get_user_content_manager(w->view), G_CALLBACK(on_call_msg), w);
    g_signal_connect(w->view, "notify::camera-capture-state", G_CALLBACK(on_capture), w);
    g_signal_connect(w->view, "notify::microphone-capture-state", G_CALLBACK(on_capture), w);
    g_signal_connect(w->view, "notify::display-capture-state", G_CALLBACK(on_capture), w);
}

void webcalls_permission(Web *w, const int *kinds, int n, int allowed, const char *how) {
    (void)w;
    char what[64] = "";
    for (int i = 0; i < n; i++) {
        const char *k = kinds[i] == WEBREG_PERM_CAMERA ? "camera" : kinds[i] == WEBREG_PERM_MICROPHONE ? "microphone"
                      : kinds[i] == WEBREG_PERM_SCREEN ? "screen" : NULL;
        if (!k) return;                 /* notifications and location aren't about calls */
        snprintf(what + strlen(what), sizeof what - strlen(what), "%s%s", i ? "+" : "", k);
    }
    if (!what[0]) return;
    char line[160];
    snprintf(line, sizeof line, "permission %s: %s (%s)", what, allowed ? "allowed" : "denied", how);
    webcall_log(line);
}
