/* webxfer.c — what WebKit 2.48's WPE port can't take from the session by itself (docs/WEBAPPS.md, "Drag and drop"
 * and "The clipboard"): its view has no drop target (libwpe has no drag and drop at all) and its pasteboard holds
 * text only (no image in, no image out). So lord-web does it:
 *
 *   - a drop (webwin.c reads the drag's list, webdrop.c opens its local files) and an image pasted from the session's
 *     clipboard: lord-web reads the bytes itself and passes them, in 1 MB pieces, to a script in an isolated world of
 *     its own (the page's scripts can't see or change it), which makes them File objects and blob: addresses. A
 *     script in the page's world then turns those into a DataTransfer and dispatches dragenter, dragover and drop at
 *     the drop point (or paste on the focused element), the way a browser would: it has to run there, because a
 *     DataTransfer's "types" (which pages check for "Files") is empty in WebKit 2.48 for one made by a script and
 *     only the page's world can mend it. The blob: addresses are revoked right after. The page only ever gets the
 *     files the person dropped on it; lord-web never reads anything else.
 *   - the page's copies: WPE 2.48 has no Async Clipboard API (navigator.clipboard is undefined), so a script at
 *     document start gives the page navigator.clipboard.write and writeText (and ClipboardItem) when they are
 *     missing. They post to lord-web, which takes text (at most 16 MB) or an image/png or image/jpeg (at most
 *     32 MB, checked by its signature) only while the page is the app's own (not another site in the window) and
 *     the person pressed a key, clicked or touched the window in the last seconds (webwin_user_active: the browser's
 *     "user activation"), and offers it to the session. The message handler lives in lord-web's isolated world, where
 *     only a bridge script runs (the app's top frame, and only in documents of its scope by the scripts' allow-list):
 *     no script of the page's world, and no frame of another site, can reach it or forge its messages. The page's
 *     navigator.clipboard asks the bridge through events on the document. lord-web checks again, at each message,
 *     that the document's address, the main frame's committed one and the view's are inside the scope.
 *     Anything else is refused, the page's promise rejected. */
#include "web.h"
#include "webwin.h"
#include "weburl.h"
#include "../sec/json.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define XFER_WORLD "lordos-web-xfer"
#define XFER_CHUNK (1024 * 1024)
#define CLIP_HANDLER "lordosClipboard"
#define CLIP_TEXT_MAX (16 * 1024 * 1024)
#define CLIP_IMAGE_MAX (32 * 1024 * 1024)

/* ---- the page's copies ---- */

#define CLIP_ASK "lordos-clipboard-write"
#define CLIP_DONE "lordos-clipboard-done"

/* in the page's world: navigator.clipboard and ClipboardItem when WPE has none. It holds nothing secret and talks
 * only to CLIP_BRIDGE, through events on the document whose detail is JSON text (a string crosses worlds as it is);
 * the page could send those events itself, which is the same as calling writeText. Fresh objects only: nothing of
 * the page's (a setter on Object.prototype, a replaced dispatchEvent) is called with what is written. */
