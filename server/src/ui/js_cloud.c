/* js_cloud.c — lord.cloud: Cloud (cloud/cloud.h, docs/CLOUD.md section 17). JS never reaches rclone: each call here
 * is one of a closed list, with checked arguments, and lord-server's cloud thread is rclone's only client.
 *
 *   available() → bool                      state() → State (also pushed: __lordCloud(state))
 *   providers() → { req } → [{ kind, label, description, group, verified, website?, fields: [{ name, label, help, secret, required, default, examples }] }]
 *   answer(state, value) → { req } → { id } | { question } | { error }     (add answers the same way)
 *   emptyTrash(id) / measure(id) / cancelMeasure(id) / delete(paths) → { req } (answers through __lordCloudReply)
 *   beginSignIn(kind, name, { clientId, clientSecret }) → { id, url, expiresAt } | { error }
 *   openSignInPage(id) → bool               cancelSignIn(id)
 *   add(kind, name, fields) → { id } | { error, field? }
 *   remove(id, { discardPending }) → { ok } | { error: 'pending', files }
 *   rename(id, name) → { ok } | { error }   refreshAbout(id)            websiteUrl(id) → string | null
 *   retry(id|null)                          an account that isn't connected (keyring locked, offline) tries again
 *   status(paths) → { [path]: { st, pct?, error? } }
 *   keep(paths) / unkeep(paths)             free(paths) → { freed, skipped: [{ path, reason }] }
 *   link(path, expire) / unlink(path) → { req } (the answer comes in __lordCloudReply(req, result)) | { error }
 *   links(id) → [{ path, url, expires, made }]
 *   pause(id|null) / resume(id|null)        setOptions({ bwUp, bwDown, pauseOnBattery, pauseOnMetered, autoFree, freeBelowGb })
 *   restart()                               logName() → the Logs app's file name of rclone's log */
#include "js_cloud.h"
#include "js.h"
#include "js_sys.h"
#include "../apps.h"
#include "../cloud/cloud.h"
#include "../cloud/cloud_kr.h"
#include "../cloud/cloud_pure.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATHS_MAX 4096

static JSValue parsed(JSContext *c, char *j) {
    if (!j) return JS_NULL;
    JSValue v = JS_ParseJSON(c, j, strlen(j), "<cloud>");
    free(j);
    return v;
}

static const char *arg(JSContext *c, int argc, JSValueConst *argv, int i) {
    return i < argc && JS_IsString(argv[i]) ? JS_ToCString(c, argv[i]) : NULL;
}

static const char *prop(JSContext *c, JSValueConst o, const char *k) {
    if (!JS_IsObject(o)) return NULL;
    JSValue v = JS_GetPropertyStr(c, o, k);
    const char *s = JS_IsString(v) ? JS_ToCString(c, v) : NULL;
    JS_FreeValue(c, v);
    return s;
}

static void fr(JSContext *c, const char *s) { if (s) JS_FreeCString(c, s); }

static JSValue err(JSContext *c, const char *code) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "error", JS_NewString(c, code));
    return o;
}

/* the default browser, through the launcher's own rules (apps.c): one argument, no shell */
static int open_url(const char *url) {
    char def[192];
    if (!apps_default_for("x-scheme-handler/http", def, sizeof def) && !apps_default_for("x-scheme-handler/https", def, sizeof def)) return -1;
    size_t l = strlen(def);
    if (l > 8 && !strcmp(def + l - 8, ".desktop")) def[l - 8] = 0;
    return apps_launch_uri(def, url);
}

/* an array of strings (paths), at most PATHS_MAX */
static int paths_of(JSContext *c, JSValueConst a, const char ***out) {
    *out = NULL;
    if (!JS_IsArray(c, a)) return -1;
    JSValue lv = JS_GetPropertyStr(c, a, "length");
    int32_t n = 0;
    JS_ToInt32(c, &n, lv);
    JS_FreeValue(c, lv);
    if (n < 0 || n > PATHS_MAX) return -1;
    const char **p = calloc((size_t)n + 1, sizeof *p);
    if (!p) return -1;
    int k = 0;
    for (int i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(c, a, (uint32_t)i);
        if (JS_IsString(v)) p[k++] = JS_ToCString(c, v);
        JS_FreeValue(c, v);
    }
    *out = p;
    return k;
}
static void paths_free(JSContext *c, const char **p, int n) { for (int i = 0; p && i < n; i++) fr(c, p[i]); free(p); }

