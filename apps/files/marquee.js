// marquee.js — the rules of Files' rubber-band selection (GitHub #233), pure so tests/files_marquee_test.mjs checks them
// without lord-server. A drag that starts on the empty space of the icon or list view draws a rectangle; every item
// whose box it touches is selected. Shift adds them to what was selected before the drag, Ctrl (⌘) toggles them, and a
// press with no modifier starts afresh. Near the view's top or bottom edge the view scrolls by itself, faster the closer
// the pointer gets.
//
// Coordinates are physical (x grows to the right in every language) and in the scrolled content's own space: an item's
// box is where the layout put it (lord.getLayout, minus the content's origin), so it doesn't move while the view
// scrolls, and the rectangle's two corners are the press and the pointer in that same space. Right to left needs no
// flip: the layout already mirrored the items, and the rectangle covers whatever is physically under it.

export const DRAG_MIN = 4;          // px the pointer moves before a press on empty space becomes a rectangle
export const EDGE = 36;             // px from the top or bottom edge where autoscroll starts
export const MAX_STEP = 28;         // px per frame at the very edge (and past it)

// the rectangle between two points, whichever way it was drawn: { x, y, w, h }
export function rectOf(a, b) {
  const x = Math.min(a.x, b.x), y = Math.min(a.y, b.y);
  return { x, y, w: Math.abs(a.x - b.x), h: Math.abs(a.y - b.y) };
}

// true when the rectangle touches the item's box (edges count: a rectangle that only grazes a card selects it)
export function touches(r, it) {
  if (!r || !it || !(it.w > 0) || !(it.h > 0)) return false;
  return r.x <= it.x + it.w && r.x + r.w >= it.x && r.y <= it.y + it.h && r.y + r.h >= it.y;
}

// the paths of the items the rectangle touches, in the items' order. items: [{ path, x, y, w, h }]
export function hitPaths(r, items) {
  const out = [];
  for (const it of items) if (touches(r, it)) out.push(it.path);
  return out;
}

// what the press's modifiers do: 'toggle' (Ctrl or ⌘), 'add' (Shift) or 'replace'
export function modeOf(ev) {
  if (ev && (ev.ctrl || ev.meta)) return 'toggle';
  if (ev && ev.shift) return 'add';
  return 'replace';
}

// the selection while the rectangle covers `hits`, from the selection `base` there was when the drag started
export function combine(base, hits, mode) {
  if (mode === 'replace') return new Set(hits);
  const next = new Set(base);
  if (mode === 'add') { for (const p of hits) next.add(p); return next; }
  for (const p of hits) { if (base.has(p)) next.delete(p); else next.add(p); }
  return next;
}

export const sameSet = (a, b) => {
  if (a === b) return true;
  if (!a || !b || a.size !== b.size) return false;
  for (const x of a) if (!b.has(x)) return false;
  return true;
};

// How far to scroll this frame with the pointer at screen y, in a viewport from top to top + height: negative near the
// top edge, positive near the bottom, 0 in between. Grows linearly to MAX_STEP at the edge and stays there past it.
export function autoscrollStep(y, top, height, edge = EDGE, max = MAX_STEP) {
  if (!(height > 0)) return 0;
  const e = Math.min(edge, height / 3);
  const fromTop = y - top, fromBottom = top + height - y;
  if (fromTop < e) return -Math.max(1, Math.round(max * Math.min(1, (e - fromTop) / e)));
  if (fromBottom < e) return Math.max(1, Math.round(max * Math.min(1, (e - fromBottom) / e)));
  return 0;
}

// the scroll offset after a step, inside [0, max]
export const clampScroll = (offset, step, max) => Math.max(0, Math.min(Math.max(0, max), offset + step));

// Updates at most once per frame: push() keeps the latest value, and the value is handed to apply() on the next frame
// (schedule: requestAnimationFrame where there is one, else a 16 ms timer). flush() applies a pending value at once
// (the pointer was released); cancel() drops it.
export function frameGate(apply, schedule) {
  const later = schedule || ((fn) => (typeof requestAnimationFrame === 'function' ? requestAnimationFrame(fn) : setTimeout(fn, 16)));
  let pending = null, armed = false, has = false;
  const run = () => { armed = false; if (!has) return; const v = pending; has = false; pending = null; apply(v); };
  return {
    push(v) { pending = v; has = true; if (!armed) { armed = true; later(run); } },
    flush() { if (has) { const v = pending; has = false; pending = null; apply(v); } },
    cancel() { has = false; pending = null; },
    get pending() { return has; },
  };
}
