/* realm_taint_test.c — LordOS's QuickJS patches for community themes (third_party/quickjs/quickjs.c: JS_StackHasRealm,
 * JS_IsFunctionOfRealm, JS_SetUntrustedRealm, JS_RealmTaint and the tainted jobs of JS_ExecutePendingJob), which
 * server/src/ui/js_theme.c's guard of every lord.* call stands on. Two realms of one runtime: "main" with a native
 * probe() that records whether the theme's code counts as on the stack, and "theme". Every way the theme can make main
 * call probe for it — directly, through a promise job with probe (or a bound probe, or a bound main function) as the
 * handler, through a thenable, through its own built-ins (Reflect.apply) — must count; main's own calls and jobs must
 * not. */
#include "quickjs.h"
#include <stdio.h>
#include <string.h>

static JSContext *M, *T;
static int last = -1, fails;

static JSValue probe(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    last = JS_StackHasRealm(c, T);
    return JS_UNDEFINED;
}

/* trusted(fn): JS_EnqueueTrustedJob, as lord.hostMicrotask (js_theme.c) queues React's render */
static JSValue call_job(JSContext *c, int argc, JSValueConst *argv) {
    return argc > 0 ? JS_Call(c, argv[0], JS_UNDEFINED, 0, NULL) : JS_UNDEFINED;
}
static JSValue trusted(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc > 0) JS_EnqueueTrustedJob(c, call_job, 1, argv);
    return JS_UNDEFINED;
}

static void drain(JSRuntime *rt) {
    JSContext *c;
    while (JS_ExecutePendingJob(rt, &c) > 0) {}
}

static void run(JSContext *c, const char *what, const char *src, int want) {
    last = -1;
    JSValue v = JS_Eval(c, src, strlen(src), what, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) { JSValue e = JS_GetException(c); const char *s = JS_ToCString(c, e); printf("FAIL %s threw %s\n", what, s); JS_FreeCString(c, s); JS_FreeValue(c, e); fails++; }
    JS_FreeValue(c, v);
    drain(JS_GetRuntime(c));
    if (last != want) { printf("FAIL %s: theme on the stack %d, want %d\n", what, last, want); fails++; }
    else printf("ok   %s\n", what);
}

static void realm(const char *what, const char *expr, int want) {
    JSValue v = JS_Eval(M, expr, strlen(expr), what, JS_EVAL_TYPE_GLOBAL);
    int got = JS_IsFunctionOfRealm(M, v, T);
    JS_FreeValue(M, v);
    if (got != want) { printf("FAIL %s: JS_IsFunctionOfRealm %d, want %d\n", what, got, want); fails++; }
    else printf("ok   %s\n", what);
}

