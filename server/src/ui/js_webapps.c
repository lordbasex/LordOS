/* js_webapps.c — lord.webapps: web apps and their profiles (docs/WEBAPPS.md). The registry, the probe and the
 * lord-web processes are web/webmgr.c's; the permission questions are webask.c's. Desktop only: not in the
 * broker, so isolated .lpk apps get no lord.webapps. JS never names a URL to start or a command: launch takes
 * an app and a profile that must be in the registry, install takes a catalog id or a finished probe.
 *
 *   available()                         is lord-web (the lordos-web package) installed?
 *   list()                              [registry entry + iconPath], sorted by id
 *   get(app)                            one entry, or null
 *   probe(url)                          → probe id; starts lord-web --probe (https: only); throws a code
 *   probeResult(id)                     null while it runs; { done, ok, name, startUrl, scope, origin, themeColor,
 *                                       manifest, iconPath, iconKind } or { done, ok: false, error }
 *   install(entry)                      → app id; entry { source: 'catalog', id, profile } or
 *                                       { source: 'url', probe, name, profile } (profile: the first profile's name)
 *   addProfile(app, name)               → profile id
 *   renameProfile(app, profile, name) · removeProfile(app, profile, deleteData)
 *   setPermission(app, kind, value)     kind: notifications|camera|microphone|screen|location; value: allow|ask|deny
 *   setBackground(app, on) · uninstall(app, deleteData)
 *   launch(app, profile)                → 'started' or 'running' (lord-server then asks it to show its window)
 *   running()                           [{ app, profile, pid }]
 *   memory(app)                         null when none of its profiles runs; else { kb, processes, unreadable,
 *                                       profiles: [{ profile, kb, processes }] }: the Pss of each running lord-web of
 *                                       the app and of its WebKit processes (web/webmem.c; read-only, bounded)
 *   owner(surface)                      { app, profile } when window surface is one of our lord-web processes, else null
 *   answer(id, value)                   a permission question (the "webapps-ask" event): allow|deny|once|deny-once
 *   asks()                              the questions still waiting */
#include "js_webapps.h"
#include "js_err.h"
#include "js_sys.h"
#include "webask.h"
#include "../web/webmem.h"
#include "../web/webmgr.h"
#include "../wl/wl.h"
#include "../wl/xdgact.h"

#include <stdio.h>

#include <stdlib.h>
#include <string.h>

/* argument i as a C string (freed by the caller with JS_FreeCString), or NULL */
static const char *arg_str(JSContext *c, int argc, JSValueConst *argv, int i) {
    return i < argc && JS_IsString(argv[i]) ? JS_ToCString(c, argv[i]) : NULL;
}

static JSValue done(JSContext *c, const char *err, JSValue ok) {
    if (err) { JS_FreeValue(c, ok); return js_throw_code(c, err, NULL); }
    return ok;
}

static JSValue f_available(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewBool(c, webmgr_available());
}

typedef struct { JSContext *c; JSValue arr; uint32_t n; } ListCtx;

static JSValue entry_value(JSContext *c, const char *text, size_t len, const char *icon) {
    JSValue o = JS_ParseJSON(c, text, len, "<webapps>");
    if (JS_IsException(o)) { JS_FreeValue(c, JS_GetException(c)); return JS_NULL; }
    JS_SetPropertyStr(c, o, "iconPath", JS_NewString(c, icon));
    return o;
}

static void list_one(const WebApp *a, const char *text, size_t len, const char *icon, void *ctx) {
    (void)a;
    ListCtx *l = ctx;
    JSValue o = entry_value(l->c, text, len, icon);
    if (!JS_IsNull(o)) JS_SetPropertyUint32(l->c, l->arr, l->n++, o);
}

static JSValue f_list(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    ListCtx l = { c, JS_NewArray(c), 0 };
    webmgr_each(list_one, &l);
    return l.arr;
}

typedef struct { JSContext *c; const char *want; JSValue v; } GetCtx;
static void get_one(const WebApp *a, const char *text, size_t len, const char *icon, void *ctx) {
    GetCtx *g = ctx;
    if (JS_IsNull(g->v) && !strcmp(a->id, g->want)) g->v = entry_value(g->c, text, len, icon);
}

static JSValue f_get(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0);
    if (!app) return js_throw_code(c, "sys.badArgument", NULL);
    GetCtx g = { c, app, JS_NULL };
    if (webreg_id_ok(app)) webmgr_each(get_one, &g);
    JS_FreeCString(c, app);
    return g.v;
}

static JSValue f_probe(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *url = arg_str(c, argc, argv, 0);
    if (!url) return js_throw_code(c, "sys.badArgument", NULL);
    const char *err = NULL;
    int id = webmgr_probe(url, &err);
    JS_FreeCString(c, url);
    return done(c, id ? NULL : err, JS_NewInt32(c, id));
}

