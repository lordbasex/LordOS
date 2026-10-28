/* timer_order_test.c — the timers of src/ui/js_prelude.h against the prelude they replaced.
 *
 * Until 0.8.6, __lordTick looked at every pending timer on every pass of the main loop and built
 * an [id, timer] pair per timer. Now it keeps the earliest deadline and returns early when nothing
 * is due (js_prelude.h). This test runs the same scripts on both, in QuickJS, and requires the
 * same trace, byte for byte: which callbacks run, in which order, with which arguments, what the
 * tick returns (the main loop's wait, %.17g) and what is logged. The scripts are random
 * (seeded): setTimeout, setInterval and setImmediate with equal, zero, negative, fractional,
 * NaN and string delays, clearTimeout of pending, fired, due and unknown ids, timers set and
 * cleared from inside callbacks (also ones already due in that tick), callbacks that throw, and
 * ticks whose `now` is behind the clock lord.now() reads. lord.now() is a deterministic clock that
 * moves on each read, so both sides must also read it the same number of times.
 *
 * `build/timer-order-test bench` times a tick with nothing due and 200 timers pending, both ways. */
#include "ui/js_prelude.h"

#include "quickjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The prelude as of 001633b (src/ui/js.c), the reference. */
#define prelude old_prelude
static const char prelude[] =
    "(function(){\n"
    "const g = globalThis;\n"
    "const fmt = v => typeof v === 'string' ? v : v instanceof Error ? (v.message + '\\n' + (v.stack || '')) :\n"
    "  (() => { try { return JSON.stringify(v); } catch (e) { return String(v); } })();\n"
    "const out = p => (...a) => lord.log(p + a.map(fmt).join(' '));\n"
    "g.console = { log: out(''), info: out(''), debug: out(''), warn: out('[warn] '), error: out('[error] ') };\n"
    "g.performance = { now: () => lord.now() };\n"
    "let nextId = 1; const timers = new Map();\n"
    "const add = (fn, ms, args, repeat) => { const id = nextId++; ms = Math.max(0, +ms || 0);\n"
    "  timers.set(id, { fn, args, at: lord.now() + ms, ms, repeat }); return id; };\n"
    "g.setTimeout = (fn, ms, ...a) => add(fn, ms, a, false);\n"
    "g.setInterval = (fn, ms, ...a) => add(fn, ms, a, true);\n"
    "g.setImmediate = (fn, ...a) => add(fn, 0, a, false);\n"
    "g.clearTimeout = g.clearInterval = g.clearImmediate = id => { timers.delete(id); };\n"
    "g.queueMicrotask = fn => { Promise.resolve().then(fn); };\n"
    "let frames = new Map(), frameId = 1;\n"
    "g.requestAnimationFrame = fn => { const id = frameId++; frames.set(id, fn); return id; };\n"
    "g.cancelAnimationFrame = id => { frames.delete(id); };\n"
    "g.__lordTick = now => {\n"
    "  const due = [];\n"
    "  for (const [id, t] of timers) if (t.at <= now) due.push([id, t]);\n"
    "  due.sort((a, b) => a[1].at - b[1].at || a[0] - b[0]);\n"
    "  for (const [id, t] of due) {\n"
    "    if (!timers.has(id)) continue;\n"
    "    if (t.repeat) t.at = now + Math.max(t.ms, 1); else timers.delete(id);\n"
    "    try { t.fn(...t.args); } catch (e) { console.error('timer:', e); }\n"
    "  }\n"
    "  now = lord.now(); let next = -1;\n"
    "  for (const t of timers.values()) { const d = Math.max(0, t.at - now); if (next < 0 || d < next) next = d; }\n"
    "  return next;\n"
    "};\n"
    "g.__lordWantsFrame = () => frames.size > 0;\n"
    "g.__lordFrame = now => { const f = frames; frames = new Map();\n"
    "  for (const fn of f.values()) { try { fn(now); } catch (e) { console.error('raf:', e); } } };\n"
    "})();\n";
#undef prelude

