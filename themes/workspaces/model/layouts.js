// layouts.js — Workspaces' static tiling, as plain data (tested by server/tests/workspaces_model_test.mjs). The
// runtime part is ../tiler.js, which moves windows with the SDK's own window store; nothing here moves anything.
//
//   LAYOUTS                          'free' (windows where the person puts them), 'single', 'halves' (two columns),
//                                    'split' (a main tile and a column), 'three' (three columns) and 'grid'
//   tileRects(layout, area, n, ratio)    n rectangles inside the work area (null for 'free')
//   fitTile(r, min, area)            a tile too small for a window's minimum size: the window keeps its minimum, at the
//                                    tile's corner, inside the area (it overlaps its neighbour instead of being cut)
//   splitterAt / ratioFrom           the draggable divider of 'halves', 'split' and 'three' and the ratio a drop gives
//   orderFor(ids, keyOf, saved)      the tile order: a saved layout's apps first, in their saved order
//   homeFor(key, saved, connected)   the display a saved layout put an app's window on, if that display is here
//   displayKey(d, i)                 a display's stable name (its connector), for saved layouts
//   displayKeys(list)                every display's key, never two alike
//   judgeFrame(entry, frame, now)    what a window's change since the tiler placed it means: the same, the app's
//                                    answer to the size it was given, or a move by hand
//   underBudget(times, now)          whether one more move of a window fits its budget (MOVES_PER_SEC)
export const LAYOUTS = ['free', 'single', 'halves', 'split', 'three', 'grid'];
export const TILE_GAP = 10;
export const RATIO_MIN = 0.25;
export const RATIO_MAX = 0.75;
// a side column holds at most this many rows; more windows share the last one
export const SIDE_ROWS = 3;

export const cleanLayout = (l) => (LAYOUTS.includes(l) ? l : 'free');
export const defaultRatio = (layout) => (layout === 'three' ? 1 / 3 : layout === 'halves' ? 1 / 2 : 2 / 3);
const DIVIDED = ['halves', 'split', 'three'];
export function cleanRatio(layout, r) {
  const v = r == null || r === '' ? NaN : Number(r);
  if (!Number.isFinite(v)) return defaultRatio(layout);
  return Math.min(RATIO_MAX, Math.max(RATIO_MIN, v));
}

const R = (x, y, w, h) => ({ x: Math.round(x), y: Math.round(y), w: Math.max(1, Math.round(w)), h: Math.max(1, Math.round(h)) });

// k rows stacked in a column rect, gap between them
function rows(c, k, gap) {
  const h = (c.h - gap * (k - 1)) / k;
  return Array.from({ length: k }, (_, i) => R(c.x, c.y + i * (h + gap), c.w, h));
}
// k columns side by side
function cols(c, widths, gap) {
  const out = [];
  let x = c.x;
  for (const w of widths) { out.push(R(x, c.y, w, c.h)); x += w + gap; }
  return out;
}
// n windows in `cells`: the extra ones share the last cell (stacked, the front one on top)
const fill = (cells, n) => Array.from({ length: n }, (_, i) => cells[Math.min(i, cells.length - 1)]);

