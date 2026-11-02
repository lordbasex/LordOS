/* js_brokercam.c — `lord.media` inside an isolated .lpk app: the camera, through lord-server
 * (sec/brokercam.c, docs/PRIVACY.md «Camera»).
 *
 * The app never opens /dev/video* nor runs lord-media. lord-server does, after consent, and gives
 * the app a READ-ONLY fd of lord-media's frame ring: mapped PROT_READ here and drawn by the same
 * <MediaView> code as in the session (media/mview.c), from a local media source with no helper.
 * Each frame arrives as a broker event; the app acknowledges the slot it now shows (lord-server
 * validates every ACK). Photos and videos go to a file the user picks in the system's save dialog;
 * the app only learns the file name. */
#include "js_broker.h"
#include "js.h"
#include "js_err.h"
#include "../sec/broker.h"
#include "../media/media_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_ACAMS 8

typedef struct { int used, sid, local, sent_w, sent_h; } ACam;
static ACam acams[MAX_ACAMS];

static ACam *by_local(int id) { for (int i = 0; i < MAX_ACAMS; i++) if (acams[i].used && acams[i].local == id) return &acams[i]; return NULL; }
static ACam *by_sid(int sid) { for (int i = 0; i < MAX_ACAMS; i++) if (acams[i].used && acams[i].sid == sid) return &acams[i]; return NULL; }

static int cmd(const char *fmt, int sid, const char *rest, int *st_out) {
    char p[400];
    int n = snprintf(p, sizeof p, fmt, sid, rest ? rest : "");
    int st = BST_ERROR;
    if (n <= 0 || n >= (int)sizeof p || js_broker_rpc(BOP_CAMERA, p, (uint32_t)n, &st, NULL, NULL)) st = BST_ERROR;
    if (st_out) *st_out = st;
    return st == BST_OK;
}