static JSValue f_probe_result(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t id = 0;
    if (argc < 1 || JS_ToInt32(c, &id, argv[0])) return js_throw_code(c, "sys.badArgument", NULL);
    int gone = 0;
    char *j = webmgr_probe_result(id, &gone);
    if (gone) return JS_ParseJSON(c, "{\"done\":true,\"ok\":false,\"error\":\"webapps.probeGone\"}", 53, "<probe>");
    if (!j) return JS_NULL;
    JSValue v = JS_ParseJSON(c, j, strlen(j), "<probe>");
    free(j);
    return v;
}

static JSValue f_install(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return js_throw_code(c, "sys.badArgument", NULL);
    JSValue s = JS_IsString(argv[0]) ? JS_DupValue(c, argv[0]) : JS_JSONStringify(c, argv[0], JS_UNDEFINED, JS_UNDEFINED);
    size_t len = 0;
    const char *j = JS_IsString(s) ? JS_ToCStringLen(c, &len, s) : NULL;
    JS_FreeValue(c, s);
    if (!j) return js_throw_code(c, "sys.badArgument", NULL);
    char app[WEBREG_ID_MAX + 1] = "";
    const char *err = webmgr_install(j, len, app, sizeof app);
    JS_FreeCString(c, j);
    return done(c, err, JS_NewString(c, app));
}

static JSValue f_add_profile(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0), *name = arg_str(c, argc, argv, 1);
    const char *err = !app || !name ? "sys.badArgument" : NULL;
    char id[WEBREG_ID_MAX + 1] = "";
    if (!err) err = webmgr_add_profile(app, name, id, sizeof id);
    JS_FreeCString(c, app);
    JS_FreeCString(c, name);
    return done(c, err, JS_NewString(c, id));
}

static JSValue f_rename_profile(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0), *prof = arg_str(c, argc, argv, 1), *name = arg_str(c, argc, argv, 2);
    const char *err = !app || !prof || !name ? "sys.badArgument" : webmgr_rename_profile(app, prof, name);
    JS_FreeCString(c, app);
    JS_FreeCString(c, prof);
    JS_FreeCString(c, name);
    return done(c, err, JS_TRUE);
}

static JSValue f_remove_profile(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0), *prof = arg_str(c, argc, argv, 1);
    int del = argc > 2 && JS_ToBool(c, argv[2]) == 1;
    const char *err = !app || !prof ? "sys.badArgument" : webmgr_remove_profile(app, prof, del);
    JS_FreeCString(c, app);
    JS_FreeCString(c, prof);
    return done(c, err, JS_TRUE);
}

static JSValue f_set_permission(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0), *kind = arg_str(c, argc, argv, 1), *val = arg_str(c, argc, argv, 2);
    const char *err = !app || !kind || !val ? "sys.badArgument" : webmgr_set_permission(app, kind, val);
    JS_FreeCString(c, app);
    JS_FreeCString(c, kind);
    JS_FreeCString(c, val);
    return done(c, err, JS_TRUE);
}

static JSValue f_set_background(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0);
    int on = argc > 1 && JS_ToBool(c, argv[1]) == 1;
    const char *err = !app ? "sys.badArgument" : webmgr_set_background(app, on);
    JS_FreeCString(c, app);
    return done(c, err, JS_TRUE);
}

static JSValue f_uninstall(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0);
    int del = argc > 1 && JS_ToBool(c, argv[1]) == 1;
    const char *err = !app ? "sys.badArgument" : webmgr_uninstall(app, del);
    JS_FreeCString(c, app);
    return done(c, err, JS_TRUE);
}

static JSValue f_launch(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0), *prof = arg_str(c, argc, argv, 1);
    int raised = 0;
    const char *err = !app || !prof ? "sys.badArgument" : webmgr_launch(app, prof, &raised);
    if (!err && raised) {
        /* the one way a running app comes forward: lord-server's own activation token, handed to its lord-web over
         * the bus (Show); lord-web activates its window with it, or maps it again from behind its tray icon */
        char wid[2 * WEBREG_ID_MAX + 16];
        snprintf(wid, sizeof wid, "lordos-web.%s.%s", app, prof);
        const char *tk = xact_issue(xact_now(), XACT_LAUNCH_MS, 1, wid);
        if (webask_show(app, prof, tk)) fprintf(stderr, "webapps: %s/%s is running but the session bus is not there to show it\n", app, prof);
    }
    JS_FreeCString(c, app);
    JS_FreeCString(c, prof);
    return done(c, err, JS_NewString(c, raised ? "running" : "started"));
}

