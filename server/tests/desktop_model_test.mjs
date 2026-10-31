// desktop_model_test.mjs — the rules of Desktops (sdk/desktopmodel.js): what shows, what is in front
// and above all who has the keyboard. Regression: focus never lands on a hidden desktop.
//
//   node tests/desktop_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const M = await import(path.join(here, '../../sdk/desktopmodel.js'));
const S = await import(path.join(here, '../../sdk/sessionmodel.js'));
const {
  onDesktop, windowHidden, visibleOn, mountedOn, focusedOn, keyboardSurface, acceptsAppRequest, surfaceState, pickWindow, addDesktop, removeDesktop,
  nextDesktop, neighbor, arrowStep, swipeStep, cleanCount, cleanIndex, gridLayout, stripLayout, hitRect, overviewPlaceholder, MAX_DESKTOPS,
} = M;

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const a = JSON.stringify(got), b = JSON.stringify(want);
  if (a !== b) { bad++; console.error(`FAIL ${name}: ${a} != ${b}`); }
};
const ids = (l) => l.map((w) => w.id);
const surf = (id) => (String(id).startsWith('linux#') ? Number(String(id).slice(6)) : 0);

const win = (id, desktop, extra) => ({ id, desktop, ...extra });
const L = (sid, desktop, extra) => win(`linux#${sid}`, desktop, { args: { surface: sid }, ...extra });

// what shows
const list = [win('a', 1), L(5, 2), win('b', 2), win('m', 1, { minimized: true }), win('fs', 1, { fullscreen: true }), win('p', 2, { pip: { x: 0 } })];
eq('visible on 1', ids(visibleOn(list, 1)), ['a']);
eq('visible on 2', ids(visibleOn(list, 2)), ['linux#5', 'b']);
eq('a window without a desktop is on the first', ids(visibleOn([{ id: 'old' }], 1)), ['old']);
eq('pip is sticky', [onDesktop(list[5], 1), onDesktop(list[5], 2), onDesktop(list[5], 3)], [true, true, true]);
eq('hidden', [windowHidden(list[0], 2), windowHidden(list[0], 1), windowHidden(list[3], 1), windowHidden(null, 1)], [true, false, true, true]);

// a LordOS app stays mounted on a hidden desktop, a Linux window doesn't
eq('mounted on 1', ids(mountedOn(list, 1)), ['a', 'b']);
eq('mounted on 2', ids(mountedOn(list, 2)), ['a', 'linux#5', 'b']);
eq('overview brings the minimized of this desktop', ids(mountedOn(list, 1, true)), ['a', 'b', 'm']);
eq('...not of another', ids(mountedOn(list, 2, true)), ['a', 'linux#5', 'b']);

// the front window
eq('front of 1', focusedOn(list, 1).id, 'fs');
eq('front of 2 skips pip', focusedOn(list, 2).id, 'b');
eq('empty desktop has none', focusedOn(list, 3), null);
eq('pip alone is not in front', focusedOn([win('p', 1, { pip: {} })], 1), null);

// REGRESSION: the keyboard never lands on a hidden desktop, whatever the stack looks like
{
  const top = [L(7, 2), L(8, 1)];                               // the top of the stack is on desktop 1
  eq('keyboard: active desktop 1', keyboardSurface(top, 1, false, surf), 8);
  eq('keyboard: active desktop 2', keyboardSurface(top, 2, false, surf), 7);
  eq('keyboard: empty desktop has nobody', keyboardSurface(top, 3, false, surf), 0);
  eq('keyboard: shell surface open', keyboardSurface(top, 1, true, surf), 0);
  eq('keyboard: a LordOS app in front of a Linux one', keyboardSurface([L(8, 1), win('files', 1)], 1, false, surf), 0);
  eq('keyboard: pip above does not take it', keyboardSurface([L(8, 1), win('p', 2, { pip: {} })], 1, false, surf), 8);
  // every order of windows on 3 desktops: the surface chosen is always on the active desktop or 0
  const pool = [L(1, 1), L(2, 2), L(3, 3), win('x', 2), L(4, 1, { minimized: true }), L(6, 2, { minimized: true })];
  const perm = (a) => (a.length < 2 ? [a] : a.flatMap((x, i) => perm([...a.slice(0, i), ...a.slice(i + 1)]).map((p) => [x, ...p])));
  let wrong = 0;
  for (const order of perm(pool)) {
    for (const active of [1, 2, 3, 4]) {
      const s = keyboardSurface(order, active, false, surf);
      if (s && order.find((w) => w.args?.surface === s).desktop !== active) wrong++;
      if (s && order.find((w) => w.args?.surface === s).minimized) wrong++;
    }
  }
  eq('keyboard: 720 stacks × 4 desktops', wrong, 0);
}
eq('app request refused off this desktop', acceptsAppRequest(L(1, 2), 1), false);
eq('app request accepted on it', acceptsAppRequest(L(1, 2), 2), true);
eq('no window, no request', acceptsAppRequest(null, 1), false);

