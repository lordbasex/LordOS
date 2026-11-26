/* webmenu.c — a right click in a web app's page (docs/WEBAPPS.md, "Editing"): LordOS's own context menu, with the
 * items for what is under the pointer, as a browser's (webedit.h, webmenu_items):
 *
 *   - lord-web asks lord-server for it: org.lordos.WebApps1.Menu(app, profile, items, checked, text), items from the
 *     closed list, never the page's text; the one piece of the page that goes is the selection's few words for
 *     "Search the web for “…”", cleaned here (websearch_snippet) and again in lord-server. The shell shows the menu at
 *     the pointer and answers the item chosen, or "";
 *   - what the person chooses runs here, with what was under the pointer as it was then: editing commands (web_edit),
 *     Paste as Plain Text (webwin_paste_plain), a search or a link opened in the person's browser through OpenURI
 *     (http and https only for a search or an image; a link may be mail or phone too, as before), addresses and a
 *     link to the selection (a text fragment) copied, an image copied (webxfer.c), the media element's Play, Mute,
 *     Loop and Show Controls (webselect.c), Back, Forward and Reload;
 *   - Save Image As… and Save Video As…: LordOS's save dialog (the FileChooser portal's SaveFile), then the page's
 *     own download path (webengine.c's on_destination takes the chosen file) for an http(s) or data: address, or the
 *     image's bytes read by the page's origin (webxfer.c) for one the page made itself (blob:). The chosen file must
 *     be in the home folder, not in a hidden folder and not a launcher (webdl_target_ok), also once links are
 *     resolved.
 *
 * Spelling: in a text field the page left alone, WebKit asks for its own menu too (WPEPlatform; webselect.c doesn't
 * prevent that right click's default there). Its spelling suggestions, Ignore Spelling and Learn Spelling are kept
 * (web_menu_webkit) and go first in LordOS's menu; the page script's report and WebKit's request come in either
 * order, so a report from a text field waits for WebKit's at most SPELL_WAIT_MS. A suggestion chosen runs WebKit's own
 * item (it replaces the word); the suggestions are dictionary words, cleaned like the selection before they show.
 *
 * Emoji, Speech and Inspect are the shell's own (webmenu_shell_only): lord-server answers "" for them.
 *
 * ✨ AI (docs/AI.md "Writing"): with a selection that isn't cut (never a password field's: the page script reports
 * none), the menu also carries the selection, cleaned (webai_clean, at most 4,000 characters), for the shell's own
 * Writing panel; lord-web keeps the selection as it was. The shell's Replace comes back as WriteReplace(text)
 * (only from lord-server, checked by pid): the text goes in through WebKit's InsertText only when the field that had
 * that selection is still focused, still an editable field that isn't a password, and still has exactly that
 * selection (web_menu_ai_replace). */
#include "web.h"
#include "webwin.h"
#include "webdl.h"
#include "websearch.h"
#include "webback.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PORTAL_NAME  "org.freedesktop.portal.Desktop"
#define PORTAL_PATH  "/org/freedesktop/portal/desktop"
#define ASK_NAME     "org.lordos.WebApps1"
#define ASK_PATH     "/org/lordos/WebApps1"
#define ASK_TIMEOUT_MS (5 * 60 * 1000)
#define URI_MAX      8192

/* ---- addresses ---- */

static int is_web(const char *u) { return u && (!g_ascii_strncasecmp(u, "https://", 8) || !g_ascii_strncasecmp(u, "http://", 7)); }

/* an address the menu may offer: a web, mail or phone link, or (for an image) one the page itself can read again */
static int uri_ok(const char *u, int image) {
    if (!u || !u[0] || strlen(u) > URI_MAX) return 0;
    for (const unsigned char *p = (const unsigned char *)u; *p; p++) if (*p < 0x20 || *p == 0x7f) return 0;
    if (is_web(u)) return 1;
    if (image) return !g_ascii_strncasecmp(u, "blob:", 5) || !g_ascii_strncasecmp(u, "data:image/", 11);
    return !g_ascii_strncasecmp(u, "mailto:", 7) || !g_ascii_strncasecmp(u, "tel:", 4);
}