static JSValue f_available(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; (void)argv; return JS_NewBool(c, cloud_available()); }
static JSValue f_state(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; (void)argv; return parsed(c, cloud_state_json()); }
static JSValue req_or(JSContext *c, char *res, int req);
static JSValue f_providers(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    int req = 0;
    char *r = cloud_providers(&req);
    return req_or(c, r, req);
}

static JSValue f_begin(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *k = arg(c, argc, argv, 0), *n = arg(c, argc, argv, 1);
    const char *id = argc > 2 ? prop(c, argv[2], "clientId") : NULL, *sec = argc > 2 ? prop(c, argv[2], "clientSecret") : NULL;
    const char *acc = argc > 2 ? prop(c, argv[2], "accountId") : NULL;
    JSValue r = k && n ? parsed(c, cloud_begin_signin(k, n, id, sec, acc)) : err(c, "cloud.badArgument");
    fr(c, k); fr(c, n); fr(c, id); fr(c, sec); fr(c, acc);
    return r;
}

static JSValue f_open_signin(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *id = arg(c, argc, argv, 0);
    char url[256];
    int ok = id && !cloud_signin_url(id, url, sizeof url) && !open_url(url);
    fr(c, id);
    return JS_NewBool(c, ok);
}

static JSValue f_cancel_signin(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *id = arg(c, argc, argv, 0);
    if (id) cloud_cancel_signin(id);
    fr(c, id);
    return JS_UNDEFINED;
}

static JSValue f_add(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *k = arg(c, argc, argv, 0), *n = arg(c, argc, argv, 1);
    JSValue s = argc > 2 && JS_IsObject(argv[2]) ? JS_JSONStringify(c, argv[2], JS_UNDEFINED, JS_UNDEFINED) : JS_UNDEFINED;
    const char *f = JS_IsString(s) ? JS_ToCString(c, s) : NULL;
    JS_FreeValue(c, s);
    const char *acc = argc > 3 ? prop(c, argv[3], "accountId") : NULL;
    int req = 0;
    /* the call first: it sets req (C leaves the order of a call's arguments unspecified) */
    char *res_ = (k && n && f) ? cloud_add(k, n, f, acc, &req) : NULL;
    JSValue r = (k && n && f) ? req_or(c, res_, req) : err(c, "cloud.badArgument");
    fr(c, k); fr(c, n); fr(c, f); fr(c, acc);
    return r;
}

static JSValue f_remove(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *id = arg(c, argc, argv, 0);
    int discard = 0;
    if (argc > 1 && JS_IsObject(argv[1])) { JSValue v = JS_GetPropertyStr(c, argv[1], "discardPending"); discard = JS_ToBool(c, v) > 0; JS_FreeValue(c, v); }
    JSValue r = id ? parsed(c, cloud_remove(id, discard)) : err(c, "cloud.badArgument");
    fr(c, id);
    return r;
}

/* setEnabled(id, on, { how: 'ask' | 'wait' | 'force' }): an account turned off keeps its settings and sign-in */
static JSValue f_set_enabled(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *id = arg(c, argc, argv, 0);
    int on = argc > 1 && JS_ToBool(c, argv[1]) > 0, how = CS_ASK;
    const char *h = argc > 2 && JS_IsObject(argv[2]) ? prop(c, argv[2], "how") : NULL;
    if (h && !strcmp(h, "wait")) how = CS_WAIT_IDLE;
    else if (h && !strcmp(h, "force")) how = CS_FORCE;
    JSValue r = id ? parsed(c, cloud_set_enabled(id, on, how)) : err(c, "cloud.badArgument");
    fr(c, id); fr(c, h);
    return r;
}

static JSValue f_rename(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *id = arg(c, argc, argv, 0), *n = arg(c, argc, argv, 1);
    JSValue r = id && n ? parsed(c, cloud_rename(id, n)) : err(c, "cloud.badArgument");
    fr(c, id); fr(c, n);
    return r;
}

static JSValue f_about(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *id = arg(c, argc, argv, 0);
    if (id) cloud_refresh_about(id);
    fr(c, id);
    return JS_UNDEFINED;
}

static JSValue f_retry(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *id = arg(c, argc, argv, 0);
    cloud_retry(id);                        /* null or no argument: every account not connected */
    fr(c, id);
    return JS_UNDEFINED;
}

static JSValue f_website(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *id = arg(c, argc, argv, 0);
    char *u = id ? cloud_website(id) : NULL;
    fr(c, id);
    JSValue r = u ? JS_NewString(c, u) : JS_NULL;
    free(u);
    return r;
}

