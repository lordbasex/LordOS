// navmodel.js — the pure rules of sdk/navbuttons.js (a mouse's back and forward buttons, GitHub #234); tested by
// server/tests/navbuttons_test.mjs.

// In the desktop's process every native window hears the event: only the one under the pointer takes it (never one
// when the pointer is outside every window). An isolated app's process holds one window and has no window frame of
// its own around it, so a press there is its window's.
export function navFor(ev, winId, shell) {
  if (!ev || (ev.direction !== 'back' && ev.direction !== 'forward')) return null;
  if (shell) return ev.win != null && winId != null && String(ev.win) === String(winId) ? ev.direction : null;
  return ev.win == null || (winId != null && String(ev.win) === String(winId)) ? ev.direction : null;
}

// ---- a window's own history of the places it showed (Settings' sections, Help's articles) ----
const VISIT_MAX = 50;
// cur is shown now: a new place goes after the one before it and drops what was ahead; the same place changes nothing
export function visitNote(h, cur) {
  if (h.cur === undefined) return { back: [], fwd: [], cur };
  if (h.cur === cur) return h;
  return { back: [...h.back, h.cur].slice(-VISIT_MAX), fwd: [], cur };
}
// d 'back' or 'forward': the history after the step (its cur is the place to show), or null when there is none
export function visitStep(h, d) {
  if (d === 'back' && h.back.length) return { back: h.back.slice(0, -1), fwd: [h.cur, ...h.fwd], cur: h.back[h.back.length - 1] };
  if (d === 'forward' && h.fwd.length) return { back: [...h.back, h.cur], fwd: h.fwd.slice(1), cur: h.fwd[0] };
  return null;
}
