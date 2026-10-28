/* js.c — embedded QuickJS. Exposes the global `lord` object (create/modify nodes) and
 * provides setTimeout, requestAnimationFrame, console and performance, which React needs. */
#include "../app/apphost_bc.h"
#include <unistd.h>
#include "js.h"
#include "../ime.h"
#include "../wl/wl.h"
#include "../keyboard.h"
#include "js_sys.h"
#include "js_term.h"
#include "js_pkg.h"
#include "js_sound.h"
#include "js_monitor.h"
#include "js_sec.h"
#include "js_doc.h"
#include "js_print.h"
#include "js_vault.h"
#include "js_mail.h"
#include "js_input.h"
#include "js_fw.h"
#include "js_backup.h"
#include "js_broker.h"
#include "js_wasm.h"
#include "js_net.h"
#include "js_media.h"
#include "js_wm.h"
#include "js_disk.h"
#include "js_imager.h"
#include "js_logs.h"
#include "js_help.h"
#include "js_hw.h"
#include "../media/media.h"
#include "../sec/codesig.h"
#include "ui.h"
#include "../term/term.h"
#include "lord_version.h"           /* build/gen: version, channel and build (Makefile) */

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "quickjs.h"

static JSRuntime *rt;
static JSContext *ctx;
static void set_screen_displays(JSValue screen);

/* Minimal polyfills: the real event loop is lord-server's (main.c). */
static const char prelude[] =
    "(function(){\n"
    "const g = globalThis;\n"
    "const fmt = v => typeof v === 'string' ? v : v instanceof Error ? (v.message + '\\n' + (v.stack || '')) :\n"
    "  (() => { try { return JSON.stringify(v); } catch (e) { return String(v); } })();\n"
    "const out = p => (...a) => lord.log(p + a.map(fmt).join(' '));\n"
    "g.console = { log: out(''), info: out(''), debug: out(''), warn: out('[warn] '), error: out('[error] ') };\n"
    "g.performance = { now: () => lord.now() };\n"
    "let nextId = 1; const timers = new Map();\n"
    "const add = (fn, ms, args, repeat) => { const id = nextId++; ms = Math.max(0, +ms || 0);\n"
    "  timers.set(id, { fn, args, at: lord.now() + ms, ms, repeat }); return id; };\n"
    "g.setTimeout = (fn, ms, ...a) => add(fn, ms, a, false);\n"
    "g.setInterval = (fn, ms, ...a) => add(fn, ms, a, true);\n"
    "g.setImmediate = (fn, ...a) => add(fn, 0, a, false);\n"
    "g.clearTimeout = g.clearInterval = g.clearImmediate = id => { timers.delete(id); };\n"
    "g.queueMicrotask = fn => { Promise.resolve().then(fn); };\n"
    "let frames = new Map(), frameId = 1;\n"
    "g.requestAnimationFrame = fn => { const id = frameId++; frames.set(id, fn); return id; };\n"
    "g.cancelAnimationFrame = id => { frames.delete(id); };\n"
    "g.__lordTick = now => {\n"
    "  const due = [];\n"
    "  for (const [id, t] of timers) if (t.at <= now) due.push([id, t]);\n"
    "  due.sort((a, b) => a[1].at - b[1].at || a[0] - b[0]);\n"
    "  for (const [id, t] of due) {\n"
    "    if (!timers.has(id)) continue;\n"
    "    if (t.repeat) t.at = now + Math.max(t.ms, 1); else timers.delete(id);\n"
    "    try { t.fn(...t.args); } catch (e) { console.error('timer:', e); }\n"
    "  }\n"
    "  now = lord.now(); let next = -1;\n"
    "  for (const t of timers.values()) { const d = Math.max(0, t.at - now); if (next < 0 || d < next) next = d; }\n"
    "  return next;\n"
    "};\n"
    "g.__lordWantsFrame = () => frames.size > 0;\n"
    "g.__lordFrame = now => { const f = frames; frames = new Map();\n"
    "  for (const fn of f.values()) { try { fn(now); } catch (e) { console.error('raf:', e); } } };\n"
    "})();\n";

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void dump_exception(JSContext *c) {
    JSValue e = JS_GetException(c);
    const char *msg = JS_ToCString(c, e);
    fprintf(stderr, "js: %s\n", msg ? msg : "(exception)");
    if (msg) JS_FreeCString(c, msg);
    if (JS_IsObject(e)) {
        JSValue st = JS_GetPropertyStr(c, e, "stack");
        const char *s = JS_IsUndefined(st) ? NULL : JS_ToCString(c, st);
        if (s) { fprintf(stderr, "%s\n", s); JS_FreeCString(c, s); }
        JS_FreeValue(c, st);
    }
    JS_FreeValue(c, e);
}

static void drain_jobs(void) {
    JSContext *c;
    int r;
    while ((r = JS_ExecutePendingJob(rt, &c)) != 0)
        if (r < 0) dump_exception(c);
}

static Node *arg_node(JSContext *c, JSValueConst v) {
    int32_t id;
    if (JS_ToInt32(c, &id, v)) return NULL;
    return node_get(id);
}

/* ---- native functions: lord.* ---- */

static JSValue f_create_node(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    const char *t = JS_ToCString(c, argv[0]);
    if (!t) return JS_EXCEPTION;
    int type = !strcmp(t, "text") ? NODE_TEXT : !strcmp(t, "image") ? NODE_IMAGE
             : !strcmp(t, "raw") ? NODE_RAW : !strcmp(t, "terminal") ? NODE_TERM
             : !strcmp(t, "surface") ? NODE_SURFACE : !strcmp(t, "media") ? NODE_MEDIA : NODE_VIEW;
    JS_FreeCString(c, t);
    Node *n = node_new(type);
    return n ? JS_NewInt32(c, n->id) : JS_ThrowOutOfMemory(c);
}

