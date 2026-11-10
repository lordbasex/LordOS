/* js_sec.c — lord.perms and lord.sign (see js_sec.h). Strict input validation; the permission
 * state lives in perms.c (a file the apps cannot write). */
#include "js_sec.h"
#include "js_sys.h"
#include "js_err.h"
#include "../sec/perms.h"
#include "../sec/sign.h"
#include "../sec/pkgstate.h"
#include "../lpk.h"
#include "../sec/broker.h"
#include "../sec/codesig.h"
#include "../wl/wl.h"
#include "../wl/kbinhibit.h"

#include <string.h>

/* ---- lord.perms ---- */

static const char *state_name(PermState s) { return s == PERM_ALLOW ? "allow" : s == PERM_DENY ? "deny" : "ask"; }
static PermState state_from(const char *s) { return s && !strcmp(s, "allow") ? PERM_ALLOW : s && !strcmp(s, "deny") ? PERM_DENY : PERM_ASK; }

static JSValue f_perm_get(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    const char *app = JS_ToCString(c, argv[0]), *pn = JS_ToCString(c, argv[1]);
    int p = app && pn ? perm_from_name(pn) : -1;
    JSValue r = JS_NewString(c, p < 0 ? "ask" : state_name(perm_get(app, (Perm)p)));
    if (app) JS_FreeCString(c, app);
    if (pn) JS_FreeCString(c, pn);
    return r;
}

static JSValue f_perm_set(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    const char *app = JS_ToCString(c, argv[0]), *pn = JS_ToCString(c, argv[1]), *st = JS_ToCString(c, argv[2]);
    int p = app && pn ? perm_from_name(pn) : -1;
    PermState ns = state_from(st);
    /* a Linux app's "shortcuts" goes by its desktop id or program (wl/kbinhibit.h); everything else by an .lpk id */
    int named = app && (lpk_valid_id(app) || (p == PERM_SHORTCUTS && kbi_key_valid(app)));
    int ok = named && p >= 0 && perm_set(app, (Perm)p, ns) == 0;
    if (ok && ns != PERM_ALLOW) brokersrv_revoke(app, pn);      /* the isolated app releases the device instantly */
    if (ok && p == PERM_SHORTCUTS) wl_shortcuts_changed();      /* a window that holds them is told now */
    if (app) JS_FreeCString(c, app);
    if (pn) JS_FreeCString(c, pn);
    if (st) JS_FreeCString(c, st);
    return JS_NewBool(c, ok);
}

static JSValue f_perm_list(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    static PermEntry buf[512];
    int n = perm_list(buf, 512);
    JSValue a = JS_NewArray(c);
    for (int i = 0; i < n; i++) {
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "app", JS_NewString(c, buf[i].app));
        JS_SetPropertyStr(c, o, "perm", JS_NewString(c, perm_name(buf[i].perm)));
        JS_SetPropertyStr(c, o, "state", JS_NewString(c, state_name(buf[i].state)));
        JS_SetPropertyStr(c, o, "when", JS_NewInt64(c, buf[i].when));
        JS_SetPropertyUint32(c, a, (uint32_t)i, o);
    }
    return a;
}

static JSValue f_perm_names(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue a = JS_NewArray(c);
    for (int i = 0; i < PERM_COUNT; i++) JS_SetPropertyUint32(c, a, (uint32_t)i, JS_NewString(c, perm_name((Perm)i)));
    return a;
}

static const JSCFunctionListEntry perms_funcs[] = {
    JS_CFUNC_DEF("get", 2, f_perm_get),
    JS_CFUNC_DEF("set", 3, f_perm_set),
    JS_CFUNC_DEF("list", 0, f_perm_list),
    JS_CFUNC_DEF("names", 0, f_perm_names),
};

/* ---- lord.sign ---- */

