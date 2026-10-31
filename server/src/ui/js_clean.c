/* js_clean.c — lord.clean: Cleaner's native API (docs/CLEANER-ARCHITECTURE.md, clean/clean.h).
 *
 *   start(kind, opts)             a new scan: 'rules' (the default, no options), 'tree' ({ volume, minSize, exclude, remember, saved }),
 *                                 'dups' or 'images' ({ folders, exclude }). Options only name folders relative to the home
 *                                 folder or a volume id from volumes(): what to read, never what to change
 *   poll(kind)                    { job, state, error, scan, plan, emptied }: real counters, never a percentage
 *   volumes()                     [{ id, path, total, free }]
 *   tree(scanId, node, limit)     a folder of a tree and its largest children
 *   groups(scanId, offset, limit) a page of groups of duplicates or similar images
 *   restore(planId, id)           back from the Trash, for an item that plan moved
 *   history() / clearHistory()    what ran: counts and bytes, never a path
 *   noteSystem(name, freeBefore)  a system cleanup, for the history: a fixed name
 *   saved() / forgetSaved()       the storage map kept between sessions: { at, volume } or null; its folder names stay in C
 *   cancel(forget)                stops the job; forget (boolean) also drops the results
 *   results(scanId | 'plan', offset, limit, filter)  a page of { id, rule, path, size, allocated, … };
 *                                 filter: { rule, minSize, olderThanDays, type, under }
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
#include "../clean/cleanfs.h"
#include "../clean/cleanproto.h"
#include "js_sys.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    case CLEAN_E_BADFOLDER: return js_throw_code(c, "clean.badFolder", NULL);
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

static int kind_of(JSContext *c, JSValueConst v, int *kind) {
    const char *k = JS_IsString(v) ? JS_ToCString(c, v) : NULL;
    if (!k) return -1;
    *kind = !strcmp(k, "rules") ? CLEAN_KIND_RULES : !strcmp(k, "tree") ? CLEAN_KIND_TREE : !strcmp(k, "dups") ? CLEAN_KIND_DUPS : !strcmp(k, "images") ? CLEAN_KIND_IMAGES : -1;
    JS_FreeCString(c, k);
    return *kind < 0 ? -1 : 0;
}

/* an array of at most max strings; frees with free_strs */
static int read_strs(JSContext *c, JSValueConst obj, const char *key, const char **out, int max, int *n) {
    JSValue a = JS_GetPropertyStr(c, obj, key);
    *n = 0;
    if (JS_IsUndefined(a)) return 0;
    double len;
    JSValue l = JS_IsArray(c, a) ? JS_GetPropertyStr(c, a, "length") : JS_UNDEFINED;
    int bad = exact_int(c, l, 0, max, &len);
    JS_FreeValue(c, l);
    for (uint32_t i = 0; !bad && i < (uint32_t)len; i++) {
        JSValue v = JS_GetPropertyUint32(c, a, i);
        const char *t = JS_IsString(v) ? JS_ToCString(c, v) : NULL;
        JS_FreeValue(c, v);
        if (!t) bad = 1;
        else out[(*n)++] = t;
    }
    JS_FreeValue(c, a);
    return bad ? -1 : 0;
}
static void free_strs(JSContext *c, const char **s, int n) { for (int i = 0; i < n; i++) JS_FreeCString(c, s[i]); }

static JSValue f_start(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int kind = CLEAN_KIND_RULES;
    if (argc > 2 || (argc >= 1 && kind_of(c, argv[0], &kind)) || (argc == 2 && !JS_IsObject(argv[1]))) return js_throw_code(c, "sys.badArgument", NULL);
    CleanOpts o = { 0 };
    int bad = 0;
    const char *volume = NULL;
    if (argc == 2) {
        bad = read_strs(c, argv[1], "folders", o.folders, CLEAN_MAX_FOLDERS, &o.nfolders) | read_strs(c, argv[1], "exclude", o.exclude, 32, &o.nexclude);
        JSValue flag = JS_GetPropertyStr(c, argv[1], "remember");
        o.remember = JS_ToBool(c, flag) == 1;
        JS_FreeValue(c, flag);
        flag = JS_GetPropertyStr(c, argv[1], "saved");
        o.saved = JS_ToBool(c, flag) == 1;
        JS_FreeValue(c, flag);
        JSValue v = JS_GetPropertyStr(c, argv[1], "volume");
        if (JS_IsString(v)) o.volume = volume = JS_ToCString(c, v);
        else if (!JS_IsUndefined(v)) bad = 1;
        JS_FreeValue(c, v);
        JSValue m = JS_GetPropertyStr(c, argv[1], "minSize");
        if (!JS_IsUndefined(m) && exact_int(c, m, 1, 1e15, &o.min_size)) bad = 1;
        JS_FreeValue(c, m);
    }
    clean_set_sysapps(js_sys_assets());
    int r = bad ? CLEAN_E_BADFOLDER : clean_start_kind(kind, &o);
    free_strs(c, o.folders, o.nfolders);
    free_strs(c, o.exclude, o.nexclude);
    if (volume) JS_FreeCString(c, volume);
    return result(c, r);
}

static JSValue f_note_system(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    double before;
    const char *name = argc == 2 ? JS_ToCString(c, argv[0]) : NULL;
    int ok = name && (!strcmp(name, "journal") || !strcmp(name, "apt") || !strcmp(name, "vartmp") || !strcmp(name, "kernels")) && !exact_int(c, argv[1], 0, 1e18, &before);
    if (ok) clean_history_system(name, before);
    if (name) JS_FreeCString(c, name);
    return ok ? JS_UNDEFINED : js_throw_code(c, "sys.badArgument", NULL);
}

