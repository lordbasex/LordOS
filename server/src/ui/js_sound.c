/* js_sound.c — lord.sound: outputs, inputs and per-app volume (PipeWire client in
 * sound/snd.c). It launches nothing and touches no files: it only talks to the user's sound server.
 * Each command validates its type and index; changes arrive later as a "state" event to
 * __lordSound (see docs/SOUND.md). */
#include "js_sound.h"
#include "../sound/snd.h"

#include <stdlib.h>
#include <string.h>

/* "sink" | "source" | "input" | "output" → SND_* (-1 otherwise) */
static int arg_kind(JSContext *c, JSValueConst v) {
    const char *s = JS_ToCString(c, v);
    int k = -1;
    if (s) {
        if (!strcmp(s, "sink")) k = SND_SINK;
        else if (!strcmp(s, "source")) k = SND_SOURCE;
        else if (!strcmp(s, "input")) k = SND_INPUT;
        else if (!strcmp(s, "output")) k = SND_OUTPUT;
        JS_FreeCString(c, s);
    }
    return k;
}

static int arg_index(JSContext *c, JSValueConst v, uint32_t *out) {
    double d;
    if (!JS_IsNumber(v) || JS_ToFloat64(c, &d, v) || d < 0 || d > 0xfffffffe || d != (double)(uint32_t)d) return -1;
    *out = (uint32_t)d;
    return 0;
}

/* Short string without control characters (device and port names). */
static const char *arg_name(JSContext *c, JSValueConst v) {
    const char *s = JS_ToCString(c, v);
    if (!s) return NULL;
    size_t n = strlen(s);
    int ok = n > 0 && n < 256;
    for (size_t i = 0; ok && i < n; i++) if ((unsigned char)s[i] < 0x20) ok = 0;
    if (!ok) { JS_FreeCString(c, s); return NULL; }
    return s;
}

static JSValue f_state(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    char *j = snd_state_json();
    if (!j) return JS_NULL;
    JSValue v = JS_ParseJSON(c, j, strlen(j), "<sound>");
    free(j);
    return v;
}

/* setVolume(kind, index, level | [per channel]): 1 = 100 %, up to 1.5. */
static JSValue f_set_volume(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t idx;
    int kind = argc >= 3 ? arg_kind(c, argv[0]) : -1;
    if (kind < 0 || arg_index(c, argv[1], &idx)) return JS_FALSE;
    if (JS_IsNumber(argv[2])) {
        double v;
        if (JS_ToFloat64(c, &v, argv[2])) return JS_FALSE;
        return JS_NewBool(c, snd_set_level(kind, idx, v) == 0);
    }
    if (!JS_IsArray(c, argv[2])) return JS_FALSE;
    double vol[32];
    int n = 0;
    JSValue len = JS_GetPropertyStr(c, argv[2], "length");
    int32_t l = 0;
    JS_ToInt32(c, &l, len);
    JS_FreeValue(c, len);
    if (l < 1 || l > 32) return JS_FALSE;
    for (; n < l; n++) {
        JSValue e = JS_GetPropertyUint32(c, argv[2], (uint32_t)n);
        int bad = !JS_IsNumber(e) || JS_ToFloat64(c, &vol[n], e);
        JS_FreeValue(c, e);
        if (bad) return JS_FALSE;
    }
    return JS_NewBool(c, snd_set_volume(kind, idx, vol, n) == 0);
}

static JSValue f_set_mute(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t idx;
    int kind = argc >= 3 ? arg_kind(c, argv[0]) : -1;
    if (kind < 0 || arg_index(c, argv[1], &idx)) return JS_FALSE;
    return JS_NewBool(c, snd_set_mute(kind, idx, JS_ToBool(c, argv[2])) == 0);
}

static JSValue f_set_default(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int kind = argc >= 2 ? arg_kind(c, argv[0]) : -1;
    if (kind != SND_SINK && kind != SND_SOURCE) return JS_FALSE;
    const char *name = arg_name(c, argv[1]);
    if (!name) return JS_FALSE;
    int ok = snd_set_default(kind, name) == 0;
    JS_FreeCString(c, name);
    return JS_NewBool(c, ok);
}

static JSValue f_set_port(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t idx;
    int kind = argc >= 3 ? arg_kind(c, argv[0]) : -1;
    if ((kind != SND_SINK && kind != SND_SOURCE) || arg_index(c, argv[1], &idx)) return JS_FALSE;
    const char *port = arg_name(c, argv[2]);
    if (!port) return JS_FALSE;
    int ok = snd_set_port(kind, idx, port) == 0;
    JS_FreeCString(c, port);
    return JS_NewBool(c, ok);
}

/* move("input", app, output) or move("output", app, input) */
static JSValue f_move(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t idx, dev;
    int kind = argc >= 3 ? arg_kind(c, argv[0]) : -1;
    if ((kind != SND_INPUT && kind != SND_OUTPUT) || arg_index(c, argv[1], &idx) || arg_index(c, argv[2], &dev)) return JS_FALSE;
    return JS_NewBool(c, snd_move(kind, idx, dev) == 0);
}

/* setProfile(card, profile): a Bluetooth card's profile, one of the state's "cards" (issue #78). */
static JSValue f_set_profile(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t idx;
    if (argc < 2 || arg_index(c, argv[0], &idx)) return JS_FALSE;
    const char *profile = arg_name(c, argv[1]);
    if (!profile) return JS_FALSE;
    int ok = snd_set_profile(idx, profile) == 0;
    JS_FreeCString(c, profile);
    return JS_NewBool(c, ok);
}

/* meter(input index) turns the meter on ("peak" events); meter(-1) or meter() turns it off. */
static JSValue f_meter(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t idx = 0xffffffffu;
    if (argc >= 1 && JS_IsNumber(argv[0])) {
        double d;
        JS_ToFloat64(c, &d, argv[0]);
        if (d >= 0 && arg_index(c, argv[0], &idx)) return JS_FALSE;
    }
    return JS_NewBool(c, snd_meter(idx) == 0);
}

static JSValue f_play(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_FALSE;
    const char *name = arg_name(c, argv[0]);
    if (!name) return JS_FALSE;
    int ok = snd_play(name) == 0;
    JS_FreeCString(c, name);
    return JS_NewBool(c, ok);
}

static const JSCFunctionListEntry sound_funcs[] = {
    JS_CFUNC_DEF("state", 0, f_state),
    JS_CFUNC_DEF("setVolume", 3, f_set_volume),
    JS_CFUNC_DEF("setMute", 3, f_set_mute),
    JS_CFUNC_DEF("setDefault", 2, f_set_default),
    JS_CFUNC_DEF("setPort", 3, f_set_port),
    JS_CFUNC_DEF("move", 3, f_move),
    JS_CFUNC_DEF("setProfile", 2, f_set_profile),
    JS_CFUNC_DEF("meter", 1, f_meter),
    JS_CFUNC_DEF("play", 1, f_play),
};

void js_sound_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, sound_funcs, sizeof sound_funcs / sizeof sound_funcs[0]);
    JS_SetPropertyStr(c, lord, "sound", o);
}
