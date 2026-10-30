/* js_clean.c — lord.clean: Cleaner's native API (docs/CLEANER-ARCHITECTURE.md, clean/clean.h).
 *
 *   start()                       a new scan of every rule: its id. No arguments: JS names no path or root
 *   poll()                        { job, state, error, scan, plan }: real counters, never a percentage
 *   cancel(forget)                stops the job; forget (boolean) also drops the results
 *   results(kind, offset, limit)  a page ('scan' | 'plan') of { id, rule, path, size, allocated, … }
 *   plan(scanId, ids)             builds a plan from candidate ids, or { rules: [ids] }, on a thread: its id
 *   emptyTrash()                  the real emptyTrash, on a thread; can't be undone
 *   execute(planId)               runs that plan once, on a thread
 *   status()                      { available, free, total }
 *
 * JS only holds ids of the current scan and display paths: authority is never a path from JS. The real
 * delete is reached only through a plan of a regenerable rule. Registered for the session's JS only, never for an isolated app. */
#include "js_clean.h"
#include "js_err.h"
#include "../clean/clean.h"
#include "../clean/cleanproto.h"

#include <stdlib.h>
#include <string.h>

static JSValue json_value(JSContext *c, char *j) {
    if (!j) return js_throw_code(c, "clean.unavailable", NULL);
    JSValue v = JS_ParseJSON(c, j, strlen(j), "<clean>");
    free(j);
    return v;
}

static JSValue result(JSContext *c, int r) {
    switch (r) {
    case CLEAN_E_BUSY:    return js_throw_code(c, "clean.busy", NULL);
    case CLEAN_E_UNAVAILABLE: return js_throw_code(c, "clean.unavailable", NULL);
    case CLEAN_E_NOSCAN:  return js_throw_code(c, "clean.noScan", NULL);
    case CLEAN_E_BADID:   return js_throw_code(c, "clean.badCandidate", NULL);
    case CLEAN_E_NOPLAN:  return js_throw_code(c, "clean.noPlan", NULL);
    case CLEAN_E_EXPIRED: return js_throw_code(c, "clean.planExpired", NULL);
    default:              return JS_NewInt32(c, r);
    }
}

/* a JS number that is an exact integer in [min, max] */
static int exact_int(JSContext *c, JSValueConst v, double min, double max, double *out) {
    double d;
    if (!JS_IsNumber(v) || JS_ToFloat64(c, &d, v)) return -1;
    if (!(d >= min && d <= max) || d != (double)(long long)d) return -1;
    *out = d;
    return 0;
}

static JSValue f_start(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argv;
    if (argc) return js_throw_code(c, "sys.badArgument", NULL);
    return result(c, clean_start());
}

static JSValue f_poll(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return json_value(c, clean_poll());
}

static JSValue f_status(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return json_value(c, clean_status());
}

static JSValue f_cancel(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc > 1 || (argc == 1 && !JS_IsBool(argv[0]) && !JS_IsUndefined(argv[0]))) return js_throw_code(c, "sys.badArgument", NULL);
    clean_cancel(argc == 1 && JS_ToBool(c, argv[0]) == 1);
    return JS_UNDEFINED;
}

static JSValue f_results(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    double off, lim;
    if (argc != 3 || !JS_IsString(argv[0]) || exact_int(c, argv[1], 0, CLEAN_MAX_FILES, &off) ||
        exact_int(c, argv[2], 1, CLEAN_PAGE_MAX, &lim)) return js_throw_code(c, "sys.badArgument", NULL);
    const char *kind = JS_ToCString(c, argv[0]);
    if (!kind) return JS_EXCEPTION;
    int want_plan = !strcmp(kind, "plan"), want_scan = !strcmp(kind, "scan");
    JS_FreeCString(c, kind);
    if (!want_plan && !want_scan) return js_throw_code(c, "sys.badArgument", NULL);
    return json_value(c, clean_results(want_plan, (long)off, (int)lim));
}

static JSValue f_plan(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    double scan, len;
    if (argc != 2 || exact_int(c, argv[0], 1, 2147483647.0, &scan) || !JS_IsObject(argv[1])) return js_throw_code(c, "sys.badArgument", NULL);
    if (!JS_IsArray(c, argv[1])) {                  /* { rules: ['thumbnails', …] } */
        JSValue rules = JS_GetPropertyStr(c, argv[1], "rules");
        JSValue l = JS_IsArray(c, rules) ? JS_GetPropertyStr(c, rules, "length") : JS_UNDEFINED;
        double count;
        int r = CLEAN_E_BADID;
        if (!exact_int(c, l, 1, CLEAN_MAX_ROOTS, &count)) {
            const char *names[CLEAN_MAX_ROOTS];
            int n = 0, ok = 1;
            for (uint32_t i = 0; i < (uint32_t)count; i++) {
                JSValue v = JS_GetPropertyUint32(c, rules, i);
                const char *t = JS_IsString(v) ? JS_ToCString(c, v) : NULL;
                JS_FreeValue(c, v);
                if (!t) { ok = 0; break; }
                names[n++] = t;
            }
            if (ok) r = clean_plan_rules((int)scan, names, n);
            for (int i = 0; i < n; i++) JS_FreeCString(c, names[i]);
        }
        JS_FreeValue(c, l);
        JS_FreeValue(c, rules);
        return result(c, r);
    }
    JSValue l = JS_GetPropertyStr(c, argv[1], "length");
    int bad = exact_int(c, l, 1, CLEAN_MAX_FILES, &len);
    JS_FreeValue(c, l);
    if (bad) return js_throw_code(c, "clean.badCandidate", NULL);
    uint32_t *ids = malloc((size_t)len * sizeof *ids);
    if (!ids) return js_throw_code(c, "clean.unavailable", NULL);
    for (uint32_t i = 0; i < (uint32_t)len; i++) {
        JSValue v = JS_GetPropertyUint32(c, argv[1], i);
        double id;
        bad = exact_int(c, v, 1, CLEAN_MAX_FILES, &id);
        JS_FreeValue(c, v);
        if (bad) { free(ids); return js_throw_code(c, "clean.badCandidate", NULL); }
        ids[i] = (uint32_t)id;
    }
    int r = clean_plan((int)scan, ids, (size_t)len);
    free(ids);
    return result(c, r);
}

static JSValue f_execute(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    double plan;
    if (argc != 1 || exact_int(c, argv[0], 1, 2147483647.0, &plan)) return js_throw_code(c, "sys.badArgument", NULL);
    return result(c, clean_execute((int)plan));
}

static JSValue f_empty_trash(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return result(c, clean_empty_trash());
}

void js_clean_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
#define F(name, fn, n) JS_SetPropertyStr(c, o, name, JS_NewCFunction(c, fn, name, n))
    F("start", f_start, 0);
    F("poll", f_poll, 0);
    F("cancel", f_cancel, 1);
    F("results", f_results, 3);
    F("plan", f_plan, 2);
    F("execute", f_execute, 1);
    F("status", f_status, 0);
    F("emptyTrash", f_empty_trash, 0);
#undef F
    JS_SetPropertyStr(c, lord, "clean", o);
}