static const char CLIP_JS[] =
    "(() => {\n"
    "  if (navigator.clipboard) return;\n"
    "  const doc = document, dispatch = EventTarget.prototype.dispatchEvent, listen = EventTarget.prototype.addEventListener;\n"
    "  const CE = CustomEvent, DE = DOMException, json = JSON.stringify, parse = JSON.parse, waiting = new Map();\n"
    "  const mget = Map.prototype.get, mset = Map.prototype.set, mdel = Map.prototype.delete;\n"
    "  let next = 1;\n"
    "  listen.call(doc, '" CLIP_DONE "', (e) => {\n"
    "    if (typeof e.detail !== 'string') return;\n"
    "    let r; try { r = parse(e.detail); } catch (x) { return; }\n"
    "    const w = r && mget.call(waiting, r.id);\n"
    "    if (!w) return;\n"
    "    mdel.call(waiting, r.id);\n"
    "    if (!r.error) { w.ok(); return; }\n"
    "    const m = String(r.error), i = m.indexOf(': ');\n"
    "    w.no(new DE(i > 0 ? m.slice(i + 2) : m, i > 0 ? m.slice(0, i) : 'NotAllowedError'));\n"
    "  }, true);\n"
    "  const post = (type, text, data) => new Promise((ok, no) => {\n"
    "    const id = next++;\n"
    "    mset.call(waiting, id, { ok, no });\n"
    "    dispatch.call(doc, new CE('" CLIP_ASK "', { detail: json({ id: id, type: type, text: text, data: data }) }));\n"
    "  });\n"
    "  const dataUrl = (blob) => new Promise((ok, no) => { const r = new FileReader(); r.onload = () => ok(String(r.result)); r.onerror = () => no(r.error); r.readAsDataURL(blob); });\n"
    "  if (typeof window.ClipboardItem !== 'function') {\n"
    "    class ClipboardItem {\n"
    "      #items;\n"
    "      constructor(items) { this.#items = Object.assign({}, items); }\n"
    "      get types() { return Object.freeze(Object.keys(this.#items)); }\n"
    "      async getType(t) { const v = await this.#items[t]; if (v === undefined) throw new DOMException('No such type', 'NotFoundError'); return v instanceof Blob ? v : new Blob([String(v)], { type: t }); }\n"
    "      static supports(t) { return t === 'text/plain' || t === 'image/png' || t === 'image/jpeg'; }\n"
    "    }\n"
    "    window.ClipboardItem = ClipboardItem;\n"
    "  }\n"
    "  const clipboard = {\n"
    "    async writeText(t) { await post('text/plain', String(t), ''); },\n"
    "    async write(items) {\n"
    "      const item = items && items[0];\n"
    "      if (!item) throw new DOMException('Nothing to write', 'DataError');\n"
    "      const types = Array.from(item.types);\n"
    "      const img = types.find((t) => t === 'image/png' || t === 'image/jpeg');\n"
    "      if (img) { const u = await dataUrl(await item.getType(img)); await post(img, '', u.slice(u.indexOf(',') + 1)); return; }\n"
    "      if (types.includes('text/plain')) { await post('text/plain', await (await item.getType('text/plain')).text(), ''); return; }\n"
    "      throw new DOMException('Type not supported', 'NotAllowedError');\n"
    "    },\n"
    "  };\n"
    "  Object.defineProperty(Navigator.prototype, 'clipboard', { get: () => clipboard, configurable: true, enumerable: true });\n"
    "})();\n";

/* in lord-web's isolated world (XFER_WORLD), the app's top frame only: the one script that can reach the message
 * handler, which no script of the page's world (of any frame, of any site) can see. It passes CLIP_JS's requests
 * on with the document's own address (location as this world sees it: the page can't change it here) and answers. */
static const char CLIP_BRIDGE[] =
    "(() => {\n"
    "  const h = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers." CLIP_HANDLER ";\n"
    "  if (!h) return;\n"
    "  document.addEventListener('" CLIP_ASK "', (e) => {\n"
    "    if (typeof e.detail !== 'string') return;\n"
    "    let m; try { m = JSON.parse(e.detail); } catch (x) { return; }\n"
    "    if (!m || typeof m !== 'object') return;\n"
    "    const id = Number(m.id);\n"
    "    const done = (error) => document.dispatchEvent(new CustomEvent('" CLIP_DONE "', { detail: JSON.stringify(error ? { id, error } : { id }) }));\n"
    "    const msg = { type: String(m.type), text: typeof m.text === 'string' ? m.text : '', data: typeof m.data === 'string' ? m.data : '', href: String(location.href) };\n"
    "    h.postMessage(msg).then(() => done(''), (err) => done(String((err && err.message) || err || 'NotAllowedError')));\n"
    "  }, true);\n"
    "})();\n";

