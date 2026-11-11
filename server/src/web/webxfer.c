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
 *     "user activation"), and offers it to the session. Anything else is refused, the page's promise rejected. */
#include "web.h"
#include "webwin.h"
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

static const char CLIP_JS[] =
    "(() => {\n"
    "  const h = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers." CLIP_HANDLER ";\n"
    "  if (!h || navigator.clipboard) return;\n"
    "  const post = (m) => h.postMessage(m);\n"
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
    "    async writeText(t) { await post({ type: 'text/plain', text: String(t) }); },\n"
    "    async write(items) {\n"
    "      const item = items && items[0];\n"
    "      if (!item) throw new DOMException('Nothing to write', 'DataError');\n"
    "      const types = Array.from(item.types);\n"
    "      const img = types.find((t) => t === 'image/png' || t === 'image/jpeg');\n"
    "      if (img) { const u = await dataUrl(await item.getType(img)); await post({ type: img, data: u.slice(u.indexOf(',') + 1) }); return; }\n"
    "      if (types.includes('text/plain')) { await post({ type: 'text/plain', text: await (await item.getType('text/plain')).text() }); return; }\n"
    "      throw new DOMException('Type not supported', 'NotAllowedError');\n"
    "    },\n"
    "  };\n"
    "  Object.defineProperty(Navigator.prototype, 'clipboard', { get: () => clipboard, configurable: true, enumerable: true });\n"
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

/* NULL when taken, else why not */
static const char *clip_take(Web *w, JSCValue *v) {
    if (!w->win || !jsc_value_is_object(v)) return "NotAllowedError: nothing to write";
    if (w->outside || !webwin_user_active(w->win)) return "NotAllowedError: the clipboard needs a key or a click in the window first";
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

void webxfer_init(Web *w) {
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(w->view);
    if (!ucm || !webkit_user_content_manager_register_script_message_handler_with_reply(ucm, CLIP_HANDLER, NULL)) return;
    g_signal_connect(ucm, "script-message-with-reply-received::" CLIP_HANDLER, G_CALLBACK(on_clip), w);
    WebKitUserScript *s = webkit_user_script_new(CLIP_JS, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
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

static void dispatch(Xfer *x) {
    GVariantBuilder names, types, mtimes;
    g_variant_builder_init(&names, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&types, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&mtimes, G_VARIANT_TYPE("ad"));
    for (int i = 0; i < x->n; i++) {
        g_variant_builder_add(&names, "s", x->files[i].name);
        g_variant_builder_add(&types, "s", x->types[i]);
        g_variant_builder_add(&mtimes, "d", (double)x->files[i].mtime_ms);
    }
    GVariantDict args;
    g_variant_dict_init(&args, NULL);
    const char *const none[] = { NULL };
    g_variant_dict_insert_value(&args, "urls", g_variant_new_strv(x->urls ? (const char *const *)x->urls : none, -1));
    g_variant_dict_insert_value(&args, "names", g_variant_builder_end(&names));
    g_variant_dict_insert_value(&args, "types", g_variant_builder_end(&types));
    g_variant_dict_insert_value(&args, "mtimes", g_variant_builder_end(&mtimes));
    g_variant_dict_insert_value(&args, "paste", g_variant_new_boolean(x->paste));
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
