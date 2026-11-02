/* js_prelude.h — the JS polyfills evaluated before any bundle (console, timers, animation frames).
 * Only ui/js.c includes it, and tests/timer_order_test.c, which checks the timers against the
 * algorithm of 0.8.6's first build (the same callbacks in the same order, the same waits).
 *
 * The real event loop is lord-server's (main.c): it calls __lordTick(now) and sleeps for what it
 * returns (ms until the next timer, -1 with none). Due timers run by their deadline, then by id
 * (the order they were set), each at most once per tick; one set while the tick runs waits for the
 * next tick, as does one cleared before its turn. minAt is the earliest deadline, so a tick with
 * nothing due returns without looking at the timers: it is exact except after clearing the
 * earliest one (stale), and then it is computed again before it is used. */
#ifndef LORD_JS_PRELUDE_H
#define LORD_JS_PRELUDE_H

static const char prelude[] =
    "(function(){\n"
    "const g = globalThis;\n"
    "const fmt = v => typeof v === 'string' ? v : v instanceof Error ? (v.message + '\\n' + (v.stack || '')) :\n"
    "  (() => { try { return JSON.stringify(v); } catch (e) { return String(v); } })();\n"
    "const out = p => (...a) => lord.log(p + a.map(fmt).join(' '));\n"
    "g.console = { log: out(''), info: out(''), debug: out(''), warn: out('[warn] '), error: out('[error] ') };\n"
    "g.performance = { now: () => lord.now() };\n"
    "let nextId = 1; const timers = new Map();\n"
    "let minAt = Infinity, stale = false;\n"
    "const add = (fn, ms, args, repeat) => { const id = nextId++; ms = Math.max(0, +ms || 0);\n"
    "  const at = lord.now() + ms; timers.set(id, { id, fn, args, at, ms, repeat });\n"
    "  if (at < minAt) minAt = at; return id; };\n"
    "g.setTimeout = (fn, ms, ...a) => add(fn, ms, a, false);\n"
    "g.setInterval = (fn, ms, ...a) => add(fn, ms, a, true);\n"
    "g.setImmediate = (fn, ...a) => add(fn, 0, a, false);\n"
    "g.clearTimeout = g.clearInterval = g.clearImmediate = id => {\n"
    "  const t = timers.get(id); if (t === undefined) return;\n"
    "  timers.delete(id); if (t.at <= minAt) stale = true; };\n"
    "const lowest = () => { let m = Infinity; for (const t of timers.values()) if (t.at < m) m = t.at; minAt = m; stale = false; };\n"
    "g.queueMicrotask = fn => { Promise.resolve().then(fn); };\n"
    "let frames = new Map(), frameId = 1;\n"
    "g.requestAnimationFrame = fn => { const id = frameId++; frames.set(id, fn); return id; };\n"
    "g.cancelAnimationFrame = id => { frames.delete(id); };\n"
    "g.__lordTick = now => {\n"
    "  if (stale) lowest();\n"
    "  if (now >= minAt) {\n"
    /* minAt leaves out the due ones from here on: each is fired (a repeating one comes back with
     * its new deadline) or cleared before the loop ends */
    "    const due = []; let m = Infinity;\n"
    "    for (const t of timers.values()) if (t.at <= now) due.push(t); else if (t.at < m) m = t.at;\n"
    "    minAt = m;\n"
    "    due.sort((a, b) => a.at - b.at || a.id - b.id);\n"
    "    for (const t of due) {\n"
    "      if (!timers.has(t.id)) continue;\n"
    "      if (t.repeat) { t.at = now + Math.max(t.ms, 1); if (t.at < minAt) minAt = t.at; } else timers.delete(t.id);\n"
    "      try { t.fn(...t.args); } catch (e) { console.error('timer:', e); }\n"
    "    }\n"
    "  }\n"
    "  if (stale) lowest();\n"
    "  const n = lord.now(); return timers.size ? Math.max(0, minAt - n) : -1;\n"
    "};\n"
    "g.__lordWantsFrame = () => frames.size > 0;\n"
    "g.__lordFrame = now => { const f = frames; frames = new Map();\n"
    "  for (const fn of f.values()) { try { fn(now); } catch (e) { console.error('raf:', e); } } };\n"
    "})();\n";

#endif