static JSValue f_set_text(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Node *n = arg_node(c, argv[0]);
    const char *s = JS_ToCString(c, argv[1]);
    if (n && s) node_set_text(n, s);
    if (s) JS_FreeCString(c, s);
    return JS_UNDEFINED;
}

static JSValue f_set_style(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Node *n = arg_node(c, argv[0]);
    const char *key = JS_ToCString(c, argv[1]);
    if (!n || !key) { if (key) JS_FreeCString(c, key); return JS_UNDEFINED; }
    JSValueConst v = argv[2];
    int err = 0;
    Style before = n->s;
    StyleExt ext_before;
    if (n->s.ext) memcpy(&ext_before, n->s.ext, STYLE_EXT_VALUES);
    if (!strcmp(key, "gradient")) {
        if (JS_IsArray(c, v)) {
            double a[3] = {180, 0, 0};
            for (int i = 0; i < 3; i++) {
                JSValue e = JS_GetPropertyUint32(c, v, (uint32_t)i);
                JS_ToFloat64(c, &a[i], e);
                JS_FreeValue(c, e);
            }
            n->s.has_gradient = 1;
            n->s.grad_angle = (float)a[0];
            n->s.grad_from = (uint32_t)(int64_t)a[1];
            n->s.grad_to = (uint32_t)(int64_t)a[2];
        } else n->s.has_gradient = 0;
    } else if (JS_IsUndefined(v) || JS_IsNull(v)) {
        err = style_reset(&n->s, key);
    } else if (JS_IsNumber(v)) {
        double d;
        JS_ToFloat64(c, &d, v);
        err = style_set_number(&n->s, key, d);
    } else if (JS_IsString(v)) {
        const char *s = JS_ToCString(c, v);
        err = s ? style_set_string(&n->s, key, s) : -1;
        if (s) JS_FreeCString(c, s);
    }
    if (err) fprintf(stderr, "js: unsupported style '%s'\n", key);
    JS_FreeCString(c, key);
    node_restyled(n, &before, before.ext ? &ext_before : NULL);     /* node.c (engine-test does the same) */
    return JS_UNDEFINED;
}

static JSValue f_set_prop(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Node *n = arg_node(c, argv[0]);
    const char *key = JS_ToCString(c, argv[1]);
    if (n && key && !strcmp(key, "src")) {
        image_release(n->img);
        n->img = NULL;
        const char *src = JS_IsString(argv[2]) ? JS_ToCString(c, argv[2]) : NULL;
        if (src) {
            char path[2048];
            if (src[0] == '/') snprintf(path, sizeof path, "%s", src);
            else if (!strncmp(src, "asset:", 6) && !strstr(src, "..")) snprintf(path, sizeof path, "%s/%s", js_sys_assets(), src + 6);
            else snprintf(path, sizeof path, "%s/%s", ui.theme_dir, src);
            n->img = image_get(path);
            JS_FreeCString(c, src);
        }
        node_invalidate(n);
    } else if (n && key && !strcmp(key, "pixels") && n->type == NODE_IMAGE) {
        /* { width, height, data: ArrayBuffer | Uint8Array | Uint8ClampedArray (RGBA) } */
        int32_t w = 0, h = 0;
        JSValue wv = JS_GetPropertyStr(c, argv[2], "width"), hv = JS_GetPropertyStr(c, argv[2], "height");
        JSValue dv = JS_GetPropertyStr(c, argv[2], "data");
        JS_ToInt32(c, &w, wv);
        JS_ToInt32(c, &h, hv);
        size_t len = 0, off = 0, el = 0;
        uint8_t *data = JS_GetArrayBuffer(c, &len, dv);
        if (!data) {
            JS_FreeValue(c, JS_GetException(c));
            JSValue ab = JS_GetTypedArrayBuffer(c, dv, &off, &len, &el);
            if (JS_IsException(ab)) JS_FreeValue(c, JS_GetException(c));
            else {
                size_t abl = 0;
                uint8_t *base = JS_GetArrayBuffer(c, &abl, ab);
                if (base && off + len <= abl) data = base + off;
                JS_FreeValue(c, ab);
            }
        }
        if (data && w > 0 && h > 0 && w <= 4096 && h <= 4096 && len >= (size_t)w * h * 4) {
            Image *img = image_pixels(n->img, w, h, data);
            n->img = img;
        } else if (!JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) fprintf(stderr, "js: invalid pixels (%dx%d, %zu bytes)\n", w, h, len);
        JS_FreeValue(c, wv);
        JS_FreeValue(c, hv);
        JS_FreeValue(c, dv);
        node_invalidate(n);
    } else if (n && key && !strcmp(key, "session") && n->type == NODE_TERM) {
        int32_t id = 0;
        if (JS_IsNumber(argv[2])) JS_ToInt32(c, &id, argv[2]);
        n->term = id;
        Session *s = term_get(id);
        if (s) s->node = n->id;
        node_invalidate(n);
    } else if (n && key && !strcmp(key, "media") && n->type == NODE_MEDIA) {
        int32_t id = 0;                     /* lord.media source (video, photo, screenshot, camera) */
        if (JS_IsNumber(argv[2])) JS_ToInt32(c, &id, argv[2]);
        js_media_node(n, id);
        node_invalidate(n);
    } else if (n && key && !strcmp(key, "protected")) {
        n->protect = JS_ToBool(c, argv[2]) ? 1 : 0;     /* protected window (wm.setProtected) */
        node_invalidate(n);
    } else if (n && key && !strcmp(key, "surface") && n->type == NODE_SURFACE) {
        int32_t id = 0;                     /* window of a Wayland app (wl/wl.c) */
        if (JS_IsNumber(argv[2])) JS_ToInt32(c, &id, argv[2]);
        n->term = id;
        node_invalidate(n);
    }
    if (key) JS_FreeCString(c, key);
    return JS_UNDEFINED;
}