static char *prop_str(JSCValue *o, const char *name) {
    JSCValue *v = jsc_value_object_get_property(o, name);
    char *s = v && jsc_value_is_string(v) ? jsc_value_to_string(v) : NULL;
    if (v) g_object_unref(v);
    return s;
}

static int image_ok(const char *mime, const guchar *b, gsize n) {
    static const guchar png[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    if (!strcmp(mime, "image/png")) return n > 8 && !memcmp(b, png, 8);
    if (!strcmp(mime, "image/jpeg")) return n > 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff;
    return 0;
}

static char *committed;                 /* the main frame's document, as last committed */

/* 1 while the document that sent a copy is the app's own: the address it says it has (from the isolated world), the
 * main frame's committed document and the view's current address are all inside the scope */
static int clip_in_scope(Web *w, const char *href) {
    const char *now = webkit_web_view_get_uri(w->view);
    return !w->outside && href && committed && now && weburl_in_scope(href, w->app.scope) &&
           weburl_in_scope(committed, w->app.scope) && weburl_in_scope(now, w->app.scope);
}

/* NULL when taken, else why not */
static const char *clip_take(Web *w, JSCValue *v) {
    if (!w->win || !jsc_value_is_object(v)) return "NotAllowedError: nothing to write";
    char *href = prop_str(v, "href");
    int mine = clip_in_scope(w, href);
    g_free(href);
    if (!mine) return "NotAllowedError: not this app's page";
    if (!webwin_user_active(w->win)) return "NotAllowedError: the clipboard needs a key or a click in the window first";
    char *type = prop_str(v, "type");
    const char *why = "NotAllowedError: type not supported";
    if (type && !strcmp(type, "text/plain")) {
        char *text = prop_str(v, "text");
        if (text && strlen(text) <= CLIP_TEXT_MAX) { webwin_offer(w->win, NULL, g_bytes_new_take(text, strlen(text))); text = NULL; why = NULL; }
        else why = "NotAllowedError: too long";
        g_free(text);
    } else if (type && (!strcmp(type, "image/png") || !strcmp(type, "image/jpeg"))) {
        char *b64 = prop_str(v, "data");
        gsize n = 0;
        guchar *img = b64 && strlen(b64) <= (size_t)CLIP_IMAGE_MAX / 3 * 4 + 8 ? g_base64_decode(b64, &n) : NULL;
        g_free(b64);
        if (img && n <= CLIP_IMAGE_MAX && image_ok(type, img, n)) { webwin_offer(w->win, type, g_bytes_new_take(img, n)); img = NULL; why = NULL; }
        else why = "DataError: not a PNG or JPEG image of at most 32 MB";
        g_free(img);
    }
    g_free(type);
    return why;
}

static gboolean on_clip(WebKitUserContentManager *m, JSCValue *v, WebKitScriptMessageReply *reply, gpointer d) {
    (void)m;
    const char *why = clip_take(d, v);
    if (why) webkit_script_message_reply_return_error_message(reply, why);
    else {
        JSCValue *t = jsc_value_new_boolean(jsc_value_get_context(v), TRUE);
        webkit_script_message_reply_return_value(reply, t);
        g_object_unref(t);
    }
    return TRUE;
}

static void on_committed(WebKitWebView *v, WebKitLoadEvent e, gpointer d) {
    (void)d;
    if (e != WEBKIT_LOAD_COMMITTED) return;
    g_free(committed);
    committed = g_strdup(webkit_web_view_get_uri(v));
}

void webxfer_init(Web *w) {
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(w->view);
    if (!ucm) return;
    g_signal_connect(w->view, "load-changed", G_CALLBACK(on_committed), w);
    if (!webkit_user_content_manager_register_script_message_handler_with_reply(ucm, CLIP_HANDLER, XFER_WORLD)) return;
    g_signal_connect(ucm, "script-message-with-reply-received::" CLIP_HANDLER, G_CALLBACK(on_clip), w);
    /* only into the app's own documents (another site's page in the window keeps WPE's own: no clipboard API) */
    char pat[WEBURL_MAX + 16];
    const char *allow[2] = { pat, NULL };
    const char *const *only = !weburl_scope_pattern(w->app.scope, pat, sizeof pat) ? allow : NULL;
    WebKitUserScript *s = webkit_user_script_new_for_world(CLIP_BRIDGE, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                                                           XFER_WORLD, only, NULL);
    webkit_user_content_manager_add_script(ucm, s);
    webkit_user_script_unref(s);
    s = webkit_user_script_new(CLIP_JS, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, only, NULL);
    webkit_user_content_manager_add_script(ucm, s);
    webkit_user_script_unref(s);
}

/* ---- drops and pasted images ---- */

static const char JS_INIT[] = "globalThis.__lordosXfer = { parts: [], files: [], urls: [] }; return true;";
static const char JS_ADD[] =
    "const s = atob(b), u = new Uint8Array(s.length);\n"
    "for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i);\n"
    "__lordosXfer.parts.push(u);\n"
    "return true;";
static const char JS_END[] =
    "const x = __lordosXfer;\n"
    "x.files.push(new File(x.parts, name, { type, lastModified: mtime }));\n"
    "x.parts = [];\n"
    "return true;";
static const char JS_URLS[] = "const x = __lordosXfer; x.urls = x.files.map((f) => URL.createObjectURL(f)); x.files = []; return JSON.stringify(x.urls);";
static const char JS_DONE[] =
    "const x = globalThis.__lordosXfer;\n"
    "if (x) for (const u of x.urls) URL.revokeObjectURL(u);\n"
    "delete globalThis.__lordosXfer;\n"
    "return true;";
/* in the page's world */
static const char JS_DISPATCH[] =
    "const { urls, names, types, mtimes } = JSON.parse(meta);\n"
    "const dt = new DataTransfer();\n"
    "for (let i = 0; i < urls.length; i++) {\n"
    "  const blob = await (await fetch(urls[i])).blob();\n"
    "  dt.items.add(new File([blob], names[i], { type: types[i], lastModified: mtimes[i] }));\n"
    "}\n"
    "if (text) dt.setData('text/plain', text);\n"
    "if (uris) dt.setData('text/uri-list', uris);\n"
    "const kinds = Object.freeze(Array.from(dt.types).filter((t) => t !== 'Files').concat(dt.files.length ? ['Files'] : []));\n"
    "try { Object.defineProperty(dt, 'types', { get: () => kinds, configurable: true }); } catch (e) { }\n"
    "try { dt.dropEffect = 'copy'; } catch (e) { }\n"
    "if (paste) {\n"
    "  let t = document.activeElement;\n"
    "  if (!t || t === document.documentElement) t = document.body;\n"
    "  return t ? t.dispatchEvent(new ClipboardEvent('paste', { bubbles: true, cancelable: true, composed: true, clipboardData: dt })) : false;\n"
    "}\n"
    "const t = document.elementFromPoint(x, y) || document.body;\n"
    "if (!t) return false;\n"
    "const init = { bubbles: true, cancelable: true, composed: true, clientX: x, clientY: y, dataTransfer: dt };\n"
    "t.dispatchEvent(new DragEvent('dragenter', init));\n"
    "t.dispatchEvent(new DragEvent('dragover', init));\n"
    "return t.dispatchEvent(new DragEvent('drop', init));";

typedef struct {
    Web *w;
    int paste;
    double x, y;                        /* CSS pixels */
    WebDropFile files[WEBDROP_FILES_MAX];
    char *types[WEBDROP_FILES_MAX];
    int n, i;
    size_t done_bytes;                  /* of file i */
    GBytes *img;                        /* a pasted image: file 0, from memory */
    char *uris, *text;
    char **urls;
} Xfer;

static Xfer *running;                   /* one transfer at a time */

static void xfer_free(Xfer *x) {
    webdrop_close(x->files, x->n);
    for (int i = 0; i < x->n; i++) g_free(x->types[i]);
    if (x->img) g_bytes_unref(x->img);
    g_free(x->uris);
    g_free(x->text);
    g_strfreev(x->urls);
    if (running == x) running = NULL;
    g_free(x);
}

static void step(Xfer *x);

static void call(Xfer *x, const char *body, GVariant *args, const char *world, GAsyncReadyCallback cb) {
    webkit_web_view_call_async_javascript_function(x->w->view, body, -1, args, world, NULL, NULL, cb, x);
}

static int finished(GObject *src, GAsyncResult *res, char **str) {
    GError *e = NULL;
    JSCValue *v = webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(src), res, &e);
    if (!v) {
        fprintf(stderr, "lord-web: the drop could not be handed to the page (%s)\n", e ? e->message : "?");
        g_clear_error(&e);
        return -1;
    }
    if (str) *str = jsc_value_is_string(v) ? jsc_value_to_string(v) : NULL;
    g_object_unref(v);
    return 0;
}

