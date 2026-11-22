/* js_studio.c — lord.studio: LordOS Studio's settings (docs/CAMERA-STUDIO-PLAN.md 9; AGENTS.md, lord.studio).
 *
 * Registered only in the desktop's own JS context (js.c session_context), never in an isolated app's: apps see
 * "LordOS Studio" as one more camera (lord.media.cameras(), device "studio") under the camera permission, and never
 * control it. Every value is a number, a mode name or a path that lord-server checks itself (media/mstudio.c).
 *   status()                       → { available, on, camera, mode, blur, mirror, background, video, key, similarity,
 *                                      smoothness, spill, framing, light, match, filter, filterStrength, retouch, faces,
 *                                      fps, segMs, fxMs, frameMs, videoMs, gpu,
 *                                      loopback: 'missing'|'ready'|'running', hub, readers, cameraOpen, shows, cameraName,
 *                                      stopped, apps: [linux app ids reading the loopback], sources }
 *   set({ on, camera, mode, blur, mirror, key, similarity, smoothness, spill, framing, light, match, filter,
 *         filterStrength, retouch })
 *                                  on: LordOS Studio's camera for every app (the loopback; phase 2);
 *                                  mode 'none' | 'blur' | 'picture' | 'video' (picture and video: the background file,
 *                                  whichever it is); camera '/dev/videoN' or '' (the first one); phase 3: key 'none' |
 *                                  'green' | 'blue' (a real green screen), filter 'none' | 'warm' | 'cool' | 'vivid' |
 *                                  'mono' | 'noir' | 'film' | 'fade' | 'sepia', match 0..60, the rest 0..100 (framing:
 *                                  Center Stage, light: Studio Light; 0 off); throws studio.invalid for anything else
 *   setBackground(path | null)     'asset:wallpapers/…', 'asset:studio/backgrounds/…', a catalog download
 *                                  ('/var/lib/lordos/studio/<sha256>.jpg|.mp4', studio-bg-get) or a picture or short
 *                                  video inside lord.fs's roots; throws studio.badPicture
 *   preview(width, height) → id    the popover's own live preview: the loopback writer's own frames while it runs (the
 *                                  real camera opened for it), else a LordOS Studio camera source of the desktop (the
 *                                  "no camera" restriction for LordOS's apps applies to both)
 *   endPreview()                   the popover closed: the loopback writer's preview frames end (the camera closes when
 *                                  no app reads either) */
#include "js_studio.h"
#include "js_err.h"
#include "../media/media_int.h"
#include "../media/mstudio.h"
#include "../media/mstudiohub.h"
#include "../sec/restrict.h"
#include "../fw/rules.h"

#include <limits.h>
#include <string.h>

static JSValue f_status(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    static char json[8192];
    mstudio_status(json, sizeof json);
    return JS_ParseJSON(c, json, strlen(json), "<studio>");
}

static int opt_int(JSContext *c, JSValueConst o, const char *k, int def) {
    JSValue v = JS_GetPropertyStr(c, o, k);
    int32_t r = def;
    if (JS_IsNumber(v)) JS_ToInt32(c, &r, v);
    else if (JS_IsBool(v)) r = JS_ToBool(c, v);
    JS_FreeValue(c, v);
    return r;
}

/* a string property that must be one of names (index), def when absent; -1 when present but not one of them */
static int opt_enum(JSContext *c, JSValueConst o, const char *k, const char *const *names, int n, int def) {
    JSValue v = JS_GetPropertyStr(c, o, k);
    int r = def;
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(c, v);
        r = -1;
        for (int i = 0; s && i < n; i++) if (!strcmp(s, names[i])) r = i;
        if (s) JS_FreeCString(c, s);
    } else if (!JS_IsUndefined(v) && !JS_IsNull(v)) r = -1;
    JS_FreeValue(c, v);
    return r;
}

