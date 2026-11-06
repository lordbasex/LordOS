/* js_input.c — lord.input: which touchpads and touchscreens exist and the driver settings
 * (tp/tp.c). It only reads and changes the driver's in-memory configuration: it opens no devices and
 * touches no files (settings are saved to settings.json from the SDK, sdk/trackpad.js).
 *
 *   lord.input.devices()      → [{ name, kind: 'touchpad'|'touchscreen', clickpad, width, height, fingers }]
 *   lord.input.config()       → { speed, mouseSpeed, mouseNatural, mouseLeftHanded, scrollSpeed,
 *                                 sensitivity: 'low'|'medium'|'high', pointerSize, tap, tapDrag,
 *                                 natural, secondary, threeDrag, dwt, scroll, kinetic, pinch,
 *                                 swipe3, swipe4 }
 *   lord.input.configure(obj) → applies the valid fields (the rest stay as they are); true
 *
 * The on-screen keyboard (docs/ON-SCREEN-KEYBOARD.md). The theme's and the login screen's context
 * only: an isolated app's process has the broker's `lord`, without any of these.
 *
 *   lord.input.hasKeyboard()  → is a keyboard someone can type on attached?
 *   lord.input.keyLabels()    → { altgr, keys: { code: [plain, shift, altgr, altgr + shift] } } of the
 *                               active layout, or null without XKB
 *   lord.input.tap(code, level) → queues one press and release of a key from a closed list; the main
 *                               loop sends it down js_key, the path of a physical key, so it goes
 *                               to the focused client or field, through the layout and the input
 *                               method. level: 1 Shift, 2 AltGr. No Ctrl, Alt or Super: a tap
 *                               can't be a shortcut. Nothing here logs or keeps what is typed. */
#include "js_input.h"
#include "js.h"
#include "../hw/pkagent.h"
#include "../keyboard.h"
#include "../osk.h"
#include "../tp/tp.h"
#include "../cursor.h"

#include <linux/input.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static JSValue f_devices(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc; (void)argv;
    char buf[4096];
    tp_list_json(buf, sizeof buf);
    return JS_ParseJSON(c, buf, strlen(buf), "<input>");
}

static const struct { const char *name; size_t off; } flags[] = {
    { "mouseNatural", offsetof(TpConfig, mouse_natural) }, { "mouseLeftHanded", offsetof(TpConfig, mouse_left_handed) },
    { "tap", offsetof(TpConfig, tap) },           { "tapDrag", offsetof(TpConfig, tap_drag) },
    { "natural", offsetof(TpConfig, natural) },   { "threeDrag", offsetof(TpConfig, three_drag) },
    { "dwt", offsetof(TpConfig, dwt) },           { "scroll", offsetof(TpConfig, scroll) },
    { "kinetic", offsetof(TpConfig, kinetic) },   { "pinch", offsetof(TpConfig, pinch) },
    { "swipe3", offsetof(TpConfig, swipe3) },     { "swipe4", offsetof(TpConfig, swipe4) },
};
static const char *const secondary[] = { "two-fingers", "bottom-right", "bottom-left" };
static const char *const sensitivity[] = { "low", "medium", "high" };

/* a number in 0..1 (or lo..hi) from the object, if it's there and valid */
static int num_field(JSContext *c, JSValueConst o, const char *name, double lo, double hi, double *out) {
    JSValue v = JS_GetPropertyStr(c, o, name);
    double d;
    int ok = JS_IsNumber(v) && !JS_ToFloat64(c, &d, v) && d >= lo && d <= hi;
    JS_FreeValue(c, v);
    if (ok) *out = d;
    return ok;
}

static int str_field(JSContext *c, JSValueConst o, const char *name, const char *const *names, int n, int *out) {
    JSValue v = JS_GetPropertyStr(c, o, name);
    int found = 0;
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(c, v);
        for (int i = 0; s && i < n; i++) if (!strcmp(s, names[i])) { *out = i; found = 1; }
        JS_FreeCString(c, s);
    }
    JS_FreeValue(c, v);
    return found;
}

static JSValue f_config(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc; (void)argv;
    const TpConfig *k = tp_config();
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "speed", JS_NewFloat64(c, k->speed));
    JS_SetPropertyStr(c, o, "mouseSpeed", JS_NewFloat64(c, k->mouse_speed));
    JS_SetPropertyStr(c, o, "scrollSpeed", JS_NewFloat64(c, k->scroll_speed));
    JS_SetPropertyStr(c, o, "sensitivity", JS_NewString(c, sensitivity[k->sensitivity]));
    JS_SetPropertyStr(c, o, "pointerSize", JS_NewFloat64(c, cursor_size() / 100.0));
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++)
        JS_SetPropertyStr(c, o, flags[i].name, JS_NewBool(c, *(const int *)((const char *)k + flags[i].off)));
    JS_SetPropertyStr(c, o, "secondary", JS_NewString(c, secondary[k->secondary]));
    return o;
}

