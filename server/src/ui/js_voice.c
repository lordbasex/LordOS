/* js_voice.c — lord.voice: reading text aloud (docs/VOICE.md, phase 2; speak.c starts lord-say).
 *
 *   lord.voice.available()              → lord-say and lord-tts are installed
 *   lord.voice.speak(text, lang?, voice?) → true when it started; ends the reading before. lord-say tells the text's
 *                                         language; lang ('es', 'en' or 'pt', the interface's) is for a text too short;
 *                                         voice, a plain name, is say's -v (Settings' "Try this voice")
 *   lord.voice.speakSelection(lang?)    → true when it started, false when the app with the keyboard (a Linux app)
 *                                         has selected nothing: its primary selection, read here, never given to JS
 *   lord.voice.save(text, path, lang?)  → true when it started: a WAV at path, nothing heard. path is in the home
 *                                         folder or a mount (as lord.fs.write), not a protected one, and ends in .wav
 *   lord.voice.holdSelection()          → true when the app with the keyboard (a Linux app) has a selection: it is kept
 *                                         here for saveSelection, as the dialog that asks where takes the keyboard
 *   lord.voice.saveSelection(path, lang?)   the same as save, with the selection kept; forgets it
 *   lord.voice.preview(lang, voice)     → true when it started: a voice's recorded sample (say --preview), heard as a
 *                                         reading is, with the wave; lang is es, en or pt, voice a plain name
 *   lord.voice.pause(on)                → freezes the reading where it is, or goes on
 *   lord.voice.replay()                 → reads the last text again from the start
 *   lord.voice.stop()
 *   lord.voice.state()                  → { speaking, paused, saving, exit }: exit is how the last one ended (say.c's
 *                                         statuses); speaking is also true while saving
 *   lord.voice.levels()                 → an array of SAY_BANDS numbers 0–255, how loud each band of what is heard now
 *                                         is (the player's wave), or null when nothing is
 *
 * Only in the session's context: an isolated app has no lord.voice (js_broker.c doesn't offer it). */
#include "js_voice.h"
#include "js_sys.h"
#include "../tts/speak.h"
#include "../wl/wl.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static char *held;                      /* holdSelection's, until saveSelection */
static size_t held_n;

static void forget_held(void) {
    if (held) { explicit_bzero(held, held_n); free(held); }
    held = NULL;
    held_n = 0;
}

static const char *lang_arg(JSContext *c, int argc, JSValueConst *argv, int i, char out[3]) {
    out[0] = 0;
    if (argc <= i || !JS_IsString(argv[i])) return NULL;
    const char *s = JS_ToCString(c, argv[i]);
    if (s && (!strcmp(s, "es") || !strcmp(s, "en") || !strcmp(s, "pt"))) { out[0] = s[0]; out[1] = s[1]; out[2] = 0; }
    if (s) JS_FreeCString(c, s);
    return out[0] ? out : NULL;
}

/* where a WAV may go: as lord.fs.write's targets, and named *.wav */
static int wav_target(JSContext *c, JSValueConst v, char *out, size_t n) {
    const char *p = JS_IsString(v) ? JS_ToCString(c, v) : NULL;
    if (!p) return -1;
    size_t l = strlen(p);
    int r = l > 4 && l < PATH_MAX && !strcasecmp(p + l - 4, ".wav") ? js_sys_resolve_target(p, out, n) : -1;
    JS_FreeCString(c, p);
    return r;
}

static JSValue f_available(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewBool(c, speak_available());
}

static JSValue f_speak(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    char lang[3];
    size_t n = 0;
    const char *text = argc > 0 && JS_IsString(argv[0]) ? JS_ToCStringLen(c, &n, argv[0]) : NULL;
    const char *voice = argc > 2 && JS_IsString(argv[2]) ? JS_ToCString(c, argv[2]) : NULL;
    int ok = text && n && n <= SPEAK_TEXT_MAX && !speak_start(text, n, lang_arg(c, argc, argv, 1, lang), voice);
    if (text) JS_FreeCString(c, text);
    if (voice) JS_FreeCString(c, voice);
    return JS_NewBool(c, ok);
}