/* (re)map the ring lord-server gives us: read-only */
static void fetch_ring(ACam *a) {
    char p[32], info[128];
    int n = snprintf(p, sizeof p, "ring %d", a->sid);
    int fd = js_broker_rpc_fd(BOP_CAMERA, p, (uint32_t)n, info, sizeof info);
    if (fd < 0) return;
    int gen = 0, w = 0, h = 0, slots = 0;
    MediaSrc *s = media_src_get(a->local);
    struct stat st;
    size_t need = 0;
    if (sscanf(info, "%d %d %d %d", &gen, &w, &h, &slots) == 4 && w > 1 && h > 1 && w <= MP_MAX_DIM && h <= MP_MAX_DIM &&
        slots >= 1 && slots <= MP_RING_SLOTS && !fstat(fd, &st))
        need = (size_t)w * (size_t)h * 4 * (size_t)slots;
    void *m = s && need && (size_t)st.st_size >= need ? mmap(NULL, need, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
    close(fd);
    if (m == MAP_FAILED) return;
    if (s->map) munmap(s->map, s->map_len);
    s->map = m;
    s->map_len = need;
    s->ring_gen = gen;
    s->ring_w = w;
    s->ring_h = h;
    s->ring_slots = slots;
    s->cur_slot = -1;
}

static void close_local(ACam *a) {
    MediaSrc *s = media_src_get(a->local);
    if (s) media_src_close(s);           /* unmaps the ring; there is no helper here */
    a->used = 0;
}

void js_brokercam_event(const char *data, size_t len) {
    char buf[1100];
    if (len >= sizeof buf) return;
    memcpy(buf, data, len);
    buf[len] = 0;
    char what[16] = "";
    int sid = 0;
    if (sscanf(buf, "%15s %d", what, &sid) != 2) return;
    ACam *a = by_sid(sid);
    if (!a) return;
    MediaSrc *s = media_src_get(a->local);
    if (!s) { a->used = 0; return; }
    if (!strcmp(what, "ring")) { fetch_ring(a); return; }
    if (!strcmp(what, "frame")) {
        unsigned id = 0;
        sscanf(buf, "%*s %*d %u", &id);
        int gen = (int)(id >> 8), slot = (int)(id & 0xff);
        if (gen != s->ring_gen) fetch_ring(a);
        if (!s->map || gen != s->ring_gen || slot >= s->ring_slots) return;
        s->cur_slot = slot;
        s->w = s->ring_w;
        s->h = s->ring_h;
        s->frame_seq++;
        media_src_damage(s);
        char p[48];
        int n = snprintf(p, sizeof p, "ack %d %u", sid, id);
        js_broker_send(BOP_CAMERA, p, (uint32_t)n);
        /* lord-media scales to the box where it is shown (as MM_SIZE does in the session) */
        int bw = s->box_w, bh = s->box_h;
        if (bw > 0 && bh > 0 && (bw != a->sent_w || bh != a->sent_h) && !s->view.zoom && !s->view.fit) {
            int rw = (s->view.rot % 180) ? bh : bw, rh = (s->view.rot % 180) ? bw : bh;
            n = snprintf(p, sizeof p, "size %d %d %d", sid, rw, rh);
            if (!js_broker_send(BOP_CAMERA, p, (uint32_t)n)) { a->sent_w = bw; a->sent_h = bh; }
        }
        return;
    }
    if (!strcmp(what, "event")) {
        char type[16] = "";
        int off = 0;
        if (sscanf(buf, "%*s %*d %15s %n", type, &off) < 1 || !off) return;
        js_media_event(a->local, type, buf + off);
        return;
    }
    if (!strcmp(what, "closed")) {
        int revoked = strstr(buf, "revoked") != NULL;
        js_media_event(a->local, "closed", revoked ? "{\"crashed\":false,\"revoked\":true}" : "{\"crashed\":false}");
        close_local(a);
    }
}

/* ---- lord.media (camera subset) ---- */

static int arg_id(JSContext *c, JSValueConst v) { int32_t id = 0; JS_ToInt32(c, &id, v); return id; }

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

static JSValue f_cameras(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    int st = BST_ERROR;
    char *out = NULL;
    uint32_t len = 0;
    if (js_broker_rpc(BOP_CAMERA, "list", 4, &st, &out, &len) || st != BST_OK || !out) { free(out); return JS_NewArray(c); }
    JSValue v = JS_ParseJSON(c, out, len, "<cameras>");
    free(out);
    if (JS_IsException(v)) { JS_FreeValue(c, JS_GetException(c)); return JS_NewArray(c); }
    return v;
}

/* camera(app, '/dev/videoN', { width, height }) → id (lord-server checks the permission) */
static JSValue f_camera(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *dev = argc > 1 ? JS_ToCString(c, argv[1]) : NULL;
    JSValueConst o = argc > 2 ? argv[2] : JS_UNDEFINED;
    ACam *a = NULL;
    for (int i = 0; i < MAX_ACAMS && !a; i++) if (!acams[i].used) a = &acams[i];
    char p[96];
    const char *codec = NULL;
    if (JS_IsObject(o)) { JSValue v = JS_GetPropertyStr(c, o, "codec"); if (JS_IsString(v)) codec = JS_ToCString(c, v); JS_FreeValue(c, v); }
    int n = dev && a ? snprintf(p, sizeof p, "open %.40s %d %d %d %.6s %d", dev, opt_int(c, o, "width", 1280, 160, 3840), opt_int(c, o, "height", 720, 120, 2160),
                                opt_int(c, o, "link", 0, 0, 99999), codec && !strcmp(codec, "vp8") ? "vp8" : "h264", opt_int(c, o, "kbps", 600, 100, 8000)) : -1;
    if (codec) JS_FreeCString(c, codec);
    if (dev) JS_FreeCString(c, dev);
    if (n <= 0) return js_throw_code(c, "camera.invalid", NULL);
    int st = BST_ERROR;
    char *out = NULL;
    uint32_t len = 0;
    if (js_broker_rpc(BOP_CAMERA, p, (uint32_t)n, &st, &out, &len) || st != BST_OK || !out) {
        char line[160] = "";
        if (st == BST_DENIED && out && len && len < sizeof line) { memcpy(line, out, len); line[len] = 0; }
        free(out);
        if (lerr_is(line)) return js_throw_line(c, line);          /* fw.restricted.camera */
        return js_throw_code(c, st == BST_DENIED ? "perm.cameraDenied" : "camera.openFailed", NULL);
    }
    int sid = atoi(out);
    free(out);
    MediaSrc *s = media_src_new(SK_CAMERA, "app");
    if (!s) { cmd("close %d%s", sid, NULL, NULL); return js_throw_code(c, "media.tooMany", NULL); }
    *a = (ACam){ .used = 1, .sid = sid, .local = s->id };
    return JS_NewInt32(c, s->id);
}

/* live(app, tag) → id: what the app's companion hands over under `tag` shows in a <MediaView> (media/live.h) */
static JSValue f_live(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t tag = 0;
    if (argc > 1) JS_ToInt32(c, &tag, argv[1]);
    ACam *a = NULL;
    for (int i = 0; i < MAX_ACAMS && !a; i++) if (!acams[i].used) a = &acams[i];
    if (!a || tag < 1) return js_throw_code(c, "camera.invalid", NULL);
    char p[32];
    int n = snprintf(p, sizeof p, "live %d", tag), st = BST_ERROR;
    char *out = NULL;
    uint32_t len = 0;
    if (js_broker_rpc(BOP_CAMERA, p, (uint32_t)n, &st, &out, &len) || st != BST_OK || !out) { free(out); return js_throw_code(c, "media.noHelper", NULL); }
    int sid = atoi(out);
    free(out);
    MediaSrc *s = media_src_new(SK_CAMERA, "app");
    if (!s) { cmd("close %d%s", sid, NULL, NULL); return js_throw_code(c, "media.tooMany", NULL); }
    *a = (ACam){ .used = 1, .sid = sid, .local = s->id };
    return JS_NewInt32(c, s->id);
}

/* liveMic(id, on) → true: lord-server checks the permission, opens the microphone and lights its indicator */
static JSValue f_live_mic(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    ACam *a = argc >= 2 ? by_local(arg_id(c, argv[0])) : NULL;
    if (!a) return JS_FALSE;
    return JS_NewBool(c, cmd("mic %d %s", a->sid, JS_ToBool(c, argv[1]) ? "1" : "0", NULL));
}

static JSValue f_close(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    ACam *a = argc ? by_local(arg_id(c, argv[0])) : NULL;
    if (a) { cmd("close %d%s", a->sid, NULL, NULL); close_local(a); }
    return JS_UNDEFINED;
}

/* view(id, { fit, zoom, rotate, mirror, background }): how <MediaView> draws it (local) */
static JSValue f_view(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    MediaSrc *s = argc >= 2 && by_local(arg_id(c, argv[0])) ? media_src_get(arg_id(c, argv[0])) : NULL;
    if (!s || !JS_IsObject(argv[1])) return JS_FALSE;
    MediaView v = s->view;
    JSValue fit = JS_GetPropertyStr(c, argv[1], "fit");
    if (JS_IsString(fit)) { const char *f = JS_ToCString(c, fit); if (f) { v.fit = !strcmp(f, "cover"); JS_FreeCString(c, f); } }
    JS_FreeValue(c, fit);
    int rot = ((opt_int(c, argv[1], "rotate", v.rot, -3600, 3600) % 360) + 360) % 360;
    v.rot = rot - rot % 90;
    v.mirror = opt_int(c, argv[1], "mirror", v.mirror, 0, 1);
    JSValue bg = JS_GetPropertyStr(c, argv[1], "background");
    if (JS_IsNumber(bg)) { double d; JS_ToFloat64(c, &d, bg); v.bg = (uint32_t)(int64_t)d; }
    JS_FreeValue(c, bg);
    v.gen = s->view.gen + 1;
    s->view = v;
    media_src_damage(s);
    return JS_TRUE;
}

static JSValue f_info(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    ACam *a = argc ? by_local(arg_id(c, argv[0])) : NULL;
    MediaSrc *s = a ? media_src_get(a->local) : NULL;
    if (!s) return JS_NULL;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "kind", JS_NewString(c, "camera"));
    JS_SetPropertyStr(c, o, "width", JS_NewInt32(c, s->ring_w));
    JS_SetPropertyStr(c, o, "height", JS_NewInt32(c, s->ring_h));
    JS_SetPropertyStr(c, o, "alive", JS_TRUE);
    return o;
}

