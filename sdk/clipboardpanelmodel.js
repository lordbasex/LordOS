// clipboardpanelmodel.js — the clipboard history panels' list (sdk/clipboard.js, ❖V and ❖⇧V), without a screen. Pure;
// tested by server/tests/clipboard_panel_model_test.mjs.
//
//   stepSel(sel, count, step)               the selection one row up or down, kept inside the list
//   scrollFor(index, rowH, offset, viewH, pad)  the scroll offset that shows row `index` whole, or null when it already shows
//   rowWindow(y, h, rowH, count, pad)       the rows in sight (and a few around) of up to 500 entries, and the space of the rest
//   hoverGate()                             { key(), move(x, y), allow() }: a row under the pointer takes the selection only
//                                           when the pointer itself moved since the arrow keys did (GitHub #203, #204)
//
// The full panel used to draw a window of 16 rows around the selection and select a row when the pointer entered it: with
// the pointer resting on the list, each arrow key moved the window, a new row slid under the still pointer and took the
// selection back, so the list scrolled by itself and the keys seemed dead. Now it is a plain scrolling list (wheel,
// scroll bar, keys), the arrow keys move a selection that is drawn and scrolled into view, Enter pastes it, and the
// pointer selects only when it moves.

export function stepSel(sel, count, step) {
  if (count <= 0) return 0;
  const cur = Number.isInteger(sel) ? sel : 0;
  return Math.max(0, Math.min(count - 1, cur + (step < 0 ? -1 : 1)));
}

// pad: the list's padding above its first row (and below its last), kept in view with the first and last rows
export function scrollFor(index, rowH, offset, viewH, pad = 0) {
  if (index < 0 || rowH <= 0 || viewH <= 0) return null;
  const top = pad + index * rowH, bottom = top + rowH;
  if (top < offset) return Math.max(0, top - pad);
  if (bottom > offset + viewH) return Math.max(0, bottom + pad - viewH);
  return null;
}

// The rows to make for a list scrolled to y with h of it in sight (rowH apart, pad above the first): from..to (to not
// included), with `over` rows more on each side, and the space the rows left out take above (before) and below (after).
export function rowWindow(y, h, rowH, count, pad = 0, over = 6) {
  if (count <= 0 || rowH <= 0) return { from: 0, to: 0, before: 0, after: 0 };
  const first = Math.floor(Math.max(0, y - pad) / rowH), last = Math.ceil(Math.max(0, y - pad + Math.max(h, 0)) / rowH);
  const from = Math.max(0, Math.min(count, first - over)), to = Math.max(from, Math.min(count, last + over));
  return { from, to, before: from * rowH, after: (count - to) * rowH };
}

// What counts as the pointer moving: a position different from the last one seen (a list that scrolls or redraws under a
// pointer at rest sends enter events with no move). Before any position is known, a hover selects as it always did.
export function hoverGate() {
  let last = null, keyed = false;
  return {
    key() { keyed = true; },
    move(x, y) {
      if (last && last.x === x && last.y === y) return;
      last = { x, y };
      keyed = false;
    },
    allow: () => !keyed,
  };
}