static void put_sig(JSContext *c, JSValue o, const LpkSigInfo *si) {
    JS_SetPropertyStr(c, o, "trust", JS_NewString(c, sig_trust_name(si->trust)));
    JS_SetPropertyStr(c, o, "signer", JS_NewString(c, si->signer));
    JS_SetPropertyStr(c, o, "key", JS_NewString(c, si->key));
    JS_SetPropertyStr(c, o, "signed", JS_NewBool(c, si->has_signature));
    JS_SetPropertyStr(c, o, "revoked", JS_NewBool(c, si->revoked));
    if (si->revoked) JS_SetPropertyStr(c, o, "revokedWhy", JS_NewString(c, si->revoked_why));
}

/* lord.sign.status(id) → { installed, hasOriginal, modified, trust, signer, revoked, version,
 *                          changed: [{ path, kind }] } */
static JSValue f_sign_status(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    const char *id = JS_ToCString(c, argv[0]);
    if (!id) return JS_EXCEPTION;
    PkgStatus st;
    JSValue o = JS_NewObject(c);
    if (!lpk_valid_id(id) || pkg_status(id, &st)) { JS_FreeCString(c, id); JS_SetPropertyStr(c, o, "installed", JS_FALSE); return o; }
    JS_FreeCString(c, id);
    JS_SetPropertyStr(c, o, "installed", JS_NewBool(c, st.installed));
    JS_SetPropertyStr(c, o, "hasOriginal", JS_NewBool(c, st.has_original));
    JS_SetPropertyStr(c, o, "modified", JS_NewBool(c, st.modified));
    JS_SetPropertyStr(c, o, "version", JS_NewString(c, st.version));
    if (st.error[0]) JS_SetPropertyStr(c, o, "error", JS_NewString(c, st.error));
    if (st.has_original) put_sig(c, o, &st.sig);
    JSValue arr = JS_NewArray(c);
    int n = st.nchanged < PKG_MAX_CHANGED ? st.nchanged : PKG_MAX_CHANGED;
    for (int i = 0; i < n; i++) {
        JSValue e = JS_NewObject(c);
        JS_SetPropertyStr(c, e, "path", JS_NewString(c, st.changed[i]));
        char k[2] = { st.change_kind[i], 0 };
        JS_SetPropertyStr(c, e, "kind", JS_NewString(c, k));   /* m modified, a added, r deleted */
        JS_SetPropertyUint32(c, arr, (uint32_t)i, e);
    }
    JS_SetPropertyStr(c, o, "changed", arr);
    JS_SetPropertyStr(c, o, "changedCount", JS_NewInt32(c, st.nchanged));
    return o;
}

static JSValue f_sign_restore(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    const char *id = JS_ToCString(c, argv[0]);
    char err[160] = "";
    int ok = id && lpk_valid_id(id) && pkg_restore(id, err, sizeof err) == 0;
    if (id) JS_FreeCString(c, id);
    if (!ok) return js_throw_line(c, err[0] ? err : "pkgstate.restoreFailed");
    return JS_TRUE;
}

/* lord.sign.share(id, manifestText, relativeOutput) → path of the created .lpk (in the home
 * folder; the UI picks where with the powerbox, but here it is built at a validated path). */
static JSValue f_sign_share(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    const char *id = JS_ToCString(c, argv[0]);
    size_t mlen = 0;
    const char *manifest = JS_ToCStringLen(c, &mlen, argv[1]);
    const char *rel = JS_ToCString(c, argv[2]);
    char out[1024], err[160] = "";
    int ok = id && manifest && rel && lpk_valid_id(id) && !js_sys_resolve_target(rel, out, sizeof out) &&
             pkg_share(id, manifest, mlen, out, err, sizeof err) == 0;
    JSValue r = ok ? JS_NewString(c, out) : js_throw_line(c, err[0] ? err : "pkgstate.shareFailed");
    if (id) JS_FreeCString(c, id);
    if (manifest) JS_FreeCString(c, manifest);
    if (rel) JS_FreeCString(c, rel);
    return r;
}

static JSValue f_sign_userkey(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    uint8_t seed[32];
    char pub[65], name[128];
    if (pkg_user_key(seed, pub, name)) return js_throw_code(c, "pkgstate.keyFailed", NULL);
    memset(seed, 0, sizeof seed);
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "public", JS_NewString(c, pub));
    JS_SetPropertyStr(c, o, "name", JS_NewString(c, name));
    return o;
}

