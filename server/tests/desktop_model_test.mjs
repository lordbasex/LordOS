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
  onDesktop, windowHidden, visibleOn, mountedOn, focusedOn, keyboardSurface, acceptsAppRequest, activationOutcome, surfaceState, pickWindow, addDesktop, removeDesktop,
  nextDesktop, neighbor, arrowStep, swipeStep, cleanCount, cleanIndex, gridLayout, stripLayout, hitRect, overviewPlaceholder, MAX_DESKTOPS,
  cleanName, cleanNames, namesByPosition, withName, moveDesktopTo, stripSlot, MAX_NAME, isNoticeWindow, noticeFrame, NOTICE_MARGIN,
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

// a LordOS app stays mounted when it is minimized or on a hidden desktop, a Linux window doesn't
eq('mounted on 1', ids(mountedOn(list, 1)), ['a', 'b', 'm']);
eq('mounted on 2', ids(mountedOn(list, 2)), ['a', 'linux#5', 'b', 'm']);
const iconic = [L(5, 1, { minimized: true }), L(6, 2, { minimized: true }), L(7, 1)];
eq('a minimized Linux window has no node', ids(mountedOn(iconic, 1)), ['linux#7']);
eq('overview brings the minimized of this desktop', ids(mountedOn(iconic, 1, true)), ['linux#5', 'linux#7']);
eq('...not of another', ids(mountedOn(iconic, 2, true)), ['linux#6']);

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
// xdg-activation: only a granted token brings a window forward (a hidden desktop's too); anything else asks for attention
eq('activation: granted focuses, on any desktop', [activationOutcome(L(1, 2), null, true), activationOutcome(L(1, 1), 'linux#1', true)], ['focus', 'focus']);
eq('activation: refused never focuses', [activationOutcome(L(1, 2), 'linux#9', false), activationOutcome(L(1, 2, { minimized: true }), null, false)], ['attention', 'attention']);
eq('activation: refused but already in front, nothing', activationOutcome(L(1, 1), 'linux#1', false), null);
eq('activation: no window', activationOutcome(null, null, true), null);

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

// names: typed, or read from the settings file
eq('name kept', cleanName('Work'), 'Work');
eq('name trimmed and one line', cleanName('  Mail \n and\tchat  '), 'Mail and chat');
eq('control characters go', cleanName('a\u0000b\u001bc'), 'a b c');
eq('not a string', [cleanName(null), cleanName(3), cleanName(undefined), cleanName({})], ['', '', '', '']);
eq('blank is the default', cleanName('   '), '');
eq('at most MAX_NAME characters', [...cleanName('x'.repeat(100))].length, MAX_NAME);
eq('cut by characters, not bytes', cleanName('ñ'.repeat(40)), 'ñ'.repeat(MAX_NAME));
eq('saved names fit the count', cleanNames(['A', ' B ', 7], 4), ['A', 'B', '', '']);
eq('saved names, too many', cleanNames(['A', 'B', 'C'], 2), ['A', 'B']);
eq('saved names, not an array', cleanNames('A', 2), ['', '']);
eq('by position', namesByPosition([3, 1, 2], { 1: 'Mail', 3: 'Code' }), ['Code', 'Mail']);
eq('by position keeps gaps', namesByPosition([1, 2, 3], { 3: 'Music' }), ['', '', 'Music']);
eq('none named saves nothing', namesByPosition([1, 2], {}), null);
eq('rename', withName({}, 2, ' Work '), { 2: 'Work' });
eq('empty name: back to the default', withName({ 2: 'Work', 1: 'Home' }, 2, ''), { 1: 'Home' });
{
  const before = { 1: 'A' };
  withName(before, 1, 'B');
  eq('renaming does not change the old names', before, { 1: 'A' });
}