export function tileRects(layout, area, n, ratio, gap = TILE_GAP) {
  if (!area || n <= 0) return [];
  const l = cleanLayout(layout);
  if (l === 'free') return null;
  const a = R(area.x, area.y, area.w, area.h);
  if (l === 'single' || n === 1) return fill([a], n);
  if (l === 'halves') {
    // two columns; more windows stack in rows, the left column taking the odd one
    const r = cleanRatio('halves', ratio);
    const lw = (a.w - gap) * r;
    const [left, right] = cols(a, [lw, a.w - gap - lw], gap);
    const nl = Math.min(Math.ceil(n / 2), SIDE_ROWS), nr = Math.min(n - Math.ceil(n / 2), SIDE_ROWS);
    const L = rows(left, nl, gap), Rr = rows(right, nr, gap);
    // tile order alternates sides: 1 left, 2 right, 3 left…; past SIDE_ROWS a side's windows share its last row
    return Array.from({ length: n }, (_, i) => (i % 2 === 0 ? L[Math.min(i / 2, nl - 1)] : Rr[Math.min((i - 1) / 2, nr - 1)]));
  }
  if (l === 'split') {
    const r = cleanRatio('split', ratio);
    const mw = (a.w - gap) * r;
    const [main, side] = cols(a, [mw, a.w - gap - mw], gap);
    return fill([main, ...rows(side, Math.min(n - 1, SIDE_ROWS), gap)], n);
  }
  if (l === 'three') {
    const r = cleanRatio('three', ratio);
    const inner = a.w - 2 * gap;
    const first = inner * r;
    if (n === 2) return cols(a, [(a.w - gap) * r, (a.w - gap) * (1 - r)], gap);
    return fill(cols(a, [first, (inner - first) / 2, (inner - first) / 2], gap), n);
  }
  // grid: as square as possible; a short last row stretches its cells
  const c = Math.ceil(Math.sqrt(n));
  const rs = Math.ceil(n / c);
  const h = (a.h - gap * (rs - 1)) / rs;
  const out = [];
  for (let row = 0; row < rs; row++) {
    const k = Math.min(c, n - row * c);
    const w = (a.w - gap * (k - 1)) / k;
    for (let i = 0; i < k; i++) out.push(R(a.x + i * (w + gap), a.y + row * (h + gap), w, h));
  }
  return out;
}

// min: [w, h] (an app's minSize) or null
export function fitTile(r, min, area) {
  const mw = Array.isArray(min) ? +min[0] || 0 : 0, mh = Array.isArray(min) ? +min[1] || 0 : 0;
  const w = Math.max(r.w, mw), h = Math.max(r.h, mh);
  if (w === r.w && h === r.h) return { ...r, tight: false };
  const x = Math.max(area.x, Math.min(r.x, area.x + area.w - w));
  const y = Math.max(area.y, Math.min(r.y, area.y + area.h - h));
  return { x: Math.round(x), y: Math.round(y), w: Math.round(w), h: Math.round(h), tight: true };
}

// The divider's x, or null where there is none (one window, 'single', 'grid', 'free').
export function splitterAt(layout, area, ratio, n, gap = TILE_GAP) {
  if (!area || n < 2 || !DIVIDED.includes(layout)) return null;
  const r = cleanRatio(layout, ratio);
  const inner = layout === 'three' && n > 2 ? area.w - 2 * gap : area.w - gap;
  return Math.round(area.x + inner * r + gap / 2);
}
export function ratioFrom(layout, area, x, n, gap = TILE_GAP) {
  const inner = layout === 'three' && n > 2 ? area.w - 2 * gap : area.w - gap;
  if (inner <= 0) return defaultRatio(layout);
  return cleanRatio(layout, (x - area.x - gap / 2) / inner);
}

// ids in their current tile order; keyOf(id) the app's key; saved: the app keys of a saved layout, in tile order.
// Each saved key takes the first window of that app (once); the windows left keep their order after them.
export function orderFor(ids, keyOf, saved) {
  if (!Array.isArray(saved) || !saved.length) return [...ids];
  const left = [...ids];
  const out = [];
  for (const k of saved) {
    const i = left.findIndex((id) => keyOf(id) === k);
    if (i >= 0) out.push(left.splice(i, 1)[0]);
  }
  return [...out, ...left];
}

// saved: { [displayKey]: { apps: [...] } }; connected: the keys of the displays here. The first display whose saved
// apps name this app, if that display is connected; else null (the window stays on the display it is on).
export function homeFor(key, saved, connected) {
  if (!saved || typeof saved !== 'object' || !key) return null;
  for (const [dk, v] of Object.entries(saved)) {
    if (Array.isArray(v?.apps) && v.apps.includes(key) && connected.includes(dk)) return dk;
  }
  return null;
}

