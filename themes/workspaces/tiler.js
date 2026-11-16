// tiler.js — Lord Workspaces' static tiling at runtime (the rules are in model/layouts.js). Each activity has a layout
// per display ('free' leaves the windows alone); the windows of the activity in view are laid out in their display's
// work area, each display on its own: a window belongs to one display and never straddles two.
//
// It only writes the SDK's window store (one windows.set for every window that moves, the same fields wm.setFrame
// writes), when something changes: a window opens, closes, is minimized or comes back, the layout or its divider
// changes, the activity in view changes, or the displays do. Nothing runs per frame and nothing polls. A window the
// person moves or resizes by hand (or maximizes, or snaps) leaves its tile and floats until "Tile this window".
//
// Several displays: one activity spans all of them (the SDK's desktops are global), each display with its own layout.
// "Save layout" keeps, per display (by connector name), the layout, its divider and the app of each tile; windows of
// those apps go back to that display and tile when they open again. With fewer displays connected, a gone display's
// apps join the main display's tiles (model/layouts.js foldMissing). A display unplugged: the SDK moves its windows to
// the main display (keepWindowsInWorkAreas) and they join its tiles; plugged back, they return to their saved display.
import { windows, activeDesktop, getApp, launcherIdOf, screen, screenDisplays, displayFor, workAreaOf, createStore } from '@lordos/sdk';
import { tileRects, fitTile, cleanLayout, cleanRatio, orderFor, homeFor, displayKey, savedDisplay, foldMissing } from './model/layouts.js';
import { shownDisplays } from './model/geometry.js';
import { activities, activityOf, updateActivity } from './state.js';

const floating = new Set();          // window ids out of the tiles
const applied = new Map();           // window id → the rect the tiler gave it last
const home = new Map();              // window id → display key
const order = new Map();             // `${desk}|${dkey}` → [window ids] in tile order
// windows whose tile is smaller than their minimum size (the frame says so), and a revision for the UI
export const tiled = createStore({ rev: 0, tight: [], counts: {} });

let applying = false;
let pending = 0;
const sameRect = (a, b) => !!a && !!b && a.x === b.x && a.y === b.y && a.w === b.w && a.h === b.h;

// Theme panels (the SDK's built-in windows) and dialogs never tile.
export function tileable(w) {
  if (!w || w.minimized || w.fullscreen || w.pip || w.maximized) return false;
  const a = w.args || {};
  if (a.dialog || a.parent || a.notice || a.fixed) return false;
  const app = getApp(w.id);
  if (app?.builtin && !String(w.id).startsWith('linux#')) return false;
  return true;
}

export const displaysHere = () => shownDisplays(screenDisplays(), screen.mirror);
export const keyOfDisplay = (d) => { const list = displaysHere(); return displayKey(d, Math.max(0, list.findIndex((x) => x.id === d.id))); };
const mainKey = () => { const list = displaysHere(); const m = list.find((d) => d.main) || list[0]; return m ? keyOfDisplay(m) : '#0'; };

// the layout of one display of an activity (an unset display takes 'free': windows stay where they are)
export function layoutOf(desk, dkey) {
  const l = activityOf(desk).layouts[dkey];
  return l ? { layout: cleanLayout(l.layout), ratio: cleanRatio(l.layout, l.ratio) } : { layout: 'free', ratio: cleanRatio('split') };
}
export function setLayout(desk, dkey, layout, ratio) {
  updateActivity(desk, (m) => {
    const cur = m.layouts[dkey] || {};
    const l = cleanLayout(layout ?? cur.layout);
    return { ...m, layouts: { ...m.layouts, [dkey]: { ...cur, layout: l, ratio: cleanRatio(l, ratio ?? cur.ratio) } } };
  });
  // a new layout takes every window of that display back into the tiles
  if (layout != null) for (const w of windows.get()) if (w.desktop === desk && home.get(w.id) === dkey) floating.delete(w.id);
  schedule();
}

export function setFloating(id, on) {
  if (on) floating.add(id); else { floating.delete(id); applied.delete(id); }
  if (!on) { const w = windows.get().find((x) => x.id === id); if (w) home.set(id, keyOfDisplay(displayFor(wFrame(w)))); }
  schedule();
}
export const isFloating = (id) => floating.has(id);

const wFrame = (w) => ({ x: w.x, y: w.y, w: w.w, h: w.h });