static JSValue f_status(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char **p;
    int n = argc ? paths_of(c, argv[0], &p) : -1;
    if (n < 0) return JS_NewObject(c);
    JSValue r = parsed(c, cloud_status(p, n));
    paths_free(c, p, n);
    return r;
}

/* a folder of an account Files shows (docs/CLOUD.md 7.3): nothing is waited for */
static JSValue f_browse(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *p = arg(c, argc, argv, 0);
    if (p) cloud_browse(p);
    fr(c, p);
    return JS_UNDEFINED;
}

static JSValue keep_(JSContext *c, int argc, JSValueConst *argv, int on) {
    const char **p;
    int n = argc ? paths_of(c, argv[0], &p) : -1;
    if (n < 0) return JS_FALSE;
    cloud_keep(p, n, on);
    paths_free(c, p, n);
    return JS_TRUE;
}
static JSValue f_keep(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; return keep_(c, argc, argv, 1); }
static JSValue f_unkeep(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; return keep_(c, argc, argv, 0); }

static JSValue f_free(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char **p;
    int n = argc ? paths_of(c, argv[0], &p) : -1;
    if (n < 0) return err(c, "cloud.badArgument");
    JSValue r = parsed(c, cloud_free(p, n));
    paths_free(c, p, n);
    return r;
}

static JSValue req_or(JSContext *c, char *res, int req) {
    if (res) return parsed(c, res);
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "req", JS_NewInt32(c, req));
    return o;
}

static JSValue f_link(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *p = arg(c, argc, argv, 0), *e = arg(c, argc, argv, 1);
    int req = 0;
    /* the call first: it sets req (C leaves the order of a call's arguments unspecified) */
    char *res_ = (p) ? cloud_link(p, e ? e : "never", &req) : NULL;
    JSValue r = (p) ? req_or(c, res_, req) : err(c, "cloud.badArgument");
    fr(c, p); fr(c, e);
    return r;
}

static JSValue f_answer(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *st = arg(c, argc, argv, 0), *v = arg(c, argc, argv, 1);
    int req = 0, apply = 1;      /* { applyToOthers } (N4), true unless said */
    if (argc > 2 && JS_IsObject(argv[2])) { JSValue x = JS_GetPropertyStr(c, argv[2], "applyToOthers"); if (JS_IsBool(x)) apply = JS_ToBool(c, x) > 0; JS_FreeValue(c, x); }
    /* the call first: it sets req (C leaves the order of a call's arguments unspecified) */
    char *res_ = (st && v) ? cloud_answer(st, v, apply, &req) : NULL;
    JSValue r = (st && v) ? req_or(c, res_, req) : err(c, "cloud.badArgument");
    fr(c, st); fr(c, v);      /* not wiped: it may be the JS string's own memory */
    return r;
}

static JSValue acc_call(JSContext *c, int argc, JSValueConst *argv, char *(*fn)(const char *, int *)) {
    const char *id = arg(c, argc, argv, 0);
    int req = 0;
    /* the call first: it sets req (C leaves the order of a call's arguments unspecified) */
    char *res_ = (id) ? fn(id, &req) : NULL;
    JSValue r = (id) ? req_or(c, res_, req) : err(c, "cloud.badArgument");
    fr(c, id);
    return r;
}
static JSValue f_trash(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; return acc_call(c, argc, argv, cloud_empty_trash); }
static JSValue f_measure(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; return acc_call(c, argc, argv, cloud_measure); }
static JSValue f_cancel_measure(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; return acc_call(c, argc, argv, cloud_cancel_measure); }

static JSValue f_delete(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char **p;
    int n = argc ? paths_of(c, argv[0], &p) : -1;
    if (n < 0) return err(c, "cloud.badArgument");
    int req = 0;
    /* the call first: it sets req (C leaves the order of a call's arguments unspecified) */
    char *res_ = cloud_delete(p, n, &req);
    JSValue r = req_or(c, res_, req);
    paths_free(c, p, n);
    return r;
}

static JSValue f_search(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *p = arg(c, argc, argv, 0), *n = arg(c, argc, argv, 1);
    int req = 0;
    char *res_ = p && n ? cloud_search(p, n, &req) : NULL;
    JSValue r = (p && n) ? req_or(c, res_, req) : err(c, "cloud.badArgument");
    fr(c, p); fr(c, n);
    return r;
}