// reordering: a desktop moves, its id (so its windows and its name) with it
eq('move to the end', moveDesktopTo([1, 2, 3], 1, 2), [2, 3, 1]);
eq('move to the start', moveDesktopTo([1, 2, 3], 3, 0), [3, 1, 2]);
eq('move one step', moveDesktopTo([1, 2, 3, 4], 2, 2), [1, 3, 2, 4]);
eq('index clamped', [moveDesktopTo([1, 2, 3], 1, 99), moveDesktopTo([1, 2, 3], 3, -5)], [[2, 3, 1], [3, 1, 2]]);
{
  const l = [1, 2, 3];
  eq('same place: the same list', moveDesktopTo(l, 2, 1) === l, true);
  eq('unknown desktop: the same list', moveDesktopTo(l, 9, 0) === l, true);
  eq('not a position: the same list', moveDesktopTo(l, 1, '2') === l, true);
}
{
  // windows name their desktop, so after a reorder each one is still on the same desktop, now at another position
  const ws = [win('a', 1), win('b', 3)];
  const order = moveDesktopTo([1, 2, 3], 3, 0);
  eq('windows go with their desktop', ws.map((w) => order.indexOf(w.desktop)), [1, 0]);
  eq('and their names', namesByPosition(order, { 3: 'Code' }), ['Code']);
  // what a new session reads back: names by position onto fresh ids 1..n
  const back = cleanNames(namesByPosition(order, { 3: 'Code', 1: 'Mail' }), 3);
  eq('names come back in the new order', back, ['Code', 'Mail', '']);
}
{
  const a = { x: 0, y: 28, w: 1280, h: 700 };
  const st = stripLayout([1, 2, 3], a, { canAdd: true });
  const mid = (c) => c.x + c.w / 2;
  eq('slot under each card', st.cards.map((c) => stripSlot(st.cards, mid(c))), [0, 1, 2]);
  eq('slot before the first and after the last', [stripSlot(st.cards, -500), stripSlot(st.cards, 5000)], [0, 2]);
  const rt = stripLayout([1, 2, 3], a, { canAdd: true, rtl: true });
  eq('slot in a mirrored strip', [stripSlot(rt.cards, 5000), stripSlot(rt.cards, -500)], [0, 2]);
  eq('no cards', stripSlot([], 10), -1);
}

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
  // a maximized window is drawn at the work area's size: the grid scales by that, not by its own w and h
  const big = gridLayout([{ id: 's', w: 960, h: 640 }, { id: 'm', w: 1280, h: 800, maximized: true }], a, (w) => (w.maximized ? { w: a.w, h: a.h } : w)).get('m');
  eq('a maximized window stays in the area', big.x + big.w <= a.x + a.w && big.y + big.h <= a.y + a.h, true);
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

// A browser's own notification popup (Chrome without a notification service: an xdg toplevel with no app id,
// title or parent) floats and never has the keyboard; a real window, a dialog or an X11 window doesn't.
eq('notice: Chrome\'s popup', isNoticeWindow({ appId: '', title: '', width: 360, height: 102, csd: true, dialog: false, fixed: false, parent: 0, modal: false }), true);
// a nameless window bigger than a notification is a normal window: it can't float over everything
eq('notice: a big nameless window is not one', isNoticeWindow({ appId: '', title: '', width: 1280, height: 800 }), false);
eq('notice: just over the height is not one', isNoticeWindow({ appId: '', title: '', width: 360, height: 301 }), false);
eq('notice: the frame never passes the cap', JSON.stringify((({ w, h }) => [w, h])(noticeFrame({ x: 0, y: 0, w: 1920, h: 1080 }, [], 2000, 2000))), '[480,300]');
eq('notice: not a window with an app id, a title, a parent, a dialog or X11', [
  isNoticeWindow({ appId: 'google-chrome', title: '' }), isNoticeWindow({ appId: '', title: 'Picture in picture' }),
  isNoticeWindow({ appId: '', title: '', parent: 4 }), isNoticeWindow({ appId: '', title: '', dialog: true }),
  isNoticeWindow({ appId: '', title: '', x11: true }), isNoticeWindow(null)], [false, false, false, false, false, false]);
{
  const A = { x: 0, y: 30, w: 1280, h: 700 }, M = NOTICE_MARGIN;
  const f1 = noticeFrame(A, [], 360, 102);
  eq('notice: top right of the work area', f1, { x: 1280 - M - 360, y: 30 + M, w: 360, h: 102 });
  eq('notice: right to left: top left', noticeFrame(A, [], 360, 102, true).x, M);
  const f2 = noticeFrame(A, [f1], 360, 128);
  eq('notice: the next one goes below', f2.y, f1.y + f1.h + M / 2);
  eq('notice: back at the top when the column is full', noticeFrame(A, [{ x: f1.x, y: 600, w: 360, h: 100 }], 360, 102).y, 30 + M);
  eq('notice: one on another display is not counted', noticeFrame(A, [{ x: 2000, y: 42, w: 360, h: 102 }], 360, 102).y, 30 + M);
  eq('notice: never larger than the area', noticeFrame({ x: 0, y: 0, w: 300, h: 200 }, [], 4000, 4000), { x: M, y: M, w: 300 - 2 * M, h: 200 - 2 * M });
  // as picture-in-picture: in front, yet never the front window, so the keyboard stays with the browser
  const ws = [L(3, 1), L(7, 1, { pip: f1, args: { surface: 7, notice: true } })];
  eq('notice: the keyboard stays with the window below', keyboardSurface(ws, 1, false, surf), 3);
  eq('notice: shown on every desktop', onDesktop(ws[1], 2), true);
}

