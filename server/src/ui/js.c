/* js.c — embedded QuickJS. Exposes the global `lord` object (create/modify nodes) and
 * provides setTimeout, requestAnimationFrame, console and performance, which React needs. */
#include "../app/apphost_bc.h"
#include <unistd.h>
#include "js.h"
#include "../ime.h"
#include "../wl/wl.h"
#include "../wl/wl_input.h"
#include "../keyboard.h"
#include "js_sys.h"
#include "js_term.h"
#include "js_pkg.h"
#include "../apps.h"
#include "js_sound.h"
#include "wave.h"
#include "js_monitor.h"
#include "js_sec.h"
#include "js_doc.h"
#include "js_print.h"
#include "js_vault.h"
#include "js_mail.h"
#include "js_sql.h"
#include "js_weather.h"
#include "js_webapps.h"
#include "js_sftp.h"
#include "js_termconf.h"
#include "js_input.h"
#include "js_fw.h"
#include "js_backup.h"
#include "js_cloud.h"
#include "js_clean.h"
#include "js_lan.h"
#include "js_rdesk.h"
#include "js_vm.h"
#include "js_guestwin.h"
#include "js_broker.h"
#include "js_wasm.h"
#include "../media/svgicon.h"
#include "js_net.h"
#include "js_media.h"
#include "js_wm.h"
#include "js_disk.h"
#include "../fsasync.h"
#include "js_imager.h"
#include "js_logs.h"
#include "js_flatpak.h"
#include "js_help.h"
#include "js_voice.h"
#include "js_hw.h"
#include "portal.h"
#include "../wl/keygrab.h"
#include "../media/media.h"
#include "../sec/codesig.h"
#include "../wdog.h"
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

#include "js_prelude.h"         /* the polyfills: console, timers, animation frames */

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

/* ---- the main-thread stall detector (docs/MAIN-THREAD.md) ----
 * Everything the desktop draws waits for the main thread, the pointer too: a JavaScript task that runs for 200 ms
 * stops the pointer for 200 ms. Every entry from C into JavaScript (an event, a timer tick, a frame, a reply, the
 * promise jobs after it) is timed with two clock reads; one that ran longer than STALL_MS is logged with what it
 * was, whose window it was for (__lordStallWho, runtime/events.js) and where the JavaScript was when it passed
 * STALL_MS (a stack sample taken from QuickJS's interrupt handler, every 10000 bytecode steps). Native lord.<ns>.*
 * calls are timed too (js_prof_wrap) and logged by name past SLOW_CALL_MS; layout and paint report through
 * js_stall_note (main.c). At most STALL_LINES lines per STALL_WINDOW_MS; the rest are counted. The counts are in
 * lord.stats() (stalls, stallWorstMs, slowCalls). LORD_STALL_MS overrides the threshold (tests). */
#define STALL_MS 50
#define SLOW_CALL_MS 20
#define STALL_LINES 30
#define STALL_WINDOW_MS 10000

static struct {
    double limit;                        /* STALL_MS, or LORD_STALL_MS */
    int count, slow_calls, lines, dropped;
    double busy_js, busy_layout, busy_paint, max_run;   /* totals for lord.stats(true) (the perf driver) */
    double worst, window_at;
    int depth;                           /* nesting of timed entries: only the outermost one is timed */
    double t0;
    int sampled;
    char sample[384];                    /* where the JS was when it passed the limit */
} stall;

static double stall_limit(void) {
    if (stall.limit <= 0) {
        const char *e = getenv("LORD_STALL_MS");
        double v = e ? atof(e) : 0;
        stall.limit = v > 0 ? v : STALL_MS;
    }
    return stall.limit;
}

/* one line, rate-limited; kind: "js" "call" "layout" "paint" */
static void stall_line(const char *kind, const char *what, const char *who, const char *where, double ms) {
    double now = now_ms();
    if (now - stall.window_at > STALL_WINDOW_MS) {
        if (stall.dropped) fprintf(stderr, "lord-server: stall: %d more not logged\n", stall.dropped);
        stall.window_at = now;
        stall.lines = stall.dropped = 0;
    }
    if (stall.lines >= STALL_LINES) { stall.dropped++; return; }
    stall.lines++;
    fprintf(stderr, "lord-server: stall: %.1f ms %s %s%s%s%s%s\n", ms, kind, what ? what : "?",
            who && who[0] ? " [" : "", who && who[0] ? who : "", who && who[0] ? "]" : "",
            where && where[0] ? where : "");
}

void js_stall_note(const char *kind, const char *what, double ms) {
    if (kind[0] == 'l') stall.busy_layout += ms;
    else stall.busy_paint += ms;
    if (ms <= stall_limit()) return;
    stall.count++;
    if (ms > stall.worst) stall.worst = ms;
    stall_line(kind, what, NULL, NULL, ms);
}

/* QuickJS's interrupt handler: the first time a timed entry passes the limit, the JS stack (its innermost frames,
 * file names cut to their base name) */