static const JSCFunctionListEntry sign_funcs[] = {
    JS_CFUNC_DEF("status", 1, f_sign_status),
    JS_CFUNC_DEF("restore", 1, f_sign_restore),
    JS_CFUNC_DEF("share", 3, f_sign_share),
    JS_CFUNC_DEF("userKey", 0, f_sign_userkey),
};

/* lord.broker.resolve(reqid, allow): the user's answer to a permission prompt from an isolated
 * app (from sdk/privacy.js). It only exists in the compositor process. */
static JSValue f_broker_resolve(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    int64_t reqid = 0;
    JS_ToInt64(c, &reqid, argv[0]);
    brokersrv_resolve((uint32_t)reqid, JS_ToBool(c, argv[1]));
    return JS_UNDEFINED;
}
/* lord.broker.publish(type, json): the theme publishes its tokens ("theme") or the language ("locale") and
 * lord-server forwards them live to the isolated apps. */
static JSValue f_broker_publish(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    const char *kind = JS_ToCString(c, argv[0]);
    size_t len = 0;
    const char *json = JS_ToCStringLen(c, &len, argv[1]);
    int ok = kind && json && brokersrv_publish(kind, json, len) == 0;
    if (kind) JS_FreeCString(c, kind);
    if (json) JS_FreeCString(c, json);
    return JS_NewBool(c, ok);
}
static JSValue f_broker_pick_result(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    int64_t reqid = 0;
    JS_ToInt64(c, &reqid, argv[0]);
    const char *path = JS_IsString(argv[1]) ? JS_ToCString(c, argv[1]) : NULL;
    brokersrv_pick_result((uint32_t)reqid, path);
    if (path) JS_FreeCString(c, path);
    return JS_UNDEFINED;
}
static JSValue f_broker_menus(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    const char *app = JS_ToCString(c, argv[0]);
    JSValue r = JS_ParseJSON(c, app ? brokersrv_menus(app) : "[]", strlen(app ? brokersrv_menus(app) : "[]"), "<menus>");
    if (app) JS_FreeCString(c, app);
    if (JS_IsException(r)) { JS_FreeValue(c, JS_GetException(c)); return JS_NewArray(c); }
    return r;
}
static JSValue f_broker_menu_click(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    const char *app = JS_ToCString(c, argv[0]), *item = JS_ToCString(c, argv[1]);
    if (app && item) brokersrv_menu_click(app, item);
    if (app) JS_FreeCString(c, app);
    if (item) JS_FreeCString(c, item);
    return JS_UNDEFINED;
}
/* lord.broker.quit(app) → true if the isolated app is running and was asked to quit (❖Q, Dock →
 * Quit). The app closes its own windows, so its close guards still apply. */
static JSValue f_broker_quit(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(c, argv[0]) : NULL;
    int ok = app && lpk_valid_id(app) && brokersrv_quit(app) == 0;
    if (app) JS_FreeCString(c, app);
    return JS_NewBool(c, ok);
}
/* lord.broker.openArgs(app, json, newWindow) → true if delivered or kept: args for an isolated app
 * (wm.open(id, { args }) in the session, e.g. a notification's "Open" or the Dock's "New message").
 * Sent as an "open" event now if the app runs, or when it starts (sdk/packages.js isolatedApp). */