int main(void) {
    JSRuntime *rt = JS_NewRuntime();
    M = JS_NewContext(rt);
    T = JS_NewContext(rt);
    JSValue mg = JS_GetGlobalObject(M), tg = JS_GetGlobalObject(T);
    JSValue p = JS_NewCFunction(M, probe, "probe", 0);
    JS_SetPropertyStr(M, mg, "probe", JS_DupValue(M, p));
    JS_SetPropertyStr(T, tg, "probe", p);
    /* a function of main's the theme can get (as from the SDK): it calls probe */
    JSValue mf = JS_Eval(M, "(function viaMain() { probe(); })", strlen("(function viaMain() { probe(); })"), "<main>", JS_EVAL_TYPE_GLOBAL);
    JS_SetPropertyStr(M, mg, "viaMain", JS_DupValue(M, mf));
    JS_SetPropertyStr(T, tg, "viaMain", mf);
    /* main keeps what the theme hands it and calls it later, from its own code */
    { const char *k = "var kept = null; function keep(f) { kept = f; } function later() { Promise.resolve().then(() => kept()); }"; JS_FreeValue(M, JS_Eval(M, k, strlen(k), "<main>", JS_EVAL_TYPE_GLOBAL)); }
    JSValue keep = JS_GetPropertyStr(M, mg, "keep");
    JS_SetPropertyStr(T, tg, "keep", keep);
    /* a promise of main's that the theme can see, resolved later by main's own code */
    { const char *k = "var resolveIt = null; var mainP = new Promise((r) => { resolveIt = r; }); var resolveIt2 = null; var mainP2 = new Promise((r) => { resolveIt2 = r; });"; JS_FreeValue(M, JS_Eval(M, k, strlen(k), "<main>", JS_EVAL_TYPE_GLOBAL)); }
    JS_SetPropertyStr(T, tg, "mainP", JS_GetPropertyStr(M, mg, "mainP"));
    JS_SetPropertyStr(M, mg, "trusted", JS_NewCFunction(M, trusted, "trusted", 1));
    /* main's scheduler the theme reaches (React's setState → scheduleMicrotask): main's own render queued for later */
    { const char *k = "function render() { probe(); } function schedule() { trusted(render); } function scheduleBound() { trusted(render.bind(null)); }"; JS_FreeValue(M, JS_Eval(M, k, strlen(k), "<main>", JS_EVAL_TYPE_GLOBAL)); }
    JS_SetPropertyStr(T, tg, "schedule", JS_GetPropertyStr(M, mg, "schedule"));
    JS_SetPropertyStr(T, tg, "scheduleBound", JS_GetPropertyStr(M, mg, "scheduleBound"));
    JS_SetUntrustedRealm(rt, T);

    run(M, "main calls probe", "probe()", 0);
    run(M, "main's job calls probe through its own function", "Promise.resolve().then(() => probe())", 0);
    run(T, "the theme calls probe", "probe()", 1);
    run(T, "the theme calls main's function", "viaMain()", 1);
    run(T, "Promise.then with probe as the handler", "Promise.resolve().then(probe)", 1);
    run(T, "Promise.then with a bound probe", "Promise.resolve().then(probe.bind(null))", 1);
    run(T, "Promise.then with a bound main function", "Promise.resolve().then(viaMain.bind(null))", 1);
    run(T, "a thenable whose then is main's function", "Promise.resolve({ then: viaMain })", 1);
    run(T, "the theme's own Reflect.apply", "Reflect.apply(probe, null, [])", 1);
    run(T, "async function after await", "(async () => { await 0; probe(); })()", 1);
    /* main later calls, from a job of its own, what the theme kept with it: a theme function counts by its frame; a
     * bound main function the theme made is a laundering main can't tell apart, which the facade's lord.themeCall and
     * the theme's own always-checked natives cover (js_theme.c, sdk/themeapi.js) */
    run(T, "kept: the theme's own arrow", "keep(() => probe())", -1);
    run(M, "main calls the kept theme arrow later", "later()", 1);
    /* F2: a reaction the theme registers with main's own function, on main's promise, that main resolves later from
     * its own code: tainted from the moment the theme registered it */
    run(T, "the theme registers main's function on main's promise", "mainP.then(viaMain)", -1);
    run(M, "main resolves it later: the reaction runs tainted", "resolveIt(1)", 1);
    run(M, "main's own reaction on main's promise stays untainted", "mainP2.then(viaMain); resolveIt2(1)", 0);
    run(T, "a job the theme queues with main's function", "Promise.resolve().then(viaMain)", 1);
    /* (0.8.9.6.2) a trusted job (lord.hostMicrotask): main's own render, queued while the theme's code runs, runs
     * untainted; a bound function in it is still tainted when it runs */
    run(T, "a trusted job main queues while the theme's code runs: untainted", "schedule()", 0);
    JS_RealmTaint(rt, 1);
    { const char *k = "schedule()"; JS_FreeValue(M, JS_Eval(M, k, strlen(k), "<main>", JS_EVAL_TYPE_GLOBAL)); }
    JS_RealmTaint(rt, -1);
    run(M, "a trusted job queued inside lord.themeCall: untainted", "", 0);
    run(T, "a trusted job of a bound function: still tainted", "scheduleBound()", 1);
    JS_RealmTaint(rt, 1);
    run(M, "main calls anything while tainted (lord.themeCall)", "probe()", 1);
    JS_RealmTaint(rt, -1);
    run(M, "and not after", "probe()", 0);

    realm("main's function", "viaMain", 0);
    realm("main's native", "probe", 0);
    realm("a bound main function", "viaMain.bind(null)", 1);
    realm("a proxy", "new Proxy(viaMain, {})", 1);
    JSValue tf = JS_Eval(T, "(function own() {})", strlen("(function own() {})"), "<theme>", JS_EVAL_TYPE_GLOBAL);
    JS_SetPropertyStr(M, mg, "own", tf);
    JSValue tr = JS_Eval(T, "Reflect.apply", strlen("Reflect.apply"), "<theme>", JS_EVAL_TYPE_GLOBAL);
    JS_SetPropertyStr(M, mg, "theirApply", tr);
    realm("the theme's function", "own", 2);
    realm("the theme's built-in", "theirApply", 1);

    JS_SetUntrustedRealm(rt, NULL);
    JS_FreeValue(M, mg);
    JS_FreeValue(T, tg);
    JS_FreeContext(T);
    JS_FreeContext(M);
    JS_FreeRuntime(rt);
    printf("%s: %d failure(s)\n", fails ? "FAILED" : "ok", fails);
    return fails ? 1 : 0;
}