static void copy_text(Web *w, const char *s) { if (s && s[0] && w->win) webwin_offer(w->win, NULL, g_bytes_new(s, strlen(s))); }

/* ---- Search the web ---- */

static int search_engine(Web *w) {
    char path[PATH_MAX + 32];
    size_t len = 0;
    snprintf(path, sizeof path, "%s/" WEBSEARCH_FILE, w->config_dir);
    char *t = webreg_read_file(path, WEBSEARCH_FILE_MAX, &len);
    int e = t ? websearch_parse(t, len) : WEBSEARCH_GOOGLE;
    free(t);
    return e;
}

static void search(Web *w, const char *text) {
    char url[WEBSEARCH_URL_MAX];
    if (!websearch_url(search_engine(w), text, url, sizeof url)) webbus_open_uri(w, url);
}

static void copy_highlight(Web *w, const char *page, const char *text) {
    size_t n = strlen(page ? page : "") + 3 * WEBSEARCH_SELECTION_MAX + 64;
    char *out = g_malloc(n);
    if (!websearch_highlight(page, text, out, n)) copy_text(w, out);
    g_free(out);
}

/* ---- Save Image As…, Save Video As… ---- */

typedef struct { Web *w; char *uri; GBytes *bytes; guint sub; int done; } Save;

static void save_free(Save *sv) {
    if (sv->sub && sv->w->bus) g_dbus_connection_signal_unsubscribe(sv->w->bus, sv->sub);
    g_free(sv->uri);
    if (sv->bytes) g_bytes_unref(sv->bytes);
    g_free(sv);
}

/* the chosen file, checked as it is and with its folder's links resolved; NULL when it may not be written */
static char *target(Web *w, const char *path) {
    if (!webdl_target_ok(w->home, path)) return NULL;
    char *dir = g_path_get_dirname(path), *base = g_path_get_basename(path);
    char real[PATH_MAX];
    char *out = realpath(dir, real) ? g_build_filename(real, base, NULL) : NULL;
    g_free(dir);
    g_free(base);
    if (out && !webdl_target_ok(w->home, out)) { g_free(out); out = NULL; }
    return out;
}

static void write_bytes(Web *w, const char *path, GBytes *b) {
    gsize n = 0;
    const char *p = g_bytes_get_data(b, &n);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    size_t off = 0;
    while (fd >= 0 && off < n) {
        ssize_t k = write(fd, p + off, n - off);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        off += (size_t)k;
    }
    int ok = fd >= 0 && off == n;
    if (fd >= 0 && close(fd)) ok = 0;
    const char *base = strrchr(path, '/');
    webbus_notify_text(w, weblang(ok ? "download.done" : "download.failed"), base ? base + 1 : path);
}

static void save_end(Save *sv, GVariant *results) {
    if (sv->done) return;
    sv->done = 1;
    Web *w = sv->w;
    char *file = NULL;
    GVariant *uris = results ? g_variant_lookup_value(results, "uris", G_VARIANT_TYPE("as")) : NULL;
    if (uris && g_variant_n_children(uris) == 1) {
        const char *u = NULL;
        g_variant_get_child(uris, 0, "&s", &u);
        char *f = u ? g_filename_from_uri(u, NULL, NULL) : NULL;
        file = f ? target(w, f) : NULL;
        if (f && !file) web_note("save refused: the chosen file is outside the home folder, hidden, or a launcher");
        g_free(f);
    }
    if (uris) g_variant_unref(uris);
    if (file && sv->bytes) write_bytes(w, file, sv->bytes);
    else if (file && sv->uri && w->view) {
        WebKitDownload *dl = webkit_web_view_download_uri(w->view, sv->uri);
        if (dl) g_object_set_data_full(G_OBJECT(dl), WEBMENU_DEST_KEY, g_strdup(file), g_free);
    }
    g_free(file);
    save_free(sv);
}

