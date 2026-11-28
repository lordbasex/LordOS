/* webhang_check.c — lord-web's watch over its page (src/web/webhang.c) on the real engine, headless (WPEPlatform), with
 * short limits. Run from server/ (tests/run_suites.sh runs all three):
 *
 *   build/webhang-check fine      a page that keeps answering: its web process is never ended (8 s)
 *   build/webhang-check hang      a page whose script never returns: the periodic ping finds it, the web process is
 *                                 ended, web-process-terminated says it was webhang.c's (web_hang_take), and a new
 *                                 page loads and answers
 *   build/webhang-check urgent    the same with a long period: only web_hang_urgent (the window shown) can find it
 *
 * With a throwaway HOME and WebKit's sandbox on. Prints one line per step; exit 0 when the mode's checks pass. */
#include "../src/web/web.h"

#include <wpe/headless/wpe-headless.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GMainLoop *loop;
static Web web;
static int ok, terminated, ours, mode_urgent, hung_seen;
static gint64 hung_at;

static const char HANG_PAGE[] = "<!doctype html><title>start</title><script>"
    "setTimeout(() => { document.title = 'hanging'; setTimeout(() => { for (;;) {} }, 50); }, 300);</script>";
static const char FINE_PAGE[] = "<!doctype html><title>fine</title><script>"
    "let n = 0; setInterval(() => { n++; document.title = 'fine ' + n; }, 200);</script>";
static const char AFTER_PAGE[] = "<!doctype html><title>after</title><script>setTimeout(() => { document.title = 'done'; }, 100);</script>";

static void on_crash(WebKitWebView *v, WebKitWebProcessTerminationReason why, gpointer d) {
    (void)d;
    terminated++;
    ours = web_hang_take(&web);
    printf("web process ended: %s, %s\n", why == WEBKIT_WEB_PROCESS_TERMINATED_BY_API ? "by the API" : "otherwise", ours ? "webhang's" : "not webhang's");
    if (hung_at) printf("found %.1f s after the page hung\n", (double)(g_get_monotonic_time() - hung_at) / 1e6);
    fflush(stdout);
    /* lord-web reloads the app's page; here a page that answers, to see the new web process work */
    webkit_web_view_load_html(v, AFTER_PAGE, "https://lordos.invalid/");
}

static void on_title(WebKitWebView *v, GParamSpec *p, gpointer d) {
    (void)p; (void)d;
    const char *t = webkit_web_view_get_title(v);
    if (!t) return;
    if (!strcmp(t, "hanging") && !hung_seen) {
        hung_seen = 1;
        hung_at = g_get_monotonic_time();
        printf("the page hangs now\n");
        fflush(stdout);
        if (mode_urgent) web_hang_urgent(&web);   /* as web_show does when the window is shown */
    }
    if (!strcmp(t, "done")) {
        printf("the new web process answers\n");
        ok = terminated == 1 && ours;
        g_main_loop_quit(loop);
    }
}

static gboolean on_timeout(gpointer d) {
    (void)d;
    printf("timed out (%d ended)\n", terminated);
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static gboolean fine_done(gpointer d) {
    (void)d;
    printf("%s\n", terminated ? "FAIL: a page that answers had its web process ended" : "a page that answers: left alone");
    ok = !terminated;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    int fine = !strcmp(mode, "fine"), hang = !strcmp(mode, "hang");
    mode_urgent = !strcmp(mode, "urgent");
    if (!fine && !hang && !mode_urgent) { fprintf(stderr, "usage: webhang-check fine|hang|urgent\n"); return 2; }
    if (getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS")) { fprintf(stderr, "WebKit's sandbox stays on\n"); return 2; }
    /* a ping every second, 1.5 s to answer, two misses; urgent: one ping, 1.5 s; 1 s of grace. In urgent mode the
     * period is a minute: only the urgent ping can find the hang before the timeout */
    web_hang_set_limits((WebWatchLimits){ .period = mode_urgent ? 60000 : 1000, .wait = 1500, .urgent = 1500, .grace = 1000, .misses = 2 });
    WebKitWebContext *ctx = webkit_web_context_new();
    WebKitNetworkSession *session = webkit_network_session_new_ephemeral();
    WPEDisplay *disp = wpe_display_headless_new();
    web.view = g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", disp, "web-context", ctx, "network-session", session, NULL);
    g_signal_connect(web.view, "web-process-terminated", G_CALLBACK(on_crash), NULL);
    g_signal_connect(web.view, "notify::title", G_CALLBACK(on_title), NULL);
    printf("engine: WPE WebKit %u.%u.%u, mode %s\n", webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version(), mode);
    web_hang_start(&web);
    webkit_web_view_load_html(web.view, fine ? FINE_PAGE : HANG_PAGE, "https://lordos.invalid/");
    loop = g_main_loop_new(NULL, FALSE);
    if (fine) g_timeout_add_seconds(8, fine_done, NULL);
    else g_timeout_add_seconds(mode_urgent ? 12 : 20, on_timeout, NULL);
    g_main_loop_run(loop);
    if (!fine && ok && mode_urgent && hung_at) ok = 1;
    printf("webhang-check %s: %s\n", mode, ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