static JSValue f_unlink(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *p = arg(c, argc, argv, 0);
    int req = 0;
    /* the call first: it sets req (C leaves the order of a call's arguments unspecified) */
    char *res_ = (p) ? cloud_unlink(p, &req) : NULL;
    JSValue r = (p) ? req_or(c, res_, req) : err(c, "cloud.badArgument");
    fr(c, p);
    return r;
}

static JSValue f_links(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *id = arg(c, argc, argv, 0);
    JSValue r = id ? parsed(c, cloud_links(id)) : JS_NewArray(c);
    fr(c, id);
    return r;
}

static JSValue pause_(JSContext *c, int argc, JSValueConst *argv, int on) {
    const char *id = arg(c, argc, argv, 0);
    cloud_pause(id, on);
    fr(c, id);
    return JS_UNDEFINED;
}
static JSValue f_pause(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; return pause_(c, argc, argv, 1); }
static JSValue f_resume(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; return pause_(c, argc, argv, 0); }

static JSValue f_options(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || !JS_IsObject(argv[0])) return err(c, "cloud.badOption");
    JSValue s = JS_JSONStringify(c, argv[0], JS_UNDEFINED, JS_UNDEFINED);
    const char *j = JS_IsString(s) ? JS_ToCString(c, s) : NULL;
    JS_FreeValue(c, s);
    JSValue r = j ? parsed(c, cloud_set_options(j)) : err(c, "cloud.badOption");
    fr(c, j);
    return r;
}

static JSValue f_restart(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)c; (void)t; (void)argc; (void)argv; cloud_restart(); return JS_UNDEFINED; }
static JSValue f_logname(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) { (void)t; (void)argc; (void)argv; return JS_NewString(c, cloud_log_name()); }

static const JSCFunctionListEntry fns[] = {
    JS_CFUNC_DEF("available", 0, f_available),
    JS_CFUNC_DEF("state", 0, f_state),
    JS_CFUNC_DEF("providers", 0, f_providers),
    JS_CFUNC_DEF("beginSignIn", 3, f_begin),
    JS_CFUNC_DEF("openSignInPage", 1, f_open_signin),
    JS_CFUNC_DEF("cancelSignIn", 1, f_cancel_signin),
    JS_CFUNC_DEF("add", 3, f_add),
    JS_CFUNC_DEF("answer", 2, f_answer),
    JS_CFUNC_DEF("emptyTrash", 1, f_trash),
    JS_CFUNC_DEF("measure", 1, f_measure),
    JS_CFUNC_DEF("cancelMeasure", 1, f_cancel_measure),
    JS_CFUNC_DEF("delete", 1, f_delete),
    JS_CFUNC_DEF("search", 2, f_search),
    JS_CFUNC_DEF("remove", 2, f_remove),
    JS_CFUNC_DEF("rename", 2, f_rename),
    JS_CFUNC_DEF("setEnabled", 3, f_set_enabled),
    JS_CFUNC_DEF("refreshAbout", 1, f_about),
    JS_CFUNC_DEF("retry", 1, f_retry),
    JS_CFUNC_DEF("websiteUrl", 1, f_website),
    JS_CFUNC_DEF("status", 1, f_status),
    JS_CFUNC_DEF("browse", 1, f_browse),
    JS_CFUNC_DEF("keep", 1, f_keep),
    JS_CFUNC_DEF("unkeep", 1, f_unkeep),
    JS_CFUNC_DEF("free", 1, f_free),
    JS_CFUNC_DEF("link", 2, f_link),
    JS_CFUNC_DEF("unlink", 1, f_unlink),
    JS_CFUNC_DEF("links", 1, f_links),
    JS_CFUNC_DEF("pause", 1, f_pause),
    JS_CFUNC_DEF("resume", 1, f_resume),
    JS_CFUNC_DEF("setOptions", 1, f_options),
    JS_CFUNC_DEF("restart", 0, f_restart),
    JS_CFUNC_DEF("logName", 0, f_logname),
};

void js_cloud_register(JSContext *c, JSValue lord) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, fns, sizeof fns / sizeof fns[0]);
    JS_SetPropertyStr(c, lord, "cloud", o);
    if (js_sys_assets() && js_sys_assets()[0]) cloud_set_assets(js_sys_assets());
}

int js_cloud_start(const char *home) {
    if (js_sys_assets() && js_sys_assets()[0]) cloud_set_assets(js_sys_assets());
    int r = cloud_start(home, &cloud_keyring);
    if (!r) cloud_kr_watch();
    return r;
}

void js_cloud_dispatch(void) { cloud_dispatch(js_cloud_event, js_cloud_reply, open_url); }