// A display's stable name for saved layouts: its connector (DP-1, HDMI-A-1…), never its model (three monitors of
// the same model share it), or its place when it has none.
export const displayKey = (d, i = 0) => (d && typeof d.connector === 'string' && d.connector ? d.connector : `#${i}`);
// The keys of a list of displays, each one different (a repeated one takes its place: '#2').
export function displayKeys(list) {
  const seen = new Set();
  return (list || []).map((d, i) => {
    let k = displayKey(d, i);
    if (seen.has(k)) k = `#${i}`;
    seen.add(k);
    return k;
  });
}

// A window the tiler placed, compared with where it is now. entry: { tile, got, t } (the rect the tiler asked for,
// the frame it took as the answer, when it asked). 'same': as it was left. 'answer': the app gave itself another size
// at the tile's corner shortly after it was asked (a minimum size, a terminal's character grid): kept as the answer,
// the window stays in its tile and is never asked again for that tile. 'hand': moved or resized by the person, or
// changed later: the window floats. A window is asked again only when its tile changes, so an app that never takes
// the size it is given can't make the tiler loop.
export const ANSWER_MS = 1500;
export function judgeFrame(entry, frame, now, answerMs = ANSWER_MS) {
  if (!entry || !frame) return 'hand';
  const g = entry.got || entry.tile;
  if (g && g.x === frame.x && g.y === frame.y && g.w === frame.w && g.h === frame.h) return 'same';
  const t = entry.tile;
  if (t && frame.x === t.x && frame.y === t.y && now - entry.t >= 0 && now - entry.t < answerMs) return 'answer';
  return 'hand';
}

// At most MOVES_PER_SEC moves of one window per second by the tiler, whatever asks for them. times: when it was moved
// (ms); returns the list with this move added, or null if it doesn't fit (the move waits).
export const MOVES_PER_SEC = 10;
export function underBudget(times, now, max = MOVES_PER_SEC, per = 1000) {
  const recent = (times || []).filter((t) => now - t < per && now >= t);
  if (recent.length >= max) return null;
  return [...recent, now];
}

// What "Save layout" keeps for one display: the layout, its ratio and the app of each tile, in tile order.
export function savedDisplay(layout, ratio, keys) {
  return { layout: cleanLayout(layout), ratio: cleanRatio(layout, ratio), apps: keys.filter((k) => typeof k === 'string' && k).slice(0, 12) };
}

// A saved layout for displays that are gone: their apps join the main display's list (after its own), so with fewer
// monitors the windows still find a tile.
export function foldMissing(saved, connected, mainKey) {
  const out = {};
  const extra = [];
  for (const [dk, v] of Object.entries(saved || {})) {
    if (connected.includes(dk)) out[dk] = v;
    else if (Array.isArray(v?.apps)) extra.push(...v.apps);
  }
  if (extra.length && mainKey) {
    const m = out[mainKey] || { layout: 'split', ratio: defaultRatio('split'), apps: [] };
    out[mainKey] = { ...m, apps: [...(m.apps || []), ...extra.filter((k) => !(m.apps || []).includes(k))] };
  }
  return out;
}