static JSValue f_append(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Node *p = arg_node(c, argv[0]), *ch = arg_node(c, argv[1]);
    if (p && ch) node_append(p, ch);
    return JS_UNDEFINED;
}

static JSValue f_insert_before(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Node *p = arg_node(c, argv[0]), *ch = arg_node(c, argv[1]), *b = arg_node(c, argv[2]);
    if (p && ch) node_insert_before(p, ch, b);
    return JS_UNDEFINED;
}

static JSValue f_remove(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Node *ch = arg_node(c, argv[1]);
    if (ch) node_detach(ch);
    return JS_UNDEFINED;
}

static JSValue f_destroy(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Node *n = arg_node(c, argv[0]);
    if (n) node_destroy(n);
    return JS_UNDEFINED;
}

static JSValue f_set_root(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    ui.root = arg_node(c, argv[0]);
    ui.dirty = 1;
    return JS_UNDEFINED;
}

static JSValue f_get_layout(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    Node *n = arg_node(c, argv[0]);
    JSValue a = JS_NewArray(c);
    if (n) {
        JS_SetPropertyUint32(c, a, 0, JS_NewFloat64(c, n->ax));
        JS_SetPropertyUint32(c, a, 1, JS_NewFloat64(c, n->ay));
        JS_SetPropertyUint32(c, a, 2, JS_NewFloat64(c, n->w));
        JS_SetPropertyUint32(c, a, 3, JS_NewFloat64(c, n->h));
    }
    return a;
}

static JSValue f_now(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewFloat64(c, now_ms());
}

static JSValue f_log(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    const char *s = JS_ToCString(c, argv[0]);
    if (s) { fprintf(stderr, "js: %s\n", s); JS_FreeCString(c, s); }
    return JS_UNDEFINED;
}

/* Metrics for the theme (e.g. the "About" window). */
static JSValue f_stats(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    long pages = 0, rss = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) { if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0; fclose(f); }
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "rssKB", JS_NewInt64(c, rss * 4));
    JS_SetPropertyStr(c, o, "jsKB", JS_NewInt64(c, (int64_t)(JS_GetMallocSize(rt) / 1024)));
    JS_SetPropertyStr(c, o, "nodes", JS_NewInt32(c, node_count()));
    JS_SetPropertyStr(c, o, "layoutMs", JS_NewFloat64(c, ui.layout_ms));
    JS_SetPropertyStr(c, o, "paintMs", JS_NewFloat64(c, ui.paint_ms));
    return o;
}

static const JSCFunctionListEntry lord_funcs[] = {
    JS_CFUNC_DEF("createNode", 1, f_create_node),
    JS_CFUNC_DEF("setText", 2, f_set_text),
    JS_CFUNC_DEF("setStyle", 3, f_set_style),
    JS_CFUNC_DEF("setProp", 3, f_set_prop),
    JS_CFUNC_DEF("appendChild", 2, f_append),
    JS_CFUNC_DEF("insertBefore", 3, f_insert_before),
    JS_CFUNC_DEF("removeChild", 2, f_remove),
    JS_CFUNC_DEF("destroy", 1, f_destroy),
    JS_CFUNC_DEF("setRoot", 1, f_set_root),
    JS_CFUNC_DEF("getLayout", 1, f_get_layout),
    JS_CFUNC_DEF("now", 0, f_now),
    JS_CFUNC_DEF("log", 1, f_log),
    JS_CFUNC_DEF("stats", 0, f_stats),
};

/* Runs JS code. Only used with the internal prelude and with the theme bundle that
 * is passed on the command line (trusted local file); never with external data. */
static int eval(const char *src, size_t len, const char *name) {
    JSValue r = JS_Eval(ctx, src, len, name, JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(r);
    if (!ok) dump_exception(ctx);
    JS_FreeValue(ctx, r);
    drain_jobs();
    return ok ? 0 : -1;
}

static JSValue call_global(const char *name, int argc, JSValueConst *argv) {
    JSValue g = JS_GetGlobalObject(ctx), f = JS_GetPropertyStr(ctx, g, name), r = JS_UNDEFINED;
    if (JS_IsFunction(ctx, f)) {
        r = JS_Call(ctx, f, g, argc, argv);
        if (JS_IsException(r)) { dump_exception(ctx); r = JS_UNDEFINED; }
    }
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, g);
    return r;
}

/* Reads a bundle of LordOS's own code through the verified-code check (sec/codesig.c): the bytes
 * returned are exactly the ones that were hashed. NULL (and *refused = 1) if it was refused. */
static char *load_code(const char *path, size_t *len, int *refused) {
    codesig_init(js_sys_assets());
    char why[160];
    int before = codesig_refused_count();
    char *src = codesig_load(path, CODE_FILE_MAX, len, why, sizeof why);
    if (!src) {
        *refused = codesig_refused_count() > before || codesig_state() == CODE_INVALID;
        fprintf(stderr, "js: %s not loaded: %s\n", path, why);
    }
    return src;
}

