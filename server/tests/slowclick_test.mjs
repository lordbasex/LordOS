// slowclick_test.mjs — the slow second click that renames an item in place (sdk/slowclick.js), as Files uses it: a
// double click vs a slow click, too late, a drag, a moved pointer, modifier keys, a multiple selection, a click that
// only focused the window, the icon instead of the name, an item that can't be renamed, a cancel while it waits.
import { slowClick, DOUBLE_CLICK_MS as D, SLOW_CLICK_MAX_MS as MAX, SLOW_CLICK_SLOP as SLOP } from '../../sdk/slowclick.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};

const base = { key: 'a', x: 100, y: 50, button: 0, mods: false, onName: true, focused: true, sole: true, canRename: true };
// one click (press and release at the same spot); returns what the release said
const click = (s, t, over = {}, upAt = null) => {
  const p = { ...base, t, ...over };
  s.press(p);
  if (upAt) s.move(upAt.x, upAt.y);
  return s.release({ t: t + 80, x: upAt ? upAt.x : p.x, y: upAt ? upAt.y : p.y });
};
// the whole gesture: a first click (it selects the item), a second click `gap` ms later; the key renamed, or null
const gesture = (first = {}, second = {}, gap = 800, opts = {}) => {
  const s = slowClick();
  click(s, 1000, { sole: false, ...first });
  const at = click(s, 1000 + gap, second, opts.upAt);
  if (opts.between) opts.between(s);
  return at === null ? null : s.due(at);
};

// ---- the slow click ----
eq('the slow second click renames', gesture(), 'a');
eq('just past the double-click interval', gesture({}, {}, D), 'a');
eq('at the latest', gesture({}, {}, MAX), 'a');
eq('too late', gesture({}, {}, MAX + 1), null);
eq('within the double-click interval: a double click, never a rename', gesture({}, {}, D - 1), null);
eq('the release says when it is due', (() => { const s = slowClick(); click(s, 1000); return click(s, 1800); })(), 1800 + 80 + D);
eq('not due before the double-click interval has passed', (() => { const s = slowClick(); click(s, 1000); const at = click(s, 1800); return s.due(at - 1); })(), null);
eq('due once only', (() => { const s = slowClick(); click(s, 1000); const at = click(s, 1800); s.due(at); return s.due(at + 10); })(), null);

// ---- a real double click still opens ----
eq('a press after the release, within the interval (the double click): nothing', (() => {
  const s = slowClick(); click(s, 1000); const at = click(s, 1800); s.press({ ...base, t: 1800 + 80 + 100 }); return s.due(at);
})(), null);
eq('a double click and then a slow click: the second press of a double click arms nothing', (() => {
  const s = slowClick(); click(s, 1000); click(s, 1200); const at = click(s, 1900); return at;
})(), null);
eq('a double click, then two slow clicks: the last one renames', (() => {
  const s = slowClick(); click(s, 1000); click(s, 1200); click(s, 1900); const at = click(s, 2600); return at === null ? null : s.due(at);
})(), 'a');

// ---- a drag still drags ----
eq('the second press dragged: no rename', gesture({}, {}, 800, { upAt: { x: 140, y: 50 } }), null);
eq('a small wobble within the slop still renames', gesture({}, {}, 800, { upAt: { x: 100 + SLOP, y: 50 } }), 'a');
eq('the first press dragged (the rubber band or a move): it arms nothing', (() => {
  const s = slowClick(); click(s, 1000, { sole: false }, { x: 160, y: 90 }); const at = click(s, 1800); return at;
})(), null);
eq('released away without moves seen: no rename', (() => {
  const s = slowClick(); click(s, 1000); s.press({ ...base, t: 1800 }); return s.release({ t: 1880, x: 130, y: 50 });
})(), null);

// ---- the pointer moved between the clicks ----
eq('the second click a few px away: renames', gesture({}, { x: 100 + SLOP }), 'a');
eq('the second click farther away: no rename', gesture({}, { x: 100 + SLOP + 3 }), null);

// ---- modifier keys and other buttons ----
eq('the second click with a modifier', gesture({}, { mods: true }), null);
eq('the first click with a modifier (Shift, Ctrl, ❖)', gesture({ mods: true }), null);
eq('the second press with the right button', gesture({}, { button: 2 }), null);
eq('the first press with the middle button', gesture({ button: 1 }), null);

// ---- the selection ----
eq('more than one item selected before the second click', gesture({}, { sole: false }), null);
eq('the second click on another item', gesture({}, { key: 'b' }), null);
eq('the first click on another item', gesture({ key: 'b' }), null);

// ---- the name, not the icon; what can be renamed ----
eq('the second click on the icon', gesture({}, { onName: false }), null);
eq('the first click on the icon, the second on the name: renames', gesture({ onName: false }), 'a');
eq('an item that cannot be renamed (read-only, Trash, mount point)', gesture({}, { canRename: false }), null);

// ---- the focus ----
eq('the first click only focused the window', gesture({ focused: false }), null);
eq('focused by the first click, then two slow clicks: the third click renames', (() => {
  const s = slowClick(); click(s, 1000, { focused: false, sole: false }); click(s, 1700); const at = click(s, 2400); return at === null ? null : s.due(at);
})(), 'a');
eq('the window lost the focus before the second click', gesture({}, { focused: false }), null);

// ---- cancelled while it waits ----
eq('a key while it waits', gesture({}, {}, 800, { between: (s) => s.cancel() }), null);
eq('waiting() while it waits', (() => { const s = slowClick(); click(s, 1000); click(s, 1800); return s.waiting(); })(), true);
eq('nothing waits after cancel()', (() => { const s = slowClick(); click(s, 1000); click(s, 1800); s.cancel(); return s.waiting(); })(), false);
eq('after a rename started, a third slow click doesn\'t start it again by itself', (() => {
  const s = slowClick(); click(s, 1000); const at = click(s, 1800); s.due(at); return click(s, 2600);
})(), null);

// ---- right to left: only distances count, so a mirrored layout behaves the same ----
eq('right to left (x mirrored)', gesture({ x: 1180 }, { x: 1180 - SLOP }), 'a');

console.log(`slowclick: ${ok} passed, ${bad} failed`);
process.exit(bad ? 1 : 0);