static void stall_sample(void) {
    if (!stall.depth || stall.sampled || !ctx || JS_HasException(ctx)) return;
    if (now_ms() - stall.t0 <= stall_limit()) return;
    stall.sampled = 1;
    static const char probe[] = "new Error().stack";
    JSValue st = JS_Eval(ctx, probe, sizeof probe - 1, "<stall>", JS_EVAL_TYPE_GLOBAL);
    const char *s = JS_IsString(st) ? JS_ToCString(ctx, st) : NULL;
    size_t o = 0;
    int frames = 0;
    stall.sample[0] = 0;
    for (const char *p = s; p && *p && frames < 5; ) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char line[256];
        snprintf(line, sizeof line, "%.*s", (int)(n < 255 ? n : 255), p);
        p = nl ? nl + 1 : p + n;
        char *a = strstr(line, "at ");
        if (!a || strstr(line, "<stall>")) continue;
        a += 3;
        char *paren = strchr(a, '(');                  /* "name (/path/file.js:1:234)" → "name (file.js:1:234)" */
        if (paren) {
            char *slash = strrchr(paren, '/');
            if (slash) memmove(paren + 1, slash + 1, strlen(slash + 1) + 1);
        }
        o += (size_t)snprintf(stall.sample + o, o < sizeof stall.sample ? sizeof stall.sample - o : 0, "%s%s", frames ? " < " : " at ", a);
        if (o >= sizeof stall.sample) { o = sizeof stall.sample - 1; break; }
        frames++;
    }
    if (s) JS_FreeCString(ctx, s);
    if (JS_IsException(st)) JS_FreeValue(ctx, JS_GetException(ctx));
    else JS_FreeValue(ctx, st);
}

/* A call from C (call_global_atom) and the promise jobs right after it (drain_jobs) are one run: React's
 * synchronous render after a click is a job. The call leaves the run open with its name; drain_jobs closes it, or
 * the next run does (a call with no jobs after it). */
static struct { int open; double t_end; JSAtom name; JSValue detail; } run;

static void js_run_finish(void);

static void js_run_begin(void) {
    if (stall.depth == 0 && run.open) js_run_finish();
    if (stall.depth++ == 0) { stall.t0 = now_ms(); stall.sampled = 0; stall.sample[0] = 0; }
}

/* the outermost call returned: the run stays open for its jobs (name and first argument kept) */
static void js_run_called(JSAtom name, int argc, JSValueConst *argv) {
    if (--stall.depth > 0) return;
    run.open = 1;
    run.t_end = now_ms();
    run.name = JS_DupAtom(ctx, name);
    run.detail = argc > 0 && JS_IsString(argv[0]) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
}

/* where: what the JS was doing when it passed the limit (the sample); who: __lordStallWho(what) */
static void stall_report(const char *what, const char *detail, double ms) {
    stall.count++;
    if (ms > stall.worst) stall.worst = ms;
    char who[160] = "", name[128];
    snprintf(name, sizeof name, "%s%s%s", what, detail ? " " : "", detail ? detail : "");
    /* whose it was: runtime/events.js knows the window under the last event and the timers that just ran */
    stall.depth++;                        /* this call is not a run of its own */
    JSValue g = JS_GetGlobalObject(ctx), f = JS_GetPropertyStr(ctx, g, "__lordStallWho");
    if (JS_IsFunction(ctx, f)) {
        JSValue arg = JS_NewString(ctx, what), r = JS_Call(ctx, f, g, 1, &arg);
        const char *s = JS_IsString(r) ? JS_ToCString(ctx, r) : NULL;
        if (s) { snprintf(who, sizeof who, "%s", s); JS_FreeCString(ctx, s); }
        if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, arg);
    }
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, g);
    stall.depth--;
    stall_line("js", name, who, stall.sample, ms);
}

static void js_run_finish(void) {
    if (!run.open) return;
    run.open = 0;
    double ms = run.t_end - stall.t0;
    stall.busy_js += ms;
    if (ms > stall.max_run) stall.max_run = ms;
    if (ms > stall_limit()) {
        const char *n = JS_AtomToCString(ctx, run.name);
        const char *d = JS_IsString(run.detail) ? JS_ToCString(ctx, run.detail) : NULL;
        stall_report(n ? n : "?", d, ms);
        if (d) JS_FreeCString(ctx, d);
        if (n) JS_FreeCString(ctx, n);
    }
    JS_FreeAtom(ctx, run.name);
    JS_FreeValue(ctx, run.detail);
    run.detail = JS_UNDEFINED;
}

/* a run that isn't a call_global_atom (js_wm_end, jobs with no call before them) */
static void js_run_end(const char *what, const char *detail) {
    if (--stall.depth > 0) return;
    double ms = now_ms() - stall.t0;
    stall.busy_js += ms;
    if (ms > stall.max_run) stall.max_run = ms;
    if (ms > stall_limit()) stall_report(what, detail, ms);
}

void js_stall_begin(void) { js_run_begin(); }
void js_stall_end(const char *what) { js_run_end(what, NULL); }

/* lord.<ns>.<fn>(): the native call, timed; data[0] the real function, data[1] its name ("pkg.apps") */
static JSValue prof_call(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValue *data) {
    (void)magic;
    double t0 = now_ms();
    JSValue r = JS_Call(c, data[0], this_val, argc, argv);
    double ms = now_ms() - t0;
    if (ms > SLOW_CALL_MS) {
        stall.slow_calls++;
        const char *n = JS_ToCString(c, data[1]);
        stall_line("call", n ? n : "?", NULL, NULL, ms);
        if (n) JS_FreeCString(c, n);
    }
    return r;
}

/* Every function of every lord.<ns> object (and one level below: lord.settings.x, lord.fs.trash.y) is replaced by a
 * timed one with the same length. Done once the native API is complete, before any JavaScript runs. */