/* A new context with the whole `lord` object of a session (or of the login screen). */
static void session_context(void) {
    rt = JS_NewRuntime();
    JS_SetMemoryLimit(rt, 256u << 20);
    JS_SetMaxStackSize(rt, 4u << 20);
    ctx = JS_NewContext(rt);

    JSValue g = JS_GetGlobalObject(ctx);
    JSValue lord = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, lord, lord_funcs, sizeof lord_funcs / sizeof lord_funcs[0]);
    JSValue screen = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, screen, "width", JS_NewFloat64(ctx, ui.width));
    JS_SetPropertyStr(ctx, screen, "height", JS_NewFloat64(ctx, ui.height));
    set_screen_displays(screen);
    JS_SetPropertyStr(ctx, lord, "screen", screen);
    JS_SetPropertyStr(ctx, lord, "version", JS_NewString(ctx, LORD_VERSION));
    js_sys_init();
    js_sys_register(ctx, lord);
    js_term_register(ctx, lord);
    js_pkg_register(ctx, lord);
    js_sound_register(ctx, lord);
    js_wasm_register(ctx, g);           /* the WebAssembly global (our own interpreter, src/wasm) */
    js_net_register(ctx, lord);
    js_monitor_register(ctx, lord, js_sys_assets());   /* lord.monitor (app Monitor) */
    js_sec_register(ctx, lord);        /* lord.perms, lord.sign and lord.code (privacy, signing, verified code) */
    js_media_register(ctx, lord);      /* lord.media (video, photos, screenshots, camera, recorder) */
    js_doc_register(ctx, lord);        /* lord.doc: PDF, archives and fonts through the lord-doc helper */
    js_print_register(ctx, lord);      /* lord.print: CUPS over IPP and eSCL scanners */
    js_vault_register(ctx, lord);      /* lord.vault: Passwords and the keyring through the lord-vault helper */
    js_mail_register(ctx, lord);       /* lord.mail: Mail, Calendar, Contacts, Reminders via lord-maild (docs/MAIL.md) */
    js_wm_register(ctx, lord);         /* lord.wm: moving and resizing windows in C (grab.c) */
    js_input_register(ctx, lord);      /* lord.input: touchpads and touchscreens (tp/) */
    js_fw_register(ctx, lord);         /* lord.fw: per-app network permissions (lord-netperm, docs/FIREWALL.md) */
    js_disk_register(ctx, lord);       /* lord.disks: disks and partitions (Disks app) */
    js_imager_register(ctx, lord);     /* lord.imager: LordOS Imager (docs/IMAGER.md) */
    js_backup_register(ctx, lord);     /* lord.backup: Backups (backup/) */
    js_logs_register(ctx, lord);       /* lord.logs: journal and LordOS logs (Logs app) */
    js_help_register(ctx, lord);       /* lord.help (Help) and lord.time.zoneOffset (Clock, tzif.c) */
    js_hw_register(ctx, lord);         /* lord.display/power/bt/mime/timedate (Settings, src/hw) */
    JS_SetPropertyStr(ctx, g, "lord", lord);
    JS_FreeValue(ctx, g);
}

int js_init(const char *bundle_path) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", bundle_path);
    snprintf(ui.theme_dir, sizeof ui.theme_dir, "%s", dirname(dir));

    size_t len = 0;
    int refused = 0;
    char *src = load_code(bundle_path, &len, &refused);
    if (!src) return refused ? JS_REFUSED : -1;

    session_context();
    double t0 = now_ms();
    int r = eval(prelude, sizeof prelude - 1, "<prelude>");
    if (!r) r = eval(src, len, bundle_path);
    fprintf(stderr, "js: theme %s loaded in %.1f ms (%zu KB)\n", bundle_path, now_ms() - t0, len / 1024);
    free(src);
    return r;
}

/* ---- the recovery screen ----
 * When the theme (or the login screen, or an isolated app's code) was refused, lord-server shows
 * this instead of a blank screen. It is compiled into the binary, so it is as trusted as
 * lord-server itself. Texts: the "shell" catalog (code.recovery.*) when it can be read, English
 * otherwise. */
