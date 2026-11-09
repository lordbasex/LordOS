/* js_sql.c — lord.sql: the Databases app's MySQL and MariaDB connections through the sandboxed
 * helper lord-sql (server/src/sql, docs/DATABASES.md). Here we only pass requests on and read the
 * favorites file; the protocol, TLS and authentication run in lord-sql, and sql/sqlc.c resolves
 * and connects the favorite's server (never a host from JS). Everything is asynchronous:
 * request() returns an id and the answer arrives through poll(). Not in the broker: isolated
 * .lpk apps get no lord.sql.
 *
 *   available()            is lord-sql installed?
 *   favorites()            the favorites file as JSON text (the empty one when missing)
 *   saveFavorites(json)    validates and writes it (0600, atomic); throws on a bad file
 *   request(json)          → request id, or -1; json is {"op": open|query|use|ping|close, …}
 *   poll()                 → [{ id, type: 'ok'|'err'|'evt', data }]
 */
#include "js_sql.h"
#include "js_err.h"
#include "../sql/sqlc.h"

#include <stdlib.h>
#include <string.h>

static JSValue f_available(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewBool(c, sql_available() == 0);
}

static JSValue f_favorites(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    size_t n = 0;
    char *s = sql_favorites_read(&n);
    if (!s) return JS_NewString(c, "{\"version\":1,\"groups\":[],\"favorites\":[]}");
    JSValue v = JS_NewStringLen(c, s, n);
    free(s);
    return v;
}

/* The favorites file names the servers lord-server connects to and the certificates the user
 * trusted: JS cannot write it through lord.fs (fsops_protected), only here, validated. */
static JSValue f_save_favorites(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    size_t len = 0;
    const char *j = argc > 0 && JS_IsString(argv[0]) ? JS_ToCStringLen(c, &len, argv[0]) : NULL;
    if (!j) return js_throw_code(c, "sys.badArgument", NULL);
    int r = sql_favorites_save(j, len);
    JS_FreeCString(c, j);
    if (r == 1) return js_throw_code(c, "sys.badArgument", NULL);
    if (r) return js_throw_code(c, "sys.writeFailed", "path", sql_favorites_path(), NULL);
    return JS_TRUE;
}

static JSValue f_request(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    size_t len = 0;
    const char *j = argc > 0 && JS_IsString(argv[0]) ? JS_ToCStringLen(c, &len, argv[0]) : NULL;
    if (!j) return js_throw_code(c, "sys.badArgument", NULL);
    int id = sql_request(j, len);
    JS_FreeCString(c, j);
    return JS_NewInt32(c, id);
}

static JSValue f_poll(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    SqlItem *items = NULL;
    int n = sql_poll(&items);
    JSValue arr = JS_NewArray(c);
    static const char *const types[] = { "ok", "err", "evt" };
    for (int i = 0; i < n; i++) {
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "id", JS_NewInt32(c, items[i].id));
        JS_SetPropertyStr(c, o, "type", JS_NewString(c, types[items[i].type < 3 ? items[i].type : 1]));
        /* lord-sql's JSON is checked here: a helper that sent something else gives an error */
        JSValue d = JS_ParseJSON(c, items[i].data, items[i].len, "<sql>");
        if (JS_IsException(d)) {
            JS_FreeValue(c, JS_GetException(c));
            d = JS_ParseJSON(c, "{\"error\":\"closed\",\"message\":\"bad reply\"}", 40, "<sql>");
            JS_SetPropertyStr(c, o, "type", JS_NewString(c, "err"));
        }
        JS_SetPropertyStr(c, o, "data", d);
        JS_SetPropertyUint32(c, arr, (uint32_t)i, o);
    }
    sql_items_free(items, n);
    return arr;
}

static const JSCFunctionListEntry sql_funcs[] = {
    JS_CFUNC_DEF("available", 0, f_available),
    JS_CFUNC_DEF("favorites", 0, f_favorites),
    JS_CFUNC_DEF("saveFavorites", 1, f_save_favorites),
    JS_CFUNC_DEF("request", 1, f_request),
    JS_CFUNC_DEF("poll", 0, f_poll),
};

void js_sql_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, sql_funcs, sizeof sql_funcs / sizeof sql_funcs[0]);
    JS_SetPropertyStr(c, lord, "sql", o);
}
