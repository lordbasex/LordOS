// displaylayout.js — the pure math of Settings → Displays' arrangement (docs/DISPLAYS.md,
// "Numbers, identify and snapping"). No React, no lord.*: tested by server/tests/display_layout_test.mjs.
//
//   displayNumbers(outputs)                     { [id]: 1, 2, 3… }: main display first, then the others in
//                                               the order lord.display.list() gives them (connector order)
//   snapDisplay(rects, i, x, y, threshold)      while dragging: the "magnetic" position of rects[i] at (x, y)
//   resolveDrop(rects, i, x, y, threshold)      on drop: a position that touches another display edge to edge
//                                               and overlaps none (the nearest one to the snapped position)
//   placementOk(rects, i, x, y)                 the rule lord-server checks too (out_fits in outputs.c)
//
// Every rect is { x, y, w, h } in the desktop's logical px (integers). `threshold` is in the same
// units: Settings converts its 12 preview px with the preview's scale.

export const SNAP_PREVIEW_PX = 12;

const overlaps = (a, b) => a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
const shared = (a0, a1, b0, b1) => Math.min(a1, b1) - Math.max(a0, b0);
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

// Two displays touch when an edge of one lies on an edge of the other along at least one pixel
// (a corner alone is not enough: the pointer could not cross there).
export function touches(a, b) {
  if ((a.x + a.w === b.x || b.x + b.w === a.x) && shared(a.y, a.y + a.h, b.y, b.y + b.h) > 0) return true;
  if ((a.y + a.h === b.y || b.y + b.h === a.y) && shared(a.x, a.x + a.w, b.x, b.x + b.w) > 0) return true;
  return false;
}

// rects[i] at (x, y): overlaps no other display and touches at least one (always fine when alone).
export function placementOk(rects, i, x, y) {
  const m = { x, y, w: rects[i].w, h: rects[i].h };
  let touching = rects.length < 2;
  for (let j = 0; j < rects.length; j++) {
    if (j === i) continue;
    if (overlaps(m, rects[j])) return false;
    if (touches(m, rects[j])) touching = true;
  }
  return touching;
}

// Numbers like macOS's "Identify": the main display is 1, the others follow in the server's order.
// Only active displays with a size get one.
export function displayNumbers(outputs) {
  const act = (outputs || []).filter((o) => o && o.active && o.w);
  const main = act.find((o) => o.main);
  const order = main ? [main, ...act.filter((o) => o !== main)] : act;
  const out = {};
  order.forEach((o, k) => { out[o.id] = k + 1; });
  return out;
}

// The targets one axis can snap to against another display `o`: edge to edge (both sides), then
// starts aligned, ends aligned and centers aligned.
function targets(p, len, op, olen) {
  return [op + olen, op - len, op, op + olen - len, Math.round(op + (olen - len) / 2)];
}

// While dragging: each axis independently jumps to the nearest target within `threshold`, taken
// from the displays that are near on the other axis (no further than `threshold` away), so a
// display far below another doesn't stick to its side.
export function snapDisplay(rects, i, x, y, threshold) {
  const { w, h } = rects[i];
  let bx = Math.round(x), by = Math.round(y), dx = threshold + 1, dy = threshold + 1;
  for (let j = 0; j < rects.length; j++) {
    if (j === i) continue;
    const o = rects[j];
    const gapY = Math.max(0, o.y - (y + h), y - (o.y + o.h));
    const gapX = Math.max(0, o.x - (x + w), x - (o.x + o.w));
    if (gapY <= threshold) {
      for (const t of targets(x, w, o.x, o.w)) {
        const d = Math.abs(x - t);
        if (d <= threshold && d < dx) { dx = d; bx = t; }
      }
    }
    if (gapX <= threshold) {
      for (const t of targets(y, h, o.y, o.h)) {
        const d = Math.abs(y - t);
        if (d <= threshold && d < dy) { dy = d; by = t; }
      }
    }
  }
  return { x: bx, y: by };
}

// Along an edge of `o`, the coordinate `v` (length `len`) is kept so the edges share at least one
// pixel, and pulled to o's start, end or center when within `threshold`.
function alongEdge(v, len, op, olen, threshold) {
  const c = clamp(v, op - len + 1, op + olen - 1);
  let best = c, bd = threshold + 1;
  for (const t of [op, op + olen - len, Math.round(op + (olen - len) / 2)]) {
    const d = Math.abs(c - t);
    if (d <= threshold && d < bd && t >= op - len + 1 && t <= op + olen - 1) { bd = d; best = t; }
  }
  return [best, c];
}

// On drop: the snapped position if it is a valid one; otherwise the nearest position that touches a
// display edge to edge without overlapping any (beside it, aligned when close, or slid along its edge
// past a display in the way). lord-server keeps such a position exactly (display_arrange, out_fits).
export function resolveDrop(rects, i, x, y, threshold) {
  if (rects.length < 2) return { x: Math.round(x), y: Math.round(y) };
  const s = snapDisplay(rects, i, x, y, threshold);
  if (placementOk(rects, i, s.x, s.y)) return s;
  const { w, h } = rects[i];
  let best = null, bd = Infinity;
  const consider = (cx, cy) => {
    if (!placementOk(rects, i, cx, cy)) return;
    const d = (cx - s.x) ** 2 + (cy - s.y) ** 2;
    if (d < bd) { bd = d; best = { x: cx, y: cy }; }
  };
  for (let j = 0; j < rects.length; j++) {
    if (j === i) continue;
    const o = rects[j];
    // the positions along an edge: where it was dropped, plus just past every other display (to slide
    // around one that is in the way)
    const ys = [s.y], xs = [s.x];
    for (let q = 0; q < rects.length; q++) {
      if (q === i || q === j) continue;
      ys.push(rects[q].y + rects[q].h, rects[q].y - h);
      xs.push(rects[q].x + rects[q].w, rects[q].x - w);
    }
    for (const sx of [o.x + o.w, o.x - w]) {                 // right of it, left of it
      for (const v of ys) for (const yy of alongEdge(v, h, o.y, o.h, threshold)) consider(sx, yy);
    }
    for (const sy of [o.y + o.h, o.y - h]) {                 // below it, above it
      for (const v of xs) for (const xx of alongEdge(v, w, o.x, o.w, threshold)) consider(xx, sy);
    }
  }
  if (best) return best;
  // nothing free next to anyone (can't happen with a few displays): right of the rightmost one
  let r = null;
  for (let j = 0; j < rects.length; j++) if (j !== i && (!r || rects[j].x + rects[j].w > r.x + r.w)) r = rects[j];
  return { x: r.x + r.w, y: r.y };
}