static const char recovery_js[] =
    "(function(){\n"
    "const R = globalThis.__lordRecovery || {};\n"
    "const EN = {\n"
    "  'code.recovery.title': 'This LordOS component was modified and was not loaded',\n"
    "  'code.recovery.session': 'LordOS checks its own code before running it. A part of the desktop no longer matches the signed copy, so it was not started. Your files were not touched.',\n"
    "  'code.recovery.greeter': 'LordOS checks its own code before running it. The login screen no longer matches the signed copy, so it was not started. Your files were not touched.',\n"
    "  'code.recovery.app': 'LordOS checks its own code before running it. This app no longer matches the signed copy, so it was not started.',\n"
    "  'code.recovery.fix': 'To repair it, reinstall LordOS: sudo apt install --reinstall lordos',\n"
    "  'code.recovery.details': 'Not loaded:',\n"
    "  'code.recovery.logout': 'Log Out',\n"
    "};\n"
    "let cat = {};\n"
    "let lang = R.lang || 'en';\n"
    "try { const s = lord.settings && lord.settings.load && lord.settings.load(); const o = typeof s === 'string' ? JSON.parse(s) : s;\n"
    "  if (o && typeof o.language === 'string' && /^[a-z]{2,3}(-[A-Za-z0-9]+)?$/.test(o.language)) lang = o.language; } catch (e) { /* no settings */ }\n"
    "try { const l = String(lang); const txt = lord.locale && lord.locale.catalog && (lord.locale.catalog(l, 'shell') || lord.locale.catalog(l.split('-')[0], 'shell'));\n"
    "  if (txt) cat = JSON.parse(txt); } catch (e) { cat = {}; }\n"
    "const t = k => (typeof cat[k] === 'string' && cat[k]) || EN[k];\n"
    "const W = (lord.screen && lord.screen.width) || 800, H = (lord.screen && lord.screen.height) || 600;\n"
    "/* colors are ARGB numbers, as runtime/style.js sends them */\n"
    "const view = (style, parent) => { const id = lord.createNode('view'); for (const k in style) lord.setStyle(id, k, style[k]); if (parent) lord.appendChild(parent, id); return id; };\n"
    "let lastRaw = 0;\n"
    "const text = (s, style, parent) => { const id = lord.createNode('text'); for (const k in style) lord.setStyle(id, k, style[k]);\n"
    "  const raw = lord.createNode('raw'); lord.setText(raw, s); lord.appendChild(id, raw); lastRaw = raw; lord.appendChild(parent, id); return id; };\n"
    "const ds = (lord.screen && lord.screen.displays) || [];\n"
    "const d = ds.find(x => x && x.main) || ds[0] || { x: 0, y: 0, w: W, h: H };\n"
    "const app = R.kind === 'app';   /* an app window: fill it; a session: the main display */\n"
    "const root = view(app ? { flex: 1, backgroundColor: 0xff1c1c1e } : { width: W, height: H, backgroundColor: 0xff1c1c1e });\n"
    "const area = view(app ? { flex: 1, alignItems: 'stretch', justifyContent: 'center', paddingLeft: 16, paddingRight: 16 }\n"
    "  : { position: 'absolute', left: d.x, top: d.y, width: d.w, height: d.h, alignItems: 'center', justifyContent: 'center' }, root);\n"
    "const card = view({ ...(app ? {} : { width: Math.min(560, d.w - 40) }), paddingTop: 28, paddingBottom: 28, paddingLeft: 32, paddingRight: 32, borderRadius: 14,\n"
    "  backgroundColor: 0xff2c2c2e, gap: 12 }, area);\n"
    "const mark = view({ width: 44, height: 44, borderRadius: 22, backgroundColor: 0xffff9f0a, alignItems: 'center', justifyContent: 'center' }, card);\n"
    "text('!', { color: 0xffffffff, fontSize: 26, fontWeight: 700 }, mark);\n"
    "text(t('code.recovery.title'), { color: 0xffffffff, fontSize: 18, fontWeight: 600, textAlign: 'start' }, card);\n"
    "text(t('code.recovery.' + (R.kind || 'session')), { color: 0xffd1d1d6, fontSize: 14, lineHeight: 20, textAlign: 'start' }, card);\n"
    "const list = (R.refused || []).slice(0, 6);\n"
    "if (list.length) { text(t('code.recovery.details'), { color: 0xff98989d, fontSize: 12, textAlign: 'start' }, card);\n"
    "  for (const p of list) text(p, { color: 0xff98989d, fontSize: 12, fontFamily: 'monospace', direction: 'ltr', textAlign: 'start' }, card); }\n"
    "text(t('code.recovery.fix'), { color: 0xffd1d1d6, fontSize: 13, lineHeight: 19, textAlign: 'start' }, card);\n"
    "let btn = 0; const hits = new Set();\n"
    "if (R.kind === 'session' && typeof lord.quit === 'function') {\n"
    "  btn = view({ alignSelf: 'flex-end', paddingTop: 6, paddingBottom: 6, paddingLeft: 16, paddingRight: 16, borderRadius: 7, backgroundColor: 0xff0a84ff, cursor: 'pointer' }, card);\n"
    "  hits.add(btn); hits.add(text(t('code.recovery.logout'), { color: 0xffffffff, fontSize: 13, fontWeight: 600 }, btn)); hits.add(lastRaw);\n"
    "}\n"
    "globalThis.__lordEvent = (type, id) => { if (type === 'up' && hits.has(id)) lord.quit(); };\n"
    "globalThis.__lordKey = () => {};\n"
    "lord.setRoot(root);\n"
    "})();\n";

/* Fills __lordRecovery = { kind, lang, refused: [paths] } and runs the recovery screen: in a fresh
 * context (with the prelude), or in place over a running one (an isolated app whose own code was
 * refused after its host started), where the old timers and frames are stopped first. */
static int run_recovery(const char *kind, int inplace) {
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue o = JS_NewObject(ctx), arr = JS_NewArray(ctx);
    for (int i = 0; i < codesig_refused_count(); i++) JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, codesig_refused(i)->path));
    const char *lang = getenv("LANGUAGE");
    if (!lang || !lang[0]) lang = getenv("LANG");
    if (text_language()[0]) lang = text_language();     /* what the SDK showed until now (an app) */
    char l[16] = "en";
    if (lang && lang[0]) { snprintf(l, sizeof l, "%s", lang); l[strcspn(l, "_.:@")] = 0; }
    JS_SetPropertyStr(ctx, o, "kind", JS_NewString(ctx, kind));
    JS_SetPropertyStr(ctx, o, "lang", JS_NewString(ctx, l));
    JS_SetPropertyStr(ctx, o, "refused", arr);
    JS_SetPropertyStr(ctx, g, "__lordRecovery", o);
    JS_FreeValue(ctx, g);
    static const char stop[] = "globalThis.__lordTick = () => -1; globalThis.__lordWantsFrame = () => false; globalThis.__lordFrame = () => {};";
    int r = inplace ? eval(stop, sizeof stop - 1, "<recovery>") : eval(prelude, sizeof prelude - 1, "<prelude>");
    if (!r) r = eval(recovery_js, sizeof recovery_js - 1, "<recovery>");
    fprintf(stderr, "js: recovery screen (%s): %d component(s) refused\n", kind, codesig_refused_count());
    return r;
}

int js_init_recovery(int greeter) {
    session_context();
    return run_recovery(greeter ? "greeter" : "session", 0);
}

/* "App mode" (lord-server --app <id>): the same JS engine, but the `lord` object only has the
 * drawing primitives (local) plus the broker (fs/clipboard/perms through lord-server, which checks
 * the permissions). Loads the "app host" (React + SDK + the app), see app/appclient.c. */
static void app_context(const char *app_id) {
    rt = JS_NewRuntime();
    JS_SetMemoryLimit(rt, 256u << 20);
    JS_SetMaxStackSize(rt, 4u << 20);
    ctx = JS_NewContext(rt);
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue lord = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, lord, lord_funcs, sizeof lord_funcs / sizeof lord_funcs[0]);
    JSValue screen = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, screen, "width", JS_NewFloat64(ctx, ui.width));
    JS_SetPropertyStr(ctx, screen, "height", JS_NewFloat64(ctx, ui.height));
    JS_SetPropertyStr(ctx, lord, "screen", screen);
    JS_SetPropertyStr(ctx, lord, "version", JS_NewString(ctx, LORD_VERSION));
    js_broker_register(ctx, lord, app_id);         /* fs/clipboard/perms through the broker + appId */
    js_vault_register_app(ctx, lord);              /* Passwords only: its own channel to lord-vault */
    js_sys_register_app(ctx, lord);                /* lord.text (language, direction) and lord.image */
    js_wasm_register(ctx, g);                      /* WebAssembly for the app's loadWasm */
    JS_SetPropertyStr(ctx, g, "lord", lord);
    JS_FreeValue(ctx, g);
}

