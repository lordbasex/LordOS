// taskbarfit.js — how many app buttons fit on a Classic taskbar, and how big (GitHub #111). Pure logic, no `lord`:
// tests/classic_taskbarfit_test.mjs runs it in node.
//
// Along the bar (its width at the top or bottom, its height at the left or right): the bar's padding, the start,
// search and task view buttons, the app buttons, a separator and the Trash, then the tray (measured from the last
// frame). With more apps than that room holds the app buttons get smaller, down to MIN; past that the ones that
// still don't fit move into a "more apps" button at the end of the row, so the row never runs over the tray. When
// the bar is centered, the row sits in the middle of the display while it can, and moves toward the start
// once the tray would be in its way.

export const BTN = 40;        // a button along the bar
export const MIN = 30;        // the smallest app button
export const PAD = 6;         // the bar's padding at each end
export const GAP = 2;         // between the row's buttons
export const SEP = 7;         // the separator before the Trash (1 px and its margins)
const FIXED = 3;              // start, search, task view

// the row's length with n app buttons of size b (and `more`: the "more apps" button, also of size b)
export function rowLength(n, b, more = false) {
  const kids = FIXED + n + (more ? 1 : 0) + 2;          // + the separator and the Trash
  return FIXED * BTN + (n + (more ? 1 : 0)) * b + SEP + BTN + (kids - 1) * GAP;
}

// length: the bar's length; tray: the tray's length; count: the app buttons wanted; center: centered alignment.
// → { size, shown, more, spacer }: the app buttons' size, how many are shown (the first `shown`), whether the
// "more apps" button is there, and the space before the row (0 when aligned to the start).
export function taskbarFit({ length, tray, count, center }) {
  const avail = Math.max(0, length - 2 * PAD - Math.max(0, tray));
  let size = BTN, shown = count, more = false;
  if (count > 0 && rowLength(count, BTN) > avail) {
    // the size that fits them all: avail = rowLength(count, b)
    const b = Math.floor((avail - rowLength(0, 0) - count * GAP) / count);
    if (b >= MIN) size = b;
    else {
      size = MIN;
      more = true;
      shown = 0;
      while (shown < count - 1 && rowLength(shown + 1, MIN, true) <= avail) shown++;
    }
  }
  const row = rowLength(shown, size, more);
  // centered: the row's start at the middle of the display less half the row, but never so far that the tray
  // no longer fits after it
  const spacer = center ? Math.max(0, Math.min(Math.floor((length - row) / 2) - PAD, avail - row)) : 0;
  return { size, shown, more, spacer };
}

// the size of an app's icon in a button of that size (26 in a full one)
export const iconFor = (size) => Math.min(26, size - 8);
