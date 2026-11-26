/* js_spell.c — lord.spell: the spell checker (spell/spellsvc.h, docs/SPELLING.md). Registered only in the desktop's own
 * JS context; an isolated app's broker has no lord.spell. Nothing here waits: check and suggest queue a job for the
 * service's thread and take returns its result later.
 *
 *   installed()          ["en_US","es_ES",…] the dictionaries in /usr/share/hunspell
 *   setLanguages(tags)   the languages to check with (at most 4, installed ones only); how many were kept
 *   defaults(locale)     the default languages for "es_AR" / "pt_BR.UTF-8" (spellang.h, the rule lord-web uses)
 *   state()              { langs, loaded, state, queued }
 *   check(words)         a job id (0 when the queue is full): at most 64 words of at most 64 bytes
 *   suggest(word)        a job id (0 when the queue is full)
 *   take(id)             { ok, good: [1|0…] } or { ok, suggestions: […] } or { ok: false, error }, once; null while it runs
 *   words()              the person's dictionary: enchant's word lists of the languages in use (shared with lord-web)
 *   addWord(w)           true (into each language in use's list), or throws sys.badArgument / sys.writeFailed
 *   removeWord(w)        true, or throws */
#include "js_spell.h"
#include "js_err.h"
#include "../spell/spellsvc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static JSValue parsed(JSContext *c, char *j, const char *what) {
    if (!j) return JS_NULL;
    JSValue v = JS_ParseJSON(c, j, strlen(j), what);
    free(j);
    if (JS_IsException(v)) { JS_FreeValue(c, JS_GetException(c)); return JS_NULL; }
    return v;
}

/* a string argument of at most max bytes with no NUL in it, or NULL */
static const char *str_arg(JSContext *c, JSValueConst v, size_t max) {
    if (!JS_IsString(v)) return NULL;
    size_t n;
    const char *s = JS_ToCStringLen(c, &n, v);
    if (s && (n > max || strlen(s) != n)) { JS_FreeCString(c, s); return NULL; }
    return s;
}

/* an array of at most max items, each a string of at most len bytes or NULL (anything else: not a word, counted as
 * good); their count, -1 when it isn't one (nothing to free) */
static int str_list(JSContext *c, JSValueConst v, const char **out, int max, size_t len) {
    if (!JS_IsArray(c, v)) return -1;
    int32_t n = 0;
    JSValue lv = JS_GetPropertyStr(c, v, "length");
    int bad = JS_ToInt32(c, &n, lv);
    JS_FreeValue(c, lv);
    if (bad || n < 0 || n > max) return -1;
    for (int i = 0; i < (int)n; i++) {
        JSValue e = JS_GetPropertyUint32(c, v, (uint32_t)i);
        out[i] = str_arg(c, e, len);
        JS_FreeValue(c, e);
    }
    return (int)n;
}

static JSValue f_installed(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue v = parsed(c, spell_installed_json(), "<spell.installed>");
    return JS_IsNull(v) ? JS_NewArray(c) : v;
}

static JSValue f_set_languages(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *tags[SPELL_LANGS_MAX];
    int n = argc > 0 ? str_list(c, argv[0], tags, SPELL_LANGS_MAX, SPELL_TAG_MAX) : -1;
    if (n < 0) return js_throw_code(c, "sys.badArgument", NULL);
    int k = spell_set_langs(tags, n);
    for (int i = 0; i < n; i++) if (tags[i]) JS_FreeCString(c, tags[i]);
    return JS_NewInt32(c, k);
}

static JSValue f_defaults(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *loc = argc > 0 ? str_arg(c, argv[0], 64) : NULL;
    SpellTag out[2];
    int n = spell_defaults(loc ? loc : "", out);
    if (loc) JS_FreeCString(c, loc);
    JSValue a = JS_NewArray(c);
    for (int i = 0; i < n; i++) JS_SetPropertyUint32(c, a, (uint32_t)i, JS_NewString(c, out[i]));
    return a;
}

static JSValue f_state(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return parsed(c, spell_state_json(), "<spell.state>");
}

static JSValue f_check(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *w[SPELL_BATCH_MAX];
    int n = argc > 0 ? str_list(c, argv[0], w, SPELL_BATCH_MAX, 4 * SPELL_WORD_MAX) : -1;
    if (n < 1) { if (n == 0) return JS_NewInt32(c, 0); return js_throw_code(c, "sys.badArgument", NULL); }
    int id = spell_submit_check(w, n);
    for (int i = 0; i < n; i++) if (w[i]) JS_FreeCString(c, w[i]);
    return JS_NewInt32(c, id > 0 ? id : 0);
}

static JSValue f_suggest(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *w = argc > 0 ? str_arg(c, argv[0], 4 * SPELL_WORD_MAX) : NULL;
    if (!w) return js_throw_code(c, "sys.badArgument", NULL);
    int id = spell_submit_suggest(w);
    JS_FreeCString(c, w);
    return JS_NewInt32(c, id > 0 ? id : 0);
}

static JSValue f_take(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int id;
    if (argc < 1 || JS_ToInt32(c, &id, argv[0])) return JS_NULL;
    return parsed(c, spell_take(id), "<spell.take>");
}

static JSValue f_words(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue v = parsed(c, spell_words_json(), "<spell.words>");
    return JS_IsNull(v) ? JS_NewArray(c) : v;
}

static JSValue word_op(JSContext *c, int argc, JSValueConst *argv, int add) {
    const char *w = argc > 0 ? str_arg(c, argv[0], SPELL_WORD_MAX) : NULL;
    int r = w ? (add ? spell_add_word(w) : spell_remove_word(w)) : (errno = EINVAL, -1);
    int e = errno;
    if (w) JS_FreeCString(c, w);
    if (r) return js_throw_code(c, e == EINVAL ? "sys.badArgument" : "sys.writeFailed", NULL);
    return JS_TRUE;
}
static JSValue f_add_word(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; return word_op(c, argc, argv, 1); }
static JSValue f_remove_word(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; return word_op(c, argc, argv, 0); }

static const JSCFunctionListEntry funcs[] = {
    JS_CFUNC_DEF("installed", 0, f_installed),
    JS_CFUNC_DEF("setLanguages", 1, f_set_languages),
    JS_CFUNC_DEF("defaults", 1, f_defaults),
    JS_CFUNC_DEF("state", 0, f_state),
    JS_CFUNC_DEF("check", 1, f_check),
    JS_CFUNC_DEF("suggest", 1, f_suggest),
    JS_CFUNC_DEF("take", 1, f_take),
    JS_CFUNC_DEF("words", 0, f_words),
    JS_CFUNC_DEF("addWord", 1, f_add_word),
    JS_CFUNC_DEF("removeWord", 1, f_remove_word),
};

void js_spell_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, funcs, sizeof funcs / sizeof funcs[0]);
    JS_SetPropertyStr(c, lord, "spell", o);
}
