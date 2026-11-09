/* js_media.c — lord.media: video, music, photos, screenshots, camera and recorder (see
 * server/src/media and docs/MEDIA.md).
 *
 * Rules:
 *   - every file goes through the same roots as lord.fs (home folder and user
 *     mounts); outputs never overwrite anything (temporary + rename without replacing) and cannot go
 *     to the protected folders;
 *   - camera, microphone and screen are checked against the system permissions (sec/perms.c,
 *     per-app permission prompts) in C, not only in the SDK: the prompt is shown by the system (sdk/privacy.js) and here
 *     the saved decision is required to be "allow";
 *   - a firewall restriction of the built-in apps (firewall id "lordos", sec/restrict.h) beats the
 *     permission: refused with fw.restricted.<perm>, and restricting while a camera, microphone
 *     or screen recording runs stops it at once (media_restricted below);
 *   - the decoder (lord-media) never receives a path: lord-server opens the file and passes the fd. */
#include "js_media.h"
#include "../media/media_int.h"
#include "ui.h"
#include "../media/proto.h"
#include "../media/hwdec.h"
#include "../media/imgops.h"
#include "../doc/doc.h"
#include "../doc/proto.h"
#include "../sec/json.h"
#include "../sec/perms.h"
#include "../sec/restrict.h"
#include "../fw/rules.h"
#include "ui.h"
#include "js_err.h"

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- validation ---- */

static int app_ok(const char *a) {
    size_t n = a ? strlen(a) : 0;
    if (n < 1 || n >= MEDIA_APP_MAX) return 0;
    for (size_t i = 0; i < n; i++) if (!isalnum((unsigned char)a[i]) && a[i] != '-' && a[i] != '_' && a[i] != '.') return 0;
    return 1;
}

static const char *arg_app(JSContext *c, JSValueConst v) {
    const char *s = JS_ToCString(c, v);
    if (s && !app_ok(s)) { JS_FreeCString(c, s); return NULL; }
    return s;
}

static const char *arg_path(JSContext *c, JSValueConst v) {
    if (!JS_IsString(v)) return NULL;
    const char *s = JS_ToCString(c, v);
    if (!s) return NULL;
    size_t n = strlen(s);
    int ok = n > 0 && n < PATH_MAX;
    for (size_t i = 0; ok && i < n; i++) if ((unsigned char)s[i] < 0x20) ok = 0;
    if (!ok) { JS_FreeCString(c, s); return NULL; }
    return s;
}

static int opt_int(JSContext *c, JSValueConst o, const char *k, int def, int lo, int hi) {
    if (!JS_IsObject(o)) return def;
    JSValue v = JS_GetPropertyStr(c, o, k);
    double d = def;
    if (JS_IsNumber(v)) JS_ToFloat64(c, &d, v);
    else if (JS_IsBool(v)) d = JS_ToBool(c, v);
    JS_FreeValue(c, v);
    if (!(d >= lo)) d = lo;
    if (d > hi) d = hi;
    return (int)d;
}

static double opt_num(JSContext *c, JSValueConst o, const char *k, double def) {
    if (!JS_IsObject(o)) return def;
    JSValue v = JS_GetPropertyStr(c, o, k);
    double d = def;
    if (JS_IsNumber(v)) JS_ToFloat64(c, &d, v);
    JS_FreeValue(c, v);
    return d == d ? d : def;
}

/* Short string from an options object (only letters, numbers and -_.:/ space). */
static void opt_str(JSContext *c, JSValueConst o, const char *k, char *out, size_t n) {
    out[0] = 0;
    if (!JS_IsObject(o)) return;
    JSValue v = JS_GetPropertyStr(c, o, k);
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(c, v);
        if (s) {
            size_t j = 0;
            for (size_t i = 0; s[i] && j + 1 < n; i++) if ((unsigned char)s[i] >= 0x20 && s[i] != '\n') out[j++] = s[i];
            out[j] = 0;
            JS_FreeCString(c, s);
        }
    }
    JS_FreeValue(c, v);
}

static int arg_id(JSContext *c, JSValueConst v) {
    int32_t id = 0;
    if (!JS_IsNumber(v) || JS_ToInt32(c, &id, v)) return 0;
    return id;
}

/* Built-in apps run inside lord-server: their firewall id is "lordos" (docs/FIREWALL.md). */
#define BUILTIN_FWID "lordos"

static int permitted(JSContext *c, const char *app, Perm p) {
    uint32_t rf = restrict_flag_of(p);
    if (rf && restrict_blocks(BUILTIN_FWID, rf)) {           /* restricted: no prompt, no permission */
        js_throw_code(c, restrict_code(rf), "app", app, NULL);
        return 0;
    }
    if (perm_get(app, p) == PERM_ALLOW) return 1;
    char code[48];
    snprintf(code, sizeof code, "media.denied.%s", perm_name(p));    /* media.denied.camera, .microphone, .screen */
    js_throw_code(c, code, "app", app, NULL);
    return 0;
}

static JSValue path_error(JSContext *c, int r) {
    return r == -2 ? js_throw_code(c, "media.fileExists", NULL) : js_throw_code(c, "media.outsideHome", NULL);
}

