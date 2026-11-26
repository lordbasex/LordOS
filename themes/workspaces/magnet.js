// magnet.js — Workspaces' magnet at runtime (the rules are in model/magnet.js). In a display with the free layout,
// a window dragged to the left or right edge takes that half (the SDK's snapping) and the other half is filled: by the
// only other window of that display, or by the one the person picks in the chooser (`assist`). The two windows are
// then a pair: resizing one of them by its shared edge moves the other's edge to it, on release (the SDK commits a
// resize once, at the end). Moving either one, minimizing, maximizing or closing it ends the pair.
//
// It reads the SDK's window store on each change and writes it only for a window that follows (one windows.set);
// nothing runs per frame or polls. Its own write is ignored when it comes back, so it can't loop.
import { windows, activeDesktop, displayFor, workAreaOf, getApp, snapRect, createStore } from '@lordos/sdk';
import { assistFor, follow, other, SIDES } from './model/magnet.js';
import { tileable, layoutOf, keyOfDisplay, windowLocked } from './tiler.js';

// { id, side, rect, ids } while the chooser offers windows for the other half of window `id`, else null
export const assist = createStore(null);
let pairs = [];                      // [{ left, right, desk }]
let before = {};                     // id → the frame last seen
let writing = false;

const frameOf = (w) => ({ x: w.x, y: w.y, w: w.w, h: w.h, minimized: !!w.minimized, maximized: !!w.maximized });
const minW = (id) => { const m = getApp(id)?.minSize; return Array.isArray(m) ? Math.max(160, +m[0] || 0) : 160; };
const freeOn = (desk, w) => layoutOf(desk, keyOfDisplay(displayFor(w))).layout === 'free';

// Window `id` into `side` of `area` (the SDK's snapped state, so dragging it out gives its size back).
export function snapInto(id, side, area) {
  const r = snapRect(side, area);
  writing = true;
  try {
    windows.set((ws) => ws.map((w) => {
      if (w.id !== id) return w;
      const restore = w.snapped && w.restore ? w.restore : { x: w.x, y: w.y, w: w.w, h: w.h };
      return { ...w, ...r, w: Math.max(r.w, Math.min(minW(id), area.w)), maximized: false, minimized: false, snapped: side, restore, desktop: activeDesktop.get() };
    }));
  } finally { writing = false; }
}

const unpair = (id) => { pairs = pairs.filter((p) => p.left !== id && p.right !== id); };
// a pair made in this change is judged from the next one on (its windows' frames before were from before the snap)
function pair(a, side, b, desk) {
  unpair(a); unpair(b);
  const q = side === 'left' ? { left: a, right: b, desk } : { left: b, right: a, desk };
  pairs.push(q);
  return q;
}

// The person picked window `pick` in the chooser.
export function choose(pick) {
  const a = assist.get();
  assist.set(null);
  if (!a || !pick) return;
  snapInto(pick, a.side, a.area);
  pair(a.id, other(a.side), pick, activeDesktop.get());
  remember();
}

const remember = () => { before = {}; for (const w of windows.get()) before[w.id] = frameOf(w); };

function changed(ws) {
  const desk = activeDesktop.get();
  const prev = before;
  const now = {};
  for (const w of ws) now[w.id] = frameOf(w);
  const made = new Set();
  const a = assist.get();
  if (a) {
    const me = ws.find((w) => w.id === a.id);
    if (!me || me.snapped !== other(a.side) || me.desktop !== desk) assist.set(null);
  }
  // a window just snapped to a half, by hand, in a free layout: fill or offer the other half
  for (const w of ws) {
    if (!SIDES.includes(w.snapped) || w.desktop !== desk || !prev[w.id] || windowLocked(w)) continue;
    const p = prev[w.id];
    if (p.x === w.x && p.y === w.y && p.w === w.w && p.h === w.h) continue;
    if (pairs.some((q) => q.left === w.id || q.right === w.id)) continue;
    if (!freeOn(desk, w)) continue;
    const area = workAreaOf(displayFor(w));
    const pool = ws.filter((x) => x.desktop === desk && !x.minimized && (x.id === w.id || tileable(x)));
    const next = assistFor(pool, w.id, area);
    if (!next) continue;
    if (next.pair) made.add(pair(w.id, w.snapped, next.pair, desk));
    else if (next.fill) { snapInto(next.fill, other(w.snapped), area); made.add(pair(w.id, w.snapped, next.fill, desk)); }
    else assist.set({ id: w.id, side: other(w.snapped), area, rect: snapRect(other(w.snapped), area), ids: next.choose });
  }
  // the pairs: one side resized by the shared edge moves the other
  const after = {};
  for (const w of windows.get()) after[w.id] = frameOf(w);
  const keep = [];
  const patches = [];
  for (const q of pairs) {
    if (q.desk !== desk || made.has(q)) { keep.push(q); continue; }
    const lw = windows.get().find((x) => x.id === q.left), rw = windows.get().find((x) => x.id === q.right);
    if (!lw || !rw || lw.desktop !== desk || rw.desktop !== desk) continue;
    // a locked display: the pair stays, nothing follows
    if (windowLocked(lw) || windowLocked(rw)) { keep.push(q); continue; }
    const r = follow(q, prev, after, workAreaOf(displayFor(lw)), minW);
    if (r?.break) continue;
    keep.push(q);
    if (r?.patch) patches.push(r.patch);
  }
  pairs = keep;
  if (patches.length) {
    writing = true;
    try {
      windows.set((list) => list.map((w) => { const p = patches.find((x) => x.id === w.id); return p ? { ...w, x: p.x, w: p.w } : w; }));
    } finally { writing = false; }
  }
  remember();
}

let wired = false;
export function startMagnet() {
  if (wired) return;
  wired = true;
  remember();
  windows.subscribe(() => { if (!writing) changed(windows.get()); });
  activeDesktop.subscribe(() => { assist.set(null); remember(); });
}