static JSValue f_set(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || !JS_IsObject(argv[0])) return js_throw_code(c, "studio.invalid", NULL);
    JSValueConst o = argv[0];
    char camera[32] = "";
    int have_cam = 0;
    JSValue v = JS_GetPropertyStr(c, o, "camera");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(c, v);
        if (s && strlen(s) < sizeof camera) { snprintf(camera, sizeof camera, "%s", s); have_cam = 1; }
        if (s) JS_FreeCString(c, s);
    }
    JS_FreeValue(c, v);
    static const char *const modes[] = { "none", "blur", "picture", "video" }, *const keys[] = { "none", "green", "blue" };
    const char *filters[STUDIO_FILTERS + 1];
    for (int i = 0; i <= STUDIO_FILTERS; i++) filters[i] = studio_filter_name(i);
    MStudioSet s = {
        .on = opt_int(c, o, "on", 1), .camera = have_cam ? camera : NULL,
        .mode = opt_enum(c, o, "mode", modes, 4, 1), .blur = opt_int(c, o, "blur", 50), .mirror = opt_int(c, o, "mirror", 0),
        .key = opt_enum(c, o, "key", keys, 3, 0), .similarity = opt_int(c, o, "similarity", 40), .smoothness = opt_int(c, o, "smoothness", 20),
        .spill = opt_int(c, o, "spill", 50), .framing = opt_int(c, o, "framing", 0), .light = opt_int(c, o, "light", 0),
        .match = opt_int(c, o, "match", 0), .filter = opt_enum(c, o, "filter", filters, STUDIO_FILTERS + 1, 0),
        .filter_amt = opt_int(c, o, "filterStrength", 100), .retouch = opt_int(c, o, "retouch", 0),
    };
    if (s.mode < 0 || s.key < 0 || s.filter < 0) return js_throw_code(c, "studio.invalid", NULL);
    const int pct[] = { s.blur, s.similarity, s.smoothness, s.spill, s.framing, s.light, s.filter_amt, s.retouch };
    for (size_t i = 0; i < sizeof pct / sizeof pct[0]; i++) if (pct[i] < 0 || pct[i] > 100) return js_throw_code(c, "studio.invalid", NULL);
    if (s.match < 0 || s.match > 60) return js_throw_code(c, "studio.invalid", NULL);
    if (mstudio_set(&s)) return js_throw_code(c, "studio.invalid", NULL);
    return JS_TRUE;
}

static JSValue f_set_background(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) { mstudio_set_background(NULL); return JS_TRUE; }
    if (!JS_IsString(argv[0])) return js_throw_code(c, "studio.badPicture", NULL);
    const char *p = JS_ToCString(c, argv[0]);
    int r = p && strlen(p) < PATH_MAX ? mstudio_set_background(p) : -1;
    if (p) JS_FreeCString(c, p);
    if (r) return js_throw_code(c, "studio.badPicture", NULL);
    return JS_TRUE;
}

/* the popover's preview: the desktop's own source, under the restriction of LordOS's apps (no per-app permission: the
 * person opened it from the system's camera item; the capture indicator shows it like any camera) */
static JSValue f_preview(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t w = 1280, h = 720;
    if (argc > 0 && JS_IsNumber(argv[0])) JS_ToInt32(c, &w, argv[0]);
    if (argc > 1 && JS_IsNumber(argv[1])) JS_ToInt32(c, &h, argv[1]);
    if (restrict_blocks("lordos", FW_R_NO_CAMERA)) return js_throw_code(c, restrict_code(FW_R_NO_CAMERA), "app", "studio", NULL);
    int hub = mstudio_preview_hub();
    if (hub > 0) return JS_NewInt32(c, hub);
    MediaSrc *s = media_src_new(SK_CAMERA, "studio");
    if (!s) return js_throw_code(c, "media.tooMany", NULL);
    const char *code = NULL;
    if (mstudio_open(s, w, h, &code)) { media_src_close(s); return js_throw_code(c, code ? code : "studio.unavailable", NULL); }
    return JS_NewInt32(c, s->id);
}

static JSValue f_end_preview(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    mstudio_preview_end();
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry studio_funcs[] = {
    JS_CFUNC_DEF("status", 0, f_status),
    JS_CFUNC_DEF("set", 1, f_set),
    JS_CFUNC_DEF("setBackground", 1, f_set_background),
    JS_CFUNC_DEF("preview", 2, f_preview),
    JS_CFUNC_DEF("endPreview", 0, f_end_preview),
};

void js_studio_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, studio_funcs, sizeof studio_funcs / sizeof studio_funcs[0]);
    JS_SetPropertyStr(c, lord, "studio", o);
}