static const char *basename_of(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

/* ---- playback ---- */

/* play(app, path, { loop, native, autoplay, audio, hw, hwOnly, rate, volume, music, appName }) → id */
static JSValue f_play(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_EXCEPTION;
    const char *app = arg_app(c, argv[0]), *path = arg_path(c, argv[1]);
    JSValueConst o = argc > 2 ? argv[2] : JS_UNDEFINED;
    JSValue ret = JS_NULL;
    if (!app || !path) { ret = js_throw_code(c, "media.invalidArguments", NULL); goto out; }
    char real[PATH_MAX];
    int in = media_open_input(path, real, sizeof real);
    if (in < 0) { ret = path_error(c, -1); goto out; }
    MediaSrc *s = media_src_new(SK_PLAYER, app);
    if (!s) { close(in); ret = js_throw_code(c, "media.tooMany", NULL); goto out; }
    int ring = -1, render = -1;
    char appname[64];
    opt_str(c, o, "appName", appname, sizeof appname);
    if (opt_int(c, o, "audio", 1, 0, 1)) s->audio = maudio_open(0, appname[0] ? appname : app, basename_of(real), opt_int(c, o, "music", 0, 0, 1) ? "music" : "video", &ring);
    if (s->audio) maudio_volume(s->audio, opt_num(c, o, "volume", 1));
    int hw = opt_int(c, o, "hw", 1, 0, 1);
    if (hw) render = media_render_fd();
    char params[256];
    snprintf(params, sizeof params, "native=%d\nloop=%d\nautoplay=%d\naudio=%d\nhw=%d\nhwonly=%d\nrate=%d\nhwaccel=%s\n", opt_int(c, o, "native", 0, 0, 1),
             opt_int(c, o, "loop", 0, 0, 1), opt_int(c, o, "autoplay", 1, 0, 1), s->audio ? 1 : 0, hw && render >= 0, opt_int(c, o, "hwOnly", 0, 0, 1), opt_int(c, o, "rate", 100, 10, 100), media_hwaccel());
    int fds[3] = { in, ring, render };
    s->sock = media_spawn(MK_PLAY, params, fds, 3, &s->pid);
    close(in);
    if (render >= 0) close(render);
    if (s->sock < 0) { media_src_close(s); ret = js_throw_code(c, "media.noHelper", NULL); goto out; }
    ret = JS_NewInt32(c, s->id);
out:
    if (app) JS_FreeCString(c, app);
    if (path) JS_FreeCString(c, path);
    return ret;
}

/* control(id, 'play'|'pause'|'seek'|'volume'|'audioTrack'|'subTrack'|'loop', value) */
static JSValue f_control(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    MediaSrc *s = argc >= 2 ? media_src_get(arg_id(c, argv[0])) : NULL;
    if (!s || s->sock < 0) return JS_FALSE;
    const char *cmd = JS_ToCString(c, argv[1]);
    if (!cmd) return JS_EXCEPTION;
    double v = 0;
    if (argc > 2 && JS_IsNumber(argv[2])) JS_ToFloat64(c, &v, argv[2]);
    else if (argc > 2 && JS_IsBool(argv[2])) v = JS_ToBool(c, argv[2]);
    int r = -1;
    if (!strcmp(cmd, "play")) { r = mp_sendv(s->sock, MM_PLAY, 0, 0, 0, 0, 0); if (s->audio) maudio_playing(s->audio, 1); }
    else if (!strcmp(cmd, "pause")) { r = mp_sendv(s->sock, MM_PAUSE, 0, 0, 0, 0, 0); if (s->audio) maudio_playing(s->audio, 0); }
    else if (!strcmp(cmd, "seek") && v >= 0 && v < 1e10) r = mp_sendv(s->sock, MM_SEEK, 0, (int64_t)v, 0, 0, 0);
    else if (!strcmp(cmd, "volume") && v >= 0 && v <= 1.5) { if (s->audio) maudio_volume(s->audio, v); r = 0; }
    else if (!strcmp(cmd, "audioTrack") && v >= 0 && v < 64) r = mp_sendv(s->sock, MM_TRACK, 0, 'a', (int64_t)v, 0, 0);
    else if (!strcmp(cmd, "subTrack") && v >= -1 && v < 64) r = mp_sendv(s->sock, MM_TRACK, 0, 's', (int64_t)v, 0, 0);
    else if (!strcmp(cmd, "loop")) r = mp_sendv(s->sock, MM_LOOP, 0, v != 0, 0, 0, 0);
    JS_FreeCString(c, cmd);
    return JS_NewBool(c, r == 0);
}

static JSValue f_close(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    MediaSrc *s = argc ? media_src_get(arg_id(c, argv[0])) : NULL;
    if (s) media_src_close(s);
    return JS_UNDEFINED;
}

/* view(id, { fit: 'contain'|'cover', zoom, cx, cy, rotate, mirror, background }) */
static JSValue f_view(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    MediaSrc *s = argc >= 2 ? media_src_get(arg_id(c, argv[0])) : NULL;
    if (!s || !JS_IsObject(argv[1])) return JS_FALSE;
    char fit[16];
    opt_str(c, argv[1], "fit", fit, sizeof fit);
    MediaView v = s->view;
    if (fit[0]) v.fit = !strcmp(fit, "cover");
    double z = opt_num(c, argv[1], "zoom", v.zoom);
    v.zoom = (float)(z > 0 && z <= 64 ? z : 0);
    v.cx = (float)fmin(1, fmax(0, opt_num(c, argv[1], "cx", v.cx)));
    v.cy = (float)fmin(1, fmax(0, opt_num(c, argv[1], "cy", v.cy)));
    int rot = ((opt_int(c, argv[1], "rotate", v.rot, -3600, 3600) % 360) + 360) % 360;
    v.rot = rot - rot % 90;
    v.mirror = opt_int(c, argv[1], "mirror", v.mirror, 0, 1);
    v.bright = opt_int(c, argv[1], "brightness", v.bright, -100, 100);
    v.contrast = opt_int(c, argv[1], "contrast", v.contrast, -100, 100);
    JSValue bg = JS_GetPropertyStr(c, argv[1], "background");
    if (JS_IsNumber(bg)) { double d; JS_ToFloat64(c, &d, bg); v.bg = (uint32_t)(int64_t)d; }
    JS_FreeValue(c, bg);
    v.gen = s->view.gen + 1;
    int resized = v.rot != s->view.rot || v.zoom != s->view.zoom || v.fit != s->view.fit;
    s->view = v;
    if (resized) s->sent_w = s->sent_h = 0;            /* the helper rescales again */
    media_src_damage(s);
    return JS_TRUE;
}

static const char *kind_name(int k) {
    static const char *const n[] = { "", "player", "camera", "image", "capture", "screenrec", "audiorec" };
    return k > 0 && k <= SK_AUDIOREC ? n[k] : "";
}

static JSValue f_info(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    MediaSrc *s = argc ? media_src_get(arg_id(c, argv[0])) : NULL;
    if (!s) return JS_NULL;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "kind", JS_NewString(c, kind_name(s->kind)));
    JS_SetPropertyStr(c, o, "width", JS_NewInt32(c, s->ring_slots ? s->ring_w : s->w));
    JS_SetPropertyStr(c, o, "height", JS_NewInt32(c, s->ring_slots ? s->ring_h : s->h));
    JS_SetPropertyStr(c, o, "pos", JS_NewInt64(c, s->pos));
    JS_SetPropertyStr(c, o, "duration", JS_NewInt64(c, s->dur));
    JS_SetPropertyStr(c, o, "playing", JS_NewBool(c, s->playing));
    JS_SetPropertyStr(c, o, "alive", JS_NewBool(c, s->sock >= 0));
    JS_SetPropertyStr(c, o, "frames", JS_NewInt64(c, s->rec_frames));
    JS_SetPropertyStr(c, o, "dropped", JS_NewInt64(c, s->rec_dropped));
    return o;
}