static JSValue f_running(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    WebRunning r[WEB_MAX_RUNNING];
    int n = webmgr_running(r, WEB_MAX_RUNNING);
    JSValue arr = JS_NewArray(c);
    for (int i = 0; i < n; i++) {
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "app", JS_NewString(c, r[i].app));
        JS_SetPropertyStr(c, o, "profile", JS_NewString(c, r[i].profile));
        JS_SetPropertyStr(c, o, "pid", JS_NewInt32(c, (int32_t)r[i].pid));
        JS_SetPropertyUint32(c, arr, (uint32_t)i, o);
    }
    return arr;
}

static JSValue f_memory(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = arg_str(c, argc, argv, 0);
    if (!app) return js_throw_code(c, "sys.badArgument", NULL);
    if (!webreg_id_ok(app)) { JS_FreeCString(c, app); return js_throw_code(c, "webapps.badId", NULL); }
    WebRunning r[WEB_MAX_RUNNING];
    int n = webmgr_running(r, WEB_MAX_RUNNING), found = 0;
    long long total = 0;
    int procs = 0, unreadable = 0;
    JSValue arr = JS_NewArray(c);
    for (int i = 0; i < n; i++) {
        WebMem m;
        if (strcmp(r[i].app, app) || webmem_tree("/proc", r[i].pid, &m)) continue;
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "profile", JS_NewString(c, r[i].profile));
        JS_SetPropertyStr(c, o, "kb", JS_NewInt64(c, m.pss_kb));
        JS_SetPropertyStr(c, o, "processes", JS_NewInt32(c, m.procs));
        JS_SetPropertyUint32(c, arr, (uint32_t)found++, o);
        total += m.pss_kb;
        procs += m.procs;
        unreadable += m.unreadable;
    }
    JS_FreeCString(c, app);
    if (!found) { JS_FreeValue(c, arr); return JS_NULL; }
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "kb", JS_NewInt64(c, total));
    JS_SetPropertyStr(c, o, "processes", JS_NewInt32(c, procs));
    JS_SetPropertyStr(c, o, "unreadable", JS_NewInt32(c, unreadable));
    JS_SetPropertyStr(c, o, "profiles", arr);
    return o;
}

/* A window says it is lordos-web.<app>.<profile>: only a window of the lord-web we started for that app and
 * profile is shown as that web app (any client can set any app id). */
static JSValue f_owner(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t sid = 0;
    if (argc < 1 || JS_ToInt32(c, &sid, argv[0])) return js_throw_code(c, "sys.badArgument", NULL);
    int pid = wl_surface_pid(sid);
    char app[WEBREG_ID_MAX + 1], prof[WEBREG_ID_MAX + 1];
    if (pid <= 0 || webmgr_owner((pid_t)pid, app, prof)) return JS_NULL;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "app", JS_NewString(c, app));
    JS_SetPropertyStr(c, o, "profile", JS_NewString(c, prof));
    return o;
}

static JSValue f_answer(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int32_t id = 0;
    const char *v = arg_str(c, argc, argv, 1);
    if (argc < 1 || JS_ToInt32(c, &id, argv[0]) || !v) { JS_FreeCString(c, v); return js_throw_code(c, "sys.badArgument", NULL); }
    int r = webask_answer(id, v);
    JS_FreeCString(c, v);
    return JS_NewBool(c, r == 0);
}

static JSValue f_asks(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char *j = webask_pending();
    if (!j) return JS_NewArray(c);
    JSValue v = JS_ParseJSON(c, j, strlen(j), "<asks>");
    free(j);
    return v;
}

static const JSCFunctionListEntry webapps_funcs[] = {
    JS_CFUNC_DEF("available", 0, f_available),
    JS_CFUNC_DEF("list", 0, f_list),
    JS_CFUNC_DEF("get", 1, f_get),
    JS_CFUNC_DEF("probe", 1, f_probe),
    JS_CFUNC_DEF("probeResult", 1, f_probe_result),
    JS_CFUNC_DEF("install", 1, f_install),
    JS_CFUNC_DEF("addProfile", 2, f_add_profile),
    JS_CFUNC_DEF("renameProfile", 3, f_rename_profile),
    JS_CFUNC_DEF("removeProfile", 3, f_remove_profile),
    JS_CFUNC_DEF("setPermission", 3, f_set_permission),
    JS_CFUNC_DEF("setBackground", 2, f_set_background),
    JS_CFUNC_DEF("uninstall", 2, f_uninstall),
    JS_CFUNC_DEF("launch", 2, f_launch),
    JS_CFUNC_DEF("running", 0, f_running),
    JS_CFUNC_DEF("memory", 1, f_memory),
    JS_CFUNC_DEF("owner", 1, f_owner),
    JS_CFUNC_DEF("answer", 2, f_answer),
    JS_CFUNC_DEF("asks", 0, f_asks),
};

void js_webapps_register(JSContext *c, JSValue lord) {
    webmgr_init(js_sys_assets());
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, webapps_funcs, sizeof webapps_funcs / sizeof webapps_funcs[0]);
    JS_SetPropertyStr(c, lord, "webapps", o);
}