static JSValue f_speak_selection(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    char lang[3];
    size_t n = 0;
    const char *text = wl_focus_primary(&n);
    if (!text) return JS_FALSE;
    return JS_NewBool(c, !speak_start(text, n > SPEAK_TEXT_MAX ? SPEAK_TEXT_MAX : n, lang_arg(c, argc, argv, 0, lang), NULL));
}

static JSValue f_save(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    char lang[3], target[PATH_MAX + 300];
    if (argc < 2 || wav_target(c, argv[1], target, sizeof target)) return JS_FALSE;
    size_t n = 0;
    const char *text = JS_IsString(argv[0]) ? JS_ToCStringLen(c, &n, argv[0]) : NULL;
    int ok = text && n && n <= SPEAK_TEXT_MAX && !speak_save(text, n, lang_arg(c, argc, argv, 2, lang), target);
    if (text) JS_FreeCString(c, text);
    return JS_NewBool(c, ok);
}

static JSValue f_hold_selection(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)c; (void)this_val; (void)argc; (void)argv;
    forget_held();
    size_t n = 0;
    const char *text = wl_focus_primary(&n);
    if (!text || !n) return JS_FALSE;
    if (n > SPEAK_TEXT_MAX) n = SPEAK_TEXT_MAX;
    if (!(held = malloc(n))) return JS_FALSE;
    memcpy(held, text, n);
    held_n = n;
    return JS_TRUE;
}

static JSValue f_save_selection(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    char lang[3], target[PATH_MAX + 300];
    int ok = held && argc > 0 && !wav_target(c, argv[0], target, sizeof target)
        && !speak_save(held, held_n, lang_arg(c, argc, argv, 1, lang), target);
    forget_held();
    return JS_NewBool(c, ok);
}

static JSValue f_preview(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) return JS_FALSE;
    const char *lang = JS_ToCString(c, argv[0]), *voice = JS_ToCString(c, argv[1]);
    int ok = lang && voice && !speak_preview(lang, voice);
    if (lang) JS_FreeCString(c, lang);
    if (voice) JS_FreeCString(c, voice);
    return JS_NewBool(c, ok);
}

static JSValue f_pause(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    return JS_NewBool(c, !speak_pause(argc > 0 && JS_ToBool(c, argv[0])));
}

static JSValue f_replay(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewBool(c, !speak_replay());
}

static JSValue f_stop(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)c; (void)this_val; (void)argc; (void)argv;
    speak_stop();
    return JS_UNDEFINED;
}

static JSValue f_state(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    int code = -1;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "speaking", JS_NewBool(c, speak_state(&code)));
    JS_SetPropertyStr(c, o, "paused", JS_NewBool(c, speak_paused()));
    JS_SetPropertyStr(c, o, "saving", JS_NewBool(c, speak_saving()));
    JS_SetPropertyStr(c, o, "exit", JS_NewInt32(c, code));
    return o;
}

static JSValue f_levels(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    uint8_t b[SAY_BANDS];
    if (!speak_levels(b)) return JS_NULL;
    JSValue a = JS_NewArray(c);
    for (int i = 0; i < SAY_BANDS; i++) JS_SetPropertyUint32(c, a, (uint32_t)i, JS_NewInt32(c, b[i]));
    return a;
}

static const JSCFunctionListEntry voice_funcs[] = {
    JS_CFUNC_DEF("available", 0, f_available),
    JS_CFUNC_DEF("speak", 3, f_speak),
    JS_CFUNC_DEF("speakSelection", 1, f_speak_selection),
    JS_CFUNC_DEF("save", 3, f_save),
    JS_CFUNC_DEF("holdSelection", 0, f_hold_selection),
    JS_CFUNC_DEF("saveSelection", 2, f_save_selection),
    JS_CFUNC_DEF("preview", 2, f_preview),
    JS_CFUNC_DEF("pause", 1, f_pause),
    JS_CFUNC_DEF("replay", 0, f_replay),
    JS_CFUNC_DEF("stop", 0, f_stop),
    JS_CFUNC_DEF("state", 0, f_state),
    JS_CFUNC_DEF("levels", 0, f_levels),
};

void js_voice_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, voice_funcs, sizeof voice_funcs / sizeof voice_funcs[0]);
    JS_SetPropertyStr(c, lord, "voice", o);
}