/* ---- photos, thumbnails and jobs ---- */

static JSValue f_image(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_EXCEPTION;
    const char *app = arg_app(c, argv[0]), *path = arg_path(c, argv[1]);
    JSValue ret = JS_NULL;
    int in = app && path ? media_open_input(path, NULL, 0) : -1;
    if (in < 0) ret = path_error(c, -1);
    else {
        MediaSrc *s = media_src_new(SK_IMAGE, app);
        if (!s) { close(in); ret = js_throw_code(c, "media.tooMany", NULL); }
        else {
            s->job = media_job(0, MW_IMAGE, JP_IMAGE, &in, 1, 0, 0, NULL, NULL, NULL, s->id);
            ret = JS_NewInt32(c, s->id);
        }
    }
    if (app) JS_FreeCString(c, app);
    if (path) JS_FreeCString(c, path);
    return ret;
}

static JSValue f_thumb(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *path = argc ? arg_path(c, argv[0]) : NULL;
    if (!path) return JS_NULL;
    int32_t size = 256;
    if (argc > 1 && JS_IsNumber(argv[1])) JS_ToInt32(c, &size, argv[1]);
    char out[PATH_MAX];
    const char *r = media_thumb(path, size, out, sizeof out);
    JS_FreeCString(c, path);
    return r ? JS_NewString(c, r) : JS_NULL;
}

/* probe / waveform / poster: background job → "job" event with the id */
static JSValue simple_job(JSContext *c, int argc, JSValueConst *argv, int kind, int purpose, int bg, int64_t def) {
    const char *path = argc ? arg_path(c, argv[0]) : NULL;
    if (!path) return js_throw_code(c, "media.invalidPath", NULL);
    int in = media_open_input(path, NULL, 0);
    JS_FreeCString(c, path);
    if (in < 0) return path_error(c, -1);
    int32_t a = (int32_t)def;
    if (argc > 1 && JS_IsNumber(argv[1])) JS_ToInt32(c, &a, argv[1]);
    int id = media_job(bg, kind, purpose, &in, 1, a, 0, NULL, NULL, NULL, 0);
    return id > 0 ? JS_NewInt32(c, id) : js_throw_code(c, "media.queueFull", NULL);
}
static JSValue f_probe(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; return simple_job(c, argc, argv, MW_PROBE, JP_EVENT, 1, 0); }
static JSValue f_waveform(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; return simple_job(c, argc, argv, MW_WAVEFORM, JP_EVENT, 0, 200); }
static JSValue f_poster(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; return simple_job(c, argc, argv, MW_THUMB, JP_PIXELS, 0, 512); }

static void free_ab(JSRuntime *rt, void *opaque, void *ptr) { (void)rt; (void)opaque; free(ptr); }

static JSValue f_take(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    uint8_t *px;
    int w, h;
    if (!argc || media_take_pixels(arg_id(c, argv[0]), &px, &w, &h)) return JS_NULL;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "width", JS_NewInt32(c, w));
    JS_SetPropertyStr(c, o, "height", JS_NewInt32(c, h));
    JS_SetPropertyStr(c, o, "data", JS_NewArrayBuffer(c, px, (size_t)w * h * 4, free_ab, NULL, 0));
    return o;
}

/* Job with an output file (edit, export audio). */
static JSValue file_job(JSContext *c, int argc, JSValueConst *argv, int kind, const char *params) {
    const char *app = argc > 0 ? arg_app(c, argv[0]) : NULL, *path = argc > 1 ? arg_path(c, argv[1]) : NULL, *out = argc > 2 ? arg_path(c, argv[2]) : NULL;
    JSValue ret;
    char tmp[PATH_MAX + 64], dst[PATH_MAX];
    int in = app && path && out ? media_open_input(path, NULL, 0) : -1;
    int o = in >= 0 ? media_open_output(out, tmp, sizeof tmp, dst, sizeof dst) : -1;
    if (in < 0 || o < 0) { if (in >= 0) close(in); ret = path_error(c, in < 0 ? -1 : o); }
    else {
        int fds[2] = { in, o };
        int id = media_job(0, kind, JP_FILE, fds, 2, 0, 0, params, tmp, dst, 0);
        ret = id > 0 ? JS_NewInt32(c, id) : js_throw_code(c, "media.queueFull", NULL);
        if (id <= 0) unlink(tmp);
    }
    if (app) JS_FreeCString(c, app);
    if (path) JS_FreeCString(c, path);
    if (out) JS_FreeCString(c, out);
    return ret;
}

/* edit(app, path, output, { rotate, brightness, contrast, cx, cy, cw, ch, refw, width, height, format, quality }) */
static JSValue f_edit(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    JSValueConst o = argc > 3 ? argv[3] : JS_UNDEFINED;
    char fmt[8], params[400];
    opt_str(c, o, "format", fmt, sizeof fmt);
    if (strcmp(fmt, "jpeg") && strcmp(fmt, "png")) snprintf(fmt, sizeof fmt, "png");
    int cw = opt_int(c, o, "cw", -1, -1, 65536), ch = opt_int(c, o, "ch", -1, -1, 65536);
    snprintf(params, sizeof params, "rotate=%d\nbrightness=%d\ncontrast=%d\nformat=%s\nquality=%d\n",
             opt_int(c, o, "rotate", 0, -3600, 3600), opt_int(c, o, "brightness", 0, -100, 100), opt_int(c, o, "contrast", 0, -100, 100),
             fmt, opt_int(c, o, "quality", 92, 1, 100));
    if (cw > 0 && ch > 0) {
        size_t l = strlen(params);
        snprintf(params + l, sizeof params - l, "cx=%d\ncy=%d\ncw=%d\nch=%d\nrefw=%d\n", opt_int(c, o, "cx", 0, 0, 65536), opt_int(c, o, "cy", 0, 0, 65536),
                 cw, ch, opt_int(c, o, "refw", 0, 0, 65536));
    }
    /* a new size after the crop: both sides or none (lord-media checks the bound again) */
    int sw = opt_int(c, o, "width", 0, 0, MP_MAX_DIM), sh = opt_int(c, o, "height", 0, 0, MP_MAX_DIM);
    if (sw > 0 && sh > 0) {
        size_t l = strlen(params);
        snprintf(params + l, sizeof params - l, "sw=%d\nsh=%d\n", sw, sh);
    }
    return file_job(c, argc, argv, MW_EDIT, params);
}

