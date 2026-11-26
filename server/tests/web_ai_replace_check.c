/* web_ai_replace_check.c — ✨ AI's Replace in a page (docs/AI.md "Writing"), on the real engine, headless (WPEPlatform
 * only). Run from server/:
 *
 *   build/web-ai-replace-check
 *
 * The scripts lord-web runs (webedit.h WEBAI_MARK_JS, WEBAI_REPLACE_JS), in the page script's world, on a page with a
 * textarea, a plain contenteditable box, a password field and a Lexical-like editor (WhatsApp's message box is
 * Lexical: it keeps its own model, takes typed and pasted text from beforeinput and draws the box again from the model,
 * so a change it didn't see is undone):
 *   - while the view doesn't have the keyboard (the shell's panel holds it), the page counts as unfocused
 *     (document.hasFocus() false): why the first Replace (which required it) did nothing in WhatsApp;
 *   - with the keyboard back, the text replaces exactly the selection in each kind of field, and the Lexical-like
 *     editor's own model has it;
 *   - it refuses with its closed words: a password field, another field focused, a selection that changed;
 *   - webai_refusal's words.
 * Prints "ok" and exits 0, or the first failure and 1; 77 when it can't run here. */
#include "web/webback.h"
#include "web/webedit.h"
#include "web/web.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static WebBack *back;
static WebKitWebView *view;
static int failed;

static gboolean done_later(gpointer d) { (void)d; webback_frame_done(back); return G_SOURCE_REMOVE; }
static void on_frame(void *d, const WebFrame *f) { (void)d; if (f->kind != WEBFRAME_SHM) webback_release(back, f->key); g_idle_add(done_later, NULL); }
static void on_cursor(void *d, const char *name) { (void)d; (void)name; }
static const WebBackSink sink = { on_frame, on_cursor };

static gboolean timed_out;
static gboolean on_timeout(gpointer d) { (void)d; timed_out = TRUE; return G_SOURCE_REMOVE; }
typedef int (*Cond)(const void *arg);
static int wait_for(Cond cond, const void *arg, int secs) {
    timed_out = FALSE;
    guint t = g_timeout_add_seconds((guint)secs, on_timeout, NULL);
    while (!cond(arg) && !timed_out) g_main_context_iteration(NULL, TRUE);
    if (!timed_out) g_source_remove(t);
    return cond(arg);
}
static int never(const void *arg) { (void)arg; return 0; }

static void check(int ok, const char *what) {
    if (ok) { printf("  ok: %s\n", what); return; }
    fprintf(stderr, "FAILED: %s\n", what);
    failed = 1;
}

static int load_done;
static void on_load(WebKitWebView *v, WebKitLoadEvent ev, gpointer d) { (void)v; (void)d; if (ev == WEBKIT_LOAD_FINISHED) load_done = 1; }
static int loaded(const void *arg) { (void)arg; return load_done; }

/* The Lexical-like editor: a model of paragraphs; beforeinput (insertText, insertReplacementText, insertFromPaste,
 * deleteContentBackward) is prevented and applied to the model, the box drawn again from it in a microtask with the
 * caret after the change; any other change to the box's text is undone at the next draw (a MutationObserver), as
 * Lexical's reconciler does. */
