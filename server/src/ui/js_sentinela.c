/* js_sentinela.c — lord.sentinela: Sentinela's local model engine and a process's origin (llm/engine.h,
 * docs/SENTINELA.md). Registered only in the desktop's own JS context; the broker of isolated apps has no lord.sentinela.
 * Nothing here waits: the calls queue a job for the engine's thread and take its result later.
 *
 *   available()                    lord-llm and the engine (LordOS's llama.cpp package) are installed
 *   models()                       [{ id, bytes }] the installed models (/usr/share/lordos/llm/<id>.gguf)
 *   analyze(model, system, user)   a job id; the texts at most 4 KB and 8 KB (Sentinela's prompt and summary)
 *   facts(pid, start)              a job id: the process's executable, its package, where it lives, its parents
 *   investigate(model, system, user, pid, start)   (0.8.9.6, the person's click in Monitor's Get Info) a job id: the
 *                                  facts in user, then the program's script text or a binary's metadata read by the
 *                                  engine's thread (llm/evidence.c), fenced as data; the investigation's schema
 *   translate(model, system, text) (0.8.9.6) a job id: a description in the person's language (one "text")
 *   block(pid, start, withPkg)     (0.8.9.6, the person's click) a request id: the process's program (or script) on the
 *                                  blocklist, ended whenever it starts (llm/blockwatch.h)
 *   unblock(id)                    a request id: that entry off the list
 *   blockTake(id)                  the request's result once, or null while it runs
 *   blocked()                      { version, entries: [{ id, kind, path, size, interp, pkg, name, at }] }
 *   blockEvents()                  what the blocklist ended since the last call: [{ id, name, pid, at }]
 *   take(id)                       the job's result once (an object, see engine.h) or null while it runs
 *   state()                        { state: 'off'|'starting'|'ready'|'busy', model, pid, idleMs, queued, (0.8.9.6.2) mode,
 *                                  gpus: { vulkan, cuda }, backend, device, layers, layersTotal, fallback } (engine.h)
 *   stop()                         stops the engine now and drops what is queued
 *   backend(mode)                  (0.8.9.6.2) where the engine computes: 'auto' | 'cpu' | 'gpu' (Settings → AI); true,
 *                                  or throws sys.badArgument
 *
 * lord.writing (0.8.9.6.2, docs/AI.md "Writing"): "✨ AI" on selected text, for the shell's text fields only.
 *   start(model, action, target, text)  a job id: action "fix" | "improve" | "shorter" | "translate" (target: an
 *                                  interface language), the text at most 16,000 bytes; the prompts are fixed in C
 *   peek(id)                       { state: 'queued'|'starting'|'writing', text } while it runs, else null
 *   take(id)                       { ok, content, truncated } or { ok: false, error } once
 *   cancel(id)                     drops it or stops the answer being streamed
 *   serve(on, chosen)              the shell's Writing switch and chosen model, for the `lumo` command (ui/writebus.h) */
#include "js_sentinela.h"
#include "js_err.h"
#include "../llm/engine.h"
#include "../llm/blockwatch.h"
#include "../llm/writing.h"
#include "writebus.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static JSValue parsed(JSContext *c, char *j, const char *what) {
    if (!j) return JS_NULL;
    JSValue v = JS_ParseJSON(c, j, strlen(j), what);
    free(j);
    if (JS_IsException(v)) { JS_FreeValue(c, JS_GetException(c)); return JS_NULL; }
    return v;
}

static JSValue f_available(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewBool(c, llm_available());
}

static JSValue f_models(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue v = parsed(c, llm_models_json(), "<sentinela.models>");
    return JS_IsNull(v) ? JS_NewArray(c) : v;
}

/* a string argument of at most max bytes, or NULL */
static const char *str_arg(JSContext *c, JSValueConst v, size_t max) {
    if (!JS_IsString(v)) return NULL;
    size_t n;
    const char *s = JS_ToCStringLen(c, &n, v);
    if (s && (n > max || strlen(s) != n)) { JS_FreeCString(c, s); return NULL; }
    return s;
}