/* transcode(app, path, output, { format: 'opus'|'mp3'|'wav', from, to }) */
static JSValue f_transcode(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    JSValueConst o = argc > 3 ? argv[3] : JS_UNDEFINED;
    char fmt[8], params[128];
    opt_str(c, o, "format", fmt, sizeof fmt);
    if (strcmp(fmt, "mp3") && strcmp(fmt, "wav")) snprintf(fmt, sizeof fmt, "opus");
    snprintf(params, sizeof params, "format=%s\nfrom=%d\nto=%d\n", fmt, opt_int(c, o, "from", 0, 0, 1 << 30), opt_int(c, o, "to", -1, -1, 1 << 30));
    return file_job(c, argc, argv, MW_TRANSCODE, params);
}

/* ---- screenshots ---- */

/* screenshot(app, { x, y, width, height, display, until, also }?) → id (with protected windows in black). until: the
 * node of a window's frame (wm.frameNode): what is above that window is left out (Screenshots' window capture).
 * also: with until, up to PAINT_ALSO_MAX node ids painted over the window (its sheets, wm.sheetNodes); others ignored. */
static JSValue f_screenshot(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = argc ? arg_app(c, argv[0]) : NULL;
    if (!app) return js_throw_code(c, "media.invalidApp", NULL);
    JSValue ret;
    if (!permitted(c, app, PERM_SCREEN)) ret = JS_EXCEPTION;
    else {
        JSValueConst o = argc > 1 ? argv[1] : JS_UNDEFINED;
        int also[PAINT_ALSO_MAX], nalso = 0;
        if (JS_IsObject(o)) {
            JSValue a = JS_GetPropertyStr(c, o, "also");
            if (JS_IsArray(c, a)) {
                uint32_t len = 0;
                JSValue lv = JS_GetPropertyStr(c, a, "length");
                JS_ToUint32(c, &len, lv);
                JS_FreeValue(c, lv);
                for (uint32_t i = 0; i < len && nalso < PAINT_ALSO_MAX; i++) {
                    JSValue v = JS_GetPropertyUint32(c, a, i);
                    int32_t id = 0;
                    if (JS_IsNumber(v) && !JS_ToInt32(c, &id, v) && id > 0) also[nalso++] = id;
                    JS_FreeValue(c, v);
                }
            }
            JS_FreeValue(c, a);
        }
        MediaSrc *s = media_screenshot(app, opt_int(c, o, "x", 0, -65536, 65536), opt_int(c, o, "y", 0, -65536, 65536),
                                       opt_int(c, o, "width", 0, 0, 65536), opt_int(c, o, "height", 0, 0, 65536),
                                       (uint32_t)opt_int(c, o, "display", 0, 0, 1 << 30), opt_int(c, o, "until", 0, 0, 1 << 30),
                                       also, nalso);
        ret = s ? JS_NewInt32(c, s->id) : js_throw_code(c, "media.captureFailed", NULL);
    }
    JS_FreeCString(c, app);
    return ret;
}

/* crop(id, x, y, w, h[, radius]) → id of a new picture. radius (px): the corners outside a rounded
 * rectangle become transparent, so a window capture keeps the window's shape (issue #70). */
static JSValue f_crop(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    MediaSrc *s = argc >= 5 ? media_src_get(arg_id(c, argv[0])) : NULL;
    if (!s) return JS_NULL;
    int v[5] = { 0 };
    for (int i = 0; i < 5 && 1 + i < argc; i++) { double d = 0; JS_ToFloat64(c, &d, argv[1 + i]); v[i] = d == d && fabs(d) < 65536 ? (int)d : 0; }
    MediaSrc *n = media_crop(s, v[0], v[1], v[2], v[3]);
    if (n && v[4] > 0) {
        img_round_corners(n->px, n->w, n->h, v[4]);
        n->opaque = 0;
    }
    return n ? JS_NewInt32(c, n->id) : JS_NULL;
}

/* An integer argument in lo..hi, or `bad` (not a number, not finite, fractional or out of range). */
static int arg_int_in(JSContext *c, JSValueConst v, int lo, int hi, int bad) {
    double d;
    if (!JS_IsNumber(v) || JS_ToFloat64(c, &d, v) || !isfinite(d) || d != floor(d) || d < lo || d > hi) return bad;
    return (int)d;
}

/* resize(id, width, height) → id of a new picture (the image editor; imgops.c). Only a screenshot or a
 * decoded picture, sides 1..MP_MAX_DIM; the source is left as it was. */
static JSValue f_resize(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    MediaSrc *s = argc >= 3 ? media_src_get(arg_id(c, argv[0])) : NULL;
    if (!s || !s->px || (s->kind != SK_CAPTURE && s->kind != SK_IMAGE)) return JS_NULL;
    int w = arg_int_in(c, argv[1], 1, MP_MAX_DIM, 0), h = arg_int_in(c, argv[2], 1, MP_MAX_DIM, 0);
    if (!w || !h) return js_throw_code(c, "media.invalidArguments", NULL);
    MediaSrc *n = media_resize(s, w, h);
    return n ? JS_NewInt32(c, n->id) : JS_NULL;
}

/* pixels(id, x, y, w, h) → ArrayBuffer of w × h RGBA bytes (straight alpha) from a screenshot or a decoded picture, or
 * null: Screenshots' magnifier and color picker read a few pixels around the pointer. At most 64 × 64; pixels
 * outside the picture come back transparent. */
#define PIXELS_MAX 64
static JSValue f_pixels(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    MediaSrc *s = argc >= 5 ? media_src_get(arg_id(c, argv[0])) : NULL;
    if (!s || !s->px || (s->kind != SK_CAPTURE && s->kind != SK_IMAGE)) return JS_NULL;
    int x = arg_int_in(c, argv[1], -65536, 65536, 1 << 30), y = arg_int_in(c, argv[2], -65536, 65536, 1 << 30);
    int w = arg_int_in(c, argv[3], 1, PIXELS_MAX, 0), h = arg_int_in(c, argv[4], 1, PIXELS_MAX, 0);
    if (x == 1 << 30 || y == 1 << 30 || !w || !h) return js_throw_code(c, "media.invalidArguments", NULL);
    uint8_t out[PIXELS_MAX * PIXELS_MAX * 4];
    memset(out, 0, sizeof out);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int sx = x + i, sy = y + j;
            if (sx < 0 || sy < 0 || sx >= s->w || sy >= s->h) continue;
            uint32_t p = s->px[(size_t)sy * (size_t)s->w + (size_t)sx];
            uint32_t a = p >> 24;
            uint8_t *o = out + ((size_t)j * (size_t)w + (size_t)i) * 4;
            for (int k = 0; k < 3; k++) {        /* premultiplied → straight */
                uint32_t v = (p >> (16 - 8 * k)) & 255;
                o[k] = (uint8_t)(a == 255 || !a ? v : (v * 255 + a / 2) / a > 255 ? 255 : (v * 255 + a / 2) / a);
            }
            o[3] = (uint8_t)a;
        }
    return JS_NewArrayBufferCopy(c, out, (size_t)w * (size_t)h * 4);
}

