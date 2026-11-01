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
#include "../lan/rconnect.h"

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

/* connect(kind, host, port, user, domain, password, path) · connectPoll() · disconnect(path) */
static JSValue f_connect(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 7) return JS_FALSE;
    const char *k = JS_ToCString(c, argv[0]), *h = JS_ToCString(c, argv[1]), *u = JS_ToCString(c, argv[3]), *d = JS_ToCString(c, argv[4]),
               *p = JS_ToCString(c, argv[5]), *w = JS_ToCString(c, argv[6]);
    int port = 0, r = 0;
    JS_ToInt32(c, &port, argv[2]);
    if (k && h && u && d && p && w) r = rconnect_start(k, h, port, u, d, p, w);
    const char *all[] = { k, h, u, d, p, w };
    for (int i = 0; i < 6; i++) if (all[i]) JS_FreeCString(c, all[i]);
    return JS_NewBool(c, r);
}

static JSValue f_connect_poll(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char *s = rconnect_poll();
    if (!s) return JS_ThrowOutOfMemory(c);
    JSValue v = JS_NewString(c, s);
    free(s);
    return v;
}

static JSValue f_trust(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    rconnect_trust(argc && JS_ToBool(c, argv[0]) > 0);
    return JS_UNDEFINED;
}

static JSValue f_disconnect(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *p = argc ? JS_ToCString(c, argv[0]) : NULL;
    int r = p ? rconnect_unmount(p) : -1;
    if (p) JS_FreeCString(c, p);
    return JS_NewBool(c, r == 0);
}

static const JSCFunctionListEntry funcs[] = {
    JS_CFUNC_DEF("start", 1, f_start),
    JS_CFUNC_DEF("stop", 0, f_stop),
    JS_CFUNC_DEF("poll", 0, f_poll),
    JS_CFUNC_DEF("listShares", 4, f_list),
    JS_CFUNC_DEF("listPoll", 0, f_list_poll),
    JS_CFUNC_DEF("connect", 7, f_connect),
    JS_CFUNC_DEF("connectPoll", 0, f_connect_poll),
    JS_CFUNC_DEF("disconnect", 1, f_disconnect),
    JS_CFUNC_DEF("connectTrust", 1, f_trust),
};

void js_lan_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, funcs, sizeof funcs / sizeof funcs[0]);
    JS_SetPropertyStr(c, lord, "lan", o);
}
