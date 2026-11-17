// widgetfeedmodel.js — the desk widgets' data feed, pure (no lord.*, no React; tested by
// server/tests/widget_feed_test.mjs). docs/WORKSPACES.md, "Widget data".
//
// Each piece of data (the CPU usage, the network rates, Claude Code's sessions…) is one source, read once however
// many widgets show it. A widget subscribes with how often it wants it; a source runs at the shortest interval among
// its *visible* subscribers and not at all when none is visible. Every source's ticks fall on multiples of its
// interval since the epoch, so all that are due in the same second are read together and their widgets re-render in
// one frame (one wake-up instead of one per widget). A read that returns the same data notifies nobody.

// The first multiple of ms after t.
export const alignedDue = (t, ms) => Math.floor(t / ms) * ms + ms;

// Whether a widget's rectangle is wholly inside one of the window frames in front of it (a window ending a few
// pixels short of an edge still hides it; two windows each covering half count as not covering: it keeps updating).
export function coveredBy(r, frames, slack = 2) {
  if (!r || !(r.w > 0) || !(r.h > 0)) return false;
  for (const f of frames || []) {
    if (!f || !(f.w > 0) || !(f.h > 0)) continue;
    if (r.x >= f.x - slack && r.y >= f.y - slack && r.x + r.w <= f.x + f.w + slack && r.y + r.h <= f.y + f.h + slack) return true;
  }
  return false;
}

// now(): ms; setTimer(fn, ms) / clearTimer(id): one timer for the whole feed; seen(): false while nobody sees the
// screen (displays off, session locked), when due sources skip their read; same(a, b): equal data.
export function createFeed({ now, setTimer, clearTimer, seen = () => true, same = Object.is, slack = 30 }) {
  const sources = new Map();
  let timer = null, timerAt = 0;

  const msOf = (src) => {
    let m = 0;
    for (const s of src.subs) if (s.visible && s.ms > 0 && (!m || s.ms < m)) m = s.ms;
    return m;
  };
  const read = (src, t) => {
    let v = null;
    try { v = src.read(src.state); } catch (e) { v = null; }
    src.at = t;
    src.reads++;
    if (!src.has || !same(src.value, v)) src.value = v;
    src.has = true;
  };
  const tell = (s, src) => {
    if (s.visible && s.last !== src.value) { s.last = src.value; s.cb(src.value); }
  };

  function plan() {
    const t = now();
    let next = Infinity;
    for (const src of sources.values()) {
      const ms = msOf(src);
      if (!ms) { src.ms = 0; src.due = 0; continue; }
      if (src.ms !== ms || !src.due) { src.ms = ms; src.due = alignedDue(t, ms); }
      if (src.due < next) next = src.due;
    }
    if (next === Infinity) {
      if (timer != null) { clearTimer(timer); timer = null; }
      return;
    }
    if (timer != null && timerAt === next) return;
    if (timer != null) clearTimer(timer);
    timerAt = next;
    timer = setTimer(fire, Math.max(0, next - t));
  }

  function fire() {
    timer = null;
    const t = now(), look = seen();
    for (const src of sources.values()) {
      if (!src.due || src.due > t + slack) continue;
      src.due = alignedDue(t, src.ms);
      if (!look) continue;                       // nobody sees the screen: the read waits for the next tick
      read(src, t);
      for (const s of src.subs) tell(s, src);
    }
    plan();
  }

  // a visible subscriber whose source is older than it asks gets a fresh read now (stale-while-revalidate: until
  // then the widget keeps showing the last value it had)
  function freshen(src, s) {
    const t = now();
    if (!src.has || (s.ms > 0 && t - src.at >= s.ms)) read(src, t);
    tell(s, src);
  }

  function sourceOf(key, readFn) {
    let src = sources.get(key);
    if (!src) {
      src = { key, read: readFn, state: {}, subs: new Set(), value: null, has: false, at: 0, due: 0, ms: 0, reads: 0 };
      sources.set(key, src);
    } else if (readFn) src.read = readFn;
    return src;
  }

  return {
    // the source's value, read once if it never was (a widget's first render)
    peek(key, readFn) {
      const src = sourceOf(key, readFn);
      if (!src.has) read(src, now());
      return src.value;
    },
    subscribe(key, readFn, ms, visible, cb) {
      const src = sourceOf(key, readFn);
      const s = { ms, visible: !!visible, cb, last: src.has ? src.value : undefined };
      src.subs.add(s);
      if (s.visible) freshen(src, s);
      plan();
      return {
        setRead(fn) { if (fn) src.read = fn; },
        setVisible(v) {
          v = !!v;
          if (v === s.visible) return;
          s.visible = v;
          if (v) freshen(src, s);
          plan();
        },
        close() {
          src.subs.delete(s);
          if (!src.subs.size) sources.delete(key);   // nobody shows it: its state and value go
          plan();
        },
      };
    },
    // for the tests and the diagnostics: what runs and how often
    active: () => [...sources.values()].filter((s) => s.ms > 0).map((s) => ({ key: s.key, ms: s.ms, reads: s.reads })),
    stats: (key) => { const s = sources.get(key); return s ? { ms: s.ms, reads: s.reads, subs: s.subs.size, has: s.has } : null; },
  };
}

// The widgets' visibility with a grace period: shown at once, hidden only after it stayed hidden for hideMs (a window
// dragged across the desk doesn't stop and restart every source under it). now/setTimer/clearTimer as above.
export function createVisibility({ setTimer, clearTimer, hideMs = 1000 }) {
  const state = new Map();        // id → { visible, want, timer, cbs:Set }
  const entry = (id) => {
    let e = state.get(id);
    if (!e) { e = { visible: true, want: true, timer: null, cbs: new Set() }; state.set(id, e); }
    return e;
  };
  const apply = (e, v) => {
    if (e.visible === v) return;
    e.visible = v;
    for (const cb of [...e.cbs]) cb(v);
  };
  return {
    visible: (id) => (state.has(id) ? state.get(id).visible : true),
    set(id, want) {
      const e = entry(id);
      e.want = !!want;
      if (e.want) {
        if (e.timer != null) { clearTimer(e.timer); e.timer = null; }
        apply(e, true);
      } else if (e.visible && e.timer == null) {
        e.timer = setTimer(() => { e.timer = null; if (!e.want) apply(e, false); }, hideMs);
      }
    },
    listen(id, cb) {
      const e = entry(id);
      e.cbs.add(cb);
      return () => { e.cbs.delete(cb); };
    },
    forget(id) {
      const e = state.get(id);
      if (!e) return;
      if (e.timer != null) clearTimer(e.timer);
      state.delete(id);
    },
    ids: () => [...state.keys()],
  };
}