/* ocr(id, langs) → { sid, req } of a lord-ocr request (the SDK waits for it: media.ocr): the text in a screenshot or
 * picture. The pixels go to the confined lord-ocr helper in a sealed memfd (src/doc/h_ocr.c); lord-server never
 * links Tesseract. One OCR session is kept and started again if it crashed. */
static int ocr_sid;
static JSValue f_ocr(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    MediaSrc *s = argc >= 2 ? media_src_get(arg_id(c, argv[0])) : NULL;
    if (!s || !s->px || (s->kind != SK_CAPTURE && s->kind != SK_IMAGE) || s->w < 1 || s->h < 1) return JS_NULL;
    const char *langs = JS_ToCString(c, argv[1]);
    if (!langs) return JS_EXCEPTION;
    /* Tesseract data names ("spa+eng"); lord-ocr checks them again */
    size_t ll = strlen(langs);
    int ok = ll >= 3 && ll <= 64;
    for (size_t i = 0; ok && i < ll; i++) ok = (langs[i] >= 'a' && langs[i] <= 'z') || langs[i] == '_' || langs[i] == '+';
    if (!ok) { JS_FreeCString(c, langs); return js_throw_code(c, "media.invalidArguments", NULL); }
    char json[160];
    int n = snprintf(json, sizeof json, "{\"w\":%d,\"h\":%d,\"langs\":", s->w, s->h);
    size_t q = n > 0 && (size_t)n < sizeof json ? json_quote(json + n, sizeof json - (size_t)n - 2, langs) : 0;
    JS_FreeCString(c, langs);
    if (!q || (size_t)n + q + 2 > sizeof json) return js_throw_code(c, "media.invalidArguments", NULL);
    n += (int)q;
    json[n++] = '}';
    json[n] = 0;
    if (ocr_sid <= 0 || !doc_alive(ocr_sid)) {
        if (ocr_sid > 0) doc_close(ocr_sid);
        ocr_sid = doc_session("ocr", NULL, 0);
        if (ocr_sid <= 0) { ocr_sid = 0; return js_throw_code(c, "media.ocrUnavailable", NULL); }
    }
    int fd = media_memfd_from_pixels(s->px, s->w, s->h);
    if (fd < 0) return js_throw_code(c, "media.captureFailed", NULL);
    int req = doc_request(ocr_sid, DQ_OCR, json, (size_t)n, &fd, 1, 0, 30000);
    if (req <= 0) return js_throw_code(c, "media.ocrUnavailable", NULL);
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "sid", JS_NewInt32(c, ocr_sid));
    JS_SetPropertyStr(c, o, "req", JS_NewInt32(c, req));
    return o;
}

/* rotate(id, quarterTurns) → id of a new picture turned clockwise (-3..3). */
static JSValue f_rotate(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    MediaSrc *s = argc >= 2 ? media_src_get(arg_id(c, argv[0])) : NULL;
    if (!s || !s->px || (s->kind != SK_CAPTURE && s->kind != SK_IMAGE)) return JS_NULL;
    int turns = arg_int_in(c, argv[1], -3, 3, 99);
    if (turns == 99) return js_throw_code(c, "media.invalidArguments", NULL);
    MediaSrc *n = media_rotate(s, turns);
    return n ? JS_NewInt32(c, n->id) : JS_NULL;
}

/* An annotation's coordinate: finite, within ±65536 (the drawing code clamps to the image again). */
static float coord(JSContext *c, JSValueConst o, const char *k) {
    double d = opt_num(c, o, k, 0);
    return (float)(isfinite(d) ? fmin(65536, fmax(-65536, d)) : 0);
}

static uint32_t color_of(JSContext *c, JSValueConst o) {
    JSValue v = JS_GetPropertyStr(c, o, "color");
    uint32_t col = 0xffff3b30u;
    if (JS_IsNumber(v)) { double d; JS_ToFloat64(c, &d, v); col = (uint32_t)(int64_t)d; if (!(col >> 24)) col |= 0xff000000u; }
    else if (JS_IsString(v)) {
        const char *s = JS_ToCString(c, v);
        if (s && s[0] == '#' && strlen(s) == 7) col = 0xff000000u | (uint32_t)strtoul(s + 1, NULL, 16);
        if (s) JS_FreeCString(c, s);
    }
    JS_FreeValue(c, v);
    return col;
}

/* annotate(base, [{ type, x1, y1, x2, y2, width, size, color, text }], dest?) → id */
static JSValue f_annotate(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    MediaSrc *base = argc >= 2 ? media_src_get(arg_id(c, argv[0])) : NULL;
    if (!base || base->kind != SK_CAPTURE || !base->px || !JS_IsArray(c, argv[1])) return JS_NULL;
    MediaSrc *dst = argc > 2 ? media_src_get(arg_id(c, argv[2])) : NULL;
    if (dst && (dst == base || dst->kind != SK_CAPTURE || dst->w != base->w || dst->h != base->h)) dst = NULL;
    if (!dst) {
        dst = media_src_new(SK_CAPTURE, base->app);
        if (!dst) return JS_NULL;
        dst->px = malloc((size_t)base->w * base->h * 4);
        if (!dst->px) { media_src_close(dst); return JS_NULL; }
        dst->w = base->w;
        dst->h = base->h;
    }
    dst->opaque = base->opaque;          /* a picture opened with transparency keeps it */
    memcpy(dst->px, base->px, (size_t)base->w * base->h * 4);
    JSValue lenv = JS_GetPropertyStr(c, argv[1], "length");
    int32_t n = 0;
    JS_ToInt32(c, &n, lenv);
    JS_FreeValue(c, lenv);
    if (n > 200) n = 200;
    static MediaOp ops[200];
    int k = 0;
    for (int i = 0; i < n; i++) {
        JSValue o = JS_GetPropertyUint32(c, argv[1], (uint32_t)i);
        char type[12];
        opt_str(c, o, "type", type, sizeof type);
        MediaOp *op = &ops[k];
        memset(op, 0, sizeof *op);
        op->type = !strcmp(type, "arrow") ? 'a' : !strcmp(type, "box") ? 'b' : !strcmp(type, "text") ? 't' : !strcmp(type, "blur") ? 'z'
                 : !strcmp(type, "line") ? 'l' : !strcmp(type, "fill") ? 'f' : !strcmp(type, "pixelate") ? 'p' : 0;
        op->x1 = coord(c, o, "x1");
        op->y1 = coord(c, o, "y1");
        op->x2 = coord(c, o, "x2");
        op->y2 = coord(c, o, "y2");
        op->width = (float)fmin(64, fmax(1, opt_num(c, o, "width", 4)));
        op->size = (float)fmin(200, fmax(8, opt_num(c, o, "size", 24)));
        op->color = color_of(c, o);
        opt_str(c, o, "text", op->text, sizeof op->text);
        JS_FreeValue(c, o);
        if (op->type) k++;
    }
    media_annotate(dst->px, dst->w, dst->h, ops, k, dst->opaque);
    dst->frame_seq++;
    media_src_damage(dst);
    return JS_NewInt32(c, dst->id);
}

