// menunav.js — how a menu answers the keyboard and the pointer, with no screen. Lord Flow's menus and menu bar
// (themes/default/components/Menu.jsx, MenuBar.jsx), Lord Classic's (themes/classic/components/Menu.jsx and the
// window's menu strip) and the apps' right-click menus (sdk/contextmenu.jsx) read these rules. Tested by
// server/tests/menu_nav_test.mjs.
//
//   selectable(item)                       a row the highlight can rest on (not '-', a header, swatches or disabled)
//   stepIndex(items, active, dir)          the next (+1) or previous (-1) one, wrapping; -1 when there is none
//   edgeIndex(items, end)                  the first one, or the last one with end = true (Home, End)
//   menuAction(e, rtl)                     what a key does in an open menu ('next', 'open', 'back', 'type'…)
//   typeAhead(state, ch, now, ms)          the letters typed so far ({ buf, at }), reset after a pause
//   matchIndex(items, active, buf)         the next row whose label starts with them
//   subPlace(parent, width, area, rtl)     where a submenu goes: { x, side }, beside the parent on the side with room
//   aiming(from, to, box, side)            the pointer is on its way to the open submenu (the safe triangle)
//   barIndex(count, at, dir)               the menu of a bar after (dir +1) or before (-1) this one, wrapping:
//                                          'open' on a row without a submenu and 'back' in a top menu move along
//                                          the bar
//
// Directions follow the reading order: in Arabic and Hebrew → goes back and ← opens a submenu, because the menus
// and their submenus are mirrored.

// The submenu under the pointer opens after this pause, so passing over a row on the way to another does not flash
// its submenu; on the way to an open submenu the pointer gets longer before another row takes over.
export const SUB_DELAY = 180;
export const SUB_GRACE = 400;
// Letters typed further apart than this start a new search.
export const TYPE_MS = 900;

export const selectable = (it) => !!it && it !== '-' && typeof it === 'object' && !it.header && !it.swatches && !it.disabled;

export function stepIndex(items, active, dir) {
  const list = items || [];
  const n = list.length;
  if (!n) return -1;
  // with nothing highlighted ↓ starts at the top and ↑ at the bottom
  const from = active >= 0 && active < n ? active : dir > 0 ? -1 : n;
  for (let k = 1; k <= n; k++) {
    const i = (((from + dir * k) % n) + n) % n;
    if (selectable(list[i])) return i;
  }
  return -1;
}

export const edgeIndex = (items, end = false) => stepIndex(items, -1, end ? -1 : 1);

// e: { key, char, ctrl, alt, meta }. A printable character with no modifier (other than Shift) searches the labels.
export function menuAction(e, rtl = false) {
  const k = e && e.key;
  if (k === 'ArrowDown') return 'next';
  if (k === 'ArrowUp') return 'prev';
  if (k === 'Home' || k === 'PageUp') return 'first';
  if (k === 'End' || k === 'PageDown') return 'last';
  if (k === 'Enter' || k === ' ' || k === 'Space') return 'activate';
  if (k === 'Escape') return 'close';
  if (k === 'ArrowRight') return rtl ? 'back' : 'open';
  if (k === 'ArrowLeft') return rtl ? 'open' : 'back';
  if (!e || e.ctrl || e.alt || e.meta) return null;
  const ch = typeof e.char === 'string' ? e.char : '';
  return ch.length && /\S/u.test(ch) && [...ch].length === 1 ? 'type' : null;
}

export function typeAhead(state, ch, now, ms = TYPE_MS) {
  const fresh = !state || !state.buf || now - state.at > ms;
  return { buf: (fresh ? '' : state.buf) + String(ch).toLocaleLowerCase(), at: now };
}

const labelOf = (it) => (selectable(it) && typeof it.label === 'string' ? it.label.trim().toLocaleLowerCase() : '');

// One letter (or the same letter again and again) goes to the next row that starts with it, round the menu; more
// letters narrow the search and keep the current row while it still matches. -1: no row matches.
export function matchIndex(items, active, buf) {
  const list = items || [];
  const n = list.length;
  if (!n || !buf) return -1;
  const chars = [...buf];
  const same = chars.every((c) => c === chars[0]);
  const tries = same && chars.length > 1 ? [buf, chars[0]] : [buf];
  for (const want of tries) {
    const start = want.length > 1 && want === buf ? Math.max(active, 0) : active + 1;
    for (let k = 0; k < n; k++) {
      const i = (((start + k) % n) + n) % n;
      if (labelOf(list[i]).startsWith(want)) return i;
    }
  }
  return -1;
}

// parent: { x, w } of the open menu; width: the submenu's; area: { x, w } of the display (or the window, for an
// app's menu). The submenu starts at the parent's far edge in the reading direction, overlapping it by `overlap`;
// with no room there it goes to the other side, and with room on neither, to the side with more of it.
export function subPlace(parent, width, area, rtl = false, overlap = 4, margin = 4) {
  const lo = area.x + margin, hi = area.x + area.w - margin;
  const right = parent.x + parent.w - overlap, left = parent.x + overlap - width;
  const fitsRight = right + width <= hi, fitsLeft = left >= lo;
  let side = rtl ? 'left' : 'right';
  if (side === 'right' && !fitsRight && (fitsLeft || parent.x - lo > hi - (parent.x + parent.w))) side = 'left';
  else if (side === 'left' && !fitsLeft && (fitsRight || hi - (parent.x + parent.w) > parent.x - lo)) side = 'right';
  const x = side === 'right' ? right : left;
  return { x: Math.round(Math.max(lo, Math.min(x, hi - width))), side };
}

// The safe triangle: from the point the pointer was at, the triangle to the near edge of the submenu's box
// ({ x, y, w, h }) holds the point it is at now. While it does, the row under it does not take over yet.
export function aiming(from, to, box, side) {
  if (!from || !to || !box) return false;
  if (from.x === to.x && from.y === to.y) return false;
  const ex = side === 'left' ? box.x + box.w : box.x;
  const a = from, b = { x: ex, y: box.y - 4 }, c = { x: ex, y: box.y + box.h + 4 };
  const cross = (p, q, r) => (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
  const d1 = cross(a, b, to), d2 = cross(b, c, to), d3 = cross(c, a, to);
  const neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
  return !(neg && pos);
}

export const barIndex = (count, at, dir) => (count > 0 ? (((at + dir) % count) + count) % count : -1);