static void on_cleaned(GObject *src, GAsyncResult *res, gpointer d) { finished(src, res, NULL); xfer_free(d); }

static void cleanup(Xfer *x) {
    if (x->n) call(x, JS_DONE, NULL, XFER_WORLD, on_cleaned);
    else xfer_free(x);
}

static void on_dispatched(GObject *src, GAsyncResult *res, gpointer d) { finished(src, res, NULL); cleanup(d); }

/* one JSON string value, quoted, appended to g */
static void add_quoted(GString *g, const char *v) {
    size_t n = strlen(v) * 6 + 3;
    char *q = g_malloc(n);
    json_quote(q, n, v);
    g_string_append(g, q);
    g_free(q);
}

/* WebKit 2.48 takes only numbers, strings and dictionaries as a function's arguments ("Invalid parameter types"
 * for an array: no drop or pasted image ever reached the page), so the files' lists go as one JSON text, "meta" */
static void dispatch(Xfer *x) {
    GString *m = g_string_new("{\"urls\":[");
    for (int i = 0; i < x->n; i++) { if (i) g_string_append_c(m, ','); add_quoted(m, x->urls && x->urls[i] ? x->urls[i] : ""); }
    g_string_append(m, "],\"names\":[");
    for (int i = 0; i < x->n; i++) { if (i) g_string_append_c(m, ','); add_quoted(m, x->files[i].name); }
    g_string_append(m, "],\"types\":[");
    for (int i = 0; i < x->n; i++) { if (i) g_string_append_c(m, ','); add_quoted(m, x->types[i]); }
    g_string_append(m, "],\"mtimes\":[");
    for (int i = 0; i < x->n; i++) g_string_append_printf(m, "%s%lld", i ? "," : "", (long long)x->files[i].mtime_ms);
    g_string_append(m, "]}");
    GVariantDict args;
    g_variant_dict_init(&args, NULL);
    g_variant_dict_insert_value(&args, "meta", g_variant_new_take_string(g_string_free(m, FALSE)));
    g_variant_dict_insert_value(&args, "paste", g_variant_new_double(x->paste ? 1 : 0));
    g_variant_dict_insert_value(&args, "x", g_variant_new_double(x->x));
    g_variant_dict_insert_value(&args, "y", g_variant_new_double(x->y));
    g_variant_dict_insert_value(&args, "uris", g_variant_new_string(x->uris ? x->uris : ""));
    g_variant_dict_insert_value(&args, "text", g_variant_new_string(x->text ? x->text : ""));
    call(x, JS_DISPATCH, g_variant_dict_end(&args), NULL, on_dispatched);
}