/* the scripts: the same source on both sides, seeded */
static const char script[] =
    "(function(){\n"
    "let s = (SEED >>> 0) || 1;\n"
    "const rnd = n => { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s % n; };\n"
    "const ids = [], trace = []; globalThis.__trace = trace;\n"
    "const delays = [0, 0, 0, 1, 1, 2, 5, 16, 16, 33, 100, 2.5, 0.25, -5, NaN, '7', undefined, 1e9];\n"
    "let label = 0;\n"
    "const pick = () => ids.length ? ids[rnd(ids.length)] : rnd(5);\n"
    "const body = (L, depth) => (a, b) => {\n"
    "  trace.push('run ' + L + ' ' + a + b);\n"
    "  if (depth < 3) for (let i = rnd(4); i-- > 0;) op(depth + 1);\n"
    "  if (rnd(9) === 0) throw 'boom ' + L;\n"
    "};\n"
    "const op = depth => {\n"
    "  const k = rnd(12), L = label++;\n"
    "  if (k < 4) { const id = setTimeout(body(L, depth), delays[rnd(delays.length)], L, '/t'); ids.push(id); trace.push('timeout ' + L + '=' + id); }\n"
    "  else if (k < 6) {\n"
    "    let left = 1 + rnd(5); const f = body(L, depth);\n"
    "    const id = setInterval((a, b) => { f(a, b); if (--left === 0) { clearInterval(id); trace.push('stop ' + id); } }, delays[rnd(delays.length)], L, '/i');\n"
    "    ids.push(id); trace.push('interval ' + L + '=' + id);\n"
    "  }\n"
    "  else if (k < 7) { const id = setImmediate(body(L, depth), L, '/m'); ids.push(id); trace.push('immediate ' + L + '=' + id); }\n"
    "  else if (k < 10) { const id = pick(); clearTimeout(id); trace.push('clear ' + id); }\n"
    "  else if (k < 11) { const id = pick(); clearInterval(id); clearImmediate(id); trace.push('clear2 ' + id); }\n"
    "  else { clearTimeout('' + pick()); trace.push('clear string'); }\n"
    "};\n"
    "globalThis.__poke = () => { for (let i = rnd(5); i-- > 0;) op(0); };\n"
    "})();\n";

/* the clock lord.now() reads: it moves on every read, by steps that are often 0 (equal deadlines) */
static double clk;
static unsigned crng;
static unsigned rnd(unsigned n) { crng ^= crng << 13; crng ^= crng >> 17; crng ^= crng << 5; return crng % n; }

static char *logbuf;
static size_t loglen, logcap;
static void put(const char *s) {
    size_t n = strlen(s);
    if (loglen + n + 2 > logcap) {
        logcap = (loglen + n + 2) * 2;
        logbuf = realloc(logbuf, logcap);
        if (!logbuf) { perror("realloc"); exit(2); }
    }
    memcpy(logbuf + loglen, s, n);
    loglen += n;
    logbuf[loglen++] = '\n';
    logbuf[loglen] = 0;
}

static JSValue f_now(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    static const double steps[] = { 0, 0, 0, 0.25, 1, 4, 16.666666666666668 };
    double t = clk;
    clk += steps[rnd(sizeof steps / sizeof *steps)];
    return JS_NewFloat64(c, t);
}

static JSValue f_log(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *s = argc > 0 ? JS_ToCString(c, argv[0]) : NULL;
    put(s ? s : "(log)");
    if (s) JS_FreeCString(c, s);
    return JS_UNDEFINED;
}