// Tiles as slots (0.8.8.10): each tile is a stack of windows with at most one shown. A window minimized while tiled
// keeps its slot, so the others stay where they are and the slot shows as a free space; a window that opens takes the
// first free slot; a window brought back takes its slot again, and the one shown there meanwhile is minimized (they
// swap). prev: the stacks of the last pass ([[ids]], the shown one last); live: the display's windows in their order
// (shown and minimized); isMin(id); back: ids just brought back from minimized. A minimized window the tiler never
// placed gets no slot. Returns { stacks, swapOut } (swapOut: windows to minimize).
// min: the layout's slots, kept even empty (Two halves always shows two: an empty one is a free space); max: at most that
// many (One at a time: one slot, the others minimized behind the window shown).
export const SLOTS = { single: [1, 1], halves: [2, Infinity], three: [3, Infinity], split: [3, Infinity], grid: [4, Infinity] };
export const slotLimits = (layout) => SLOTS[layout] || [0, Infinity];
// empty slots beyond `min` go, from the end (a slot left by a window that moved away)
export function trimSlots(stacks, min) {
  const out = [...stacks];
  for (let i = out.length - 1; i >= 0 && out.length > min; i--) if (!out[i].length) out.splice(i, 1);
  return out;
}
export function slotStacks(prev, live, isMin, back = new Set(), { min = 0, max = Infinity } = {}) {
  const here = new Set(live);
  let stacks = (prev || []).map((s) => s.filter((id) => here.has(id)));
  // empty slots beyond the layout's own count go (from the end), and slots beyond its maximum join the last one
  stacks = trimSlots(stacks, min);
  while (stacks.length > Math.max(1, max)) { const extra = stacks.pop(); stacks[stacks.length - 1].unshift(...extra); }
  while (stacks.length < min) stacks.push([]);
  const placed = new Set(stacks.flat());
  for (const id of live) {
    if (placed.has(id) || isMin(id)) continue;
    const free = stacks.find((s) => s.every(isMin));
    if (free) free.push(id); else if (stacks.length >= max) stacks[stacks.length - 1].push(id); else stacks.push([id]);
    placed.add(id);
  }
  const swapOut = [];
  for (const s of stacks) {
    const shown = s.filter((id) => !isMin(id));
    if (!shown.length) continue;
    const keep = shown.find((id) => back.has(id)) || shown[shown.length - 1];
    for (const id of shown) if (id !== keep) swapOut.push(id);
    s.splice(s.indexOf(keep), 1);
    s.push(keep);
  }
  stacks = stacks.filter((st, i) => st.length || i < min);
  return { stacks, swapOut };
}
// the window a slot shows (its last one, if not minimized), or null for a free slot
export const shownIn = (stack, isMin) => (stack.length && !isMin(stack[stack.length - 1]) ? stack[stack.length - 1] : null);

// A window dropped by hand on a tiled display (0.8.8.10, drag to rearrange): stacks are that display's slots, at the
// slot under the drop. A window already in them swaps slots with the one there; one from elsewhere gets a slot of its
// own at that place (the others shift). at: the index of the slot under the drop (slotAt).
// A free slot (its windows all minimized) under the drop is taken: the window joins that slot (in front of the minimized
// ones, which come back by swapping) and its own slot goes, so a half stays one half. isMin(id): minimized.
export function dropInto(stacks, id, at, isMin = () => false) {
  const out = (stacks || []).map((s) => [...s]);
  const src = out.findIndex((s) => s.includes(id));
  const free = at >= 0 && at < out.length && at !== src && out[at].every(isMin);
  if (free) {
    if (src >= 0) out[src] = out[src].filter((x) => x !== id);
    out[at].push(id);
    return out;
  }
  if (src >= 0) {
    if (at === src || at < 0 || at >= out.length) return out;
    [out[src], out[at]] = [out[at], out[src]];
    return out;
  }
  out.splice(Math.max(0, Math.min(at, out.length)), 0, [id]);
  return out;
}
// The slot a drop at (x, y) takes among `stacks` on a display with that layout and area: a slot already there (a free one
// to take, or one to swap with), or a new one for a window from elsewhere dropped on a shown window. Returns { at, n }:
// the index for dropInto and the count of slots to lay out for the preview.
export function dropSlot(stacks, id, x, y, rectsFor, isMin = () => false) {
  const n0 = (stacks || []).length;
  const inHere = (stacks || []).some((s) => s.includes(id));
  if (n0) {
    const i = slotAt(rectsFor(n0), x, y);
    if (i >= 0 && (inHere || stacks[i].every(isMin))) return { at: i, n: n0 };
  }
  const n = n0 + (inHere ? 0 : 1);
  return { at: slotAt(rectsFor(n), x, y), n };
}
// the index of the rect under (x, y), else the nearest one by its center; -1 with none
export function slotAt(rects, x, y) {
  if (!rects || !rects.length) return -1;
  const i = rects.findIndex((r) => x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h);
  if (i >= 0) return i;
  let best = 0, bd = Infinity;
  rects.forEach((r, j) => { const d = (r.x + r.w / 2 - x) ** 2 + (r.y + r.h / 2 - y) ** 2; if (d < bd) { bd = d; best = j; } });
  return best;
}