/* saveImage(id, output, { format, quality }) → job id */
static JSValue f_save_image(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    MediaSrc *s = argc >= 2 ? media_src_get(arg_id(c, argv[0])) : NULL;
    const char *out = argc >= 2 ? arg_path(c, argv[1]) : NULL;
    if (!s || !s->px || !out) { if (out) JS_FreeCString(c, out); return js_throw_code(c, "media.invalidArguments", NULL); }
    char tmp[PATH_MAX + 64], dst[PATH_MAX], fmt[8], params[64];
    opt_str(c, argc > 2 ? argv[2] : JS_UNDEFINED, "format", fmt, sizeof fmt);
    if (strcmp(fmt, "jpeg")) snprintf(fmt, sizeof fmt, "png");
    snprintf(params, sizeof params, "format=%s\nquality=%d\n", fmt, opt_int(c, argc > 2 ? argv[2] : JS_UNDEFINED, "quality", 92, 1, 100));
    int o = media_open_output(out, tmp, sizeof tmp, dst, sizeof dst);
    JS_FreeCString(c, out);
    if (o < 0) return path_error(c, o);
    int mfd = media_memfd_from_pixels(s->px, s->w, s->h);
    if (mfd < 0) { close(o); unlink(tmp); return js_throw_code(c, "sys.outOfMemory", NULL); }
    int fds[2] = { mfd, o };
    int id = media_job(0, MW_ENCODE, JP_FILE, fds, 2, s->w, s->h, params, tmp, dst, 0);
    return id > 0 ? JS_NewInt32(c, id) : js_throw_code(c, "media.queueFull", NULL);
}

/* copyImage(id) → job id: PNG to the clipboard (like an image copied in an app) */
static JSValue f_copy_image(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    MediaSrc *s = argc ? media_src_get(arg_id(c, argv[0])) : NULL;
    if (!s || !s->px) return js_throw_code(c, "media.invalidImage", NULL);
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt) return js_throw_code(c, "media.noRuntimeDir", NULL);
    static unsigned seq;
    char dir[PATH_MAX], dst[PATH_MAX + 64], tmp[PATH_MAX + 64];
    snprintf(dir, sizeof dir, "%s/lordos", rt);
    mkdir(dir, 0700);
    snprintf(dst, sizeof dst, "%s/clip-shot%d-%u.png", dir, (int)getpid(), ++seq);
    snprintf(tmp, sizeof tmp, "%s/.shot%d-%u.part", dir, (int)getpid(), seq);
    int o = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (o < 0) return js_throw_code(c, "media.createFailed", NULL);
    int mfd = media_memfd_from_pixels(s->px, s->w, s->h);
    if (mfd < 0) { close(o); unlink(tmp); return js_throw_code(c, "sys.outOfMemory", NULL); }
    int fds[2] = { mfd, o };
    int id = media_job(0, MW_ENCODE, JP_CLIP, fds, 2, s->w, s->h, "format=png\n", tmp, dst, 0);
    return id > 0 ? JS_NewInt32(c, id) : js_throw_code(c, "media.queueFull", NULL);
}

/* ---- recordings ---- */

/* screenRecord(app, salida.mp4, { mic, source, fps, hw }) → id */
static JSValue f_screen_record(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = argc > 0 ? arg_app(c, argv[0]) : NULL, *out = argc > 1 ? arg_path(c, argv[1]) : NULL;
    JSValueConst o = argc > 2 ? argv[2] : JS_UNDEFINED;
    JSValue ret = JS_EXCEPTION;
    if (!app || !out) { ret = js_throw_code(c, "media.invalidArguments", NULL); goto out; }
    int mic = opt_int(c, o, "mic", 0, 0, 1);
    if (!permitted(c, app, PERM_SCREEN) || (mic && !permitted(c, app, PERM_MICROPHONE))) goto out;
    MediaSrc *s = media_src_new(SK_SCREENREC, app);
    if (!s) { ret = js_throw_code(c, "media.tooMany", NULL); goto out; }
    int fd = media_open_output(out, s->tmp, sizeof s->tmp, s->dst, sizeof s->dst);
    if (fd < 0) { media_src_close(s); ret = path_error(c, fd); goto out; }
    int ring = -1;
    char src[256], appname[64];
    opt_str(c, o, "source", src, sizeof src);
    opt_str(c, o, "appName", appname, sizeof appname);
    if (mic) s->mic = maudio_open(1, appname[0] ? appname : app, "Screen recording", src, &ring);
    int hw = opt_int(c, o, "hw", 1, 0, 1), render = hw ? media_render_fd() : -1;
    char params[160];
    int cw, chh;
    media_capture_size(&cw, &chh);
    snprintf(params, sizeof params, "width=%d\nheight=%d\nfps=%d\nhw=%d\nhwaccel=%s\n", cw, chh,
             opt_int(c, o, "fps", 30, 5, 60), render >= 0, media_hwaccel());
    int fds[3] = { fd, ring, render };
    s->sock = media_spawn(MK_SCREENREC, params, fds, 3, &s->pid);
    close(fd);
    if (render >= 0) close(render);
    if (s->sock < 0 || media_screenrec_start(s, opt_int(c, o, "fps", 30, 5, 60))) {
        /* no lord-media (0.8.8.6 shipped without it, GitHub #77) is said as such, not as "another recording" */
        int nohelper = s->sock < 0;
        media_src_close(s);
        ret = js_throw_code(c, nohelper ? "media.noHelper" : "rec.startFailed", NULL);
        goto out;
    }
    ret = JS_NewInt32(c, s->id);
out:
    if (app) JS_FreeCString(c, app);
    if (out) JS_FreeCString(c, out);
    return ret;
}