// the compositor's view
eq('parked and iconic', surfaceState(L(1, 2), 1), { maximized: false, parked: true, iconic: true });
eq('here', surfaceState(L(1, 2, { maximized: true }), 2), { maximized: true, parked: false, iconic: false });
eq('minimized is iconic, not parked', surfaceState(L(1, 2, { minimized: true }), 2), { maximized: false, parked: false, iconic: true });

// which window an app comes back with
{
  const app = [win('t1', 1), win('t2', 2), win('t3', 2, { minimized: true })];
  eq('pick here', pickWindow(app, 2).id, 't2');
  eq('pick elsewhere: the latest', pickWindow(app, 1).id, 't1');
  eq('pick none here', pickWindow(app, 3).id, 't2');
  eq('pick only minimized', pickWindow([win('t', 1, { minimized: true })], 2).id, 't');
  eq('pick nothing', pickWindow([], 1), null);
}

// adding and removing
{
  let s = { list: [1], active: 1 };
  const a = addDesktop(s); s = a.state;
  eq('new id', a.id, 2);
  eq('list', s.list, [1, 2]);
  eq('adding does not switch', s.active, 1);
  let full = { list: Array.from({ length: MAX_DESKTOPS }, (_, i) => i + 1), active: 1 };
  eq('no more than the maximum', addDesktop(full).id, null);
  eq('next of first', nextDesktop([1, 2, 3], 1), 2);
  eq('next of last is the previous', nextDesktop([1, 2, 3], 3), 2);
  eq('only one has no next', nextDesktop([1], 1), null);
  eq('unknown', nextDesktop([1], 9), null);
  eq('closing the active shows the next', removeDesktop({ list: [1, 2, 3], active: 2 }, 2), { state: { list: [1, 3], active: 3 }, target: 3 });
  eq('closing another keeps the active', removeDesktop({ list: [1, 2, 3], active: 1 }, 3), { state: { list: [1, 2], active: 1 }, target: 2 });
  eq('the last desktop stays', removeDesktop({ list: [1], active: 1 }, 1).target, null);
  eq('ids are not reused while in the list', addDesktop({ list: [1, 3], active: 1 }).id, 4);
}

// moving between desktops; left and right follow the layout
eq('next', neighbor([1, 2, 3], 2, 1), 3);
eq('prev', neighbor([1, 2, 3], 2, -1), 1);
eq('no wrap at the end', neighbor([1, 2], 2, 1), null);
eq('no wrap at the start', neighbor([1, 2], 1, -1), null);
eq('arrows ltr', [arrowStep('left', false), arrowStep('right', false)], [-1, 1]);
eq('arrows rtl', [arrowStep('left', true), arrowStep('right', true)], [1, -1]);
eq('swipes ltr', [swipeStep('left', false), swipeStep('right', false)], [1, -1]);
eq('swipes rtl', [swipeStep('left', true), swipeStep('right', true)], [-1, 1]);

// saved values come from a file
eq('count', [cleanCount(4), cleanCount(0), cleanCount(99), cleanCount('3'), cleanCount(undefined), cleanCount(2.5)], [4, 1, 1, 1, 1, 1]);
eq('index', [cleanIndex(3), cleanIndex(-1), cleanIndex(99), cleanIndex('1'), cleanIndex(undefined)], [3, 0, 0, 0, 0]);

