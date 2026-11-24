// slowclick.js — the "slow second click" that renames an item in place, as in the reference desktop's file manager:
// the item is already selected, the person clicks its name again after the double-click interval has passed (and
// within SLOW_CLICK_MAX_MS), without moving the pointer, and once the double-click interval has passed after that
// second click without another press, the item's name becomes the rename field. Pure logic, no React and no lord.*:
// Files (apps/files/index.jsx) feeds it the presses, moves and releases and starts its own rename when due() says so;
// any desktop that shows files with a name to rename uses the same rule (tests: server/tests/slowclick_test.mjs).
//
// A press counts as the slow second click only when every one of these holds:
//   - the press before it was on the same item, with the left button, no modifier key, while the window already had
//     the focus (a click that only brought the window forward doesn't count), and was neither the second press of a
//     double click nor a press that turned into a drag;
//   - it comes at least the double-click interval and at most SLOW_CLICK_MAX_MS after that press, within SLOW_CLICK_SLOP
//     px of it;
//   - it is on the item's name (not its icon), with the left button and no modifier key, the window has the focus, the
//     item was the only one selected before the press, and the item can be renamed (the caller decides: write access,
//     not in the Trash, not a mount point…);
//   - the pointer doesn't move more than SLOW_CLICK_SLOP px before the release (a drag still drags);
//   - no other press comes within the double-click interval after the release (a real double click still opens), and
//     nothing else cancels it meanwhile (a key, a double click, the window losing the focus: cancel()).
//
// There is no system setting for the double-click interval: the runtime counts a second click within 450 ms as a double
// click (runtime/events.js), and DOUBLE_CLICK_MS is that same number.

export const DOUBLE_CLICK_MS = 450;
export const SLOW_CLICK_MAX_MS = 1500;
export const SLOW_CLICK_SLOP = 4;

const far = (a, b, slop) => Math.hypot(a.x - b.x, a.y - b.y) > slop;

// slowClick({ doubleMs, maxMs, slop }) → the state of one view (one per window). Times are in ms, from any clock that
// only goes forward (lord.now()).
export function slowClick({ doubleMs = DOUBLE_CLICK_MS, maxMs = SLOW_CLICK_MAX_MS, slop = SLOW_CLICK_SLOP } = {}) {
  let last = null;          // the previous press: { key, t, x, y, armed }
  let down = null;          // the press in progress: { x, y }
  let pending = null;       // a slow second click waiting for its release: { key, x, y }
  let due = null;           // a rename waiting for the double-click interval to pass: { key, at }

  const api = {
    // p: { key, t, x, y, button, mods (any modifier held), onName, focused, sole (the only selected item before this
    // press), canRename }. true when this press may become the slow second click (it still needs its release).
    press(p) {
      due = null;
      pending = null;
      const plain = (p.button || 0) === 0 && !p.mods;
      const prev = last;
      const same = !!prev && prev.key === p.key;
      const gap = same ? p.t - prev.t : Infinity;
      const isDouble = same && gap < doubleMs;
      const slow = same && prev.armed && plain && !!p.onName && !!p.focused && !!p.sole && !!p.canRename
        && gap >= doubleMs && gap <= maxMs && !far(prev, p, slop);
      last = { key: p.key, t: p.t, x: p.x, y: p.y, armed: plain && !!p.focused && !isDouble && !slow };
      down = { x: p.x, y: p.y };
      if (slow) pending = { key: p.key, x: p.x, y: p.y };
      return slow;
    },
    // the pointer moved: past the slop while pressed, the press is a drag (no rename now, and it arms nothing)
    move(x, y) {
      if (!down || !far(down, { x, y }, slop)) return;
      pending = null;
      if (last) last.armed = false;
      down = null;
    },
    // the release: the time the rename is due when the slow second click is complete, else null. The caller waits until
    // then (DOUBLE_CLICK_MS) and asks due().
    release({ t, x, y }) {
      down = null;
      const p = pending;
      pending = null;
      if (!p || far(p, { x, y }, slop)) return null;
      due = { key: p.key, at: t + doubleMs };
      return due.at;
    },
    // the key of the item to rename now, once; null when nothing is due yet or it was cancelled
    due(t) {
      if (!due || t < due.at) return null;
      const k = due.key;
      due = null;
      if (last) last.armed = false;
      return k;
    },
    // a double click, a key, a scroll, the window losing the focus, the item going away: nothing waits any more
    cancel() {
      due = null;
      pending = null;
      down = null;
      if (last) last.armed = false;
    },
    waiting: () => !!due || !!pending,
  };
  return api;
}
