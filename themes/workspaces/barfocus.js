// barfocus.js — the keyboard on the bars (docs/WORKSPACES.md, "Keyboard"). ⌃F2 moves it to the Context Spine and the
// activity strip, even over a Linux app (lord-server keeps ⌃F2 for this theme: the "workspaces" key table,
// server/src/wl/keygrab.c). Then the arrows (and Tab) go from one control to the next, from the crown down the spine
// and along the strip; Enter or Space presses the one with the ring, Escape gives the keyboard back. While it is on,
// an empty overlay layer holds the keyboard (no app gets the keys) and the control with the keyboard draws a ring.
//
//   barFocus                the id of the control with the ring, or null
//   setBarItems(fn)         fn() → [{ id, run }] the controls in order (index.jsx: they depend on the activities)
//   enterBars(), leaveBars()
//   barKey(e)               a key while it is on: true when used
import { createStore } from '@lordos/sdk';
import { barStep } from './model/activities.js';

export const barFocus = createStore(null);
let itemsOf = () => [];
export function setBarItems(fn) { itemsOf = fn; }

export function enterBars(start) {
  const list = itemsOf();
  if (!list.length) return;
  barFocus.set((list.find((x) => x.id === start) || list[0]).id);
}
export const leaveBars = () => barFocus.set(null);

export function barKey(e) {
  const cur = barFocus.get();
  if (cur == null) return false;
  const list = itemsOf();
  const ids = list.map((x) => x.id);
  if (e.key === 'Escape') { leaveBars(); return true; }
  if (e.key === 'Enter' || e.key === ' ' || e.key === 'Space') {
    const it = list.find((x) => x.id === cur);
    leaveBars();
    if (it) it.run();
    return true;
  }
  const dir = { ArrowDown: 1, ArrowRight: 1, ArrowUp: -1, ArrowLeft: -1, Tab: e.shift ? -1 : 1, Home: -Infinity, End: Infinity }[e.key];
  if (dir == null) return true;          // every other key is the bars' too while they have the keyboard
  barFocus.set(barStep(ids, cur, dir));
  return true;
}