int js_init_app(const char *host_bundle, const char *app_id) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", host_bundle);
    snprintf(ui.theme_dir, sizeof ui.theme_dir, "%s", dirname(dir));
    /* the app host as bytecode lord-server compiled for this session (app/apphost_bc.c) */
    const char *bcs = getenv(APPHOST_BC_ENV);
    int bcfd = bcs ? atoi(bcs) : -1, bcok = 0, how = -1;
    unsetenv(APPHOST_BC_ENV);
    size_t len = 0;
    int refused = 0;
    char *src = load_code(host_bundle, &len, &refused);   /* verified (sec/codesig.c) */
    if (!src) {
        if (bcfd > 2) close(bcfd);
        if (!refused) return -1;
        app_context(app_id);                       /* the window shows why instead of closing */
        run_recovery("app", 0);
        return JS_REFUSED;
    }
    app_context(app_id);
    double t0 = now_ms();
    int r = eval(prelude, sizeof prelude - 1, "<prelude>");
    /* the bytecode runs only if it was compiled from exactly the bytes just verified; else the source */
    if (!r && bcfd > 2) {
        how = apphost_bc_eval(ctx, src, len, bcfd, &bcok);
        if (how == 1) { dump_exception(ctx); r = -1; }
        drain_jobs();
    }
    if (bcfd > 2) close(bcfd);
    if (!r && how < 0) r = eval(src, len, host_bundle);
    fprintf(stderr, "js: app %s loaded in %.1f ms%s\n", app_id, now_ms() - t0, how == 0 ? " (bytecode)" : "");
    free(src);
    return r;
}

/* The app's own code was refused after the host loaded (assets/sysapps/<id>/app.js): the same
 * recovery screen in the app's window, drawn over the running context (freeing it from here would
 * leave the broker's and the windows' references dangling). */
int js_app_recovery(void) {
    if (!ctx) return -1;
    return run_recovery("app", 1);
}

void js_shutdown(void) {
    media_shutdown();                   /* the lord-media helpers belong to this context */
    if (ctx) js_wasm_shutdown(ctx);
    if (ctx) JS_FreeContext(ctx);
    if (rt) JS_FreeRuntime(rt);
    ctx = NULL;
    rt = NULL;
}

double js_tick(double now) {
    JSValue arg = JS_NewFloat64(ctx, now);
    JSValue r = call_global("__lordTick", 1, &arg);
    double next = -1;
    JS_ToFloat64(ctx, &next, r);
    JS_FreeValue(ctx, r);
    drain_jobs();
    return next;
}

int js_wants_frame(void) {
    JSValue r = call_global("__lordWantsFrame", 0, NULL);
    int w = JS_ToBool(ctx, r);
    JS_FreeValue(ctx, r);
    return w;
}

void js_frame(double now) {
    JSValue arg = JS_NewFloat64(ctx, now);
    JS_FreeValue(ctx, call_global("__lordFrame", 1, &arg));
    drain_jobs();
}

void js_pointer(const char *type, int target, float x, float y, int button) {
    JSValueConst args[5] = {
        JS_NewString(ctx, type), JS_NewInt32(ctx, target),
        JS_NewFloat64(ctx, x), JS_NewFloat64(ctx, y), JS_NewInt32(ctx, button),
    };
    JS_FreeValue(ctx, call_global("__lordEvent", 5, args));
    JS_FreeValue(ctx, args[0]);
    drain_jobs();
}

void js_gesture(const char *kind, const char *phase, int target, float x, float y, double a, double b, double c, double d) {
    if (!ctx) return;
    JSValueConst args[9] = {
        JS_NewString(ctx, kind), JS_NewString(ctx, phase), JS_NewInt32(ctx, target), JS_NewFloat64(ctx, x), JS_NewFloat64(ctx, y),
        JS_NewFloat64(ctx, a), JS_NewFloat64(ctx, b), JS_NewFloat64(ctx, c), JS_NewFloat64(ctx, d),
    };
    JS_FreeValue(ctx, call_global("__lordGesture", 9, args));
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    drain_jobs();
}

/* Input method state → __lordIme({engine, preedit, candidates, selected, converting}). */
static void ime_event(void) {
    const ImeState *st = ime_state();
    JSValue o = JS_NewObject(ctx), list = JS_NewArray(ctx);
    for (int i = 0; i < st->ncand; i++) JS_SetPropertyUint32(ctx, list, (uint32_t)i, JS_NewString(ctx, st->cand[i]));
    JS_SetPropertyStr(ctx, o, "engine", JS_NewString(ctx, ime_current()));
    JS_SetPropertyStr(ctx, o, "preedit", JS_NewString(ctx, st->preedit));
    JS_SetPropertyStr(ctx, o, "candidates", list);
    JS_SetPropertyStr(ctx, o, "selected", JS_NewInt32(ctx, st->sel));
    JS_SetPropertyStr(ctx, o, "converting", JS_NewBool(ctx, st->converting));
    /* Japanese: [before, focused segment, after] to highlight it */
    int b = st->focus_begin, e = st->focus_end, len = (int)strlen(st->preedit);
    if (e > b && e <= len) {
        JSValue parts = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, parts, 0, JS_NewStringLen(ctx, st->preedit, (size_t)b));
        JS_SetPropertyUint32(ctx, parts, 1, JS_NewStringLen(ctx, st->preedit + b, (size_t)(e - b)));
        JS_SetPropertyUint32(ctx, parts, 2, JS_NewString(ctx, st->preedit + e));
        JS_SetPropertyStr(ctx, o, "segments", parts);
    }
    JS_FreeValue(ctx, call_global("__lordIme", 1, (JSValueConst *)&o));
    JS_FreeValue(ctx, o);
}