static const char PAGE[] =
    "<!doctype html><html><head><title>ready</title></head><body>"
    "<textarea id=ta>hola como esas? todo bien</textarea>"
    "<div id=ce contenteditable=true>tu muñeca no funciona como tendria</div>"
    "<input id=pw type=password value='secreto123'>"
    "<div id=lx contenteditable=true data-lexical-editor=true></div>"
    "<script>\n"
    "const lx = document.getElementById('lx');\n"
    "window.model = 'hola como esas? continuacion';\n"
    "window.events = [];\n"
    "let drawing = false;\n"
    "function draw(caret) {\n"
    "  drawing = true;\n"
    "  lx.textContent = '';\n"
    "  const p = document.createElement('p'), s = document.createElement('span');\n"
    "  s.setAttribute('data-lexical-text', 'true'); s.textContent = model; p.appendChild(s); lx.appendChild(p);\n"
    "  if (caret != null && document.activeElement === lx) getSelection().collapse(s.firstChild, caret);\n"
    "  obs.takeRecords(); drawing = false;\n"
    "}\n"
    "const obs = new MutationObserver(() => { if (!drawing) { events.push('reverted'); draw(null); } });\n"
    "function offsets() {\n"
    "  const r = getSelection().getRangeAt(0), s = lx.querySelector('span');\n"
    "  const at = (n, o) => (n === s.firstChild ? o : n === s ? (o ? model.length : 0) : 0);\n"
    "  return [at(r.startContainer, r.startOffset), at(r.endContainer, r.endOffset)];\n"
    "}\n"
    "lx.addEventListener('beforeinput', (e) => {\n"
    "  events.push('beforeinput:' + e.inputType);\n"
    "  const t = e.inputType;\n"
    "  let data = e.data;\n"
    "  if (data == null && e.dataTransfer) data = e.dataTransfer.getData('text/plain');\n"
    "  if (!['insertText', 'insertReplacementText', 'insertFromPaste'].includes(t) || data == null) return;\n"
    "  e.preventDefault();\n"
    "  const [a, b] = offsets();\n"
    "  model = model.slice(0, a) + data + model.slice(b);\n"
    "  queueMicrotask(() => draw(a + data.length));\n"
    "});\n"
    "lx.addEventListener('input', (e) => events.push('input:' + e.inputType));\n"
    "draw(null);\n"
    "obs.observe(lx, { childList: true, characterData: true, subtree: true });\n"
    "</script></body></html>";

static char value[1024];
static void on_js(GObject *src, GAsyncResult *res, gpointer d) {
    int world = GPOINTER_TO_INT(d);
    GError *e = NULL;
    JSCValue *v = world ? webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(src), res, &e)
                        : webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(src), res, &e);
    char *s = v && (jsc_value_is_string(v) || jsc_value_is_boolean(v)) ? jsc_value_to_string(v) : NULL;
    snprintf(value, sizeof value, "%s", s ? s : e ? (e->message[0] ? e->message : "error") : "?");
    if (getenv("DEBUG_JS") && e) fprintf(stderr, "js error: %s %d [%s]\n", g_quark_to_string(e->domain), e->code, e->message);
    g_free(s);
    if (v) g_object_unref(v);
    g_clear_error(&e);
}
static int has_value(const void *arg) { (void)arg; return value[0] != 0; }
/* in the page's own world; a copy (a test: never freed) */
static const char *eval(const char *js) {
    value[0] = 0;
    webkit_web_view_evaluate_javascript(view, js, -1, NULL, NULL, NULL, on_js, GINT_TO_POINTER(0));
    wait_for(has_value, NULL, 5);
    return g_strdup(value);
}
/* in lord-web's page script world, as webmenu.c runs them */
static const char *in_world(const char *body, const char *orig, const char *text) {
    value[0] = 0;
    GVariant *args = NULL;
    if (orig) {
        GVariantDict d;
        g_variant_dict_init(&d, NULL);
        g_variant_dict_insert(&d, "orig", "s", orig);
        g_variant_dict_insert(&d, "text", "s", text);
        args = g_variant_dict_end(&d);
    }
    webkit_web_view_call_async_javascript_function(view, body, -1, args, WEBSELECT_WORLD, NULL, NULL, on_js, GINT_TO_POINTER(1));
    wait_for(has_value, NULL, 5);
    return g_strdup(value);
}

/* select [a, b) of field id's text (its value, or its first text node's) and focus it */
static void select_in(const char *id, int a, int b) {
    char js[512];
    snprintf(js, sizeof js,
             "(() => { const f = document.getElementById('%s'); f.focus();"
             " if ('value' in f && f.tagName !== 'DIV') f.setSelectionRange(%d, %d);"
             " else { const w = document.createTreeWalker(f, NodeFilter.SHOW_TEXT); const n = w.nextNode();"
             " getSelection().setBaseAndExtent(n, %d, n, %d); }"
             " return String(document.activeElement.id); })()", id, a, b, a, b);
    eval(js);
}

static void focus_view(int on) {
    webback_set_focused(back, on);
    wait_for(never, NULL, 1);
}

