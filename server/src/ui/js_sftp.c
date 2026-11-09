/* js_sftp.c — lord.sftp: the Terminal's Files mode over the tab's own ssh connection (term/sftpc.c,
 * docs/TERMINAL.md "Files mode"). Here we only pass requests on: the connection is found by a Terminal
 * session id (never a host or a socket from JS), SFTP runs in the sandboxed lord-sftp, and the local
 * files of transfers are opened with lord.fs's rules. Asynchronous: request() returns an id and the
 * answer arrives through poll(). Not in the broker: isolated .lpk apps get no lord.sftp.
 *
 *   available()      is lord-sftp installed?
 *   request(json)    → request id, or -1; json is {"op": open|list|stat|mkdir|remove|rename|chmod|chown|get|put|cancel|info|forward|close, …}
 *   poll()           → [{ id, type: 'ok'|'err'|'evt', data }]
 */
#include "js_sftp.h"
#include "js_err.h"
#include "../term/sftpc.h"

#include <stdlib.h>
#include <string.h>

static JSValue f_available(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewBool(c, sftp_available() == 0);
}

static JSValue f_request(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    size_t len = 0;
    const char *j = argc > 0 && JS_IsString(argv[0]) ? JS_ToCStringLen(c, &len, argv[0]) : NULL;
    if (!j) return js_throw_code(c, "sys.badArgument", NULL);
    int id = sftp_request(j, len);
    explicit_bzero((char *)j, len);          /* an open as root may carry the sudo password */
    JS_FreeCString(c, j);
    return JS_NewInt32(c, id);
}

static JSValue f_poll(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    SftpItem *items = NULL;
    int n = sftp_poll(&items);
    JSValue arr = JS_NewArray(c);
    static const char *const types[] = { "ok", "err", "evt" };
    for (int i = 0; i < n; i++) {
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "id", JS_NewInt32(c, items[i].id));
        JS_SetPropertyStr(c, o, "type", JS_NewString(c, types[items[i].type < 3 ? items[i].type : 1]));
        JSValue d = JS_ParseJSON(c, items[i].data, items[i].len, "<sftp>");
        if (JS_IsException(d)) {
            JS_FreeValue(c, JS_GetException(c));
            d = JS_ParseJSON(c, "{\"error\":\"closed\",\"message\":\"bad reply\"}", 40, "<sftp>");
            JS_SetPropertyStr(c, o, "type", JS_NewString(c, "err"));
        }
        JS_SetPropertyStr(c, o, "data", d);
        JS_SetPropertyUint32(c, arr, (uint32_t)i, o);
    }
    sftp_items_free(items, n);
    return arr;
}

static const JSCFunctionListEntry sftp_funcs[] = {
    JS_CFUNC_DEF("available", 0, f_available),
    JS_CFUNC_DEF("request", 1, f_request),
    JS_CFUNC_DEF("poll", 0, f_poll),
};

void js_sftp_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, sftp_funcs, sizeof sftp_funcs / sizeof sftp_funcs[0]);
    JS_SetPropertyStr(c, lord, "sftp", o);
}