// "Save layout": for each display, the layout, the divider and the apps in tile order.
export function saveLayout(desk) {
  const out = {};
  for (const d of displaysHere()) {
    const dk = keyOfDisplay(d);
    const { layout, ratio } = layoutOf(desk, dk);
    const ids = (order.get(`${desk}|${dk}`) || []).filter((id) => windows.get().some((w) => w.id === id && w.desktop === desk));
    const keys = ids.map((id) => { const w = windows.get().find((x) => x.id === id); return w ? launcherIdOf(w) : null; });
    out[dk] = savedDisplay(layout, ratio, keys);
  }
  updateActivity(desk, (m) => ({ ...m, layouts: { ...m.layouts, ...out } }));
  return out;
}

// The displays or the work areas changed (a monitor plugged or unplugged, the spine collapsed): every tile again,
// and a window's earlier place is no sign of a hand move.
// Each window then finds its display again: its saved one if that display is here (a monitor plugged back), else the
// one it is on now.
export function areasChanged() {
  applied.clear();
  home.clear();
  schedule();
}

export function schedule() {
  if (pending) return;
  pending = setTimeout(() => { pending = 0; retile(); }, 0);
}

export function retile() {
  if (applying) return;
  const desk = activeDesktop.get();
  const all = windows.get();
  const disp = displaysHere();
  if (!disp.length) return;
  const keys = disp.map(keyOfDisplay);
  const mk = mainKey();
  const meta = activityOf(desk);
  const saved = foldMissing(meta.layouts, keys, mk);
  const live = new Set(all.map((w) => w.id));
  for (const id of [...applied.keys()]) if (!live.has(id)) { applied.delete(id); home.delete(id); floating.delete(id); }

  const groups = new Map(keys.map((k) => [k, []]));
  for (const w of all) {
    if (w.desktop !== desk || !tileable(w)) continue;
    const was = applied.get(w.id);
    // moved or resized by hand since the tiler placed it: it floats
    if (was && !sameRect(was, wFrame(w))) { floating.add(w.id); applied.delete(w.id); }
    if (floating.has(w.id)) continue;
    let dk = home.get(w.id);
    if (!dk || !keys.includes(dk)) {
      dk = homeFor(launcherIdOf(w), saved, keys) || keyOfDisplay(displayFor(wFrame(w)));
      if (!keys.includes(dk)) dk = mk;
      home.set(w.id, dk);
    }
    groups.get(dk).push(w.id);
  }

  const moves = new Map();
  const tight = [];
  const counts = {};
  disp.forEach((d, i) => {
    const dk = keys[i];
    const ids = groups.get(dk);
    const k = `${desk}|${dk}`;
    const prev = (order.get(k) || []).filter((id) => ids.includes(id));
    const fresh = ids.filter((id) => !prev.includes(id));
    const keyOf = (id) => { const w = all.find((x) => x.id === id); return w ? launcherIdOf(w) : ''; };
    // the saved apps decide the order of windows the tiler hasn't placed yet; the others keep theirs
    const list = orderFor([...prev, ...fresh], keyOf, prev.length ? null : saved[dk]?.apps);
    order.set(k, list);
    counts[dk] = list.length;
    const { layout, ratio } = layoutOf(desk, dk);
    const area = workAreaOf(d);
    const rects = tileRects(layout, area, list.length, ratio);
    if (!rects) { for (const id of list) applied.delete(id); return; }
    list.forEach((id, j) => {
      const w = all.find((x) => x.id === id);
      const r = fitTile(rects[j], getApp(id)?.minSize, area);
      if (r.tight) tight.push(id);
      const want = { x: r.x, y: r.y, w: r.w, h: r.h };
      applied.set(id, want);
      if (!sameRect(want, wFrame(w))) moves.set(id, want);
    });
  });

  const t = tiled.get();
  if (JSON.stringify(t.tight) !== JSON.stringify(tight) || JSON.stringify(t.counts) !== JSON.stringify(counts)) tiled.set({ rev: t.rev + 1, tight, counts });
  if (!moves.size) return;
  applying = true;
  try {
    windows.set((ws) => ws.map((w) => (moves.has(w.id) ? { ...w, ...moves.get(w.id), maximized: false, snapped: null, restore: null, oskFrame: null } : w)));
  } finally { applying = false; }
}

// The tile order of one display (the frame's splitter needs the count).
export const tileCount = (dk) => tiled.get().counts[dk] || 0;

let wired = false;
export function startTiler() {
  if (wired) return;
  wired = true;
  windows.subscribe(schedule);
  activeDesktop.subscribe(schedule);
  activities.subscribe(schedule);
}