static JSValue f_configure(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this;
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_FALSE;
    TpConfig k = *tp_config();
    double d;
    num_field(c, argv[0], "speed", 0, 1, &k.speed);
    num_field(c, argv[0], "mouseSpeed", 0, 1, &k.mouse_speed);
    num_field(c, argv[0], "scrollSpeed", 0, 1, &k.scroll_speed);
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++) {
        JSValue v = JS_GetPropertyStr(c, argv[0], flags[i].name);
        if (JS_IsBool(v)) *(int *)((char *)&k + flags[i].off) = JS_ToBool(c, v);
        JS_FreeValue(c, v);
    }
    str_field(c, argv[0], "secondary", secondary, 3, &k.secondary);
    str_field(c, argv[0], "sensitivity", sensitivity, 3, &k.sensitivity);
    tp_set_config(&k);
    /* pointer size (×0.75..×2 of the display's), also for Linux apps started from now on */
    if (num_field(c, argv[0], "pointerSize", 0.5, 2, &d) && cursor_set_size((int)(d * 100 + 0.5))) {
        char v[16];
        snprintf(v, sizeof v, "%d", 24 * cursor_size() / 100);
        setenv("XCURSOR_SIZE", v, 1);
    }
    return JS_TRUE;
}

static int keyboard_present;
void js_input_set_keyboard(int present) { keyboard_present = present; }

static JSValue f_has_keyboard(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc; (void)argv;
    return JS_NewBool(c, keyboard_present);
}

static JSValue f_key_labels(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this; (void)argc; (void)argv;
    if (!kb_labels_begin()) return JS_NULL;
    JSValue o = JS_NewObject(c), keys = JS_NewObject(c);
    for (size_t i = 0; i < osk_label_count; i++) {
        JSValue row = JS_NewArray(c);
        for (int l = 0; l < 4; l++) {
            char s[32];
            kb_label(osk_label_codes[i], l, s, sizeof s);
            JS_SetPropertyUint32(c, row, (uint32_t)l, JS_NewString(c, s));
        }
        JS_SetPropertyUint32(c, keys, osk_label_codes[i], row);
    }
    kb_labels_end();
    JS_SetPropertyStr(c, o, "altgr", JS_NewBool(c, kb_has_altgr()));
    JS_SetPropertyStr(c, o, "keys", keys);
    return o;
}

static JSValue f_tap(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    (void)this;
    int32_t code = 0, level = 0;
    if (argc < 1 || JS_ToInt32(c, &code, argv[0])) return JS_FALSE;
    if (argc > 1 && JS_ToInt32(c, &level, argv[1])) return JS_FALSE;
    /* without a third level Right Alt is plain Alt: that would be a shortcut */
    if ((level & OSK_LEVEL_ALTGR) && !kb_has_altgr()) return JS_FALSE;
    return JS_NewBool(c, osk_push(code, level));
}

void js_input_flush(void) {
    OskTap t;
    while (osk_pop(&t)) {
        if (pkagent_blocks_injected(1)) continue;   /* the authentication prompt takes only physical keys */
        if (t.level & OSK_LEVEL_ALTGR) js_key(KEY_RIGHTALT, 1);
        if (t.level & OSK_LEVEL_SHIFT) js_key(KEY_LEFTSHIFT, 1);
        js_key(t.code, 1);
        js_key(t.code, 0);
        if (t.level & OSK_LEVEL_SHIFT) js_key(KEY_LEFTSHIFT, 0);
        if (t.level & OSK_LEVEL_ALTGR) js_key(KEY_RIGHTALT, 0);
    }
}

void js_input_drop(void) { osk_clear(); }

static const JSCFunctionListEntry input_funcs[] = {
    JS_CFUNC_DEF("devices", 0, f_devices),
    JS_CFUNC_DEF("config", 0, f_config),
    JS_CFUNC_DEF("configure", 1, f_configure),
    JS_CFUNC_DEF("hasKeyboard", 0, f_has_keyboard),
    JS_CFUNC_DEF("keyLabels", 0, f_key_labels),
    JS_CFUNC_DEF("tap", 2, f_tap),
};

void js_input_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, input_funcs, sizeof input_funcs / sizeof input_funcs[0]);
    JS_SetPropertyStr(c, lord, "input", o);
}
