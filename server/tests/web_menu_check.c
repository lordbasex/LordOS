/* web_menu_check.c — what lord-web's context menu takes from WebKit, on the real engine, headless (WPEPlatform only;
 * docs/WEBAPPS.md, "Spelling"). Run from server/:
 *
 *   build/web-menu-check
 *
 * With a throwaway HOME and WebKit's own sandbox, spell checking on with the dictionaries LordOS's model picks
 * (spellang.h, for es_AR and English, among those in /usr/share/hunspell):
 *   - a right click on a misspelled word in a text field the page left alone makes WebKit ask for its menu (the
 *     view's "context-menu"), with spelling suggestions, Ignore Spelling and Learn Spelling; each suggestion's title
 *     is a word websearch_snippet keeps (what lord-server shows);
 *   - lord-web answers TRUE (WebKit shows nothing), and a suggestion run later, as when the person picks it in
 *     LordOS's menu, still replaces the word;
 *   - the view still takes clicks afterwards (WPE 2.48 didn't, which is why lord-web asks only on WPEPlatform).
 * Prints "ok" and exits 0, or the first failure and 1; 77 when no dictionary is installed. */
#include "web/webback.h"
#include "web/websearch.h"
#include "spellang.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static WebBack *back;
static WebKitWebView *view;
static int frame_w, frame_h, frames;
static unsigned char *frame_px;         /* the last frame, BGRA rows of frame_w * 4 bytes */
static int failed;

static gboolean done_later(gpointer d) { (void)d; webback_frame_done(back); return G_SOURCE_REMOVE; }

static void on_frame(void *d, const WebFrame *f) {
    (void)d;
    if (f->kind == WEBFRAME_SHM) {
        free(frame_px);
        frame_px = malloc((size_t)f->w * 4 * (size_t)f->h);
        for (int y = 0; y < f->h; y++) memcpy(frame_px + (size_t)y * (size_t)f->w * 4, (const unsigned char *)f->pixels + (size_t)y * (size_t)f->stride, (size_t)f->w * 4);
        frame_w = f->w;
        frame_h = f->h;
        frames++;
    } else webback_release(back, f->key);
    g_idle_add(done_later, NULL);
}
static void on_cursor(void *d, const char *name) { (void)d; (void)name; }
static const WebBackSink sink = { on_frame, on_cursor };

static gboolean timed_out;
static gboolean on_timeout(gpointer d) { (void)d; timed_out = TRUE; return G_SOURCE_REMOVE; }

typedef int (*Cond)(const void *arg);
/* runs the main loop until cond holds, at most secs seconds */
static int wait_for(Cond cond, const void *arg, int secs) {
    timed_out = FALSE;
    guint t = g_timeout_add_seconds((guint)secs, on_timeout, NULL);
    while (!cond(arg) && !timed_out) g_main_context_iteration(NULL, TRUE);
    if (!timed_out) g_source_remove(t);
    return cond(arg);
}

static void check(int ok, const char *what) {
    if (ok) return;
    fprintf(stderr, "FAILED: %s\n", what);
    failed = 1;
}

static int load_done;
static void on_load(WebKitWebView *v, WebKitLoadEvent ev, gpointer d) { (void)v; (void)d; if (ev == WEBKIT_LOAD_FINISHED) load_done = 1; }
static int loaded(const void *arg) { (void)arg; return load_done; }
static int never(const void *arg) { (void)arg; return 0; }

static const char PAGE[] =
    "<!doctype html><html><head><title>ready</title></head><body style='margin:0'>"
    "<textarea id=t style='position:absolute;left:0;top:0;width:600px;height:100px;font:20px monospace' spellcheck=true>erorr hello</textarea>"
    "<script>window.__log = []; document.addEventListener('mousedown', e => { __log.push('down' + e.button); });"
    "document.addEventListener('contextmenu', () => __log.push('ctx'));</script></body></html>";

static uint32_t now_ms(void) { return (uint32_t)(g_get_monotonic_time() / 1000); }
static int menus, guesses, ignore, learn, words_ok = 1;
static WebKitContextMenuItem *first;

static gboolean on_menu(WebKitWebView *v, WebKitContextMenu *m, WebKitHitTestResult *h, gpointer d) {
    (void)v; (void)h; (void)d;
    menus++;
    for (GList *l = webkit_context_menu_get_items(m); l; l = l->next) {
        WebKitContextMenuItem *it = l->data;
        WebKitContextMenuAction a = webkit_context_menu_item_get_stock_action(it);
        if (a == WEBKIT_CONTEXT_MENU_ACTION_SPELLING_GUESS) {
            char w[WEBSEARCH_SNIPPET_MAX];
            if (!websearch_snippet(webkit_context_menu_item_get_title(it), WEBSEARCH_SNIPPET_CHARS, w, sizeof w)) words_ok = 0;
            if (!guesses++) first = g_object_ref(it);
        } else if (a == WEBKIT_CONTEXT_MENU_ACTION_IGNORE_SPELLING) ignore++;
        else if (a == WEBKIT_CONTEXT_MENU_ACTION_LEARN_SPELLING) learn++;
    }
    return TRUE;
}
static int asked(const void *arg) { (void)arg; return menus > 0; }