// session restore keeps each window's desktop, and only a sane number
{
  const F = { x: 10, y: 20, w: 400, h: 300 };
  eq('desk kept', S.cleanEntry({ id: 'app:files', f: F, desk: 2 }).desk, 2);
  eq('first desktop is left out', 'desk' in S.cleanEntry({ id: 'app:files', f: F, desk: 0 }), false);
  eq('too large', 'desk' in S.cleanEntry({ id: 'app:files', f: F, desk: 500 }), false);
  eq('not a number', 'desk' in S.cleanEntry({ id: 'app:files', f: F, desk: '2' }), false);
  eq('negative', 'desk' in S.cleanEntry({ id: 'app:files', f: F, desk: -1 }), false);
}

// the overview's layout
{
  const a = { x: 0, y: 28, w: 1280, h: 700 };
  const g = gridLayout([{ id: 'x', w: 800, h: 600 }, { id: 'y', w: 300, h: 900 }, { id: 'z', w: 2000, h: 1000 }], a);
  let inside = true;
  for (const p of g.values()) if (p.x < a.x || p.y < a.y || p.x + p.w > a.x + a.w || p.y + p.h > a.y + a.h) inside = false;
  eq('every window fits', inside, true);
  eq('never larger than 85 %', [...g.values()].every((p) => p.scale <= 0.85), true);
  eq('empty', gridLayout([], a).size, 0);
  const s = stripLayout([1, 2, 3], a, { canAdd: true });
  eq('cards', s.cards.map((c) => c.id), [1, 2, 3]);
  eq('in order', s.cards[0].x < s.cards[1].x && s.cards[1].x < s.add.x, true);
  const r = stripLayout([1, 2, 3], a, { canAdd: true, rtl: true });
  eq('mirrored', r.cards[0].x > r.cards[1].x && r.cards[2].x > r.add.x, true);
  eq('mirror keeps the box', Math.min(...r.cards.map((c) => c.x), r.add.x), Math.min(...s.cards.map((c) => c.x), s.add.x));
  eq('full: no add button', stripLayout([1, 2, 3, 4, 5, 6, 7, 8, 9], a, { canAdd: false }).add, null);
  const nine = stripLayout([1, 2, 3, 4, 5, 6, 7, 8, 9], { x: 0, y: 0, w: 1024, h: 700 }, { canAdd: false });
  eq('nine fit on 1024', nine.cards[8].x + nine.cards[8].w <= 1024 && nine.cards[0].x >= 0, true);
  eq('hit', [hitRect({ x: 1, y: 1, w: 2, h: 2 }, 2, 2), hitRect({ x: 1, y: 1, w: 2, h: 2 }, 3, 2), hitRect(null, 0, 0)], [true, false, false]);
}

// lord.setStyle takes native properties only: a CSS transform there is `js: unsupported style 'transform'`
// and does nothing (the Desktops animation's slide)
{
  const fs = await import('node:fs');
  const sent = (dir, out = []) => {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
      const p = path.join(dir, e.name);
      if (e.isDirectory()) { if (e.name !== 'node_modules') sent(p, out); } else if (/\.jsx?$/.test(e.name) && /\b(setStyle|style|set)\([^,()]+(\.id)?,\s*'transform'/.test(fs.readFileSync(p, 'utf8'))) out.push(p);
    }
    return out;
  };
  const root = path.join(here, '../..');
  eq('nothing sends transform to setStyle', ['sdk', 'themes', 'apps', 'runtime'].flatMap((d) => sent(path.join(root, d))).map((p) => path.relative(root, p)), []);
}

// a minimized Linux window has a placeholder behind its body in the overview, and only there; a LordOS app's body would show it through
const lin = { minimized: true, args: { surface: 5 } };
eq('placeholder: minimized Linux window, in the overview', [overviewPlaceholder(lin, true), overviewPlaceholder(lin, false), overviewPlaceholder({ args: { surface: 5 } }, true), overviewPlaceholder({ minimized: true }, true)], [true, false, false, false]);

if (bad) { console.error(`desktop-model-test: ${bad} of ${n} failed`); process.exit(1); }
console.log(`desktop-model-test: ${n} checks passed`);