static void prof_wrap_obj(JSContext *c, JSValueConst obj, const char *prefix, int level) {
    JSPropertyEnum *props = NULL;
    uint32_t n = 0;
    if (JS_GetOwnPropertyNames(c, &props, &n, obj, JS_GPN_STRING_MASK)) return;
    for (uint32_t i = 0; i < n; i++) {
        JSPropertyDescriptor d;
        int has = JS_GetOwnProperty(c, &d, obj, props[i].atom);
        if (has <= 0) continue;
        int data_prop = !(d.flags & JS_PROP_GETSET);
        const char *key = JS_AtomToCString(c, props[i].atom);
        char name[96];
        snprintf(name, sizeof name, "%s%s%s", prefix, prefix[0] ? "." : "", key ? key : "?");
        if (data_prop && JS_IsFunction(c, d.value) && level > 0) {
            int len = 0;
            JSValue l = JS_GetPropertyStr(c, d.value, "length");
            JS_ToInt32(c, &len, l);
            JS_FreeValue(c, l);
            JSValue data[2] = { d.value, JS_NewString(c, name) };
            JSValue w = JS_NewCFunctionData(c, prof_call, len, 0, 2, data);
            JS_FreeValue(c, data[1]);
            JS_DefinePropertyValue(c, obj, props[i].atom, w, d.flags & (JS_PROP_WRITABLE | JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE));
        } else if (data_prop && JS_IsObject(d.value) && !JS_IsFunction(c, d.value) && !JS_IsArray(c, d.value) && level < 2) {
            prof_wrap_obj(c, d.value, name, level + 1);
        }
        if (key) JS_FreeCString(c, key);
        JS_FreeValue(c, d.value);
        JS_FreeValue(c, d.getter);
        JS_FreeValue(c, d.setter);
    }
    for (uint32_t i = 0; i < n; i++) JS_FreeAtom(c, props[i].atom);
    js_free(c, props);
}

int js_stall_count(void) { return stall.count; }