static char value[256];
static void on_js(GObject *src, GAsyncResult *res, gpointer d) {
    (void)d;
    JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(src), res, NULL);
    char *s = v && jsc_value_is_string(v) ? jsc_value_to_string(v) : NULL;
    snprintf(value, sizeof value, "%s", s ? s : "?");
    g_free(s);
    if (v) g_object_unref(v);
}
static int has_value(const void *arg) { (void)arg; return value[0] != 0; }
static const char *eval(const char *js) {
    value[0] = 0;
    webkit_web_view_evaluate_javascript(view, js, -1, NULL, NULL, NULL, on_js, NULL);
    wait_for(has_value, NULL, 5);
    return value;
}

int main(void) {
    char inst[64][SPELLANG_TAG_MAX], pick[SPELLANG_MAX][SPELLANG_TAG_MAX];
    const char *list[64];
    int n = spellang_installed(SPELLANG_DIR, inst, 64);
    for (int i = 0; i < n; i++) list[i] = inst[i];
    int k = spellang_choose("es_AR.UTF-8", list, n, pick, SPELLANG_MAX);
    if (!k) { printf("skipped: needs a hunspell dictionary for Spanish or English (hunspell-es, hunspell-en-us)\n"); return 77; }
    char home[] = "/tmp/lord-web-menu-check-XXXXXX";
    if (!mkdtemp(home)) { perror("mkdtemp"); return 1; }
    setenv("HOME", home, 1);
    char sub[sizeof home + 32];
    snprintf(sub, sizeof sub, "%s/config", home);
    setenv("XDG_CONFIG_HOME", sub, 1);
    snprintf(sub, sizeof sub, "%s/cache", home);
    setenv("XDG_CACHE_HOME", sub, 1);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    webback_process_init();
    if (strcmp(webback_name(), "platform")) { printf("skipped: WPEPlatform only\n"); return 77; }
    WebBackConfig cfg = { .width = 640, .height = 480, .scale = 1 };
    back = webback_new(&cfg, &sink, NULL);
    if (!back) { fprintf(stderr, "FAILED: the backend did not start\n"); return 1; }
    WebKitWebContext *ctx = webkit_web_context_new();
    const char *langs[SPELLANG_MAX + 1] = { 0 };
    for (int i = 0; i < k; i++) langs[i] = pick[i];
    webkit_web_context_set_spell_checking_languages(ctx, langs);
    webkit_web_context_set_spell_checking_enabled(ctx, TRUE);
    WebKitNetworkSession *session = webkit_network_session_new_ephemeral();
    WebKitSettings *settings = webkit_settings_new();
    view = webback_view_new(back, ctx, session, settings);
    g_object_unref(settings);
    g_signal_connect(view, "load-changed", G_CALLBACK(on_load), NULL);
    g_signal_connect(view, "context-menu", G_CALLBACK(on_menu), NULL);
    webback_set_visible(back, 1);
    webback_set_focused(back, 1);
    webkit_web_view_load_html(view, PAGE, "http://127.0.0.1/");
    check(wait_for(loaded, NULL, 30), "the page loads");
    eval("(() => { const t = document.getElementById('t'); t.focus(); t.setSelectionRange(2, 2); return 'focused'; })()");
    wait_for(never, NULL, 1);               /* WebKit checks the field's words */
    webback_motion(back, now_ms(), 20, 12, 0);
    webback_button(back, now_ms(), 20, 12, WEBBTN_RIGHT, 1, WEBMOD_BTN_RIGHT);
    webback_button(back, now_ms(), 20, 12, WEBBTN_RIGHT, 0, 0);
    check(wait_for(asked, NULL, 10), "WebKit asks for its menu on a right click in the field");
    check(guesses > 0 && words_ok, "with spelling suggestions, each a word the menu may show");
    check(ignore == 1 && learn == 1, "and Ignore Spelling and Learn Spelling");
    check(!strcmp(eval("__log.join(',')"), "down2,ctx"), "the page saw the right click and its contextmenu event first");
    wait_for(never, NULL, 1);               /* the person reads LordOS's menu */
    GAction *a = first ? webkit_context_menu_item_get_gaction(first) : NULL;
    if (a) g_action_activate(a, webkit_context_menu_item_get_gaction_target(first));
    wait_for(never, NULL, 1);
    const char *v = eval("document.getElementById('t').value");
    check(strcmp(v, "erorr hello") && strstr(v, " hello") != NULL, "a suggestion run later replaces the word");
    webback_motion(back, now_ms(), 300, 300, 0);
    webback_button(back, now_ms(), 300, 300, WEBBTN_LEFT, 1, WEBMOD_BTN_LEFT);
    webback_button(back, now_ms(), 300, 300, WEBBTN_LEFT, 0, 0);
    wait_for(never, NULL, 1);
    check(!strcmp(eval("__log.join(',')"), "down2,ctx,down0"), "the view still takes clicks");
    if (first) g_object_unref(first);
    if (!failed) printf("ok (%s%s%s, %d suggestions)\n", langs[0], k > 1 ? ", " : "", k > 1 ? langs[1] : "", guesses);
    return failed;
}