static JSValue f_analyze(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 3) return js_throw_code(c, "sys.badArgument", NULL);
    const char *model = str_arg(c, argv[0], 48), *system = str_arg(c, argv[1], LLM_SYSTEM_MAX), *user = str_arg(c, argv[2], LLM_USER_MAX);
    int id = model && system && user ? llm_submit_analyze(model, system, user) : (errno = EINVAL, -1);
    int e = errno;
    if (model) JS_FreeCString(c, model);
    if (system) JS_FreeCString(c, system);
    if (user) JS_FreeCString(c, user);
    if (id < 0) return js_throw_code(c, e == EBUSY ? "llm.busy" : "sys.badArgument", NULL);
    return JS_NewInt32(c, id);
}

static JSValue f_facts(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int pid;
    double st;
    if (argc < 2 || JS_ToInt32(c, &pid, argv[0]) || JS_ToFloat64(c, &st, argv[1]) || pid < 1 || !isfinite(st) || st < 0 || st != floor(st))
        return js_throw_code(c, "sys.badArgument", NULL);
    int id = llm_submit_facts(pid, (unsigned long long)st);
    if (id < 0) return js_throw_code(c, errno == EBUSY ? "llm.busy" : "sys.badArgument", NULL);
    return JS_NewInt32(c, id);
}

static JSValue f_investigate(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int pid;
    double st;
    if (argc < 5 || JS_ToInt32(c, &pid, argv[3]) || JS_ToFloat64(c, &st, argv[4]) || pid < 1 || !isfinite(st) || st < 0 || st != floor(st))
        return js_throw_code(c, "sys.badArgument", NULL);
    const char *model = str_arg(c, argv[0], 48), *system = str_arg(c, argv[1], LLM_SYSTEM_MAX), *user = str_arg(c, argv[2], LLM_USER_MAX);
    int id = model && system && user ? llm_submit_investigate(model, system, user, pid, (unsigned long long)st) : (errno = EINVAL, -1);
    int e = errno;
    if (model) JS_FreeCString(c, model);
    if (system) JS_FreeCString(c, system);
    if (user) JS_FreeCString(c, user);
    if (id < 0) return js_throw_code(c, e == EBUSY ? "llm.busy" : "sys.badArgument", NULL);
    return JS_NewInt32(c, id);
}

static JSValue f_translate(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 3) return js_throw_code(c, "sys.badArgument", NULL);
    const char *model = str_arg(c, argv[0], 48), *system = str_arg(c, argv[1], LLM_SYSTEM_MAX), *text = str_arg(c, argv[2], LLM_TRANSLATE_MAX);
    int id = model && system && text ? llm_submit_translate(model, system, text) : (errno = EINVAL, -1);
    int e = errno;
    if (model) JS_FreeCString(c, model);
    if (system) JS_FreeCString(c, system);
    if (text) JS_FreeCString(c, text);
    if (id < 0) return js_throw_code(c, e == EBUSY ? "llm.busy" : "sys.badArgument", NULL);
    return JS_NewInt32(c, id);
}

static JSValue f_block(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int pid;
    double st;
    if (argc < 2 || JS_ToInt32(c, &pid, argv[0]) || JS_ToFloat64(c, &st, argv[1]) || pid < 2 || !isfinite(st) || st < 0 || st != floor(st))
        return js_throw_code(c, "sys.badArgument", NULL);
    int id = blockwatch_block(pid, (unsigned long long)st, argc > 2 && JS_ToBool(c, argv[2]) > 0);
    if (id < 0) return js_throw_code(c, errno == EBUSY ? "llm.busy" : "sys.badArgument", NULL);
    return JS_NewInt32(c, id);
}

static JSValue f_unblock(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *id = argc > 0 ? str_arg(c, argv[0], 64) : NULL;
    int r = id ? blockwatch_unblock(id) : (errno = EINVAL, -1);
    int e = errno;
    if (id) JS_FreeCString(c, id);
    if (r < 0) return js_throw_code(c, e == EBUSY ? "llm.busy" : "sys.badArgument", NULL);
    return JS_NewInt32(c, r);
}

static JSValue f_block_take(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int id;
    if (argc < 1 || JS_ToInt32(c, &id, argv[0])) return JS_NULL;
    return parsed(c, blockwatch_take(id), "<sentinela.blockTake>");
}

static JSValue f_blocked(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return parsed(c, blockwatch_list_json(), "<sentinela.blocked>");
}

static JSValue f_block_events(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    JSValue v = parsed(c, blockwatch_events_json(), "<sentinela.blockEvents>");
    return JS_IsNull(v) ? JS_NewArray(c) : v;
}