/* photo(id, suggestedName, mirror) → true: the system's save dialog opens; "photo" arrives later */
static JSValue f_photo(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    ACam *a = argc >= 2 ? by_local(arg_id(c, argv[0])) : NULL;
    if (!a) return JS_FALSE;
    const char *name = JS_IsString(argv[1]) ? JS_ToCString(c, argv[1]) : NULL;
    char rest[300];
    snprintf(rest, sizeof rest, "%d %.250s", argc > 2 && JS_ToBool(c, argv[2]), name ? name : "foto.jpg");
    if (name) JS_FreeCString(c, name);
    return JS_NewBool(c, cmd("photo %d %s", a->sid, rest, NULL));
}

/* cameraRecord(id, suggestedName | null, { mic, mirror }) → true; null ends it ("video" arrives) */
static JSValue f_camera_record(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    ACam *a = argc >= 2 ? by_local(arg_id(c, argv[0])) : NULL;
    if (!a) return JS_FALSE;
    if (JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])) return JS_NewBool(c, cmd("recstop %d%s", a->sid, NULL, NULL));
    JSValueConst o = argc > 2 ? argv[2] : JS_UNDEFINED;
    const char *name = JS_IsString(argv[1]) ? JS_ToCString(c, argv[1]) : NULL;
    char rest[300];
    snprintf(rest, sizeof rest, "%d %d %.250s", opt_int(c, o, "mirror", 0, 0, 1), opt_int(c, o, "mic", 1, 0, 1), name ? name : "video.mp4");
    if (name) JS_FreeCString(c, name);
    return JS_NewBool(c, cmd("record %d %s", a->sid, rest, NULL));
}