/* Text committed by the input method: arrives as a key that types that text. */
static void ime_commit(const char *text) {
    JSValueConst args[6] = { JS_NewInt32(ctx, 0), JS_NewInt32(ctx, 1), JS_NewString(ctx, text),
                             JS_NewString(ctx, text), JS_NewString(ctx, ""), JS_NewInt32(ctx, 0) };
    JS_FreeValue(ctx, call_global("__lordKey", 6, args));
    for (int i = 2; i < 5; i++) JS_FreeValue(ctx, args[i]);
}

/* Key → __lordKey(code, value, key, text, base, flags). With libxkbcommon (keyboard.c)
 * C does the translation; if no layout is loaded, only code and value. If there is an
 * active input method (ime.c) and a focused text field, the key goes through it first. */
void js_key(int code, int value) {
    KeyInfo k;
    kb_key(code, value, &k);
    if (!kb_ready()) {
        JSValueConst args[2] = { JS_NewInt32(ctx, code), JS_NewInt32(ctx, value) };
        JS_FreeValue(ctx, call_global("__lordKey", 2, args));
        drain_jobs();
        return;
    }
    if (wl_key(code, value, &k)) { drain_jobs(); return; }      /* the focused Wayland app */
    if (value) {
        char commit[512];
        int used = ime_key(&k, commit, sizeof commit);
        if (commit[0]) ime_commit(commit);
        if (ime_changed()) ime_event();
        if (used) { drain_jobs(); return; }
    }
    int flags = k.shift | k.ctrl << 1 | k.alt << 2 | k.altgr << 3 | k.meta << 4 | k.caps << 5 | k.dead << 6;
    JSValueConst args[6] = { JS_NewInt32(ctx, code), JS_NewInt32(ctx, value), JS_NewString(ctx, k.key),
                             JS_NewString(ctx, k.chr), JS_NewString(ctx, k.base), JS_NewInt32(ctx, flags) };
    JS_FreeValue(ctx, call_global("__lordKey", 6, args));
    for (int i = 2; i < 5; i++) JS_FreeValue(ctx, args[i]);
    drain_jobs();
}

/* Media (media/) → __lordMedia(id, type, JSON data). */
void js_media_event(int id, const char *type, const char *json) {
    if (!ctx) return;
    JSValueConst args[3] = { JS_NewInt32(ctx, id), JS_NewString(ctx, type), JS_ParseJSON(ctx, json, strlen(json), "<media>") };
    JS_FreeValue(ctx, call_global("__lordMedia", 3, args));
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
    drain_jobs();
}

/* Wayland app windows (wl/wl.c) → __lordWayland(id, type, JSON data). */
void js_wl_event(int id, const char *type, const char *json) {
    if (!ctx) return;
    JSValueConst args[3] = { JS_NewInt32(ctx, id), JS_NewString(ctx, type), JS_ParseJSON(ctx, json, strlen(json), "<wayland>") };
    JS_FreeValue(ctx, call_global("__lordWayland", 3, args));
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
    drain_jobs();
}

/* Sound (sound/snd.c): "state" with the full state, "peak" with the meter → __lordSound(type, data). */
void js_sound_event(const char *type, const char *json) {
    if (!ctx) return;
    JSValueConst args[2] = { JS_NewString(ctx, type), JS_ParseJSON(ctx, json, strlen(json), "<sound>") };
    JS_FreeValue(ctx, call_global("__lordSound", 2, args));
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    drain_jobs();
}

/* Backups (backup/backup.c): the status changed → __lordBackup(status). */
void js_backup_event(const char *json) {
    if (!ctx) return;
    JSValueConst arg = JS_ParseJSON(ctx, json, strlen(json), "<backup>");
    JS_FreeValue(ctx, call_global("__lordBackup", 1, &arg));
    JS_FreeValue(ctx, arg);
    drain_jobs();
}

/* Network (net/state.c): something changed in links, addresses or routes → __lordNet("link"|"addr"|"route"). */
/* Disks (disk/disk.c): something was mounted, unmounted, plugged in or changed → __lordDisks(what). */
void js_disk_event(const char *what) {
    if (!ctx) return;
    JSValueConst arg = JS_NewString(ctx, what);
    JS_FreeValue(ctx, call_global("__lordDisks", 1, &arg));
    JS_FreeValue(ctx, arg);
    drain_jobs();
}

void js_net_event(const char *what) {
    if (!ctx) return;
    JSValueConst arg = JS_NewString(ctx, what);
    JS_FreeValue(ctx, call_global("__lordNet", 1, &arg));
    JS_FreeValue(ctx, arg);
    drain_jobs();
}

/* Terminal events (title, directory, bell, exit...) → __lordTerm(id, type, data). */
/* Logs (js_logs.c): "result" (id of a finished query), "journal" or "file" (name)
 * → __lordLogs(type, id, name). */
void js_logs_event(const char *kind, int id, const char *name) {
    if (!ctx) return;
    JSValueConst args[3] = { JS_NewString(ctx, kind), JS_NewInt32(ctx, id), JS_NewString(ctx, name) };
    JS_FreeValue(ctx, call_global("__lordLogs", 3, args));
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[2]);
    drain_jobs();
}

/* Settings' hardware modules (src/hw): "display", "idle", "power", "bt", "bt-prompt", "bt-result",
 * "ppd", "ppd-result", "timedate" → __lordHw(kind, data). */