static void on_save_response(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *sig, GVariant *p, gpointer d) {
    (void)c; (void)sender; (void)path; (void)iface; (void)sig;
    guint32 code = 2;
    GVariant *results = NULL;
    if (g_variant_is_of_type(p, G_VARIANT_TYPE("(ua{sv})"))) g_variant_get(p, "(u@a{sv})", &code, &results);
    save_end(d, code == 0 ? results : NULL);
    if (results) g_variant_unref(results);
}

static void on_save_file(GObject *src, GAsyncResult *res, gpointer d) {
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    if (!r) { fprintf(stderr, "lord-web: no save dialog (%s)\n", e ? e->message : "?"); g_clear_error(&e); save_end(d, NULL); return; }
    g_variant_unref(r);
}

/* LordOS's save dialog for name; then uri is downloaded to the chosen file, or bytes written there (one of them,
 * taken over) */
static void save_ask(Web *w, const char *title_key, const char *name, char *uri, GBytes *bytes) {
    if (!w->bus) { g_free(uri); if (bytes) g_bytes_unref(bytes); return; }
    Save *sv = g_new0(Save, 1);
    sv->w = w;
    sv->uri = uri;
    sv->bytes = bytes;
    static unsigned seq;
    char token[48], path[256], sender[128], safe[WEBDL_NAME_MAX + 1];
    snprintf(token, sizeof token, "lordwebsave%u_%u", (unsigned)getpid(), ++seq);
    snprintf(sender, sizeof sender, "%s", g_dbus_connection_get_unique_name(w->bus) + 1);
    for (char *p = sender; *p; p++) if (*p == '.') *p = '_';
    snprintf(path, sizeof path, PORTAL_PATH "/request/%s/%s", sender, token);
    sv->sub = g_dbus_connection_signal_subscribe(w->bus, PORTAL_NAME, "org.freedesktop.portal.Request", "Response", path, NULL,
                                                 G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE, on_save_response, sv, NULL);
    if (webdl_name(name, safe, sizeof safe)) snprintf(safe, sizeof safe, "download");
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(token));
    g_variant_builder_add(&opts, "{sv}", "modal", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&opts, "{sv}", "current_name", g_variant_new_string(safe));
    g_dbus_connection_call(w->bus, PORTAL_NAME, PORTAL_PATH, "org.freedesktop.portal.FileChooser", "SaveFile",
                           g_variant_new("(ssa{sv})", "", weblang(title_key), &opts), G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, G_MAXINT,
                           NULL, on_save_file, sv);
}

/* the file name an address suggests: its path's last part, decoded; fallback for none (data:, a bare folder) */
static char *name_of(const char *uri, const char *fallback) {
    if (!g_ascii_strncasecmp(uri, "data:image/", 11)) {
        const char *t = uri + 11;
        size_t n = strcspn(t, ";,+");
        return n && n <= 8 ? g_strdup_printf("image.%.*s", (int)n, t) : g_strdup(fallback);
    }
    GUri *u = g_uri_parse(uri, G_URI_FLAGS_ENCODED, NULL);
    const char *p = u ? g_uri_get_path(u) : NULL;
    const char *slash = p ? strrchr(p, '/') : NULL;
    const char *last = slash ? slash + 1 : p;
    char *dec = last && last[0] ? g_uri_unescape_string(last, "/") : NULL;
    if (u) g_uri_unref(u);
    if (dec && dec[0] && g_utf8_validate(dec, -1, NULL)) return dec;
    g_free(dec);
    return g_strdup(fallback);
}

void web_menu_save_bytes(Web *w, const char *mime, GBytes *data) {
    save_ask(w, "save.image", !strcmp(mime, "image/png") ? "image.png" : "image.jpg", NULL, data);
}

/* ---- WebKit's spelling items ---- */

#define SPELL_WAIT_MS  300          /* a text field's report waits this long for WebKit's menu */
#define SPELL_FRESH_US (800 * 1000) /* WebKit's items count for a report this close to them */

typedef struct {
    int n;
    char *guess[WEBMENU_GUESSES_MAX];
    WebKitContextMenuItem *item[WEBMENU_GUESSES_MAX], *ignore, *learn;
} Spell;

