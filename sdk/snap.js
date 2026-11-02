// snap.js — the geometry of window snapping (sdk/windows.js "Snapping", docs/WINDOWS.md), with no
// runtime imports so it can be tested with node (server/tests/snap_test.mjs).

export const SNAP_ZONES = ['left', 'right', 'top', 'bottom', 'top-left', 'top-right', 'bottom-left', 'bottom-right', 'max',
  'left-third', 'center-third', 'right-third', 'left-two-thirds', 'right-two-thirds'];

// The rectangle of a zone inside a work area `a` ({ x, y, w, h }). Halves and quarters round to
// whole pixels and share the middle line, so two snapped windows never overlap or leave a gap.
export function snapRect(zone, a) {
  const hw = Math.round(a.w / 2), hh = Math.round(a.h / 2), t1 = Math.round(a.w / 3), t2 = Math.round((2 * a.w) / 3);
  switch (zone) {
    case 'left': return { x: a.x, y: a.y, w: hw, h: a.h };
    case 'right': return { x: a.x + hw, y: a.y, w: a.w - hw, h: a.h };
    case 'top': return { x: a.x, y: a.y, w: a.w, h: hh };
    case 'bottom': return { x: a.x, y: a.y + hh, w: a.w, h: a.h - hh };
    case 'top-left': return { x: a.x, y: a.y, w: hw, h: hh };
    case 'top-right': return { x: a.x + hw, y: a.y, w: a.w - hw, h: hh };
    case 'bottom-left': return { x: a.x, y: a.y + hh, w: hw, h: a.h - hh };
    case 'bottom-right': return { x: a.x + hw, y: a.y + hh, w: a.w - hw, h: a.h - hh };
    case 'left-third': return { x: a.x, y: a.y, w: t1, h: a.h };
    case 'center-third': return { x: a.x + t1, y: a.y, w: t2 - t1, h: a.h };
    case 'right-third': return { x: a.x + t2, y: a.y, w: a.w - t2, h: a.h };
    case 'left-two-thirds': return { x: a.x, y: a.y, w: t2, h: a.h };
    case 'right-two-thirds': return { x: a.x + t1, y: a.y, w: a.w - t1, h: a.h };
    default: return { x: a.x, y: a.y, w: a.w, h: a.h };
  }
}

// The zone for a window dropped with the pointer at (x, y) in work area `a`, or null: within EDGE px
// of the left or right side (or past it, over a taskbar on that side) is a half, and in the top or
// bottom fifth of that side a quarter; the top edge is the whole area ('max').
export const SNAP_EDGE = 8;
export function zoneAt(x, y, a) {
  const band = Math.max(48, a.h / 5);
  const side = x <= a.x + SNAP_EDGE ? 'left' : x >= a.x + a.w - 1 - SNAP_EDGE ? 'right' : null;
  if (side) return y < a.y + band ? `top-${side}` : y > a.y + a.h - band ? `bottom-${side}` : side;
  if (y <= a.y + SNAP_EDGE) return 'max';
  return null;
}

// Super+arrows: where each arrow takes a window from where it is (`at`: a zone, 'max' or null).
// Returns a zone, 'restore', 'minimize', or null (nothing to do).
const KEYS = {
  none: { left: 'left', right: 'right', up: 'max', down: 'minimize' },
  max: { left: 'left', right: 'right', down: 'restore' },
  left: { right: 'restore', up: 'top-left', down: 'bottom-left' },
  right: { left: 'restore', up: 'top-right', down: 'bottom-right' },
  'top-left': { right: 'top-right', up: 'max', down: 'left' },
  'top-right': { left: 'top-left', up: 'max', down: 'right' },
  'bottom-left': { right: 'bottom-right', up: 'left', down: 'minimize' },
  'bottom-right': { left: 'bottom-left', up: 'right', down: 'minimize' },
  third: { left: 'left', right: 'right', up: 'max', down: 'restore' },
};
export function snapNext(at, dir) {
  const from = !at ? 'none' : /third/.test(at) ? 'third' : at;
  return (KEYS[from] && KEYS[from][dir]) || null;
}
