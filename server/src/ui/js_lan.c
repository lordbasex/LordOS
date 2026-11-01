/* js_lan.c — lord.lan: Files → Network (lan/lan.h, docs/FILES.md, "Network"). JS names no address,
 * port or socket: it only starts a bounded pass, stops it and reads what is known.
 *
 *   start(asked)   true if a pass started. Opening the page keeps a pass under 5 minutes old; asked
 *                  (the button) only waits 30 s between passes
 *   stop()    cancels the pass and ends the helper
 *   poll()    JSON text: { state, running, probed, total, hosts: [...] }
 *   listShares(addr, domain, user, password)  true if it started; only an address on a local subnet is reached
 *   listPoll()  JSON text: { state, shares: [...] } */
#include "js_lan.h"
#include "../lan/lan.h"

#include <stdlib.h>

static JSValue f_start(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    return JS_NewBool(c, lan_start(argc > 0 && JS_ToBool(c, argv[0]) > 0));
}

static JSValue f_stop(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    lan_stop();
    return JS_UNDEFINED;
}

static JSValue f_poll(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char *s = lan_poll();
    if (!s) return JS_ThrowOutOfMemory(c);
    JSValue v = JS_NewString(c, s);
    free(s);
    return v;
}

static JSValue f_list(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 4) return JS_FALSE;
    const char *v[4] = { 0 };
    int ok = 1;
    for (int i = 0; i < 4; i++) { v[i] = JS_ToCString(c, argv[i]); if (!v[i]) ok = 0; }
    int r = ok ? lan_list_start(v[0], v[1], v[2], v[3]) : 0;
    for (int i = 0; i < 4; i++) if (v[i]) JS_FreeCString(c, v[i]);
    return JS_NewBool(c, r);
}

static JSValue f_list_poll(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char *s = lan_list_poll();
    if (!s) return JS_ThrowOutOfMemory(c);
    JSValue v = JS_NewString(c, s);
    free(s);
    return v;
}

static const JSCFunctionListEntry funcs[] = {
    JS_CFUNC_DEF("start", 1, f_start),
    JS_CFUNC_DEF("stop", 0, f_stop),
    JS_CFUNC_DEF("poll", 0, f_poll),
    JS_CFUNC_DEF("listShares", 4, f_list),
    JS_CFUNC_DEF("listPoll", 0, f_list_poll),
};

void js_lan_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, funcs, sizeof funcs / sizeof funcs[0]);
    JS_SetPropertyStr(c, lord, "lan", o);
}