static JSValue f_saved(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return json_value(c, clean_saved());
}

static JSValue f_forget_saved(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    clean_forget_saved();
    return JS_UNDEFINED;
}

static JSValue f_poll(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int kind = -1;
    if (argc > 1 || (argc == 1 && !JS_IsUndefined(argv[0]) && kind_of(c, argv[0], &kind))) return js_throw_code(c, "sys.badArgument", NULL);
    return json_value(c, clean_poll(kind));
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
    double off, lim, id = 0;
    if (argc < 3 || argc > 4 || exact_int(c, argv[1], 0, CLEAN_MAX_FILES, &off) || exact_int(c, argv[2], 1, CLEAN_PAGE_MAX, &lim))
        return js_throw_code(c, "sys.badArgument", NULL);
    if (JS_IsString(argv[0])) {
        const char *kind = JS_ToCString(c, argv[0]);
        if (!kind) return JS_EXCEPTION;
        int ok = !strcmp(kind, "plan");
        JS_FreeCString(c, kind);
        if (!ok) return js_throw_code(c, "sys.badArgument", NULL);
        return json_value(c, clean_results(1, (long)off, (int)lim));
    }
    if (exact_int(c, argv[0], 1, 2147483647.0, &id)) return js_throw_code(c, "sys.badArgument", NULL);
    CleanFilter f = { .type = -1 };
    const char *rule = NULL, *type = NULL, *under = NULL;
    int bad = 0;
    if (argc == 4 && !JS_IsUndefined(argv[3])) {
        if (!JS_IsObject(argv[3])) return js_throw_code(c, "sys.badArgument", NULL);
        JSValue r = JS_GetPropertyStr(c, argv[3], "rule"), m = JS_GetPropertyStr(c, argv[3], "minSize"),
                d = JS_GetPropertyStr(c, argv[3], "olderThanDays"), y = JS_GetPropertyStr(c, argv[3], "type");
        double days = 0;
        if (JS_IsString(r)) rule = JS_ToCString(c, r); else if (!JS_IsUndefined(r)) bad = 1;
        if (!JS_IsUndefined(m) && exact_int(c, m, 0, 1e15, &f.min_size)) bad = 1;
        if (!JS_IsUndefined(d) && exact_int(c, d, 0, 36500, &days)) bad = 1;
        if (days > 0) f.older_than = (double)time(NULL) - days * 86400;
        if (JS_IsString(y)) {
            type = JS_ToCString(c, y);
            for (int i = 0; type && i < CLEAN_TYPES; i++) if (!strcmp(type, clean_type_name(i))) f.type = i;
            if (f.type < 0) bad = 1;
        } else if (!JS_IsUndefined(y)) bad = 1;
        f.rule = rule;
        JSValue u = JS_GetPropertyStr(c, argv[3], "under");
        if (JS_IsString(u)) { under = JS_ToCString(c, u); if (!under || !clean_relpath_ok(under, CLEAN_MAX_DEPTH)) bad = 1; } else if (!JS_IsUndefined(u)) bad = 1;
        JS_FreeValue(c, u);
        f.under = under;
        JS_FreeValue(c, r); JS_FreeValue(c, m); JS_FreeValue(c, d); JS_FreeValue(c, y);
    }
    JSValue v = bad ? js_throw_code(c, "sys.badArgument", NULL) : json_value(c, clean_results_of((int)id, (long)off, (int)lim, &f));
    if (rule) JS_FreeCString(c, rule);
    if (under) JS_FreeCString(c, under);
    if (type) JS_FreeCString(c, type);
    return v;
}

static JSValue f_volumes(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return json_value(c, clean_volumes());
}

static JSValue f_tree(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    double id, node, lim;
    if (argc != 3 || exact_int(c, argv[0], 1, 2147483647.0, &id) || exact_int(c, argv[1], 0, CLEAN_MAX_NODES, &node) || exact_int(c, argv[2], 1, CLEAN_PAGE_MAX, &lim))
        return js_throw_code(c, "sys.badArgument", NULL);
    char *j = clean_tree((int)id, (long)node, (int)lim);
    return j ? json_value(c, j) : JS_NULL;
}

static JSValue f_groups(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    double id, off, lim;
    if (argc != 3 || exact_int(c, argv[0], 1, 2147483647.0, &id) || exact_int(c, argv[1], 0, CLEAN_MAX_FILES, &off) || exact_int(c, argv[2], 1, 100, &lim))
        return js_throw_code(c, "sys.badArgument", NULL);
    return json_value(c, clean_groups((int)id, (long)off, (int)lim));
}

static JSValue f_restore(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    double plan, id;
    if (argc != 2 || exact_int(c, argv[0], 1, 2147483647.0, &plan) || exact_int(c, argv[1], 1, CLEAN_MAX_FILES, &id)) return js_throw_code(c, "sys.badArgument", NULL);
    return result(c, clean_restore((int)plan, (uint32_t)id));
}

static JSValue f_history(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return json_value(c, clean_history());
}

static JSValue f_clear_history(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    clean_history_clear();
    return JS_UNDEFINED;
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
    F("volumes", f_volumes, 0);
    F("tree", f_tree, 3);
    F("groups", f_groups, 3);
    F("restore", f_restore, 2);
    F("history", f_history, 0);
    F("clearHistory", f_clear_history, 0);
    F("noteSystem", f_note_system, 2);
    F("saved", f_saved, 0);
    F("forgetSaved", f_forget_saved, 0);
#undef F
    JS_SetPropertyStr(c, lord, "clean", o);
}