void js_hw_event(const char *kind, const char *json) {
    if (!ctx) return;
    JSValueConst args[2] = { JS_NewString(ctx, kind), JS_ParseJSON(ctx, json, strlen(json), "<hw>") };
    if (JS_IsException(args[1])) { JS_FreeValue(ctx, JS_GetException(ctx)); args[1] = JS_NULL; }
    JS_FreeValue(ctx, call_global("__lordHw", 2, args));
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    drain_jobs();
}

/* Privileged helper responses: kind = LOG | OK | ERR. */
void js_admin_event(int id, const char *kind, const char *text) {
    if (!ctx) return;
    JSValueConst args[3] = { JS_NewInt32(ctx, id), JS_NewString(ctx, kind), JS_NewString(ctx, text) };
    JS_FreeValue(ctx, call_global("__lordAdmin", 3, args));
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
    drain_jobs();
}

/* The clipboard changed (copy in the terminal, in a field, in a Linux app): notifies JS
 * (history). Called on every loop iteration; it only compares two numbers. */
void js_clipboard_poll(void) {
    static unsigned seen_text, seen_img;
    unsigned g = term_clipboard_gen(), gi = wl_clip_image_gen();
    if (!ctx || (g == seen_text && gi == seen_img)) return;
    seen_text = g;
    seen_img = gi;
    JS_FreeValue(ctx, call_global("__lordClipboard", 0, NULL));
    drain_jobs();
}

/* Remote desktop: who is watching this session from the network (JSON [{ip, user}]). */
void js_remote_event(const char *json) {
    if (!ctx) return;
    JSValueConst args[1] = { JS_NewString(ctx, json) };
    JS_FreeValue(ctx, call_global("__lordRemote", 1, args));
    JS_FreeValue(ctx, args[0]);
    drain_jobs();
}

/* Remote desktop: lord-sessiond's notice for the user on the monitor (RP_NOTICE, e.g. a source blocked
 * after failed logins) → __lordRemoteNotice(object). sdk/remote.js shows it as a notification. */
void js_remote_notice(const char *json) {
    if (!ctx) return;
    JSValue v = JS_ParseJSON(ctx, json, strlen(json), "<remote-notice>");
    if (JS_IsException(v)) { JS_FreeValue(ctx, JS_GetException(ctx)); return; }
    JSValueConst args[1] = { v };
    JS_FreeValue(ctx, call_global("__lordRemoteNotice", 1, args));
    JS_FreeValue(ctx, v);
    drain_jobs();
}

/* Broker of isolated .lpk apps: "prompt" (ask for a permission) or "device" (indicator) →
 * __lordBroker(kind, json). The theme handles it in sdk/privacy.js. */
void js_broker_event(const char *kind, const char *json) {
    if (!ctx) return;
    JSValueConst args[2] = { JS_NewString(ctx, kind), JS_ParseJSON(ctx, json, strlen(json), "<broker>") };
    JS_FreeValue(ctx, call_global("__lordBroker", 2, args));
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    drain_jobs();
}

/* Process of an isolated app: events lord-server sends through the broker (theme, language…) →
 * __lordBrokerEvent(type, data). See runtime/app-host.js. */
void js_broker_app_event(const char *kind, const char *json, size_t len) {
    if (!ctx) return;
    JSValueConst args[2] = { JS_NewString(ctx, kind), JS_ParseJSON(ctx, json, len, "<broker>") };
    if (JS_IsException(args[1])) { JS_FreeValue(ctx, JS_GetException(ctx)); args[1] = JS_NULL; }
    JS_FreeValue(ctx, call_global("__lordBrokerEvent", 2, args));
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    drain_jobs();
}

/* Login screen (greeter theme): notices and responses from lord-sessiond. */
void js_greeter_event(const char *json) {
    if (!ctx) return;
    JSValueConst args[1] = { JS_NewString(ctx, json) };
    JS_FreeValue(ctx, call_global("__lordGreeter", 1, args));
    JS_FreeValue(ctx, args[0]);
    drain_jobs();
}

/* The screen changed size: lord.screen is updated and the theme is notified (__lordScreen). */
/* The displays (docs/DISPLAYS.md): main.c keeps the JSON; lord.screen.displays and .mirror. */
static char displays_json[4096] = "{\"mirror\":false,\"displays\":[]}";
void js_set_displays(const char *json) { snprintf(displays_json, sizeof displays_json, "%s", json); }
static void set_screen_displays(JSValue screen) {
    JSValue d = JS_ParseJSON(ctx, displays_json, strlen(displays_json), "<displays>");
    if (JS_IsException(d)) { JS_FreeValue(ctx, JS_GetException(ctx)); return; }
    JS_SetPropertyStr(ctx, screen, "displays", JS_GetPropertyStr(ctx, d, "displays"));
    JS_SetPropertyStr(ctx, screen, "mirror", JS_GetPropertyStr(ctx, d, "mirror"));
    JS_FreeValue(ctx, d);
}

void js_screen_resized(void) {
    if (!ctx) return;
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue lord = JS_GetPropertyStr(ctx, g, "lord");
    JSValue screen = JS_GetPropertyStr(ctx, lord, "screen");
    if (JS_IsObject(screen)) {
        JS_SetPropertyStr(ctx, screen, "width", JS_NewFloat64(ctx, ui.width));
        JS_SetPropertyStr(ctx, screen, "height", JS_NewFloat64(ctx, ui.height));
        set_screen_displays(screen);
    }
    JS_FreeValue(ctx, screen);
    JS_FreeValue(ctx, lord);
    JS_FreeValue(ctx, g);
    JS_FreeValue(ctx, call_global("__lordScreen", 0, NULL));
    drain_jobs();
}

void js_term_event(int id, const char *type, const char *data) {
    if (!ctx) return;
    JSValueConst args[3] = { JS_NewInt32(ctx, id), JS_NewString(ctx, type), JS_NewString(ctx, data) };
    JS_FreeValue(ctx, call_global("__lordTerm", 3, args));
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
    drain_jobs();
}