static JSValue f_broker_open_args(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *app = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(c, argv[0]) : NULL;
    size_t len = 0;
    const char *json = argc > 1 && JS_IsString(argv[1]) ? JS_ToCStringLen(c, &len, argv[1]) : NULL;
    int ok = app && json && brokersrv_open_args(app, json, len, argc > 2 && JS_ToBool(c, argv[2])) == 0;
    if (app) JS_FreeCString(c, app);
    if (json) JS_FreeCString(c, json);
    return JS_NewBool(c, ok);
}
static JSValue f_broker_status_click(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc;
    const char *app = JS_ToCString(c, argv[0]), *item = JS_ToCString(c, argv[1]);
    if (app && item) brokersrv_status_click(app, item);
    if (app) JS_FreeCString(c, app);
    if (item) JS_FreeCString(c, item);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry broker_funcs[] = {
    JS_CFUNC_DEF("statusClick", 2, f_broker_status_click),
    JS_CFUNC_DEF("openArgs", 3, f_broker_open_args),
    JS_CFUNC_DEF("resolve", 2, f_broker_resolve),
    JS_CFUNC_DEF("publish", 2, f_broker_publish),
    JS_CFUNC_DEF("pickResult", 2, f_broker_pick_result),
    JS_CFUNC_DEF("menus", 1, f_broker_menus),
    JS_CFUNC_DEF("menuClick", 2, f_broker_menu_click),
    JS_CFUNC_DEF("quit", 1, f_broker_quit),
};

/* ---- lord.code: verified system code (sec/codesig.c, docs/SIGNING.md "Verified system code") ----
 *   lord.code.status() → { state: 'verified'|'development'|'invalid'|'unchecked', signed, release,
 *                          version, files, ms, reason, refused: [{ path, reason }] }
 *   lord.code.takeRefused() → the refusals not reported yet ([{ path, reason }]); each is returned once
 *                          per session, for the "This LordOS component was modified" notification. */
static JSValue refusal_obj(JSContext *c, const CodeRefusal *r) {
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "path", JS_NewString(c, r->path));
    JS_SetPropertyStr(c, o, "reason", JS_NewString(c, r->reason));
    return o;
}

static JSValue f_code_status(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    codesig_init(js_sys_assets());
    const CodeManifest *m = codesig_manifest();
    JSValue o = JS_NewObject(c), arr = JS_NewArray(c);
    JS_SetPropertyStr(c, o, "state", JS_NewString(c, codesig_state_name(codesig_state())));
    JS_SetPropertyStr(c, o, "signed", JS_NewBool(c, m && m->signed_ok));
    JS_SetPropertyStr(c, o, "release", JS_NewBool(c, codesig_release_build()));
    JS_SetPropertyStr(c, o, "version", JS_NewString(c, m ? m->version : ""));
    JS_SetPropertyStr(c, o, "files", JS_NewInt32(c, m ? m->nfiles : 0));
    JS_SetPropertyStr(c, o, "ms", JS_NewFloat64(c, codesig_ms()));
    JS_SetPropertyStr(c, o, "reason", JS_NewString(c, codesig_reason()));
    for (int i = 0; i < codesig_refused_count(); i++) JS_SetPropertyUint32(c, arr, (uint32_t)i, refusal_obj(c, codesig_refused(i)));
    JS_SetPropertyStr(c, o, "refused", arr);
    return o;
}

static JSValue f_code_take(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    JSValue arr = JS_NewArray(c);
    uint32_t n = 0;
    for (int i = 0; i < codesig_refused_count(); i++) {
        CodeRefusal *r = codesig_refused(i);
        if (r->reported) continue;
        r->reported = 1;
        JS_SetPropertyUint32(c, arr, n++, refusal_obj(c, r));
    }
    return arr;
}

static const JSCFunctionListEntry code_funcs[] = {
    JS_CFUNC_DEF("status", 0, f_code_status),
    JS_CFUNC_DEF("takeRefused", 0, f_code_take),
};

void js_sec_register(JSContext *c, JSValue lord) {
    JSValue code = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, code, code_funcs, sizeof code_funcs / sizeof code_funcs[0]);
    JS_SetPropertyStr(c, lord, "code", code);
    JSValue perms = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, perms, perms_funcs, sizeof perms_funcs / sizeof perms_funcs[0]);
    JS_SetPropertyStr(c, lord, "perms", perms);
    JSValue sign = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, sign, sign_funcs, sizeof sign_funcs / sizeof sign_funcs[0]);
    JS_SetPropertyStr(c, lord, "sign", sign);
    JSValue broker = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, broker, broker_funcs, sizeof broker_funcs / sizeof broker_funcs[0]);
    JS_SetPropertyStr(c, lord, "broker", broker);
}
