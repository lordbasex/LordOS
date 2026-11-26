/* js_hands.c — lord.hands: the shell's side of Hands (hands/hands.h, sdk/hands.jsx, docs/HANDS.md). Only in the desktop's
 * own JS context (never an isolated app's broker, never a community theme's restricted lord):
 *
 *   lord.hands.serve(on, allow)    Settings → AI → Hands: on for this session, the allowlist (app ids, at most 16)
 *   lord.hands.answer(bus, yes)    the system layer's prompt: "This session" (true) or "Deny" (false) for one connection
 *   lord.hands.stop()              the banner's Stop: every client stopped, every grant forgotten
 *   lord.hands.status()            {on, qa, allow, clients: [{bus, name, exe, kind, state, actions}], events}
 *   lord.hands.injecting()         the input being handled now is the agent's (it is counted in status().events)
 *
 * lord-server calls the shell back through __lordHands(op, json) (the tree, refs, windows, opening; sdk/hands.jsx) and
 * tells it of changes through __lordHandsEvent(json). */
#include "js_hands.h"
#include "../hands/hands.h"

#include <stdlib.h>
#include <string.h>

static JSValue f_serve(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_UNDEFINED;
    int on = JS_ToBool(c, argv[0]) > 0;
    char ids[HANDS_ALLOW_MAX][HANDS_ID_MAX + 1];
    const char *ptr[HANDS_ALLOW_MAX];
    int n = 0;
    if (argc > 1 && JS_IsArray(c, argv[1])) {
        for (uint32_t i = 0; i < 64 && n < HANDS_ALLOW_MAX; i++) {
            JSValue v = JS_GetPropertyUint32(c, argv[1], i);
            if (JS_IsUndefined(v)) { JS_FreeValue(c, v); break; }
            const char *s = JS_IsString(v) ? JS_ToCString(c, v) : NULL;
            if (s && hands_id_ok(s)) { memcpy(ids[n], s, strlen(s) + 1); ptr[n] = ids[n]; n++; }
            if (s) JS_FreeCString(c, s);
            JS_FreeValue(c, v);
        }
    }
    hands_serve(on, ptr, n);
    return JS_UNDEFINED;
}

static JSValue f_answer(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_UNDEFINED;
    const char *bus = JS_ToCString(c, argv[0]);
    if (bus && bus[0] == ':' && strlen(bus) < 64) hands_answer(bus, JS_ToBool(c, argv[1]) > 0);
    if (bus) JS_FreeCString(c, bus);
    return JS_UNDEFINED;
}

static JSValue f_stop(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    hands_stop("stop");
    return JS_UNDEFINED;
}

static JSValue f_status(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char *j = hands_status_json();
    JSValue v = j ? JS_ParseJSON(c, j, strlen(j), "<hands>") : JS_NULL;
    free(j);
    return v;
}

static JSValue f_injecting(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    return JS_NewBool(c, hands_injecting());
}

static const JSCFunctionListEntry funcs[] = {
    JS_CFUNC_DEF("serve", 2, f_serve),
    JS_CFUNC_DEF("answer", 2, f_answer),
    JS_CFUNC_DEF("stop", 0, f_stop),
    JS_CFUNC_DEF("status", 0, f_status),
    JS_CFUNC_DEF("injecting", 0, f_injecting),
};

void js_hands_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, funcs, sizeof funcs / sizeof funcs[0]);
    JS_SetPropertyStr(c, lord, "hands", o);
}
