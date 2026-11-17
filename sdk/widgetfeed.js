// widgetfeed.js — the desk widgets' data feed and their visibility (the pure parts are in widgetfeedmodel.js;
// docs/WORKSPACES.md, "Widget data").
//
// useFeed(key, read, ms) gives a widget a piece of data read every ms while the widget can be seen: one read per key
// however many widgets show it, every source's ticks aligned so the ones due together render in one frame, nothing
// read for a widget behind a window (wholly inside a window frame on the desktop in view) and nothing at all while
// nobody sees the screen (lord.system.seen(): the displays off, the session locked). A widget that shows again gets
// a fresh read at once and keeps its last value until then.
import { createContext, useContext, useEffect, useRef, useState } from 'react';
import { createFeed, createVisibility, coveredBy } from './widgetfeedmodel.js';
import { sameValue } from './samevalue.js';
import { windows, activeDesktop, overviewShowing, wm } from './windows.js';
import { windowHidden } from './desktopmodel.js';

const seen = () => {
  try { return typeof lord === 'undefined' || !lord.system?.seen || lord.system.seen(); } catch (e) { return true; }
};
const setTimer = (fn, ms) => setTimeout(fn, ms);
const clearTimer = (id) => clearTimeout(id);

export const feed = createFeed({ now: () => Date.now(), setTimer, clearTimer, seen, same: sameValue });
export const widgetVisibility = createVisibility({ setTimer, clearTimer, hideMs: 1000 });

// The widget a body belongs to (WidgetCard provides it): its feeds follow that widget's visibility. Outside a
// widget (null) a feed is always visible.
export const WidgetIdContext = createContext(null);

// ---- visibility ----
const rects = new Map();          // widget id → its rectangle on screen (deskwidgets.jsx publishes them)
let wired = false, soonA = null, soonB = null;

function frames() {
  const out = [];
  if (typeof lord === 'undefined' || !lord.getLayout) return out;
  const act = activeDesktop.get();
  for (const w of windows.get()) {
    if (windowHidden(w, act)) continue;
    const n = wm.frameNode ? wm.frameNode(w.id) : 0;
    if (!n) continue;
    try {
      const l = lord.getLayout(n);
      if (l && l.length === 4) out.push({ x: l[0], y: l[1], w: l[2], h: l[3] });
    } catch (e) { /* a frame being torn down */ }
  }
  return out;
}

function evaluate() {
  const f = overviewShowing.get() ? [] : frames();       // the overview shows the desk: every widget counts as seen
  for (const [id, r] of rects) widgetVisibility.set(id, !coveredBy(r, f));
}

// windows change in bursts (a drag, an animation): look shortly after (the layout of that frame done) and once more
// when it has settled
function soon() {
  if (soonA == null) soonA = setTimeout(() => { soonA = null; evaluate(); }, 150);
  if (soonB != null) clearTimeout(soonB);
  soonB = setTimeout(() => { soonB = null; evaluate(); }, 800);
}

export function setWidgetRects(list) {
  if (!wired) {
    wired = true;
    windows.subscribe(soon);
    activeDesktop.subscribe(soon);
    overviewShowing.subscribe(soon);
  }
  const ids = new Set();
  for (const r of list) { ids.add(r.id); rects.set(r.id, { x: r.x, y: r.y, w: r.w, h: r.h }); }
  for (const id of [...rects.keys()]) if (!ids.has(id)) { rects.delete(id); widgetVisibility.forget(id); }
  soon();
}

// ---- the hook ----
let anon = 0;
// key: a shared source ('cpu', 'claude'…; read must then be the same function for every widget), or null for one of
// this widget's own (read may close over its props; deps restart it). read(state) may keep counters in state.
export function useFeed(key, read, ms, deps = []) {
  const wid = useContext(WidgetIdContext);
  const own = useRef(null);
  if (!own.current) own.current = `own:${++anon}`;
  const k = key || own.current;
  const [value, setValue] = useState(() => feed.peek(k, read));
  const sub = useRef(null);
  useEffect(() => {
    const s = feed.subscribe(k, read, ms, wid == null ? true : widgetVisibility.visible(wid), (v) => setValue(() => v));
    sub.current = s;
    const off = wid == null ? null : widgetVisibility.listen(wid, (vis) => s.setVisible(vis));
    return () => { if (off) off(); s.close(); sub.current = null; };
  }, [k, ms, wid, ...deps]);
  if (sub.current && !key) sub.current.setRead(read);   // its own source reads with this render's props
  return value;
}