static Spell spell;                 /* WebKit's last menu's items */
static gint64 spell_at;             /* when it came (monotonic µs), 0 none */

static void spell_clear(Spell *sp) {
    for (int i = 0; i < sp->n; i++) { g_free(sp->guess[i]); if (sp->item[i]) g_object_unref(sp->item[i]); }
    if (sp->ignore) g_object_unref(sp->ignore);
    if (sp->learn) g_object_unref(sp->learn);
    memset(sp, 0, sizeof *sp);
}

static void spell_run(WebKitContextMenuItem *it) {
    GAction *a = it ? webkit_context_menu_item_get_gaction(it) : NULL;
    if (a) g_action_activate(a, webkit_context_menu_item_get_gaction_target(it));
}

int web_menu_webkit_asks(void) { return !strcmp(webback_name(), "platform"); }

/* ---- ✨ AI: the selection the last menu offered, and Replace ---- */

static char *ai_orig;               /* the selection as the page had it, when the menu offered ✨ AI in a text field */

static void ai_forget(void) { if (ai_orig) { memset(ai_orig, 0, strlen(ai_orig)); g_free(ai_orig); ai_orig = NULL; } }

/* in the page script's world: the focused field is a text field (not a password) or editable content, the window has
 * the focus, and its selection is exactly the one the menu had; then the text goes in as typed ("done"), or "ok" when
 * the page's insertText did nothing (WebKit's own InsertText then), else "no" */
static const char AI_CHECK[] =
    "const a = document.activeElement;\n"
    "if (!a || !document.hasFocus()) return 'no';\n"
    "const TEXT = ['', 'text', 'search', 'url', 'tel', 'email'];\n"
    "let sel = null;\n"
    "if (a instanceof HTMLTextAreaElement || (a instanceof HTMLInputElement && TEXT.includes(a.type))) {\n"
    "  if (a.readOnly || a.disabled) return 'no';\n"
    "  sel = a.value.slice(a.selectionStart, a.selectionEnd);\n"
    "} else if (a.isContentEditable) sel = String(getSelection() || '');\n"
    "if (sel === null || sel !== orig || !sel) return 'no';\n"
    "return document.execCommand('insertText', false, text) ? 'done' : 'ok';\n";

typedef struct { Web *w; char *text; } AiCall;

static void on_ai_check(GObject *o, GAsyncResult *res, gpointer d) {
    AiCall *ac = d;
    GError *e = NULL;
    JSCValue *v = webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(o), res, &e);
    char *r = v && jsc_value_is_string(v) ? jsc_value_to_string(v) : NULL;
    if (r && !strcmp(r, "ok") && ac->w->view) webkit_web_view_execute_editing_command_with_argument(ac->w->view, "InsertText", ac->text);
    if (!r || (strcmp(r, "ok") && strcmp(r, "done"))) web_note("✨ AI's Replace refused: the field lost the focus or its selection changed");
    g_free(r);
    if (v) g_object_unref(v);
    g_clear_error(&e);
    memset(ac->text, 0, strlen(ac->text));
    g_free(ac->text);
    g_free(ac);
}

void web_menu_ai_replace(Web *w, const char *text) {
    char *clean = g_malloc(WEBAI_MAX + 1);
    if (!w->view || !ai_orig || webai_clean(text, clean)) { g_free(clean); web_note("✨ AI's Replace refused: nothing to replace"); return; }
    GVariantDict args;
    g_variant_dict_init(&args, NULL);
    g_variant_dict_insert(&args, "orig", "s", ai_orig);
    g_variant_dict_insert(&args, "text", "s", clean);
    AiCall *ac = g_new0(AiCall, 1);
    ac->w = w;
    ac->text = clean;
    ai_forget();                    /* one Replace per menu */
    webkit_web_view_call_async_javascript_function(w->view, AI_CHECK, -1, g_variant_dict_end(&args), WEBSELECT_WORLD, NULL, NULL, on_ai_check, ac);
}

/* ---- the menu ---- */

typedef struct {
    Web *w;
    int items[WEBMENU_N], n;
    char *link, *image, *media, *text, *page;
    int media_video;
    Spell spell;
} MenuCall;