static int eval(JSContext *c, const char *src, const char *name) {
    JSValue r = JS_Eval(c, src, strlen(src), name, JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(r);
    if (!ok) {
        JSValue e = JS_GetException(c);
        const char *m = JS_ToCString(c, e);
        fprintf(stderr, "%s: %s\n", name, m ? m : "?");
        if (m) JS_FreeCString(c, m);
        JS_FreeValue(c, e);
    }
    JS_FreeValue(c, r);
    return ok;
}

static void drain(JSRuntime *rt) {
    JSContext *c;
    while (JS_ExecutePendingJob(rt, &c) > 0) {}
}

static JSValue call(JSContext *c, const char *name, int argc, JSValueConst *argv) {
    JSValue g = JS_GetGlobalObject(c), f = JS_GetPropertyStr(c, g, name);
    JSValue r = JS_Call(c, f, g, argc, argv);
    JS_FreeValue(c, f);
    JS_FreeValue(c, g);
    return r;
}

static JSContext *new_context(JSRuntime *rt, const char *prel) {
    JSContext *c = JS_NewContext(rt);
    JSValue g = JS_GetGlobalObject(c), lord = JS_NewObject(c);
    JS_SetPropertyStr(c, lord, "now", JS_NewCFunction(c, f_now, "now", 0));
    JS_SetPropertyStr(c, lord, "log", JS_NewCFunction(c, f_log, "log", 1));
    JS_SetPropertyStr(c, g, "lord", lord);
    JS_FreeValue(c, g);
    if (!eval(c, prel, "<prelude>")) exit(2);
    return c;
}

/* One run: the trace (C log, then the script's own) in logbuf. */
static long fires;
static void run(const char *prel, unsigned seed, int steps) {
    loglen = 0;
    if (logbuf) logbuf[0] = 0;
    clk = 1000.125;
    crng = seed * 2654435761u + 1;
    JSRuntime *rt = JS_NewRuntime();
    JSContext *c = new_context(rt, prel);
    char src[sizeof script + 64];
    snprintf(src, sizeof src, "const SEED = %u;\n%s", seed, script);
    if (!eval(c, src, "<script>")) exit(2);
    static const double adv[] = { 0, 0, 0.25, 1, 2, 7, 16.666666666666668, 50, 200, -3 };
    double now = clk;
    for (int i = 0; i < steps; i++) {
        char line[96];
        if (rnd(3) == 0) {
            JS_FreeValue(c, call(c, "__poke", 0, NULL));
            put("poke");
        } else {
            now += adv[rnd(sizeof adv / sizeof *adv)];
            if (rnd(4) == 0 && now < clk) now = clk;        /* catch up with the clock now and then */
            JSValue arg = JS_NewFloat64(c, now);
            JSValue r = call(c, "__lordTick", 1, &arg);
            double next = -2;
            if (JS_IsException(r)) put("tick threw");
            else JS_ToFloat64(c, &next, r);
            JS_FreeValue(c, r);
            snprintf(line, sizeof line, "tick %.17g -> %.17g (clock %.17g)", now, next, clk);
            put(line);
        }
        drain(rt);
    }
    JSValue g = JS_GetGlobalObject(c), tr = JS_GetPropertyStr(c, g, "__trace");
    JSValue js = JS_JSONStringify(c, tr, JS_UNDEFINED, JS_NewInt32(c, 0));
    const char *s = JS_ToCString(c, js);
    put(s ? s : "(no trace)");
    for (const char *p = s; p && (p = strstr(p, "\"run ")); p++) fires++;
    if (s) JS_FreeCString(c, s);
    JS_FreeValue(c, js);
    JS_FreeValue(c, tr);
    JS_FreeValue(c, g);
    JS_FreeContext(c);
    JS_FreeRuntime(rt);
}

static double ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* a tick with nothing due among 200 pending timers (a React app, the clock, polls) */
static double bench(const char *prel) {
    JSRuntime *rt = JS_NewRuntime();
    JSContext *c = new_context(rt, prel);
    if (!eval(c, "for (let i = 0; i < 200; i++) setTimeout(() => {}, 1e7 + i);", "<bench>")) exit(2);
    const int n = 20000;
    double t0 = ms();
    for (int i = 0; i < n; i++) {
        JSValue arg = JS_NewFloat64(c, 1000 + i * 0.001);
        JS_FreeValue(c, call(c, "__lordTick", 1, &arg));
        drain(rt);
    }
    double per = (ms() - t0) / n * 1000;
    JS_FreeContext(c);
    JS_FreeRuntime(rt);
    return per;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "bench")) {
        printf("timer-order-test bench: a tick with 200 timers pending, nothing due: before %.2f us, now %.2f us\n",
               bench(old_prelude), bench(prelude));
        return 0;
    }
    const unsigned seeds = 400;
    const int steps = 300;
    int fails = 0;
    long bytes = 0;
    for (unsigned seed = 1; seed <= seeds; seed++) {
        run(old_prelude, seed, steps);
        char *want = strdup(logbuf ? logbuf : "");
        long f0 = fires;
        run(prelude, seed, steps);
        fires = f0;                                 /* counted once */
        if (!want || strcmp(want, logbuf ? logbuf : "")) {
            if (fails++ < 3) {
                size_t i = 0;
                while (want && want[i] && want[i] == logbuf[i]) i++;
                size_t from = i > 200 ? i - 200 : 0;
                printf("✗ seed %u differs at byte %zu:\n  before: %.300s\n  now:    %.300s\n", seed, i, want + from, logbuf + from);
            }
        }
        bytes += (long)loglen;
        free(want);
    }
    printf("timer-order-test: %u random scripts, %ld callbacks run, %ld bytes of trace compared, %d differ\n",
           seeds, fires, bytes, fails);
    return fails ? 1 : 0;
}