/* audioRecord(app, salida.wav, { source }) → id */
static JSValue f_audio_record(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = argc > 0 ? arg_app(c, argv[0]) : NULL, *out = argc > 1 ? arg_path(c, argv[1]) : NULL;
    JSValueConst o = argc > 2 ? argv[2] : JS_UNDEFINED;
    JSValue ret = JS_EXCEPTION;
    if (!app || !out) { ret = js_throw_code(c, "media.invalidArguments", NULL); goto out; }
    if (!permitted(c, app, PERM_MICROPHONE)) goto out;
    MediaSrc *s = media_src_new(SK_AUDIOREC, app);
    if (!s) { ret = js_throw_code(c, "media.tooMany", NULL); goto out; }
    int fd = media_open_output(out, s->tmp, sizeof s->tmp, s->dst, sizeof s->dst);
    if (fd < 0) { media_src_close(s); ret = path_error(c, fd); goto out; }
    char src[256], appname[64];
    opt_str(c, o, "source", src, sizeof src);
    opt_str(c, o, "appName", appname, sizeof appname);
    int ring = -1;
    s->mic = maudio_open(1, appname[0] ? appname : app, "Recording", src, &ring);
    if (!s->mic) { close(fd); media_src_close(s); ret = js_throw_code(c, "rec.noMicrophone", NULL); goto out; }
    int fds[2] = { fd, ring };
    s->sock = media_spawn(MK_AUDIOREC, "", fds, 2, &s->pid);
    close(fd);
    if (s->sock < 0) { media_src_close(s); ret = js_throw_code(c, "media.noHelper", NULL); goto out; }
    ret = JS_NewInt32(c, s->id);
out:
    if (app) JS_FreeCString(c, app);
    if (out) JS_FreeCString(c, out);
    return ret;
}

/* stop(id): ends a recording (the file is properly closed; "done" arrives) */
static JSValue f_stop(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    MediaSrc *s = argc ? media_src_get(arg_id(c, argv[0])) : NULL;
    if (!s || s->sock < 0) return JS_FALSE;
    return JS_NewBool(c, mp_sendv(s->sock, MM_STOP, 0, 0, 0, 0, 0) == 0);
}

/* ---- camera ---- */

static JSValue f_cameras(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    static char json[8192];
    media_cameras(json, sizeof json);
    return JS_ParseJSON(c, json, strlen(json), "<cameras>");
}

static int dev_ok(const char *d) {
    if (!d || strncmp(d, "/dev/video", 10)) return 0;
    size_t n = strlen(d + 10);
    if (n < 1 || n > 2) return 0;
    for (size_t i = 0; i < n; i++) if (!isdigit((unsigned char)d[10 + i])) return 0;
    return 1;
}

/* camera(app, '/dev/videoN', { width, height }) → id */
static JSValue f_camera(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = argc > 0 ? arg_app(c, argv[0]) : NULL, *dev = argc > 1 ? JS_ToCString(c, argv[1]) : NULL;
    JSValueConst o = argc > 2 ? argv[2] : JS_UNDEFINED;
    JSValue ret = JS_EXCEPTION;
    if (!app || !dev_ok(dev)) { ret = js_throw_code(c, "camera.invalid", NULL); goto out; }
    if (!permitted(c, app, PERM_CAMERA)) goto out;
    int fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { ret = js_throw_code(c, "camera.openFailed", NULL); goto out; }
    MediaSrc *s = media_src_new(SK_CAMERA, app);
    if (!s) { close(fd); ret = js_throw_code(c, "media.tooMany", NULL); goto out; }
    int render = media_render_fd();
    char params[96];
    snprintf(params, sizeof params, "width=%d\nheight=%d\nhw=%d\nhwaccel=%s\n", opt_int(c, o, "width", 1280, 160, 3840),
             opt_int(c, o, "height", 720, 120, 2160), render >= 0, media_hwaccel());
    int fds[2] = { fd, render };
    s->sock = media_spawn(MK_CAMERA, params, fds, 2, &s->pid);
    close(fd);
    if (render >= 0) close(render);
    if (s->sock < 0) { media_src_close(s); ret = js_throw_code(c, "media.noHelper", NULL); goto out; }
    ret = JS_NewInt32(c, s->id);
out:
    if (app) JS_FreeCString(c, app);
    if (dev) JS_FreeCString(c, dev);
    return ret;
}

/* photo(id, output.jpg, mirror) → true if requested; "photo" arrives */
static JSValue f_photo(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    MediaSrc *s = argc >= 2 ? media_src_get(arg_id(c, argv[0])) : NULL;
    const char *out = s && s->kind == SK_CAMERA && s->sock >= 0 ? arg_path(c, argv[1]) : NULL;
    if (!out) return JS_FALSE;
    char *tmp = s->tmp, *dst = s->dst;
    if (tmp[0]) { JS_FreeCString(c, out); return JS_FALSE; }        /* a photo is already in progress */
    int fd = media_open_output(out, tmp, 2048, dst, 2048);
    JS_FreeCString(c, out);
    if (fd < 0) { tmp[0] = 0; return path_error(c, fd); }
    int64_t v[6] = { argc > 2 && JS_ToBool(c, argv[2]), 0, 0, 0, 0, 0 };
    int r = mp_send(s->sock, MM_PHOTO, 1, v, NULL, 0, &fd, 1);
    close(fd);
    if (r) { media_finish_output(tmp, dst, 0); tmp[0] = 0; }
    return JS_NewBool(c, r == 0);
}