// covered windows (0.8.8.8): a Linux window wholly inside an opaque window above it, on the showing desktop
{
  const S = (id, x, y, w, h, more = {}) => ({ id, x, y, w, h, desktop: 1, args: { surface: id.length }, ...more });
  const N = (id, x, y, w, h, more = {}) => ({ id, x, y, w, h, desktop: 1, ...more });
  const frame = (w) => (w.maximized ? { x: 0, y: 30, w: 1280, h: 700 } : w);
  const opaque = (w) => !!w.args?.web || (!w.args?.surface && !w.transparent);
  const ids = (list, ov = false) => [...M.coveredIds(list, 1, ov, frame, opaque)].sort();
  const wa = S('linux#1', 100, 100, 600, 400);
  eq('covered: under a larger LordOS window', ids([wa, N('files#1', 50, 50, 800, 600)]), ['linux#1']);
  eq('covered: not when it sticks out', ids([wa, N('files#1', 150, 50, 800, 600)]), []);
  eq('covered: not by a window below it', ids([N('files#1', 50, 50, 800, 600), wa]), []);
  eq('covered: not by a transparent LordOS app', ids([wa, N('term#1', 0, 0, 1280, 800, { transparent: true })]), []);
  eq('covered: not by another Linux window (it may be translucent)', ids([wa, S('linux#22', 0, 0, 1280, 800)]), []);
  eq('covered: by a web app window', ids([wa, S('linux#33', 0, 0, 1280, 800, { args: { surface: 3, web: true } })]), ['linux#1']);
  eq('covered: by a maximized window', ids([wa, N('files#1', 0, 0, 10, 10, { maximized: true })]), ['linux#1']);
  eq('covered: a maximized one under another maximized', ids([S('linux#1', 0, 0, 10, 10, { maximized: true }), N('files#1', 0, 0, 10, 10, { maximized: true })]), ['linux#1']);
  eq('covered: not by a minimized window', ids([wa, N('files#1', 0, 0, 1280, 800, { minimized: true })]), []);
  eq('covered: not by one on another desktop', ids([wa, N('files#1', 0, 0, 1280, 800, { desktop: 2 })]), []);
  eq('covered: never in the overview', ids([wa, N('files#1', 0, 0, 1280, 800)], true), []);
  eq('covered: LordOS windows are never listed', ids([N('notes#1', 100, 100, 10, 10), N('files#1', 0, 0, 1280, 800)]), []);
}

if (bad) { console.error(`desktop-model-test: ${bad} of ${n} failed`); process.exit(1); }
console.log(`desktop-model-test: ${n} checks passed`);