static void menu_free(MenuCall *mc) {
    spell_clear(&mc->spell);
    g_free(mc->link);
    g_free(mc->image);
    g_free(mc->media);
    if (mc->text) { memset(mc->text, 0, strlen(mc->text)); g_free(mc->text); }
    g_free(mc->page);
    g_free(mc);
}

static void chosen(MenuCall *mc, int item) {
    Web *w = mc->w;
    switch (item) {
    case WEBMENU_UNDO: web_edit(w, WEBEDIT_UNDO); break;
    case WEBMENU_REDO: web_edit(w, WEBEDIT_REDO); break;
    case WEBMENU_CUT: web_edit(w, WEBEDIT_CUT); break;
    case WEBMENU_COPY: web_edit(w, WEBEDIT_COPY); break;
    case WEBMENU_PASTE: web_edit(w, WEBEDIT_PASTE); break;
    case WEBMENU_PASTE_PLAIN: if (w->win) webwin_paste_plain(w->win); break;
    case WEBMENU_SELECT_ALL: web_edit(w, WEBEDIT_SELECT_ALL); break;
    case WEBMENU_COPY_HIGHLIGHT: if (mc->text) copy_highlight(w, mc->page, mc->text); break;
    case WEBMENU_SEARCH: if (mc->text) search(w, mc->text); break;
    case WEBMENU_OPEN_LINK: if (mc->link) webbus_open_uri(w, mc->link); break;
    case WEBMENU_COPY_LINK: copy_text(w, mc->link); break;
    case WEBMENU_OPEN_IMAGE: if (is_web(mc->image)) webbus_open_uri(w, mc->image); break;
    case WEBMENU_SAVE_IMAGE:
        if (!mc->image) break;
        if (!g_ascii_strncasecmp(mc->image, "blob:", 5)) webxfer_save_image(w, mc->image);
        else { char *name = name_of(mc->image, "image"); save_ask(w, "save.image", name, g_strdup(mc->image), NULL); g_free(name); }
        break;
    case WEBMENU_COPY_IMAGE: if (mc->image) webxfer_copy_image(w, mc->image); break;
    case WEBMENU_COPY_IMAGE_ADDRESS: if (is_web(mc->image)) copy_text(w, mc->image); break;
    case WEBMENU_PLAY: case WEBMENU_PAUSE: case WEBMENU_MUTE: case WEBMENU_UNMUTE: case WEBMENU_LOOP: case WEBMENU_CONTROLS:
        webselect_media(w, webmenu_name(item));
        break;
    case WEBMENU_SAVE_VIDEO: case WEBMENU_SAVE_AUDIO:
        if (is_web(mc->media)) {
            char *name = name_of(mc->media, mc->media_video ? "video" : "audio");
            save_ask(w, mc->media_video ? "save.video" : "save.audio", name, g_strdup(mc->media), NULL);
            g_free(name);
        }
        break;
    case WEBMENU_BACK: if (w->view) webkit_web_view_go_back(w->view); break;
    case WEBMENU_FORWARD: if (w->view) webkit_web_view_go_forward(w->view); break;
    case WEBMENU_RELOAD: web_reload(w); break;
    case WEBMENU_SPELL_1: case WEBMENU_SPELL_2: case WEBMENU_SPELL_3: case WEBMENU_SPELL_4: case WEBMENU_SPELL_5:
        if (item - WEBMENU_SPELL_1 < mc->spell.n) spell_run(mc->spell.item[item - WEBMENU_SPELL_1]);
        break;
    case WEBMENU_IGNORE_SPELLING: spell_run(mc->spell.ignore); break;
    case WEBMENU_LEARN_SPELLING: spell_run(mc->spell.learn); break;
    default: break;                 /* emoji, speech, inspect: the shell's */
    }
}

