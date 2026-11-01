/* js_share.c — lord.share, the shell's side of the screen-sharing API (remote/share.c, docs/SCREEN-SHARING.md).
 * Only the session's own context has it: an isolated app's process gets the broker's `lord`, without this.
 *
 *   lord.share.shown(token)    the indicator is on screen: frames may leave
 *   lord.share.hidden()        it no longer is: the share ends
 *   lord.share.answer(token)   the prompt was answered; the root-owned store decides, not this call
 *   lord.share.stop()          the indicator's Stop
 *   lord.share.reload()        the store changed (Settings → Privacy)
 *   lord.share.status()        { app, token, input } of the running share, or null
 *   lord.share.grants()        [{ id, exe, perm, state }] of this user
 * Changes to the store go through the share-grant admin action. */
#include "js_share.h"
#include "../remote/share.h"
#include "../remote/sharegrant.h"
#include "../sec/json.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned token_arg(JSContext *c, int argc, JSValueConst *argv) {
    uint32_t t = 0;
    if (argc > 0) JS_ToUint32(c, &t, argv[0]);
    return t;
}

static JSValue f_shown(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; share_shown(token_arg(c, argc, argv)); return JS_UNDEFINED; }
static JSValue f_hidden(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)c; (void)t; (void)argc; (void)argv; share_hidden(); return JS_UNDEFINED; }
static JSValue f_answer(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; share_answer(token_arg(c, argc, argv)); return JS_UNDEFINED; }
static JSValue f_stop(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)c; (void)t; (void)argc; (void)argv; return JS_NewBool(c, share_user_stop()); }
static JSValue f_reload(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)c; (void)t; (void)argc; (void)argv; share_reload(); return JS_UNDEFINED; }

static JSValue f_status(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char j[256];
    share_status(j, sizeof j);
    return JS_ParseJSON(c, j, strlen(j), "<share>");
}

static JSValue f_grants(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    static SgStore s;
    char path[96];
    sg_path(getuid(), path, sizeof path);
    sg_load(path, &s);
    JSValue a = JS_NewArray(c);
    for (int i = 0; i < s.n; i++) {
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "id", JS_NewString(c, s.e[i].id));
        JS_SetPropertyStr(c, o, "exe", JS_NewString(c, s.e[i].exe));
        JS_SetPropertyStr(c, o, "perm", JS_NewString(c, sg_perm_name(s.e[i].perm)));
        JS_SetPropertyStr(c, o, "state", JS_NewString(c, sg_state_name(s.e[i].state)));
        JS_SetPropertyUint32(c, a, (uint32_t)i, o);
    }
    return a;
}

static const JSCFunctionListEntry funcs[] = {
    JS_CFUNC_DEF("shown", 1, f_shown),
    JS_CFUNC_DEF("hidden", 0, f_hidden),
    JS_CFUNC_DEF("answer", 1, f_answer),
    JS_CFUNC_DEF("stop", 0, f_stop),
    JS_CFUNC_DEF("reload", 0, f_reload),
    JS_CFUNC_DEF("status", 0, f_status),
    JS_CFUNC_DEF("grants", 0, f_grants),
};

void js_share_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, funcs, sizeof funcs / sizeof funcs[0]);
    JS_SetPropertyStr(c, lord, "share", o);
}