static void drain_jobs(void) {
    JSContext *c;
    int r;
    if (!JS_IsJobPending(rt)) { if (stall.depth == 0) js_run_finish(); return; }
    int cont = stall.depth == 0 && run.open;          /* the jobs of the call that just returned: the same run */
    if (cont) stall.depth++;
    else js_run_begin();
    while ((r = JS_ExecutePendingJob(rt, &c)) != 0)
        if (r < 0) dump_exception(c);
    if (cont) { stall.depth--; run.t_end = now_ms(); js_run_finish(); }
    else js_run_end("jobs", NULL);
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
             : !strcmp(t, "surface") ? NODE_SURFACE : !strcmp(t, "media") ? NODE_MEDIA
             : !strcmp(t, "wave") ? NODE_WAVE : NODE_VIEW;
    JS_FreeCString(c, t);
    Node *n = node_new(type);
    if (n && type == NODE_WAVE && !(n->wave = wave_conf_new())) { node_destroy(n); n = NULL; }
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

/* "js: unsupported style '<key>'" is logged once per key: a style sent every frame (an animation) would otherwise
 * write a line per frame. The first STYLE_WARN_MAX keys are remembered (compared on their first
 * STYLE_WARN_KEY - 1 bytes); the next new one is logged with a note that further unknown keys are not, and
 * nothing after it. Main thread only (setStyle runs there). */
#define STYLE_WARN_MAX 64
#define STYLE_WARN_KEY 48

static void style_warn(const char *key) {
    static char seen[STYLE_WARN_MAX][STYLE_WARN_KEY];
    static int nseen;                   /* STYLE_WARN_MAX + 1 once the last line was written */
    for (int i = 0; i < nseen && i < STYLE_WARN_MAX; i++)
        if (!strncmp(seen[i], key, STYLE_WARN_KEY - 1)) return;
    if (nseen > STYLE_WARN_MAX) return;
    if (nseen == STYLE_WARN_MAX) {
        fprintf(stderr, "js: unsupported style '%s' (further unknown style keys are not logged)\n", key);
        nseen++;
        return;
    }
    snprintf(seen[nseen++], STYLE_WARN_KEY, "%s", key);
    fprintf(stderr, "js: unsupported style '%s'\n", key);
}

/* Reading a JS value can run the caller's code (a getter, a Proxy trap, valueOf), and that code can lord.destroy() the
 * node. So every value that may be an object is read before the node is looked up, and the node pointer is never held
 * across such a read (Claude Security, 0.8.9: setProp 'wave' and setStyle 'gradient'). */
static JSValue f_set_style(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    const char *key = JS_ToCString(c, argv[1]);
    JSValueConst v = argv[2];
    double grad[3] = {180, 0, 0};
    int grad_on = 0;
    if (key && !strcmp(key, "gradient") && JS_IsArray(c, v)) {
        grad_on = 1;
        for (int i = 0; i < 3; i++) {
            JSValue e = JS_GetPropertyUint32(c, v, (uint32_t)i);
            if (JS_IsNumber(e)) JS_ToFloat64(c, &grad[i], e);
            JS_FreeValue(c, e);
        }
    }
    Node *n = arg_node(c, argv[0]);
    if (!n || !key) { if (key) JS_FreeCString(c, key); return JS_UNDEFINED; }
    int err = 0;
    Style before = n->s;
    StyleExt ext_before;
    if (n->s.ext) memcpy(&ext_before, n->s.ext, STYLE_EXT_VALUES);
    if (!strcmp(key, "gradient")) {
        if (grad_on) {
            n->s.has_gradient = 1;
            n->s.grad_angle = (float)grad[0];
            n->s.grad_from = (uint32_t)(int64_t)grad[1];
            n->s.grad_to = (uint32_t)(int64_t)grad[2];
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
    if (err) style_warn(key);
    JS_FreeCString(c, key);
    node_restyled(n, &before, before.ext ? &ext_before : NULL);     /* node.c (engine-test does the same) */
    return JS_UNDEFINED;
}

static JSValue f_set_prop(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    const char *key = JS_ToCString(c, argv[1]);         /* before the node: see f_set_style */
    Node *n = arg_node(c, argv[0]);
    if (n && key && !strcmp(key, "src")) {
        image_release(n->img);
        n->img = NULL;
        const char *src = JS_IsString(argv[2]) ? JS_ToCString(c, argv[2]) : NULL;
        if (src) {
            char path[2048];
            /* "thumb:<px>[:<stamp>]:<absolute path | asset:path>": a picture shown small (Files, the file dialog), decoded off this thread to
             * fit px x px (image.c): a folder of photos never holds full-size pictures or waits on a slow disk */
            const char *num = !strncmp(src, "thumb:", 6) ? src + 6 : NULL;
            size_t nl = num ? strspn(num, "0123456789") : 0;
            /* "svg:<px>:<rrggbb|->:<absolute path>" (sdk/iconsrc.js) and any .svg file: drawn by lord-media, never
             * parsed here (media/svgicon.c); empty until it answers */
            int svg = !num && !strncmp(src, "svg:", 4);
            size_t sl = strlen(src);
            if (svg) {
                /* "svg:<px>:<hex>:asset:<path>": an icon of LordOS's own assets, tinted like a symbolic one (Machines'
                 * sidebar and toolbar glyphs); never "..", never outside the assets folder */
                const char *as = strstr(src, ":asset:");
                char spec[2300];
                if (as && !strstr(as, "..") && (size_t)(as - src) < 40 &&
                    snprintf(spec, sizeof spec, "%.*s:%s/%s", (int)(as - src), src, js_sys_assets(), as + 7) < (int)sizeof spec)
                    n->img = svgicon_get_spec(spec);
                else n->img = svgicon_get_spec(src);
            }
            else if (num) {
                /* an optional ":<stamp>" after the size, the file's "<mtime>.<size>" (sdk/filekinds.js thumbSrc): a
                 * picture replaced under the same name gets its own thumbnail (GitHub #180). The path starts with '/'. */
                const char *rest = nl && nl <= 5 && num[nl] == ':' ? num + nl + 1 : NULL;
                char stamp[IMG_STAMP_MAX + 1] = "";
                size_t vl = rest && rest[0] != '/' ? strspn(rest, "0123456789.") : 0;
                if (vl && vl <= IMG_STAMP_MAX && rest[vl] == ':' && rest[vl + 1] == '/') {
                    memcpy(stamp, rest, vl);
                    stamp[vl] = 0;
                    rest += vl + 1;
                }
                /* the path may also be an "asset:" one (0.8.9: Lord Workspaces' wallpaper slideshow decodes the next picture
                 * off this thread, sdk/slideshow.jsx) */
                if (rest && rest[0] == '/') n->img = image_get_thumb(rest, atoi(num), stamp);
                else if (rest && !strncmp(rest, "asset:", 6) && !strstr(rest, "..")) {
                    snprintf(path, sizeof path, "%s/%s", js_sys_assets(), rest + 6);
                    n->img = image_get_thumb(path, atoi(num), stamp);
                }
            } else if (src[0] == '/') snprintf(path, sizeof path, "%s", src);
            else if (!strncmp(src, "asset:", 6) && !strstr(src, "..")) snprintf(path, sizeof path, "%s/%s", js_sys_assets(), src + 6);
            else snprintf(path, sizeof path, "%s/%s", ui.theme_dir, src);
            if (!num && !svg) n->img = sl > 4 && !strcmp(src + sl - 4, ".svg") ? svgicon_get(path, SVGICON_DEFAULT_SIDE, -1) : image_get(path);
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
    } else if (n && key && !strcmp(key, "crop") && n->type == NODE_MEDIA) {
        /* [x, y, w, h] in the source picture's pixels: the node shows that part (Machines' apps mode); null: all of it */
        double v[4];
        int ok = JS_IsArray(c, argv[2]);
        for (int i = 0; i < 4 && ok; i++) {
            JSValue e = JS_GetPropertyUint32(c, argv[2], (uint32_t)i);
            ok = JS_IsNumber(e) && !JS_ToFloat64(c, &v[i], e) && v[i] == v[i] && v[i] > -65536 && v[i] < 65536;
            JS_FreeValue(c, e);
        }
        n = arg_node(c, argv[0]);               /* the reads above can run getters (see f_set_style) */
        if (n && n->type == NODE_MEDIA) {
            if (ok && v[2] >= 1 && v[3] >= 1) {
                if (!n->crop) n->crop = malloc(4 * sizeof(float));
                if (n->crop) for (int i = 0; i < 4; i++) n->crop[i] = (float)v[i];
            } else { free(n->crop); n->crop = NULL; }
            node_invalidate(n);
        }
    } else if (n && key && !strcmp(key, "media") && n->type == NODE_MEDIA) {
        int32_t id = 0;                     /* lord.media source (video, photo, screenshot, camera) */
        if (JS_IsNumber(argv[2])) JS_ToInt32(c, &id, argv[2]);
        js_media_node(n, id);
        node_invalidate(n);
    } else if (n && key && !strcmp(key, "wave") && n->type == NODE_WAVE && n->wave && JS_IsObject(argv[2])) {
        /* the look of <SoundWave>: { lines, colors: [0xRRGGBB × 3], gain, hideIdle } (sdk/deskwidgets.jsx). The
         * property reads can run the caller's getters, which may destroy the node: read everything first, then
         * look the node up again by its id (see f_set_style). */
        int nid = n->id;
        uint64_t serial = n->serial;           /* ids are reused; the serial never is */
        double d, lines = -1, gain = -1, col[3] = { -1, -1, -1 };
        int hide = -1;
        JSValue v = JS_GetPropertyStr(c, argv[2], "lines");
        if (JS_IsNumber(v) && !JS_ToFloat64(c, &d, v) && d >= 2 && d <= 24) lines = d;
        JS_FreeValue(c, v);
        v = JS_GetPropertyStr(c, argv[2], "gain");
        if (JS_IsNumber(v) && !JS_ToFloat64(c, &d, v) && d >= 0.25 && d <= 3) gain = d;
        JS_FreeValue(c, v);
        v = JS_GetPropertyStr(c, argv[2], "hideIdle");
        if (JS_IsBool(v)) hide = JS_ToBool(c, v);
        JS_FreeValue(c, v);
        v = JS_GetPropertyStr(c, argv[2], "colors");
        for (uint32_t i = 0; i < 3 && JS_IsArray(c, v); i++) {
            JSValue e = JS_GetPropertyUint32(c, v, i);
            if (JS_IsNumber(e) && !JS_ToFloat64(c, &d, e) && d >= 0 && d <= 0xffffff) col[i] = d;
            JS_FreeValue(c, e);
        }
        JS_FreeValue(c, v);
        n = node_ref(nid, serial);
        if (n && n->type == NODE_WAVE && n->wave) {
            WaveConf *w = n->wave;
            if (lines >= 0) w->lines = (int)lines;
            if (gain >= 0) w->gain = (float)gain;
            if (hide >= 0) w->hide_idle = hide;
            for (int i = 0; i < 3; i++) if (col[i] >= 0) w->col[i] = (uint32_t)col[i];
            wave_conf_changed(w);
            node_invalidate(n);
        }
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
    (void)this_val;
    long pages = 0, rss = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) { if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0; fclose(f); }
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "rssKB", JS_NewInt64(c, rss * 4));
    JS_SetPropertyStr(c, o, "jsKB", JS_NewInt64(c, (int64_t)(JS_GetMallocSize(rt) / 1024)));
    JS_SetPropertyStr(c, o, "nodes", JS_NewInt32(c, node_count()));
    JS_SetPropertyStr(c, o, "layoutMs", JS_NewFloat64(c, ui.layout_ms));
    JS_SetPropertyStr(c, o, "paintMs", JS_NewFloat64(c, ui.paint_ms));
    JS_SetPropertyStr(c, o, "stalls", JS_NewInt32(c, stall.count));             /* main-thread pauses past STALL_MS */
    JS_SetPropertyStr(c, o, "stallWorstMs", JS_NewFloat64(c, stall.worst));
    JS_SetPropertyStr(c, o, "slowCalls", JS_NewInt32(c, stall.slow_calls));
    int32_t more = 0;
    if (argc > 0) JS_ToInt32(c, &more, argv[0]);
    if (more >= 1) {                    /* lord.stats(1): the main thread's busy totals and longest run since the last such call */
        JS_SetPropertyStr(c, o, "busyJsMs", JS_NewFloat64(c, stall.busy_js));
        JS_SetPropertyStr(c, o, "busyLayoutMs", JS_NewFloat64(c, stall.busy_layout));
        JS_SetPropertyStr(c, o, "busyPaintMs", JS_NewFloat64(c, stall.busy_paint));
        JS_SetPropertyStr(c, o, "maxRunMs", JS_NewFloat64(c, stall.max_run));
        stall.max_run = 0;
    }
    if (more >= 2) {                    /* lord.stats(2): the heap after a full collection (takes a while) */
        JS_RunGC(rt);
        JSMemoryUsage u;
        JS_ComputeMemoryUsage(rt, &u);
        JS_SetPropertyStr(c, o, "jsUsedKB", JS_NewInt64(c, (int64_t)(u.memory_used_size >> 10)));
        JS_SetPropertyStr(c, o, "jsObjects", JS_NewInt64(c, (int64_t)u.obj_count));
        JS_SetPropertyStr(c, o, "jsMallocKB", JS_NewInt64(c, (int64_t)(u.malloc_size >> 10)));
    }
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

static JSValue call_global_atom(JSAtom name, int argc, JSValueConst *argv) {
    JSValue g = JS_GetGlobalObject(ctx), f = JS_GetProperty(ctx, g, name), r = JS_UNDEFINED;
    if (JS_IsFunction(ctx, f)) {
        js_run_begin();
        r = JS_Call(ctx, f, g, argc, argv);
        if (JS_IsException(r)) { dump_exception(ctx); r = JS_UNDEFINED; }
        js_run_called(name, argc, argv);
    }
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, g);
    return r;
}

static JSValue call_global(const char *name, int argc, JSValueConst *argv) {
    JSAtom a = JS_NewAtom(ctx, name);
    JSValue r = call_global_atom(a, argc, argv);
    JS_FreeAtom(ctx, a);
    return r;
}

/* The globals called on every pass of the main loop: their atoms are made once per runtime (the
 * function is still looked up on each call, so the recovery screen's replacements are the ones run) */
enum { HOT_TICK, HOT_WANTS_FRAME, HOT_FRAME, HOT_N };
static const char *const hot_name[HOT_N] = { "__lordTick", "__lordWantsFrame", "__lordFrame" };
static JSAtom hot_atom[HOT_N];
static JSRuntime *hot_rt;

static JSValue call_hot(int which, int argc, JSValueConst *argv) {
    if (hot_rt != rt) {
        for (int i = 0; i < HOT_N; i++) hot_atom[i] = JS_NewAtom(ctx, hot_name[i]);
        hot_rt = rt;
    }
    return call_global_atom(hot_atom[which], argc, argv);
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

/* QuickJS asks every 10000 bytecode steps whether to go on (wdog.h): only while the compositor's main loop
 * has stalled does the watchdog answer anything else. 1: the JS stack is logged (an error made here and
 * dropped, an exception in flight left alone) and the task goes on; 2: the task is stopped with an
 * uncatchable "interrupted" error, which the caller logs with its stack. */
static int on_interrupt(JSRuntime *r, void *opaque) {
    (void)r;
    (void)opaque;
    stall_sample();
    int w = wdog_js_poll();
    if (w == 1 && ctx && !JS_HasException(ctx)) {
        /* inside the interpreter QuickJS adds an error's stack only while it unwinds: an Error made by a
         * line of JS called from here has every frame below it */
        static const char probe[] = "new Error().stack";
        JSValue st = JS_Eval(ctx, probe, sizeof probe - 1, "<watchdog>", JS_EVAL_TYPE_GLOBAL);
        const char *s = JS_IsString(st) ? JS_ToCString(ctx, st) : NULL;
        fprintf(stderr, "lord-server: watchdog: the JavaScript stack:\n%s\n", s ? s : "(none)");
        if (s) JS_FreeCString(ctx, s);
        if (JS_IsException(st)) JS_FreeValue(ctx, JS_GetException(ctx));
        else JS_FreeValue(ctx, st);
        return 0;
    }
    if (w == 2) fprintf(stderr, "lord-server: watchdog: the JavaScript task stopped (its stack follows)\n");
    return w == 2;
}

/* (memory report) what the session's QuickJS runtime holds, in KB: JS_ComputeMemoryUsage walks every object, a few ms */
void js_mem_line(char *b, size_t n) {
    if (!rt) { snprintf(b, n, "js none"); return; }
    JSMemoryUsage u;
    JS_ComputeMemoryUsage(rt, &u);
    snprintf(b, n, "js malloc=%lldK used=%lldK objs=%lld/%lldK funcs=%lld/%lldK(code %lldK pc2line %lldK) strs=%lld/%lldK "
             "atoms=%lld/%lldK shapes=%lld/%lldK props=%lldK arrays=%lld bin=%lldK",
             (long long)u.malloc_size >> 10, (long long)u.memory_used_size >> 10, (long long)u.obj_count,
             (long long)u.obj_size >> 10, (long long)u.js_func_count, (long long)u.js_func_size >> 10,
             (long long)u.js_func_code_size >> 10, (long long)u.js_func_pc2line_size >> 10, (long long)u.str_count,
             (long long)u.str_size >> 10, (long long)u.atom_count, (long long)u.atom_size >> 10,
             (long long)u.shape_count, (long long)u.shape_size >> 10, (long long)u.prop_size >> 10,
             (long long)u.array_count, (long long)u.binary_object_size >> 10);
}

/* A new context with the whole `lord` object of a session (or of the login screen). */
static void session_context(void) {
    rt = JS_NewRuntime();
    JS_SetMemoryLimit(rt, 256u << 20);
    JS_SetMaxStackSize(rt, 4u << 20);
    JS_SetInterruptHandler(rt, on_interrupt, NULL);
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
    js_pkg_set_app_file(apps_system_file);
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
    js_sftp_register(ctx, lord);       /* lord.sftp: the Terminal's Files mode via lord-sftp (docs/TERMINAL.md) */
    js_sql_register(ctx, lord);        /* lord.sql: Databases (MySQL / MariaDB) via lord-sql (docs/DATABASES.md) */
    js_weather_register(ctx, lord);    /* lord.weather: the weather widget's forecasts via lord-fetch (docs/WORKSPACES.md) */
    js_webapps_register(ctx, lord);    /* lord.webapps: web apps and their profiles via lord-web (docs/WEBAPPS.md) */
    js_termconf_register(ctx, lord);   /* lord.termconf: the Terminal's configuration file (docs/TERMINAL.md) */
    js_wm_register(ctx, lord);         /* lord.wm: moving and resizing windows in C (grab.c) */
    js_input_register(ctx, lord);      /* lord.input: touchpads and touchscreens (tp/) */
    js_fw_register(ctx, lord);         /* lord.fw: per-app network permissions (lord-netperm, docs/FIREWALL.md) */
    js_disk_register(ctx, lord);       /* lord.disks: disks and partitions (Disks app) */
    js_imager_register(ctx, lord);     /* lord.imager: LordOS Imager (docs/IMAGER.md) */
    js_backup_register(ctx, lord);     /* lord.backup: Backups (backup/) */
    js_cloud_register(ctx, lord);      /* lord.cloud: Cloud (cloud/, docs/CLOUD.md) */
    js_rdesk_register(ctx, lord);      /* lord.rdesk: the RDP and VNC apps */
    js_vm_register(ctx, lord);         /* lord.vm: Machines (vm/, docs/VIRTUALIZATION.md) */
    js_guestwin_register(ctx, lord);   /* lord.guestwin: apps mode's guest side, inside a Machines guest (vmagent.h) */
    js_lan_register(ctx, lord);        /* lord.lan: Files → Network (lan/, docs/FILES.md) */
    js_clean_register(ctx, lord);      /* lord.clean: Cleaner (clean/, docs/CLEANER-ARCHITECTURE.md) */
    js_logs_register(ctx, lord);       /* lord.logs: journal and LordOS logs (Logs app) */
    js_flatpak_register(ctx, lord);    /* lord.flatpak: the Store's Flathub commands (docs/FLATPAK.md, E3) */
    js_help_register(ctx, lord);       /* lord.help (Help) and lord.time.zoneOffset (Clock, tzif.c) */
    js_hw_register(ctx, lord);         /* lord.display/power/bt/mime/timedate (Settings, src/hw) */
    js_voice_register(ctx, lord);      /* lord.voice: reading text aloud through lord-say (tts/speak.c, docs/VOICE.md) */
    prof_wrap_obj(ctx, lord, "", 0);   /* every lord.<ns>.* call timed (the stall detector) */
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
    if (ctx && run.open) { run.open = 0; JS_FreeAtom(ctx, run.name); JS_FreeValue(ctx, run.detail); run.detail = JS_UNDEFINED; }
    stall.depth = 0;
    if (ctx && hot_rt == rt) for (int i = 0; i < HOT_N; i++) JS_FreeAtom(ctx, hot_atom[i]);
    hot_rt = NULL;
    if (ctx) JS_FreeContext(ctx);
    if (rt) JS_FreeRuntime(rt);
    ctx = NULL;
    rt = NULL;
}

double js_tick(double now) {
    JSValue arg = JS_NewFloat64(ctx, now);
    JSValue r = call_hot(HOT_TICK, 1, &arg);
    double next = -1;
    JS_ToFloat64(ctx, &next, r);
    JS_FreeValue(ctx, r);
    drain_jobs();
    return next;
}

int js_wants_frame(void) {
    JSValue r = call_hot(HOT_WANTS_FRAME, 0, NULL);
    int w = JS_ToBool(ctx, r);
    JS_FreeValue(ctx, r);
    return w;
}

void js_frame(double now) {
    JSValue arg = JS_NewFloat64(ctx, now);
    JS_FreeValue(ctx, call_hot(HOT_FRAME, 1, &arg));
    drain_jobs();
}

void js_pointer(const char *type, int target, float x, float y, int button) {
    JSValueConst args[5] = {
        JS_NewString(ctx, type), JS_NewInt32(ctx, target),
        JS_NewFloat64(ctx, x), JS_NewFloat64(ctx, y), JS_NewInt32(ctx, button),
    };
    int middle = button == 1 && !strcmp(type, "down");     /* the press that pastes the primary selection */
    if (middle) js_primary_gesture(1);
    JS_FreeValue(ctx, call_global("__lordEvent", 5, args));
    if (middle) js_primary_gesture(0);
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

int js_text_input_app(void) { return wl_has_key_focus() && wl_textinput_composing(); }

int js_commit_text(const char *text) {
    if (!ctx || !text || !text[0]) return 0;
    if (wl_has_key_focus()) {
        if (!wl_textinput_composing()) return 0;            /* an app without text input (X11, a terminal) takes only keys */
        /* a commit holds 511 bytes at most (wl_textinput.c): longer text goes in pieces, cut at a code point */
        for (size_t n = strlen(text); n;) {
            size_t k = n < 511 ? n : 511;
            while (k < n && k && ((unsigned char)text[k] & 0xc0) == 0x80) k--;
            if (!k) break;
            char piece[512];
            memcpy(piece, text, k);
            piece[k] = 0;
            wl_textinput_send("", 0, 0, piece);
            text += k;
            n -= k;
        }
        return 1;
    }
    ime_commit(text);
    drain_jobs();
    return 1;
}

/* A Linux app's field composes (zwp_text_input_v3, wl/wl_textinput.c): the key goes through the input
 * method before the app. The composition and the finished text go to the app, the candidates to the
 * shell's window. 1 if the input method used the key: the app never sees it, nor its release. */
static unsigned char ime_took[0x300];
static int ime_to_app(int code, int value, const KeyInfo *k) {
    if (code >= 0 && code < (int)sizeof ime_took && ime_took[code]) {
        if (!value) ime_took[code] = 0;
        return 1;
    }
    if (!value || !wl_textinput_composing()) return 0;
    char commit[512];
    int used = ime_key(k, commit, sizeof commit);
    if (ime_changed()) {
        const ImeState *st = ime_state();
        wl_textinput_send(st->preedit, st->focus_begin, st->focus_end, commit);
        ime_event();
    } else if (commit[0]) wl_textinput_send("", 0, 0, commit);
    if (used && code >= 0 && code < (int)sizeof ime_took) ime_took[code] = 1;
    return used;
}

/* Key → __lordKey(code, value, key, text, base, flags). With libxkbcommon (keyboard.c)
 * C does the translation; if no layout is loaded, only code and value. A Linux app's global shortcut
 * (portal.c) goes to that app before anything else, and then to nothing else. If there is an
 * active input method (ime.c) and a focused text field, the key goes through it first. */
void js_key(int code, int value) {
    KeyInfo k;
    kb_key(code, value, &k);
    unsigned mods = (k.shift ? KG_SHIFT : 0u) | (k.ctrl ? KG_CTRL : 0u) | (k.alt ? KG_ALT : 0u) | (k.meta ? KG_META : 0u);
    char letter = keygrab_letter(code, k.base);
    /* a window holding the shortcuts (a VM, a remote desktop) gets the press first; a held one's release still ends it */
    if ((value != 1 || !wl_key_to_inhibitor(code, letter, mods)) && portal_shortcut_key(code, value, letter, mods)) { drain_jobs(); return; }
    if (!kb_ready()) {
        JSValueConst args[2] = { JS_NewInt32(ctx, code), JS_NewInt32(ctx, value) };
        JS_FreeValue(ctx, call_global("__lordKey", 2, args));
        drain_jobs();
        return;
    }
    if (ime_to_app(code, value, &k)) { drain_jobs(); return; }
    int composed = value && wl_textinput_composing();           /* already through the input method */
    int to_app = wl_key(code, value, &k);                       /* the focused Wayland app */
    if (to_app == 1) { drain_jobs(); return; }
    if (value && !to_app && !composed) {
        char commit[512];
        int used = ime_key(&k, commit, sizeof commit);
        if (commit[0]) ime_commit(commit);
        if (ime_changed()) ime_event();
        if (used) { drain_jobs(); return; }
    }
    int flags = k.shift | k.ctrl << 1 | k.alt << 2 | k.altgr << 3 | k.meta << 4 | k.caps << 5 | k.dead << 6 | (to_app == 2) << 7;
    JSValueConst args[6] = { JS_NewInt32(ctx, code), JS_NewInt32(ctx, value), JS_NewString(ctx, k.key),
                             JS_NewString(ctx, k.chr), JS_NewString(ctx, k.base), JS_NewInt32(ctx, flags) };
    JS_FreeValue(ctx, call_global("__lordKey", 6, args));
    for (int i = 2; i < 5; i++) JS_FreeValue(ctx, args[i]);
    drain_jobs();
}

/* A keyboard was plugged in, removed, or typed for the first time → __lordKeyboards(present). */
void js_keyboards_event(int present) {
    if (!ctx) return;
    JSValueConst arg = JS_NewBool(ctx, present);
    JS_FreeValue(ctx, call_global("__lordKeyboards", 1, &arg));
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

/* The main loop ended (logout, or a signal): the session's last word (sdk/sessionrestore.jsx). */
void js_before_exit(void) {
    if (!ctx) return;
    JS_FreeValue(ctx, call_global("__lordBeforeExit", 0, NULL));
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

/* Cloud (cloud/cloud.c): the state changed → __lordCloud(state); a link answer → __lordCloudReply(req, result). */
void js_cloud_event(const char *json) {
    if (!ctx) return;
    JSValueConst arg = JS_ParseJSON(ctx, json, strlen(json), "<cloud>");
    JS_FreeValue(ctx, call_global("__lordCloud", 1, &arg));
    JS_FreeValue(ctx, arg);
    drain_jobs();
}

void js_cloud_reply(int req, const char *json) {
    if (!ctx) return;
    JSValueConst args[2] = { JS_NewInt32(ctx, req), JS_ParseJSON(ctx, json, strlen(json), "<cloud>") };
    JS_FreeValue(ctx, call_global("__lordCloudReply", 2, args));
    JS_FreeValue(ctx, args[1]);
    drain_jobs();
}

/* lord.fs.*Async (fsasync.c): a finished request → __lordFsReply(req, answer). */
static void fs_reply(void *ud, int req, const char *json) {
    (void)ud;
    if (!ctx) return;
    JSValueConst args[2] = { JS_NewInt32(ctx, req), JS_ParseJSON(ctx, json, strlen(json), "<fs>") };
    if (JS_IsException(args[1])) {
        static const char bad[] = "{\"error\":\"sys.badArgument\"}";
        JS_FreeValue(ctx, JS_GetException(ctx));
        args[1] = JS_ParseJSON(ctx, bad, sizeof bad - 1, "<fs>");
    }
    JS_FreeValue(ctx, call_global("__lordFsReply", 2, args));
    JS_FreeValue(ctx, args[1]);
}

void js_fs_dispatch(void) {
    if (fsasync_dispatch(fs_reply, NULL)) drain_jobs();
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

/* The screen-sharing API (remote/share.c): its prompt and indicator → __lordShare(json), sdk/share.js. */
void js_share_event(const char *json) {
    if (!ctx) return;
    JSValueConst args[1] = { JS_NewString(ctx, json) };
    JS_FreeValue(ctx, call_global("__lordShare", 1, args));
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