static void on_menu(GObject *src, GAsyncResult *res, gpointer d) {
    MenuCall *mc = d;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    mc->w->menu_open = 0;
    int item = -1;
    if (r) {
        const char *s = NULL;
        g_variant_get(r, "(&s)", &s);
        int k = s && s[0] ? webmenu_item(s) : -1;
        for (int i = 0; i < mc->n; i++) if (mc->items[i] == k && !webmenu_shell_only(k)) item = k;     /* only one this menu had */
        g_variant_unref(r);
    } else {
        fprintf(stderr, "lord-web: no context menu (%s)\n", e ? e->message : "?");
        g_clear_error(&e);
    }
    if (item >= 0) chosen(mc, item);
    menu_free(mc);
}

static void show(Web *w, const WebMenuAt *a) {
    if (!w->bus || !w->profile || w->menu_open || !a) return;
    unsigned at = a->at & (WEBMENU_AT_EDITABLE | WEBMENU_AT_SELECTION | WEBMENU_AT_MEDIA | WEBMENU_AT_PAUSED | WEBMENU_AT_MUTED |
                           WEBMENU_AT_LOOP | WEBMENU_AT_CONTROLS | WEBMENU_AT_VIDEO);
    const char *link = uri_ok(a->link, 0) ? a->link : NULL;
    const char *image = uri_ok(a->image, 1) && !w->outside ? a->image : NULL;
    const char *media = (at & WEBMENU_AT_MEDIA) && uri_ok(a->media, 0) && is_web(a->media) ? a->media : NULL;
    const char *text = (at & WEBMENU_AT_SELECTION) && a->text && a->text[0] ? a->text : NULL;
    const char *page = w->view ? webkit_web_view_get_uri(w->view) : NULL;
    if (link) at |= WEBMENU_AT_LINK;
    if (image) at |= WEBMENU_AT_IMAGE | (is_web(image) ? WEBMENU_AT_IMAGE_WEB : 0) | WEBMENU_AT_IMAGE_SAVE;
    if (media) at |= WEBMENU_AT_MEDIA_SAVE;
    char snip[WEBSEARCH_SNIPPET_MAX] = "";
    if (text) websearch_snippet(text, WEBSEARCH_SNIPPET_CHARS, snip, sizeof snip);
    if (snip[0]) at |= WEBMENU_AT_SEARCH;
    if (snip[0] && !a->text_cut && is_web(page)) at |= WEBMENU_AT_HIGHLIGHT;
    if (w->view && webkit_web_view_can_go_back(w->view)) at |= WEBMENU_AT_BACK;
    if (w->view && webkit_web_view_can_go_forward(w->view)) at |= WEBMENU_AT_FORWARD;
    /* the selection right-clicked is the session's primary selection too: the menu's Speech reads that one */
    if (text && !a->text_cut && w->win) webwin_set_primary(w->win, text);
    /* ✨ AI: the whole selection (not one the page cut), cleaned; kept as it was for a Replace in a text field */
    char *ai = g_malloc(WEBAI_MAX + 1);
    ai[0] = 0;
    ai_forget();
    if (text && !a->text_cut && webai_clean(text, ai)) ai[0] = 0;
    if (ai[0] && (at & WEBMENU_AT_EDITABLE)) ai_orig = g_strdup(text);
    MenuCall *mc = g_new0(MenuCall, 1);
    mc->w = w;
    /* WebKit's spelling items of this right click (a text field's), taken over */
    if ((at & WEBMENU_AT_EDITABLE) && spell_at && g_get_monotonic_time() - spell_at < SPELL_FRESH_US) {
        mc->spell = spell;
        memset(&spell, 0, sizeof spell);
        at |= WEBMENU_AT_GUESSES(mc->spell.n) | (mc->spell.ignore ? WEBMENU_AT_IGNORE : 0) | (mc->spell.learn ? WEBMENU_AT_LEARN : 0);
    }
    spell_clear(&spell);
    spell_at = 0;
    mc->n = webmenu_items(at, mc->items);
    unsigned checked = webmenu_checked(at, mc->items, mc->n);
    int has_search = 0;
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
    for (int i = 0; i < mc->n; i++) {
        g_variant_builder_add(&b, "s", webmenu_name(mc->items[i]));
        has_search |= mc->items[i] == WEBMENU_SEARCH;
    }
    mc->link = g_strdup(link);
    mc->image = g_strdup(image);
    mc->media = g_strdup(media);
    mc->media_video = (at & WEBMENU_AT_VIDEO) != 0;
    mc->text = text && strlen(text) <= WEBSEARCH_SELECTION_MAX ? g_strdup(text) : text ? g_utf8_substring(text, 0, WEBSEARCH_SELECTION_MAX / 4) : NULL;
    mc->page = g_strdup(page);
    GVariantBuilder gb;
    g_variant_builder_init(&gb, G_VARIANT_TYPE("as"));
    for (int i = 0; i < mc->spell.n; i++) g_variant_builder_add(&gb, "s", mc->spell.guess[i]);
    w->menu_open = 1;
    g_dbus_connection_call(w->bus, ASK_NAME, ASK_PATH, ASK_NAME, "Menu",
                           g_variant_new("(ssasusass)", w->app.id, w->profile->id, &b, checked, has_search ? snip : "", &gb, ai),
                           G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, ASK_TIMEOUT_MS, NULL, on_menu, mc);
    memset(ai, 0, strlen(ai));
    g_free(ai);
}

