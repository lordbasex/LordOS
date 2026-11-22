/* js_studio.c — lord.studio: LordOS Studio's settings (docs/CAMERA-STUDIO-PLAN.md 9; AGENTS.md, lord.studio).
 *
 * Registered only in the desktop's own JS context (js.c session_context), never in an isolated app's: apps see
 * "LordOS Studio" as one more camera (lord.media.cameras(), device "studio") under the camera permission, and never
 * control it. Every value is a number, a mode name or a path that lord-server checks itself (media/mstudio.c).
 *   status()                       → { available, camera, mode, blur, mirror, background, fps, segMs, fxMs, gpu, sources }
 *   set({ camera, mode, blur, mirror })   mode 'none' | 'blur' | 'picture'; camera '/dev/videoN' or '' (the first one)
 *   setBackground(path | null)     'asset:wallpapers/…' or a picture inside lord.fs's roots; throws studio.badPicture
 *   preview(width, height) → id    the popover's own live preview: a LordOS Studio camera source of the desktop (the
 *                                  "no camera" restriction for LordOS's apps still applies) */
#include "js_studio.h"
#include "js_err.h"
#include "../media/media_int.h"
#include "../media/mstudio.h"
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
    int mode = 1;
    v = JS_GetPropertyStr(c, o, "mode");
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(c, v);
        mode = s && !strcmp(s, "none") ? 0 : s && !strcmp(s, "picture") ? 2 : s && !strcmp(s, "blur") ? 1 : -1;
        if (s) JS_FreeCString(c, s);
    }
    JS_FreeValue(c, v);
    if (mode < 0) return js_throw_code(c, "studio.invalid", NULL);
    int blur = opt_int(c, o, "blur", 50), mirror = opt_int(c, o, "mirror", 0);
    if (blur < 0 || blur > 100) return js_throw_code(c, "studio.invalid", NULL);
    if (mstudio_set(have_cam ? camera : NULL, mode, blur, mirror)) return js_throw_code(c, "studio.invalid", NULL);
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
    MediaSrc *s = media_src_new(SK_CAMERA, "studio");
    if (!s) return js_throw_code(c, "media.tooMany", NULL);
    const char *code = NULL;
    s->sock = mstudio_spawn(w, h, &s->pid, &code);
    if (s->sock < 0) { media_src_close(s); return js_throw_code(c, code ? code : "studio.unavailable", NULL); }
    s->studio = 1;
    mstudio_attach(s->sock);
    return JS_NewInt32(c, s->id);
}

static const JSCFunctionListEntry studio_funcs[] = {
    JS_CFUNC_DEF("status", 0, f_status),
    JS_CFUNC_DEF("set", 1, f_set),
    JS_CFUNC_DEF("setBackground", 1, f_set_background),
    JS_CFUNC_DEF("preview", 2, f_preview),
};

void js_studio_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, studio_funcs, sizeof studio_funcs / sizeof studio_funcs[0]);
    JS_SetPropertyStr(c, lord, "studio", o);
}
