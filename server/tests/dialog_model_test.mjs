// dialog_model_test.mjs — a Linux app's dialog kept with the window it belongs to (sdk/dialogmodel.js).
import { ownerOf, overParent, followParents } from '../../sdk/dialogmodel.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};
const win = (id, x, y, more = {}) => ({ id, x, y, w: 400, h: 300, desktop: 1, minimized: false, ...more });
const dlg = (id, parent, x, y, modal = true, more = {}) => win(id, x, y, { w: 200, h: 100, args: { parent, modal, dialog: true }, ...more });
const ids = (ws) => ws.map((w) => w.id);
// the subscriber in sdk/wayland.js: each change against the one before, until it settles
const settle = (prev, next) => {
  for (let i = 0; i < 10; i++) {
    const kept = followParents(prev, next);
    if (kept === next) return next;
    prev = next;
    next = kept;
  }
  return null;
};

const area = { x: 0, y: 30, w: 1280, h: 690 };
eq('owner of a dialog', ownerOf(dlg('linux#2', 'linux#1', 0, 0)), 'linux#1');
eq('a window has no owner', ownerOf(win('linux#1', 0, 0)), null);
eq('centered over the parent', overParent({ x: 100, y: 100, w: 400, h: 300 }, 200, 100, area), { x: 200, y: 200, w: 200, h: 100 });
eq('kept inside the work area at its left edge', overParent({ x: -300, y: 100, w: 400, h: 300 }, 300, 100, area).x, 0);
eq('kept under the menu bar', overParent({ x: 100, y: 0, w: 400, h: 40 }, 200, 200, area).y, 30);
eq('kept inside at the bottom right', overParent({ x: 1200, y: 650, w: 400, h: 300 }, 200, 100, area), { x: 1080, y: 620, w: 200, h: 100 });
eq('no bigger than the work area', overParent({ x: 0, y: 30, w: 400, h: 300 }, 2000, 1000, area), { x: 0, y: 30, w: 1280, h: 690 });

const A = win('linux#1', 100, 100), D = dlg('linux#2', 'linux#1', 200, 200), O = win('other', 500, 300);
const base = [A, D, O];
eq('nothing to do: the same list', followParents(base, base) === base, true);
eq('an unrelated change: the same list', (() => { const n = [A, D, { ...O, x: 9 }]; return followParents(base, n) === n; })(), true);

// the parent moves: the dialog goes along
{
  const n = settle(base, [{ ...A, x: 150, y: 80 }, D, O]);
  eq('moved with its parent', [n[1].x, n[1].y], [250, 180]);
}
// both moved in the same change (the dialog itself was dragged): the dialog keeps its own place
{
  const n = settle(base, [{ ...A, x: 150 }, { ...D, x: 600 }, O]);
  eq('a dialog that moved itself stays', n[1].x, 600);
}
// a dialog of a dialog follows too
{
  const G = dlg('linux#3', 'linux#2', 250, 250);
  const l = [A, D, G];
  const n = settle(l, [{ ...A, x: 110, y: 110 }, D, G]);
  eq('a dialog of a dialog follows', [n[1].x, n[2].x, n[2].y], [210, 260, 260]);
}
// the parent is brought forward: a modal dialog comes right above it
{
  const n = settle(base, [D, O, A]);
  eq('a modal dialog stays above its parent', ids(n), ['other', 'linux#1', 'linux#2']);
}
{
  const N = dlg('linux#2', 'linux#1', 200, 200, false);
  const n = settle([A, N, O], [N, O, A]);
  eq('a dialog that is not modal may go behind', ids(n), ['linux#2', 'other', 'linux#1']);
}
// minimized and back with the parent
{
  const n = settle(base, [{ ...A, minimized: true }, D, O]);
  eq('minimized with its parent', n[1].minimized, true);
  const m = settle(n, [n[1], O, { ...n[0], minimized: false }]);
  eq('back with its parent, and above it', [ids(m), m[2].minimized], [['other', 'linux#1', 'linux#2'], false]);
}
{
  const n = settle(base, [A, { ...D, minimized: true }, O]);
  eq('a modal dialog minimized takes its parent', n[0].minimized, true);
  const m = settle(n, [n[0], O, { ...n[1], minimized: false }]);
  eq('and brings it back', [m[0].minimized, m[2].minimized], [false, false]);
}
{
  const N = dlg('linux#2', 'linux#1', 200, 200, false);
  const n = settle([A, N, O], [A, { ...N, minimized: true }, O]);
  eq('a dialog that is not modal is minimized alone', n[0].minimized, false);
}
// another desktop
{
  const n = settle(base, [{ ...A, desktop: 2 }, D, O]);
  eq('to the desktop of its parent', n[1].desktop, 2);
}
// a maximized dialog does not move by the delta
{
  const M = { ...D, maximized: true };
  const n = settle([A, M, O], [{ ...A, x: 300 }, M, O]);
  eq('a maximized dialog stays', n[1].x, 200);
}
// the parent closed: nothing to follow
{
  const n = [D, O];
  eq('a dialog without its parent', followParents(base, n) === n, true);
}

console.log(`dialog-model: ${ok} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