/* cameraRecord(id, output.mp4 | null, { mic, mirror, appName }) → true; null stops it ("video" arrives) */
static JSValue f_camera_record(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    MediaSrc *s = argc >= 2 ? media_src_get(arg_id(c, argv[0])) : NULL;
    if (!s || s->kind != SK_CAMERA || s->sock < 0) return JS_FALSE;
    char *tmp = s->tmp + 2048, *dst = s->dst + 2048;
    if (JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])) {
        int r = mp_send(s->sock, MM_RECORD, 2, NULL, NULL, 0, NULL, 0);
        return JS_NewBool(c, r == 0);
    }
    if (tmp[0]) return JS_FALSE;
    JSValueConst o = argc > 2 ? argv[2] : JS_UNDEFINED;
    int mic = opt_int(c, o, "mic", 1, 0, 1);
    if (mic && (perm_get(s->app, PERM_MICROPHONE) != PERM_ALLOW || restrict_blocks(BUILTIN_FWID, FW_R_NO_MIC))) mic = 0;    /* no permission: video without sound */
    const char *out = arg_path(c, argv[1]);
    if (!out) return JS_FALSE;
    int fd = media_open_output(out, tmp, 2048, dst, 2048);
    JS_FreeCString(c, out);
    if (fd < 0) { tmp[0] = 0; return path_error(c, fd); }
    int ring = -1;
    if (mic) {
        char appname[64];
        opt_str(c, o, "appName", appname, sizeof appname);
        maudio_close(s->mic);
        s->mic = maudio_open(1, appname[0] ? appname : s->app, "Camera video", "", &ring);
    }
    int64_t v[6] = { opt_int(c, o, "mirror", 0, 0, 1), 0, 0, 0, 0, 0 };
    int fds[2] = { fd, ring };
    int r = mp_send(s->sock, MM_RECORD, 2, v, NULL, 0, fds, ring >= 0 ? 2 : 1);
    close(fd);
    if (r) { media_finish_output(tmp, dst, 0); tmp[0] = 0; }
    return JS_NewBool(c, r == 0);
}

static JSValue f_protected(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewInt32(c, media_protected_count());
}

static JSValue f_hwaccel(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewString(c, media_hwaccel());
}

/* composition(): "gpu" when the GPU composes this session (main.c lord_composition). */
const char *lord_composition(void);
static JSValue f_composition(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewString(c, lord_composition());
}

/* hwDecode(): can this computer decode VP9/AV1 in hardware? The object from media/hwdec.c, or null
 * while it is being found out (the first call starts it; cached per boot). docs/VIDEO.md */
static JSValue f_hwdecode(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    const char *j = hwdec_result();
    return j ? JS_ParseJSON(c, j, strlen(j), "<hwdec>") : JS_NULL;
}

static const JSCFunctionListEntry media_funcs[] = {
    JS_CFUNC_DEF("play", 3, f_play),
    JS_CFUNC_DEF("control", 3, f_control),
    JS_CFUNC_DEF("close", 1, f_close),
    JS_CFUNC_DEF("view", 2, f_view),
    JS_CFUNC_DEF("info", 1, f_info),
    JS_CFUNC_DEF("image", 2, f_image),
    JS_CFUNC_DEF("thumb", 2, f_thumb),
    JS_CFUNC_DEF("probe", 1, f_probe),
    JS_CFUNC_DEF("waveform", 2, f_waveform),
    JS_CFUNC_DEF("poster", 2, f_poster),
    JS_CFUNC_DEF("take", 1, f_take),
    JS_CFUNC_DEF("edit", 4, f_edit),
    JS_CFUNC_DEF("transcode", 4, f_transcode),
    JS_CFUNC_DEF("screenshot", 2, f_screenshot),
    JS_CFUNC_DEF("crop", 5, f_crop),
    JS_CFUNC_DEF("pixels", 5, f_pixels),
    JS_CFUNC_DEF("ocr", 2, f_ocr),
    JS_CFUNC_DEF("resize", 3, f_resize),
    JS_CFUNC_DEF("rotate", 2, f_rotate),
    JS_CFUNC_DEF("annotate", 3, f_annotate),
    JS_CFUNC_DEF("saveImage", 3, f_save_image),
    JS_CFUNC_DEF("copyImage", 1, f_copy_image),
    JS_CFUNC_DEF("screenRecord", 3, f_screen_record),
    JS_CFUNC_DEF("audioRecord", 3, f_audio_record),
    JS_CFUNC_DEF("stop", 1, f_stop),
    JS_CFUNC_DEF("cameras", 0, f_cameras),
    JS_CFUNC_DEF("camera", 3, f_camera),
    JS_CFUNC_DEF("photo", 3, f_photo),
    JS_CFUNC_DEF("cameraRecord", 3, f_camera_record),
    JS_CFUNC_DEF("protectedCount", 0, f_protected),
    JS_CFUNC_DEF("hwaccel", 0, f_hwaccel),
    JS_CFUNC_DEF("hwDecode", 0, f_hwdecode),
    JS_CFUNC_DEF("composition", 0, f_composition),
};

/* A restriction was just added to the built-in apps: what it covers stops now, like revoking the
 * permission of an isolated app. The camera's lord-media is killed (the window gets "closed" with
 * revoked); recordings that use the microphone or the screen are finished properly (the file is
 * kept up to the moment of the restriction). Sources of isolated apps (bk) are the broker's. */
static void media_restricted(uint32_t added) {
    for (int i = 0; i < MEDIA_MAX_SRC; i++) {
        MediaSrc *s = media_src_at(i);
        if (!s || s->bk) continue;
        if (s->kind == SK_CAMERA && (added & FW_R_NO_CAMERA)) {
            int id = s->id;
            fprintf(stderr, "media: camera of %s stopped by a restriction\n", s->app);
            media_src_close(s);
            media_event(id, "closed", "{\"crashed\":false,\"revoked\":true}");
            continue;
        }
        if (s->kind == SK_CAMERA && s->mic && (added & FW_R_NO_MIC) && s->sock >= 0) {
            mp_send(s->sock, MM_RECORD, 2, NULL, NULL, 0, NULL, 0);      /* the video recording stops */
            maudio_close(s->mic);
            s->mic = NULL;
        }
        int stop = (s->kind == SK_AUDIOREC && (added & FW_R_NO_MIC)) ||
                   (s->kind == SK_SCREENREC && ((added & FW_R_NO_SCREEN) || (s->mic && (added & FW_R_NO_MIC))));
        if (stop && s->sock >= 0) {
            fprintf(stderr, "media: recording of %s stopped by a restriction\n", s->app);
            mp_sendv(s->sock, MM_STOP, 0, 0, 0, 0, 0);
        }
    }
}

void js_media_register(JSContext *c, JSValue lord) {
    restrict_set_local_hook(media_restricted);
    JSValue m = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, m, media_funcs, sizeof media_funcs / sizeof media_funcs[0]);
    JS_SetPropertyStr(c, lord, "media", m);
}

/* `media` property of a <MediaView> node: which source it draws. */
void js_media_node(Node *n, int id) {
    if (!n || n->type != NODE_MEDIA) return;
    n->term = id;
    MediaSrc *s = media_src_get(id);
    if (s) { s->node = n->id; s->sent_w = s->sent_h = 0; }
}
