/* webhang.c — lord-web keeps watch over its page (docs/WEBAPPS.md, "When a web app stops answering"; the rules are
 * webwatch.h's, WEBWATCH_PAGE).
 *
 * The ping: "0" evaluated in a script world of lord-web's own ("lordos-watch": the page's globals are never touched),
 * which the web process answers from its main thread. Every 15 s; at once, with a short wait, when the window is shown
 * (web_show: the launcher, the Dock, the tray) and when WebKit itself says the page stopped responding
 * (is-web-process-responsive). A page that doesn't answer has its web process ended (webkit_web_view_terminate_web_process);
 * web-process-terminated then comes with "terminated by the API", which web_hang_take says was ours: the app's page is
 * loaded again in a new web process and a notification says so. lord-web itself, its window, its tray icon and the
 * profile stay; a lord-web that hangs as a whole is lord-server's to restart (ui/webalive.c, org.lordos.WebApp1.Ping).
 *
 * Log lines, closed words only (each stamped with its time by lord-web's lord-log child, lordlog_stamp_start):
 * "lord-web: the page did not answer for N.N s: its web process restarted" (from the first ping it left unanswered),
 * "lord-web: WebKit says the page stopped responding", "lord-web: the page responds again after N.N s" (since WebKit
 * said so). */
#include "web.h"
#include "webwatch.h"

#include <stdio.h>

#define WATCH_WORLD "lordos-watch"

static WebWatch watch;
static guint timer;
static guint32 gen;
static int ours;                    /* the web process was ended here: the next termination is ours */
static int said_unresponsive;
static double unresponsive_at;      /* when WebKit said the page stopped responding (mono ms) */

static double mono_ms(void) { return (double)g_get_monotonic_time() / 1000.0; }

/* WEBWATCH_PAGE, or shorter ones from tests/webhang_check.c */
static WebWatchLimits limits;
static int limits_set;
static WebWatchLimits lim_now(void) { return limits_set ? limits : WEBWATCH_PAGE; }
void web_hang_set_limits(WebWatchLimits lim) { limits = lim; limits_set = 1; }

static void schedule(Web *w);

static void on_answer(GObject *o, GAsyncResult *res, gpointer d) {
    GError *e = NULL;
    JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(o), res, &e);
    if (v) g_object_unref(v);
    /* an error is an answer too (a document that is still loading, a navigation): the web process replied; a process
     * that died is web-process-terminated's */
    if (e) g_error_free(e);
    webwatch_answer(&watch, GPOINTER_TO_UINT(d), mono_ms());
}

static void restart_page(Web *w, double now) {
    /* silent since the first ping it left unanswered, or since WebKit said so if that came first */
    double quiet = watch.hung_for;
    if (said_unresponsive && unresponsive_at > 0 && now - unresponsive_at > quiet) quiet = now - unresponsive_at;
    fprintf(stderr, "lord-web: the page did not answer for %.1f s: its web process restarted\n", quiet / 1000.0);
    ours = 1;
    webkit_web_view_terminate_web_process(w->view);
}

static gboolean tick(gpointer d) {
    Web *w = d;
    timer = 0;
    if (w->quitting || !w->view) return G_SOURCE_REMOVE;
    double now = mono_ms();
    switch (webwatch_step(&watch, now)) {
    case WEBWATCH_PING:
        if (!++gen) gen = 1;
        webkit_web_view_evaluate_javascript(w->view, "0", 1, WATCH_WORLD, NULL, NULL, on_answer, GUINT_TO_POINTER(gen));
        webwatch_sent(&watch, gen, now);
        break;
    case WEBWATCH_HUNG:
        restart_page(w, now);
        break;
    default:
        break;
    }
    schedule(w);
    return G_SOURCE_REMOVE;
}

static void schedule(Web *w) {
    if (timer) g_source_remove(timer);
    double next = webwatch_next(&watch, mono_ms());
    /* at least 250 ms apart; a pass at most every 30 s keeps the stall check honest after a suspend */
    guint ms = next < 250 ? 250 : next > 30000 ? 30000 : (guint)next;
    timer = g_timeout_add(ms, tick, w);
}

static void on_responsive(WebKitWebView *v, GParamSpec *ps, gpointer d) {
    (void)ps;
    Web *w = d;
    if (!webkit_web_view_get_is_web_process_responsive(v)) {
        if (!said_unresponsive) {
            fprintf(stderr, "lord-web: WebKit says the page stopped responding\n");
            unresponsive_at = mono_ms();
        }
        said_unresponsive = 1;
        webwatch_urgent(&watch);
        schedule(w);
    } else if (said_unresponsive) {
        fprintf(stderr, "lord-web: the page responds again after %.1f s\n", (mono_ms() - unresponsive_at) / 1000.0);
        said_unresponsive = 0;
    }
}

void web_hang_start(Web *w) {
    webwatch_init(&watch, lim_now(), mono_ms());
    g_signal_connect(w->view, "notify::is-web-process-responsive", G_CALLBACK(on_responsive), w);
    schedule(w);
}

void web_hang_urgent(Web *w) {
    if (!w->view || w->quitting) return;
    webwatch_urgent(&watch);
    schedule(w);
}

int web_hang_take(Web *w) {
    int r = ours;
    ours = 0;
    said_unresponsive = 0;
    /* a new web process (ours or after a crash): its page gets the grace of a start again */
    webwatch_init(&watch, lim_now(), mono_ms());
    if (w->view && !w->quitting) schedule(w);
    return r;
}

const char *web_hang_word(void) {
    /* a page ping in flight for more than 3 s: the page is slow or stuck; lord-web itself answered */
    return watch.sent > 0 && mono_ms() - watch.sent > 3000 ? "page" : "ok";
}