/* ---- a text field's report, waiting for WebKit's menu ---- */

static struct { int on; Web *w; WebMenuAt a; guint timer; } pend;

static void pend_clear(void) {
    if (pend.timer) g_source_remove(pend.timer);
    g_free((char *)pend.a.link);
    g_free((char *)pend.a.image);
    g_free((char *)pend.a.media);
    if (pend.a.text) { memset((char *)pend.a.text, 0, strlen(pend.a.text)); g_free((char *)pend.a.text); }
    memset(&pend, 0, sizeof pend);
}

static void pend_show(void) {
    if (!pend.on) return;
    Web *w = pend.w;
    WebMenuAt a = pend.a;
    pend.timer = 0;
    show(w, &a);
    pend_clear();
}

static gboolean pend_timeout(gpointer d) { (void)d; pend.timer = 0; pend_show(); return G_SOURCE_REMOVE; }

void web_context_menu(Web *w, const WebMenuAt *a) {
    if (!a) return;
    pend_clear();
    int fresh = spell_at && g_get_monotonic_time() - spell_at < SPELL_FRESH_US;
    if (!(a->at & WEBMENU_AT_EDITABLE) || !web_menu_webkit_asks() || fresh) { show(w, a); return; }
    pend.on = 1;
    pend.w = w;
    pend.a = *a;
    pend.a.link = g_strdup(a->link);
    pend.a.image = g_strdup(a->image);
    pend.a.media = g_strdup(a->media);
    pend.a.text = g_strdup(a->text);
    pend.timer = g_timeout_add(SPELL_WAIT_MS, pend_timeout, NULL);
}

void web_menu_webkit(Web *w, WebKitContextMenu *m) {
    (void)w;
    spell_clear(&spell);
    for (GList *l = m ? webkit_context_menu_get_items(m) : NULL; l; l = l->next) {
        WebKitContextMenuItem *it = l->data;
        WebKitContextMenuAction act = webkit_context_menu_item_get_stock_action(it);
        if (act == WEBKIT_CONTEXT_MENU_ACTION_SPELLING_GUESS && spell.n < WEBMENU_GUESSES_MAX) {
            char word[WEBSEARCH_SNIPPET_MAX];
            const char *t = webkit_context_menu_item_get_title(it);
            if (!websearch_snippet(t, WEBSEARCH_SNIPPET_CHARS, word, sizeof word)) continue;
            spell.guess[spell.n] = g_strdup(word);
            spell.item[spell.n++] = g_object_ref(it);
        } else if (act == WEBKIT_CONTEXT_MENU_ACTION_IGNORE_SPELLING && !spell.ignore) spell.ignore = g_object_ref(it);
        else if (act == WEBKIT_CONTEXT_MENU_ACTION_LEARN_SPELLING && !spell.learn) spell.learn = g_object_ref(it);
    }
    spell_at = g_get_monotonic_time();
    pend_show();
}