int main(void) {
    char home[] = "/tmp/lord-web-ai-check-XXXXXX";
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
    WebKitNetworkSession *session = webkit_network_session_new_ephemeral();
    WebKitSettings *settings = webkit_settings_new();
    view = webback_view_new(back, ctx, session, settings);
    g_object_unref(settings);
    g_signal_connect(view, "load-changed", G_CALLBACK(on_load), NULL);
    webback_set_visible(back, 1);
    focus_view(1);
    /* the page script's world exists for the page's life, as lord-web's own script (webselect.c) keeps it: a world
     * nothing holds is made again for each call, without what the last one kept */
    WebKitUserScript *us = webkit_user_script_new_for_world("void 0;", WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
                                                            WEBSELECT_WORLD, NULL, NULL);
    webkit_user_content_manager_add_script(webkit_web_view_get_user_content_manager(view), us);
    webkit_user_script_unref(us);
    webkit_web_view_load_html(view, PAGE, "http://127.0.0.1/");
    check(wait_for(loaded, NULL, 30), "the page loads");

    /* 1. the root cause: with the panel open the view has lost the keyboard and the page counts as unfocused */
    select_in("lx", 0, 15);
    check(!strcmp(in_world(WEBAI_MARK_JS, NULL, NULL), "true"), "the menu marks the focused Lexical-like box");
    focus_view(0);
    check(!strcmp(eval("String(document.hasFocus())"), "false"), "without the keyboard document.hasFocus() is false (the old check refused here)");
    check(!strcmp(eval("String(document.activeElement.id)"), "lx"), "the box is still the page's focused element");
    focus_view(1);                          /* the panel closed: the keyboard is back (webwin.c kb_enter) */

    /* 2. the Lexical-like editor takes it into its model */
    const char *r = in_world(WEBAI_REPLACE_JS, "hola como esas?", "¿Hola, cómo estás?");
    printf("  lexical-like: %s, events %s\n", r, eval("events.join(' ')"));
    check(!strcmp(r, "done"), "Replace in the Lexical-like box answers done");
    check(!strcmp(eval("model"), "¿Hola, cómo estás? continuacion"), "its own model has the new text");
    wait_for(never, NULL, 1);
    check(!strcmp(eval("lx.textContent"), "¿Hola, cómo estás? continuacion"), "and its box shows it (nothing undone)");

    /* 3. a textarea */
    select_in("ta", 0, 15);
    in_world(WEBAI_MARK_JS, NULL, NULL);
    r = in_world(WEBAI_REPLACE_JS, "hola como esas?", "¿Hola, cómo estás?");
    check(!strcmp(r, "done") && !strcmp(eval("document.getElementById('ta').value"), "¿Hola, cómo estás? todo bien"), "a textarea's selection is replaced");

    /* 4. plain editable content */
    select_in("ce", 3, 9);
    in_world(WEBAI_MARK_JS, NULL, NULL);
    r = in_world(WEBAI_REPLACE_JS, "muñeca", "muñeca inflable");
    check(!strcmp(r, "done") && !strcmp(eval("document.getElementById('ce').textContent"), "tu muñeca inflable no funciona como tendria"), "a contenteditable box's selection is replaced");

    /* 5. refusals */
    select_in("ta", 0, 4);
    in_world(WEBAI_MARK_JS, NULL, NULL);
    select_in("ta", 0, 6);
    check(!strcmp(in_world(WEBAI_REPLACE_JS, "¿Hol", "X"), "selection-changed"), "a selection that changed: selection-changed");
    select_in("ta", 0, 4);
    in_world(WEBAI_MARK_JS, NULL, NULL);
    select_in("ce", 0, 2);
    check(!strcmp(in_world(WEBAI_REPLACE_JS, "¿Hol", "X"), "other-field"), "another field focused: other-field");
    select_in("pw", 0, 4);
    in_world(WEBAI_MARK_JS, NULL, NULL);
    check(!strcmp(in_world(WEBAI_REPLACE_JS, "secr", "X"), "password"), "a password field: password");
    check(!strcmp(eval("document.getElementById('pw').value"), "secreto123"), "and the password is untouched");
    check(!strcmp(in_world(WEBAI_REPLACE_JS, "secr", "X"), "gone"), "one Replace per menu: gone after it");

    /* 6. the log words */
    check(webai_refusal("done") == NULL && webai_refusal("fallback") == NULL, "done and fallback are not refusals");
    check(webai_refusal("password") && webai_refusal("no-focus") && webai_refusal("selection-changed"), "the refusals have their text");
    check(!strcmp(webai_refusal("<script>"), webai_refusal("no-answer")), "a word that isn't one is no-answer");
    if (!failed) printf("ok\n");
    return failed;
}