static void on_urls(GObject *src, GAsyncResult *res, gpointer d) {
    Xfer *x = d;
    char *s = NULL;
    if (finished(src, res, &s) || !s) { g_free(s); cleanup(x); return; }
    JDoc doc;
    int parsed = !json_parse(s, strlen(s), &doc, NULL, 0), ok = parsed;
    g_free(s);
    if (ok && doc.root && doc.root->type == J_ARR && (int)doc.root->len == x->n) {
        x->urls = g_new0(char *, (gsize)x->n + 1);
        int i = 0;
        for (const JVal *u = doc.root->kid; u && i < x->n; u = u->next)
            if (u->type == J_STR && !strncmp(u->str, "blob:", 5)) x->urls[i++] = g_strdup(u->str);
        ok = i == x->n;
    } else ok = 0;
    if (parsed) json_free(&doc);
    if (ok) dispatch(x);
    else cleanup(x);
}

static void on_step(GObject *src, GAsyncResult *res, gpointer d) {
    if (finished(src, res, NULL)) { cleanup(d); return; }
    step(d);
}

/* the next piece of file i, its end, or the end of all of them */
static void step(Xfer *x) {
    if (x->i >= x->n) { call(x, JS_URLS, NULL, XFER_WORLD, on_urls); return; }
    WebDropFile *f = &x->files[x->i];
    size_t want = f->size - x->done_bytes < XFER_CHUNK ? f->size - x->done_bytes : XFER_CHUNK;
    guchar *buf = want ? g_malloc(want) : NULL;
    size_t got = 0;
    if (want && x->img) {
        gsize n;
        const guchar *p = g_bytes_get_data(x->img, &n);
        memcpy(buf, p + x->done_bytes, want);
        got = want;
    } else while (got < want) {
        ssize_t k = read(f->fd, buf + got, want - got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;                          /* the file got shorter */
        got += (size_t)k;
    }
    GVariantDict args;
    g_variant_dict_init(&args, NULL);
    if (got) {
        /* a piece; its answer comes back here for the next one, or for the file's end */
        x->done_bytes = got < want ? f->size : x->done_bytes + got;     /* shorter than it said: that was all */
        g_variant_dict_insert_value(&args, "b", g_variant_new_take_string(g_base64_encode(buf, got)));
        g_free(buf);
        call(x, JS_ADD, g_variant_dict_end(&args), XFER_WORLD, on_step);
        return;
    }
    g_free(buf);
    g_variant_dict_insert_value(&args, "name", g_variant_new_string(f->name));
    g_variant_dict_insert_value(&args, "type", g_variant_new_string(x->types[x->i]));
    g_variant_dict_insert_value(&args, "mtime", g_variant_new_double((double)f->mtime_ms));
    x->i++;
    x->done_bytes = 0;
    call(x, JS_END, g_variant_dict_end(&args), XFER_WORLD, on_step);
}

static Xfer *xfer_new(Web *w) {
    if (running || !w->view || w->outside) return NULL;
    Xfer *x = g_new0(Xfer, 1);
    x->w = w;
    running = x;
    return x;
}

static void start(Xfer *x) {
    if (!x->n) { dispatch(x); return; }
    call(x, JS_INIT, NULL, XFER_WORLD, on_step);
}

static char *type_of(const char *name) {
    char *ct = g_content_type_guess(name, NULL, 0, NULL);
    char *mime = ct ? g_content_type_get_mime_type(ct) : NULL;
    g_free(ct);
    if (!mime || strlen(mime) > 127) { g_free(mime); mime = g_strdup("application/octet-stream"); }
    return mime;
}

void webxfer_drop(Web *w, double x, double y, WebDropFile *files, int n, const char *uris, const char *text) {
    Xfer *t = xfer_new(w);
    if (!t) { webdrop_close(files, n); return; }
    double z = webkit_web_view_get_zoom_level(w->view);
    if (!(z > 0)) z = 1;
    t->x = x / z;
    t->y = y / z;
    for (int i = 0; i < n && i < WEBDROP_FILES_MAX; i++) {
        t->files[i] = files[i];
        t->types[i] = type_of(files[i].name);
        t->n++;
    }
    for (int i = t->n; i < n; i++) webdrop_close(&files[i], 1);
    t->uris = uris && uris[0] ? g_strdup(uris) : NULL;
    t->text = text && text[0] ? g_strdup(text) : NULL;
    if (!t->n && !t->text && !t->uris) { xfer_free(t); return; }
    start(t);
}

void webxfer_paste_image(Web *w, const char *mime, GBytes *data) {
    gsize n = 0;
    const guchar *p = g_bytes_get_data(data, &n);
    Xfer *t = n <= CLIP_IMAGE_MAX && image_ok(mime, p, n) ? xfer_new(w) : NULL;
    if (!t) { g_bytes_unref(data); return; }
    t->paste = 1;
    t->img = data;
    t->n = 1;
    t->files[0].fd = -1;
    t->files[0].size = n;
    t->files[0].mtime_ms = g_get_real_time() / 1000;
    snprintf(t->files[0].name, sizeof t->files[0].name, "%s", !strcmp(mime, "image/png") ? "image.png" : "image.jpg");
    t->types[0] = g_strdup(mime);
    start(t);
}

/* ---- the context menu's Copy Image ---- */

/* in lord-web's isolated world (the page's scripts can't change fetch or the canvas there), with the page's origin:
 * the image's bytes as a data: address, a PNG unless it already is a PNG or a JPEG; "" when it can't be read */
static const char JS_IMAGE[] =
    "const r = await fetch(u);\n"
    "if (!r.ok) return '';\n"
    "let b = await r.blob();\n"
    "if (b.size > max) return '';\n"
    "if (b.type !== 'image/png' && b.type !== 'image/jpeg') {\n"
    "  const bm = await createImageBitmap(b);\n"
    "  const c = document.createElement('canvas');\n"
    "  c.width = bm.width; c.height = bm.height;\n"
    "  c.getContext('2d').drawImage(bm, 0, 0);\n"
    "  b = await new Promise((ok) => c.toBlob(ok, 'image/png'));\n"
    "  if (!b || b.size > max) return '';\n"
    "}\n"
    "return await new Promise((ok, no) => { const f = new FileReader(); f.onload = () => ok(String(f.result)); f.onerror = () => no(f.error); f.readAsDataURL(b); });";

static int copying;

static void on_image(GObject *src, GAsyncResult *res, gpointer d) {
    Web *w = d;
    copying = 0;
    GError *e = NULL;
    JSCValue *v = webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(src), res, &e);
    char *s = v && jsc_value_is_string(v) ? jsc_value_to_string(v) : NULL;
    if (v) g_object_unref(v);
    if (!v) { fprintf(stderr, "lord-web: the image could not be read (%s)\n", e ? e->message : "?"); g_clear_error(&e); }
    const char *mime = NULL, *b64 = NULL;
    if (s && !strncmp(s, "data:image/png;base64,", 22)) { mime = "image/png"; b64 = s + 22; }
    else if (s && !strncmp(s, "data:image/jpeg;base64,", 23)) { mime = "image/jpeg"; b64 = s + 23; }
    gsize n = 0;
    guchar *img = b64 && strlen(b64) <= (size_t)CLIP_IMAGE_MAX / 3 * 4 + 8 ? g_base64_decode(b64, &n) : NULL;
    g_free(s);
    if (img && n <= CLIP_IMAGE_MAX && image_ok(mime, img, n) && w->win) { webwin_offer(w->win, mime, g_bytes_new_take(img, n)); img = NULL; }
    g_free(img);
}

void webxfer_copy_image(Web *w, const char *uri) {
    if (copying || !w->view || w->outside || !uri) return;
    copying = 1;
    GVariantDict args;
    g_variant_dict_init(&args, NULL);
    g_variant_dict_insert_value(&args, "u", g_variant_new_string(uri));
    g_variant_dict_insert_value(&args, "max", g_variant_new_double((double)CLIP_IMAGE_MAX));
    webkit_web_view_call_async_javascript_function(w->view, JS_IMAGE, -1, g_variant_dict_end(&args), XFER_WORLD, NULL, NULL, on_image, w);
}
