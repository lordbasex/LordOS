// layouts.js — Lord Workspaces' static tiling, as plain data (tested by server/tests/workspaces_model_test.mjs). The
// runtime part is ../tiler.js, which moves windows with the SDK's own window store; nothing here moves anything.
//
//   LAYOUTS                          'free' (windows where the person puts them), 'single', 'split' (a main tile and a
//                                    column), 'three' (three columns) and 'grid'
//   tileRects(layout, area, n, ratio)    n rectangles inside the work area (null for 'free')
//   fitTile(r, min, area)            a tile too small for a window's minimum size: the window keeps its minimum, at the
//                                    tile's corner, inside the area (it overlaps its neighbour instead of being cut)
//   splitterAt / ratioFrom           the draggable divider of 'split' and 'three' and the ratio a drop gives
//   orderFor(ids, keyOf, saved)      the tile order: a saved layout's apps first, in their saved order
//   homeFor(key, saved, connected)   the display a saved layout put an app's window on, if that display is here
//   displayKey(d, i)                 a display's stable name (its connector), for saved layouts
export const LAYOUTS = ['free', 'single', 'split', 'three', 'grid'];
export const TILE_GAP = 10;
export const RATIO_MIN = 0.25;
export const RATIO_MAX = 0.75;
// a side column holds at most this many rows; more windows share the last one
export const SIDE_ROWS = 3;

export const cleanLayout = (l) => (LAYOUTS.includes(l) ? l : 'free');
export const defaultRatio = (layout) => (layout === 'three' ? 1 / 3 : 2 / 3);
export function cleanRatio(layout, r) {
  const v = Number(r);
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
  if (!area || n < 2 || (layout !== 'split' && layout !== 'three')) return null;
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

// A display's stable name for saved layouts: its connector (DP-1, HDMI-A-1…), or its place when it has none.
export const displayKey = (d, i = 0) => (d && typeof d.name === 'string' && d.name ? d.name : `#${i}`);

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
