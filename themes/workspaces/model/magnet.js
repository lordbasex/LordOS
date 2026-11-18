// magnet.js — Lord Workspaces' magnet for the free layout, as plain data (tested by server/tests/workspaces_model_test.mjs).
// The runtime part is ../magnet.js; nothing here moves anything.
//
// A window snapped to the left or right half of a display looks for its other half:
//   assistFor(ws, id, area, others)   what to do with the other half: pair with a window already snapped there, fill it
//                                     with the only other window of that display, offer a choice, or nothing
// Two windows that share a display's halves are a pair, and their shared edge is a magnet:
//   follow(pair, before, after, area, minW)   resizing one side's inner edge moves the other's; anything else (a move,
//                                     a resize of another edge, a window gone, minimized or maximized) breaks the pair
export const SIDES = ['left', 'right'];
export const other = (side) => (side === 'left' ? 'right' : side === 'right' ? 'left' : null);

const inside = (w, a) => {
  const cx = w.x + w.w / 2, cy = w.y + w.h / 2;
  return cx >= a.x && cx < a.x + a.w && cy >= a.y && cy < a.y + a.h;
};

// ws: the windows of the activity in view that may take a half (tileable, not minimized), the snapped one among them;
// id: the window just snapped (its `snapped` is 'left' or 'right'); area: its display's work area.
// Returns { pair: id } (a window already holds the other half), { fill: id } (the only other window on that display),
// { choose: [ids] } (several: the ones on that display first, then the others), or null.
export function assistFor(ws, id, area) {
  const me = ws.find((w) => w.id === id);
  const want = other(me?.snapped);
  if (!want || !area) return null;
  const rest = ws.filter((w) => w.id !== id);
  const there = rest.find((w) => w.snapped === want && inside(w, area));
  if (there) return { pair: there.id };
  const here = rest.filter((w) => inside(w, area));
  if (here.length === 1) return { fill: here[0].id };
  const choose = [...here, ...rest.filter((w) => !inside(w, area))].map((w) => w.id);
  return choose.length ? { choose } : null;
}

// pair: { left: id, right: id }; before / after: id → frame { x, y, w, h, minimized, maximized } (after lacks a window
// that closed); minW: id → minimum width. Returns { break: true }, { patch: { id, x, w } } (the other side follows), or
// null (nothing to do).
export function follow(pair, before, after, area, minW = () => 160) {
  const L0 = before[pair.left], R0 = before[pair.right], L = after[pair.left], R = after[pair.right];
  if (!L0 || !R0 || !L || !R || L.minimized || R.minimized || L.maximized || R.maximized) return { break: true };
  const same = (a, b) => a.x === b.x && a.y === b.y && a.w === b.w && a.h === b.h;
  const lSame = same(L, L0), rSame = same(R, R0);
  if (lSame && rSame) return null;
  const right = area.x + area.w;
  // the left window's right edge moved, nothing else of it: the right window's left edge goes there
  if (!lSame && rSame && L.x === L0.x && L.y === L0.y && L.h === L0.h) {
    const x = Math.min(L.x + L.w, right - minW(pair.right));
    if (x === R.x && right - x === R.w) return null;
    return { patch: { id: pair.right, x, w: right - x } };
  }
  // the right window's left edge moved, its right edge stayed: the left window's right edge goes there
  if (lSame && !rSame && R.x + R.w === R0.x + R0.w && R.y === R0.y && R.h === R0.h) {
    const w = Math.max(minW(pair.left), R.x - L.x);
    if (w === L.w) return null;
    return { patch: { id: pair.left, x: L.x, w } };
  }
  return { break: true };
}