static JSValue f_take(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int id;
    if (argc < 1 || JS_ToInt32(c, &id, argv[0])) return JS_NULL;
    return parsed(c, llm_take(id), "<sentinela.take>");
}

static JSValue f_state(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return parsed(c, llm_state_json(), "<sentinela.state>");
}

static JSValue f_stop(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)c; (void)t; (void)argc; (void)argv;
    llm_stop();
    return JS_TRUE;
}

static JSValue f_backend(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    const char *m = argc >= 1 ? JS_ToCString(c, argv[0]) : NULL;
    int r = m ? llm_set_backend(m) : -1;
    if (m) JS_FreeCString(c, m);
    if (r) return js_throw_code(c, "sys.badArgument", NULL);
    return JS_TRUE;
}

static JSValue w_start(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 4) return js_throw_code(c, "sys.badArgument", NULL);
    const char *model = str_arg(c, argv[0], 48), *action = str_arg(c, argv[1], 16), *text = str_arg(c, argv[3], LLM_WRITE_MAX);
    const char *target = JS_IsString(argv[2]) ? str_arg(c, argv[2], 15) : NULL;
    int id = model && action && text ? llm_submit_write(model, action, target ? target : "", text) : (errno = EINVAL, -1);
    int e = errno;
    if (model) JS_FreeCString(c, model);
    if (action) JS_FreeCString(c, action);
    if (target) JS_FreeCString(c, target);
    if (text) JS_FreeCString(c, text);
    if (id < 0) return js_throw_code(c, e == EBUSY ? "llm.busy" : "sys.badArgument", NULL);
    return JS_NewInt32(c, id);
}

static JSValue w_peek(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int id;
    if (argc < 1 || JS_ToInt32(c, &id, argv[0])) return JS_NULL;
    return parsed(c, llm_peek(id), "<writing.peek>");
}

static JSValue w_cancel(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    int id;
    if (argc < 1 || JS_ToInt32(c, &id, argv[0])) return JS_FALSE;
    llm_cancel(id);
    return JS_TRUE;
}

static JSValue w_serve(JSContext *c, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_FALSE;
    const char *chosen = argc > 1 && JS_IsString(argv[1]) ? str_arg(c, argv[1], 48) : NULL;
    writebus_serve(JS_ToBool(c, argv[0]), chosen);
    if (chosen) JS_FreeCString(c, chosen);
    return JS_TRUE;
}

static const JSCFunctionListEntry wfuncs[] = {
    JS_CFUNC_DEF("serve", 2, w_serve),
    JS_CFUNC_DEF("start", 4, w_start),
    JS_CFUNC_DEF("peek", 1, w_peek),
    JS_CFUNC_DEF("take", 1, f_take),
    JS_CFUNC_DEF("cancel", 1, w_cancel),
};

static const JSCFunctionListEntry funcs[] = {
    JS_CFUNC_DEF("available", 0, f_available),
    JS_CFUNC_DEF("models", 0, f_models),
    JS_CFUNC_DEF("analyze", 3, f_analyze),
    JS_CFUNC_DEF("facts", 2, f_facts),
    JS_CFUNC_DEF("investigate", 5, f_investigate),
    JS_CFUNC_DEF("translate", 3, f_translate),
    JS_CFUNC_DEF("block", 3, f_block),
    JS_CFUNC_DEF("unblock", 1, f_unblock),
    JS_CFUNC_DEF("blockTake", 1, f_block_take),
    JS_CFUNC_DEF("blocked", 0, f_blocked),
    JS_CFUNC_DEF("blockEvents", 0, f_block_events),
    JS_CFUNC_DEF("take", 1, f_take),
    JS_CFUNC_DEF("state", 0, f_state),
    JS_CFUNC_DEF("stop", 0, f_stop),
    JS_CFUNC_DEF("backend", 1, f_backend),
};

void js_sentinela_register(JSContext *c, JSValue lord) {
    static int watching;
    if (!watching) { watching = 1; blockwatch_init(NULL); }        /* the blocklist: its thread runs only while it has entries */
    JSValue o = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, o, funcs, sizeof funcs / sizeof funcs[0]);
    JS_SetPropertyStr(c, lord, "sentinela", o);
    JSValue w = JS_NewObject(c);
    JS_SetPropertyFunctionList(c, w, wfuncs, sizeof wfuncs / sizeof wfuncs[0]);
    JS_SetPropertyStr(c, lord, "writing", w);
}