static JSValue f_stop(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    ACam *a = argc ? by_local(arg_id(c, argv[0])) : NULL;
    return JS_NewBool(c, a && cmd("recstop %d%s", a->sid, NULL, NULL));
}

static JSValue f_zero(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; (void)argv; return JS_NewInt32(c, 0); }
static JSValue f_none(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; (void)argv; return JS_NewString(c, "none"); }

static const JSCFunctionListEntry funcs[] = {
    JS_CFUNC_DEF("cameras", 0, f_cameras), JS_CFUNC_DEF("camera", 3, f_camera), JS_CFUNC_DEF("close", 1, f_close),
    JS_CFUNC_DEF("live", 2, f_live), JS_CFUNC_DEF("liveMic", 2, f_live_mic),
    JS_CFUNC_DEF("view", 2, f_view), JS_CFUNC_DEF("info", 1, f_info), JS_CFUNC_DEF("photo", 3, f_photo),
    JS_CFUNC_DEF("cameraRecord", 3, f_camera_record), JS_CFUNC_DEF("stop", 1, f_stop),
    JS_CFUNC_DEF("protectedCount", 0, f_zero), JS_CFUNC_DEF("hwaccel", 0, f_none),
};

void js_brokercam_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, funcs, sizeof funcs / sizeof funcs[0]);
    JS_SetPropertyStr(c, lord, "media", o);
}
