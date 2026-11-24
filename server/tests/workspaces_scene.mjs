// workspaces_scene.mjs — the scene of tests/workspaces_test.mjs (not run by itself), 0.8.9: Lord Workspaces over a stub
// `lord` with three monitors side by side, the main one in the middle. It checks a group frame per display, the spine on
// the main one, each display's work area, no timer per second at idle, tiling per display (split on the main one, single
// on the right), a window moved by hand leaving its tile, Save layout, a monitor unplugged (its window joins the main
// display's tiles) and plugged back (it goes back to its saved display), a new activity, Search on the display it is
// opened for, and the Crystal / Opaque styles. Prints one line per check: "ok <name>" or "FAIL <name> <detail>".

const live = new Map();
const realSet = globalThis.setInterval, realClear = globalThis.clearInterval;
globalThis.setInterval = (fn, ms, ...a) => { const id = realSet(fn, ms, ...a); live.set(id, ms); return id; };
globalThis.clearInterval = (id) => { live.delete(id); realClear(id); };
const intervals = (ms) => [...live.values()].filter((v) => ms == null || v === ms).length;

const React = (await import('react')).default;
const rt = await import('@lordos/runtime');
const sdk = await import('@lordos/sdk');
await import('../../themes/workspaces/index.jsx');
const { displayPlan, SPINE_W } = await import('../../themes/workspaces/model/geometry.js');
const { tileRects } = await import('../../themes/workspaces/model/layouts.js');
const tiler = await import('../../themes/workspaces/tiler.js');
const state = await import('../../themes/workspaces/state.js');
const bars = await import('../../themes/workspaces/barfocus.js');

const { texts, styles, parents, removed, props } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 30; i++) await new Promise((res) => setImmediate(res)); await new Promise((res) => setTimeout(res, 5)); for (let i = 0; i < 10; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, got, want) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  console.log(g === w ? `ok ${name}` : `FAIL ${name} got ${g}, want ${w}`);
};
const attached = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return parents.has(id); };

// three monitors of one model, as lord-server describes them: the model in name, the port in connector
const L = { id: 41, name: 'S27F350', connector: 'DP-1', x: 0, y: 0, w: 1920, h: 1080, scale: 100, main: false };
const M = { id: 42, name: 'S27F350', connector: 'DP-2', x: 1920, y: 0, w: 1920, h: 1080, scale: 100, main: true };
const R = { id: 43, name: 'S27F350', connector: 'HDMI-A-1', x: 3840, y: 0, w: 1920, h: 1080, scale: 100, main: false };
const plug = async (list, mirror = false) => {
  lord.screen.displays = list;
  lord.screen.mirror = mirror;
  lord.screen.width = Math.max(...list.map((d) => d.x + d.w));
  lord.screen.height = Math.max(...list.map((d) => d.y + d.h));
  globalThis.__lordScreen();
  await flush();
};
function Shell() {
  const [rev, set] = React.useState(0);
  React.useEffect(() => rt.onScreenChange(() => set((v) => v + 1)), []);
  return React.cloneElement(globalThis.__shellElement, { screenRev: rev });
}
await plug([L, M, R]);
rt.render(h(Shell));
await flush();

const plan = (d) => displayPlan(d, { primary: !!d.main });
const at = (r) => [...styles].filter(([id, s]) => s.position === 'absolute' && s.left === r.x && s.top === r.y && s.width === r.w && s.height === r.h && attached(id));
check('a group frame on each display', [L, M, R].map((d) => at(plan(d).frame).length >= 1), [true, true, true]);
check('each display\'s work area is its frame\'s inside', [L, M, R].map((d) => sdk.workAreaOf(d)), [L, M, R].map((d) => plan(d).work));
check('the spine on the main display only', [L, M, R].map((d) => [...styles].filter(([id, s]) => s.width === SPINE_W.expanded && s.left === d.x + 12 && attached(id)).length > 0), [false, true, false]);
check('idle: no timer every second', intervals(1000), 0);
const time = sdk.formatTime(new Date());
check('one clock', [...texts].filter(([id, v]) => v === time && attached(id)).length, 1);

// tiling: three windows on the main display in "main and side", one on the right one alone
const desk = sdk.activeDesktop.get();
const frameOf = (id) => { const w = sdk.wm.get(id); return w && { x: w.x, y: w.y, w: w.w, h: w.h }; };
const a = sdk.wm.open('files', { frame: { x: M.x + 300, y: 300, w: 600, h: 400 } });
const b = sdk.wm.open('terminal', { frame: { x: M.x + 400, y: 320, w: 600, h: 400 } });
const c = sdk.wm.open('calculator', { frame: { x: M.x + 500, y: 340, w: 300, h: 400 } });
const s = sdk.wm.open('store', { frame: { x: R.x + 200, y: 200, w: 700, h: 500 } });
await flush();
check('free layout: windows stay where they opened', frameOf(a), { x: M.x + 300, y: 300, w: 600, h: 400 });
tiler.setLayout(desk, 'DP-2', 'split');
tiler.setLayout(desk, 'HDMI-A-1', 'single');
await flush();
const want = tileRects('split', plan(M).work, 3, 2 / 3);
check('split on the main display', [a, b, c].map(frameOf), want);
check('single on the right display', frameOf(s), plan(R).work);
check('the left display untouched (free)', tiler.layoutOf(desk, 'DP-1').layout, 'free');

// resized by hand: out of its tile, floating; the others take the room
sdk.wm.setFrame(c, { ...frameOf(c), x: frameOf(c).x + 20, w: frameOf(c).w - 80 });
await flush();
check('resized by hand: it floats', tiler.isFloating(c), true);
check('the others keep their tiles; main and side keeps three slots, the floated one\'s now free', [a, b].map(frameOf), tileRects('split', plan(M).work, 3, 2 / 3).slice(0, 2));
tiler.setFloating(c, false);
await flush();
check('Tile this window: back in a tile', frameOf(c), tileRects('split', plan(M).work, 3, 2 / 3)[2]);

// Save layout: per display, the layout and the app of each tile
const saved = tiler.saveLayout(desk);
check('saved: main display', saved['DP-2'], { layout: 'split', ratio: 2 / 3, apps: ['files', 'terminal', 'calculator'] });
check('saved: right display', saved['HDMI-A-1'].apps, ['store']);
await flush();
check('saved in settings', sdk.settings.get().workspaces?.activities?.[0]?.layouts?.['HDMI-A-1']?.apps, ['store']);

// the right monitor unplugged: its window joins the main display's tiles; plugged back, it returns
await plug([L, M]);
await flush();
const inside = (f, r) => f.x >= r.x && f.y >= r.y && f.x + f.w <= r.x + r.w + 1 && f.y + f.h <= r.y + r.h + 1;
check('unplugged: the store window on the main display, in a tile', inside(frameOf(s), plan(M).work), true);
check('unplugged: four tiles on the main display', tiler.tiled.get().counts['DP-2'], 4);
await plug([L, M, R]);
await flush();
check('plugged back: the store window back on its saved display', frameOf(s), plan(R).work);
check('plugged back: three tiles on the main display again', [a, b, c].map(frameOf), tileRects('split', plan(M).work, 3, 2 / 3));

// a new activity: shown at once, with its own icon and colour, kept in settings by position
const before = sdk.desktopList.get().length;
const nd = state.newActivity();
await flush();
check('new activity: one more desktop, in view', [sdk.desktopList.get().length, sdk.activeDesktop.get()], [before + 1, nd]);
check('new activity: set up, a different colour', [state.activityOf(nd).setup, state.activityOf(nd).color !== state.activityOf(desk).color], [true, true]);
check('new activity: the old activity\'s windows hidden', [a, b, c, s].every((id) => sdk.wm.get(id).desktop === desk), true);
sdk.switchDesktop(desk);
await flush();
check('back: the tiles kept', [a, b, c].map(frameOf), tileRects('split', plan(M).work, 3, 2 / 3));

// the Control Atlas: a section each for the look, tiling, activities, displays, sound, network, energy and security
state.panel.set({ name: 'control', display: M.id });
await flush();
const shownText = (v) => [...texts].some(([id, x]) => x === v && attached(id));
check('control atlas: its sections', [sdk.t('settings.section.appearance'), sdk.t('shell.ws.atlas.tiling'), sdk.t('shell.ws.spine.activities'), sdk.t('settings.section.displays'),
  sdk.t('settings.section.sound')].map(shownText), [true, true, true, true, true]);
check('control atlas: the layout in view named', shownText(sdk.t(`shell.ws.layout.${tiler.layoutOf(desk, 'DP-2').layout}`)), true);
state.panel.set(null);
await flush();

// the bars' keyboard: ⌃F2 starts at the activity in view, the arrows walk on, a ring shows, Escape gives it back
bars.enterBars(`pill:${sdk.activeDesktop.get()}`);
await flush();
check('bars: the activity in view first', bars.barFocus.get(), `pill:${sdk.activeDesktop.get()}`);
bars.barKey({ key: 'ArrowDown' });
await flush();
const ringed = () => [...styles].filter(([id, st]) => st.left === -3 && st.top === -3 && st.pointerEvents === 'none' && attached(id)).length;
check('bars: the next control, with its ring', [bars.barFocus.get() !== `pill:${sdk.activeDesktop.get()}`, ringed()], [true, 1]);
bars.barKey({ key: 'Escape' });
await flush();
check('bars: Escape gives the keyboard back', [bars.barFocus.get(), ringed()], [null, 0]);

// the status items' panels (Cloud, Cleaner) open where the notifications do: under the band, at its far edge
{
  const si = await import('../../sdk/statusitems.js');
  const band = plan(M).top;
  const box = [band.x + band.w - 300, band.y + 6, 26, 32];
  globalThis.__lordTest.layouts.set(9999, box);
  check('status panels: under the band, its far edge on the band\'s', si.statusPanelAt({ current: { id: 9999 } }),
    { position: 'absolute', top: band.y + band.h + 8 - box[1], right: box[0] + box[2] - (band.x + band.w) });
  globalThis.__lordTest.layouts.delete(9999);
}

// the shortcuts' help: the theme's own marked, the shared ones (Search) and the pointer's, searchable
{
  const help = await import('../../themes/workspaces/components/ShortcutsHelp.jsx');
  help.shortcutsHelp.set(true);
  await flush();
  const shownT = (v) => [...texts].some(([id, x]) => x === v && attached(id));
  check('help: open, with the theme\'s, the shared and the pointer\'s', [sdk.t('shell.ws.shortcuts.bars'), sdk.t('shell.shortcuts.search'), sdk.t('shell.ws.help.m.rename')].map(shownT), [true, true, true]);
  check('help: the theme\'s tagged', shownT(sdk.t('shell.ws.help.theme')), true);
  check('help: keycaps split', help.keycaps('❖⌥D'), ['❖', '⌥', 'D']);
  help.shortcutsHelp.set(false);
  await flush();
  check('help: closed', shownT(sdk.t('shell.ws.help.title')), false);
}

// Search on the display it opens for (the one under the pointer)
state.panel.set({ name: 'search', mode: 'apps', display: L.id });
await flush();
check('search: on the left display', [...styles].filter(([id, st]) => st.width === 680 && st.left === L.x + Math.round((L.w - 680) / 2) && attached(id)).length, 1);
state.panel.set(null);
await flush();

// bounded whatever the apps do (0.8.9): a pass with nothing new moves nothing; an app that answers every size it is
// given with another one (a minimum size, a character grid) stays in its tile and is asked once, never in a loop
let notes = 0;
const offNotes = sdk.windows.subscribe(() => { notes++; });
for (let i = 0; i < 5; i++) tiler.retile();
await flush();
check('idempotent: passes with nothing new change nothing', notes, 0);
const stubborn = sdk.wm.open('extra1', { frame: { x: M.x + 600, y: 300, w: 500, h: 400 } });
let answers = 0;
const offAnswer = sdk.windows.subscribe(() => {
  const w = sdk.wm.get(stubborn);
  if (!w || answers > 500) return;
  // every size the tiler gives (even widths here) is answered with another one (odd), as a geometry event does
  if (w.w % 2 === 0) { answers++; setTimeout(() => sdk.wm.resize(stubborn, w.w - 3 - 2 * (answers % 3), w.h - 5), 0); }
});
notes = 0;
for (let i = 0; i < 20; i++) await flush();
offAnswer();
check('a stubborn app: still in its tile', tiler.isFloating(stubborn), false);
check('a stubborn app: a bounded number of changes', notes < 12 && answers < 6, true);
check('a stubborn app: counted on the main display', tiler.tiled.get().counts['DP-2'], 4);
check('a stubborn app: it did answer, once', answers, 1);
notes = 0;
for (let i = 0; i < 5; i++) tiler.retile();
await flush();
check('and then idle', notes, 0);
offNotes();
sdk.wm.close(stubborn, true);
await flush();

// the layout picker, clicked: the choice changes only the display whose header it was opened from
{
  const T = (k) => sdk.t(`shell.ws.layout.${k}`);
  const textNode = (v) => [...texts].filter(([id, x]) => x === v && attached(id)).map(([id]) => id);
  const click = async (id) => { const p = parents.get(id) ?? id; globalThis.__lordEvent('down', p, 0, 0, 0); globalThis.__lordEvent('up', p, 0, 0, 0); await flush(); };
  const before = ['DP-1', 'DP-2', 'HDMI-A-1'].map((k) => tiler.layoutOf(desk, k).layout);
  const btn = textNode(T(before[0]));
  check('picker: one header button on the left display', btn.length, 1);
  await click(btn[0]);
  const opt = textNode(T('halves'));
  check('picker: open, with Two halves', opt.length, 1);
  if (opt.length) await click(opt[0]);
  check('picker: only the left display changed', ['DP-1', 'DP-2', 'HDMI-A-1'].map((k) => tiler.layoutOf(desk, k).layout), ['halves', before[1], before[2]]);
  tiler.setLayout(desk, 'DP-1', 'free');
  await flush();
}

// the padlock of one display: its windows can't be moved, resized, maximized, snapped or closed by the person (minimize stays),
// and the frame's header keeps only the padlock; the other displays are untouched
{
  const T = (k) => sdk.t(`shell.ws.layout.${k}`);
  const shown = (v) => [...texts].filter(([id, x]) => x === v && attached(id)).length;
  const k1 = sdk.wm.open('extra30', { frame: { x: L.x + 200, y: 200, w: 600, h: 400 } });
  const k2 = sdk.wm.open('extra31', { frame: { x: M.x + 200, y: 200, w: 600, h: 400 } });
  await flush();
  const f0 = frameOf(k1);
  const freeBefore = shown(T('free'));
  tiler.setLocked(desk, 'DP-1', true);
  await flush();
  check('lock: only that display', [tiler.isLocked(desk, 'DP-1'), tiler.isLocked(desk, 'DP-2'), tiler.isLocked(desk, 'HDMI-A-1')], [true, false, false]);
  check('lock: the window is locked, the one on the main display is not', [sdk.isWindowLocked(k1), sdk.isWindowLocked(k2)], [true, false]);
  sdk.wm.move(k1, L.x + 600, 500); sdk.wm.resize(k1, 300, 200); sdk.wm.toggleMaximize(k1); sdk.wm.snap(k1, 'left'); sdk.wm.snapKey(k1, 'right'); sdk.wm.center(k1);
  await flush();
  check('lock: move, resize, maximize, snap and center refused', [frameOf(k1), !!sdk.wm.get(k1).maximized], [f0, false]);
  // minimize stays: another app can take the slot while the layout holds
  sdk.wm.minimize(k1);
  await flush();
  check('lock: minimize still works', !!sdk.wm.get(k1).minimized, true);
  sdk.wm.focus(k1);
  sdk.wm.patch?.(k1, { minimized: false });
  if (sdk.wm.get(k1).minimized) sdk.windows.set((ws) => ws.map((w) => (w.id === k1 ? { ...w, minimized: false } : w)));
  await flush();
  check('lock: close refused (unless forced)', [sdk.wm.close(k1), !!sdk.wm.get(k1)], [false, true]);
  check('lock: the header keeps only the padlock', shown(T('free')), freeBefore - 1);
  sdk.wm.minimize(k2);
  await flush();
  check('lock: the main display still minimizes', !!sdk.wm.get(k2).minimized, true);
  const saved = tiler.saveLayout(desk);
  check('lock: Save layout keeps the padlock', saved['DP-1'].locked, true);
  tiler.setLocked(desk, 'DP-1', false);
  await flush();
  sdk.wm.move(k1, L.x + 300, 300);
  await flush();
  check('unlock: it moves again', [frameOf(k1).x, frameOf(k1).y], [L.x + 300, 300]);
  check('unlock: the header back', shown(T('free')), freeBefore);
  for (const id of [k1, k2]) sdk.wm.close(id, true);
  await flush();
}

// the display's bar (its frame header): every window of the activity on that display, open and minimized, in the order
// they opened; a minimized one has a line under its icon; a click brings an open one forward and a minimized one back
// where it was; no "Minimized" label any more
{
  const hiddenUp = (id) => { for (let n = id; n; n = parents.get(n)) if (styles.get(n)?.display === 'none') return true; return false; };
  // a tray: the chip (height 32, gap 1) whose children are window icons (28 × 28 with the line node under them)
  const trays = () => [...styles].filter(([id, st]) => st.height === 32 && st.gap === 1 && st.borderRadius === 12 && attached(id) && !hiddenUp(id)).map(([id]) => id);
  const iconsIn = (tray) => [...parents].filter(([c, p]) => p === tray && styles.get(c)?.width === 28 && attached(c)).map(([c]) => c);
  const lineOf = (icon) => [...parents].filter(([c, p]) => p === icon && styles.get(c)?.height === 2.5).map(([c]) => c)[0];
  const marked = (icon) => styles.get(lineOf(icon))?.display === 'flex';
  tiler.setLayout(desk, 'HDMI-A-1', 'free');   // One at a time would minimize the store window behind it
  for (let i = 0; i < 2; i++) await flush();
  check('bar: the old "Minimized" label is gone', [...texts].some(([id, v]) => v === 'Minimized' && attached(id) && !hiddenUp(id)), false);
  const before = trays().length;
  const counts0 = new Map(trays().map((id) => [id, iconsIn(id).length]));
  const m1 = sdk.wm.open('extra40', { frame: { x: R.x + 300, y: 300, w: 500, h: 400 } });
  for (let i = 0; i < 2; i++) await flush();
  const m2 = sdk.wm.open('extra41', { frame: { x: R.x + 700, y: 320, w: 500, h: 400 } });
  for (let i = 0; i < 2; i++) await flush();
  const f1 = frameOf(m1);
  const tr = trays();
  check('bar: a tray on each display with windows', tr.length >= 1 && tr.length >= before, true);
  const mine = tr.filter((id) => iconsIn(id).length - (counts0.get(id) || 0) === 2);
  check('bar: the open windows are in it, before any is minimized', mine.length, 1);
  const icons = mine.length ? iconsIn(mine[0]) : [];
  check('bar: no line under open windows', icons.map(marked), icons.map(() => false));
  sdk.wm.focus(m1);                                 // the stacking order changes; the icons keep the order they opened in
  for (let i = 0; i < 2; i++) await flush();
  check('bar: icons keep their order after a focus', mine.length ? iconsIn(mine[0]) : [], icons);
  sdk.wm.minimize(m1);
  for (let i = 0; i < 3; i++) await flush();
  const after = mine.length ? iconsIn(mine[0]) : [];
  check('bar: a minimized window stays in the bar', after.length, icons.length);
  check('bar: a line under the minimized one only', after.map(marked).filter(Boolean).length, 1);
  const target = after.find(marked);
  if (target) { globalThis.__lordEvent('down', target, 0, 0, 0); globalThis.__lordEvent('up', target, 0, 0, 0); }
  for (let i = 0; i < 3; i++) await flush();
  check('bar: a click brings it back, where it was', [!!sdk.wm.get(m1).minimized, frameOf(m1)], [false, f1]);
  check('bar: no line once back', (mine.length ? iconsIn(mine[0]) : []).map(marked).filter(Boolean).length, 0);
  // the app's windows from its icon: the Dock's rows (sdk/dock.js appWindowItems), titles cleaned, New window last
  const aw = await import('../../themes/workspaces/components/AppWindows.jsx');
  sdk.wm.setTitle(m2, 'Work\u202e - Chrome\nsecond line');
  const items = aw.appMenuItems(sdk.launcherIdOf(sdk.wm.get(m2)), desk, R, null, true);
  check('app menu: its window, then New window', items.map((x) => (x === '-' ? '-' : x.label)), ['Work - Chrome second line', '-', sdk.t('shell.dock.newWindow')]);
  for (const id of [m1, m2]) sdk.wm.close(id, true);
  tiler.setLayout(desk, 'HDMI-A-1', 'single');
  await flush();
}

// slots: in Two halves, a minimized window keeps its half; a new app takes it; bringing the first back swaps them
{
  tiler.setLayout(desk, 'DP-1', 'halves');
  const fa = sdk.wm.open('extra50', { frame: { x: L.x + 100, y: 200, w: 500, h: 400 } });
  const ch = sdk.wm.open('extra51', { frame: { x: L.x + 700, y: 200, w: 500, h: 400 } });
  for (let i = 0; i < 3; i++) await flush();
  const halves = tileRects('halves', plan(L).work, 2, 0.5);
  check('slots: files and chrome in the two halves', [frameOf(fa), frameOf(ch)], halves);
  sdk.wm.minimize(fa);
  for (let i = 0; i < 3; i++) await flush();
  check('slots: files minimized, chrome stays in its half', frameOf(ch), halves[1]);
  check('slots: the free half is shown', (tiler.tiled.get().holes['DP-1'] || []).length, 1);
  const tm = sdk.wm.open('extra52', { frame: { x: L.x + 300, y: 300, w: 400, h: 300 } });
  for (let i = 0; i < 3; i++) await flush();
  check('slots: a new app takes the free half', [frameOf(tm), frameOf(ch), (tiler.tiled.get().holes['DP-1'] || []).length], [halves[0], halves[1], 0]);
  sdk.wm.focus(fa);
  for (let i = 0; i < 3; i++) await flush();
  check('slots: files back in its half, the other app minimized, chrome untouched',
    [frameOf(fa), !!sdk.wm.get(tm).minimized, frameOf(ch)], [halves[0], true, halves[1]]);
  sdk.wm.focus(tm);
  for (let i = 0; i < 3; i++) await flush();
  check('slots: and back again', [!!sdk.wm.get(fa).minimized, frameOf(tm)], [true, halves[0]]);
  for (const id of [fa, ch, tm]) sdk.wm.close(id, true);
  tiler.setLayout(desk, 'DP-1', 'free');
  for (let i = 0; i < 2; i++) await flush();
}

// (0.8.9.1) the layout changed while a window was minimized: brought back, it fills its slot in the new layout, not
// the size it had (Two halves to Main and side: it came back at its old width)
{
  tiler.setLayout(desk, 'DP-1', 'halves');
  const a = sdk.wm.open('extra53', { frame: { x: L.x + 100, y: 200, w: 500, h: 400 } });
  const b = sdk.wm.open('extra54', { frame: { x: L.x + 700, y: 200, w: 500, h: 400 } });
  for (let i = 0; i < 3; i++) await flush();
  check('minimized relayout: two halves', [frameOf(a), frameOf(b)], tileRects('halves', plan(L).work, 2, 0.5));
  // the tiler allows 60 passes a second and waits 1 s past that: this scene goes faster, so it waits here
  await new Promise((res) => setTimeout(res, 1100));
  await flush();
  sdk.wm.minimize(a);
  for (let i = 0; i < 3; i++) await flush();
  tiler.setLayout(desk, 'DP-1', 'split');
  for (let i = 0; i < 3; i++) await flush();
  const split = tileRects('split', plan(L).work, 3, 2 / 3);   // main and side: three slots, one of them empty
  check('minimized relayout: the other window in its new slot', frameOf(b), split[1]);
  sdk.wm.focus(a);
  for (let i = 0; i < 3; i++) await flush();
  check('minimized relayout: brought back, it fills its new slot', [!!sdk.wm.get(a).minimized, frameOf(a), frameOf(b)], [false, split[0], split[1]]);
  for (const id of [a, b]) sdk.wm.close(id, true);
  tiler.setLayout(desk, 'DP-1', 'free');
  for (let i = 0; i < 2; i++) await flush();
}

// re-picking Two halves with two floating windows (moved by hand, or placed by session restore): both tile, side by side
{
  tiler.setLayout(desk, 'DP-1', 'halves');
  const a1 = sdk.wm.open('extra55', { frame: { x: L.x + 100, y: 200, w: 500, h: 400 } });
  const a2 = sdk.wm.open('extra56', { frame: { x: L.x + 700, y: 200, w: 500, h: 400 } });
  for (let i = 0; i < 3; i++) await flush();
  sdk.wm.setFrame(a1, { x: L.x + 40, y: 60, w: 900, h: 900 });
  sdk.wm.setFrame(a2, { x: L.x + 960, y: 60, w: 940, h: 900 });
  for (let i = 0; i < 3; i++) await flush();
  check('repick: moved by hand, both float', [tiler.isFloating(a1), tiler.isFloating(a2)], [true, true]);
  tiler.setLayout(desk, 'DP-1', 'halves');
  for (let i = 0; i < 3; i++) await flush();
  const halves = tileRects('halves', plan(L).work, 2, 0.5);
  check('repick: two halves, one each', [frameOf(a1), frameOf(a2)], halves);
  for (const id of [a1, a2]) sdk.wm.close(id, true);
  tiler.setLayout(desk, 'DP-1', 'free');
  for (let i = 0; i < 2; i++) await flush();
}

// drag to rearrange: a tile dragged onto another swaps with it; dragged to another display, it joins that display's tiles
{
  tiler.setLayout(desk, 'DP-1', 'halves');
  await new Promise((res) => setTimeout(res, 1100));   // the tiler's pass budget (MAX_PASSES a second) after the blocks above
  const p1 = sdk.wm.open('extra57', { frame: { x: L.x + 100, y: 200, w: 500, h: 400 } });
  const p2 = sdk.wm.open('extra58', { frame: { x: L.x + 700, y: 200, w: 500, h: 400 } });
  for (let i = 0; i < 3; i++) await flush();
  const halves = tileRects('halves', plan(L).work, 2, 0.5);
  check('rearrange: two halves', [frameOf(p1), frameOf(p2)], halves);
  const f1 = frameOf(p1);
  sdk.wm.move(p1, halves[1].x + 40, f1.y);           // dragged onto the right half, its size kept
  for (let i = 0; i < 3; i++) await flush();
  check('rearrange: dropped on the other half, they swap', [frameOf(p1), frameOf(p2), tiler.isFloating(p1)], [halves[1], halves[0], false]);
  sdk.wm.move(p2, M.x + 300, 300);                   // dragged to the main display (split, three tiles)
  for (let i = 0; i < 3; i++) await flush();
  check('rearrange: dragged to another display, it joins its tiles; Two halves keeps its two', [tiler.isFloating(p2), tiler.tiled.get().counts['DP-2'], tiler.tiled.get().counts['DP-1']], [false, 4, 2]);
  check('rearrange: the one left keeps its half, the other half free', [frameOf(p1), (tiler.tiled.get().holes['DP-1'] || []).length], [halves[1], 1]);
  for (const id of [p1, p2]) sdk.wm.close(id, true);
  tiler.setLayout(desk, 'DP-1', 'free');
  for (let i = 0; i < 3; i++) await flush();
}

// the layout flyout and the drop target (Lord Workspaces only)
{
  const snapT = await import('../../themes/workspaces/components/SnapTargets.jsx');
  await new Promise((res) => setTimeout(res, 1100));
  const g = (k) => ({ halves: { layout: 'halves', zones: ['left', 'right'] }, thirds: { layout: 'three', zones: ['left-third', 'center-third', 'right-third'] } }[k]);
  const q1 = sdk.wm.open('extra53', { frame: { x: L.x + 200, y: 200, w: 600, h: 400 } });
  await flush();
  snapT.applyZone(q1, g('thirds'), 2);
  await flush();
  check('flyout, free display: the snap zone (right third)', [frameOf(q1), sdk.wm.get(q1).snapped], [sdk.snapRect('right-third', sdk.workAreaOf(L)), 'right-third']);
  const q2 = sdk.wm.open('extra54', { frame: { x: L.x + 300, y: 300, w: 500, h: 400 } });
  await flush();
  snapT.applyZone(q2, g('thirds'), 0);
  for (let i = 0; i < 3; i++) await flush();
  // the display was free: q2 snaps to the left third
  check('flyout, free display: left third', sdk.wm.get(q2).snapped, 'left-third');
  tiler.setLayout(desk, 'DP-1', 'halves');
  for (let i = 0; i < 3; i++) await flush();
  const halves = tileRects('halves', plan(L).work, 2, 0.5);
  snapT.applyZone(q2, g('thirds'), 2);
  for (let i = 0; i < 4; i++) await flush();
  void tileRects;
  check('flyout, tiled display: the layout set there, its three slots', [tiler.layoutOf(desk, 'DP-1').layout, tiler.tiled.get().counts['DP-1']], ['three', 3]);
  check('flyout, tiled display: the window in the right third', frameOf(q2), tileRects('three', plan(L).work, 3, 1 / 3)[2]);
  tiler.setLayout(desk, 'DP-1', 'halves');
  for (let i = 0; i < 3; i++) await flush();
  const now2 = [frameOf(q1), frameOf(q2)];
  const prev = tiler.slotPreview(desk, L, q1, halves[0].x + 50, 400);
  check('drop target: the slot under the pointer', prev && prev.rect, halves[0]);
  const mine = halves.findIndex((r) => r.x === frameOf(q1).x);
  const other = 1 - mine;
  check('drop target: q1 is in a half', mine >= 0, true);
  tiler.dropHint(q1, 'DP-1', halves[other].x + 50, 400);
  sdk.wm.move(q1, frameOf(q1).x + 30, frameOf(q1).y + 30);   // its centre stays on its own half; the pointer was on the other
  for (let i = 0; i < 3; i++) await flush();
  check('drop target: the window lands where the preview showed (they swap)', [frameOf(q1), frameOf(q2)], [halves[other], halves[mine]]);
  void now2;
  for (const id of [q1, q2]) sdk.wm.close(id, true);
  tiler.setLayout(desk, 'DP-1', 'free');
  for (let i = 0; i < 3; i++) await flush();
}

// the maintainer's case: Two halves with three windows (the left half in two rows), the top one minimized; Chrome
// dragged onto that free space takes it, its own row goes, and it gets the whole left half
{
  await new Promise((res) => setTimeout(res, 1100));
  tiler.setLayout(desk, 'DP-1', 'halves');
  const fz = sdk.wm.open('extra45', { frame: { x: L.x + 100, y: 100, w: 500, h: 300 } });
  const mo = sdk.wm.open('extra46', { frame: { x: L.x + 1000, y: 100, w: 500, h: 300 } });
  const cr = sdk.wm.open('extra47', { frame: { x: L.x + 100, y: 600, w: 500, h: 300 } });
  for (let i = 0; i < 3; i++) await flush();
  const three = tileRects('halves', plan(L).work, 3, 0.5);
  check('case: three windows, the left half in two rows', [frameOf(fz), frameOf(mo), frameOf(cr)], three);
  sdk.wm.minimize(fz);
  for (let i = 0; i < 3; i++) await flush();
  check('case: the top row free, chrome still in the bottom row', [(tiler.tiled.get().holes['DP-1'] || []).length, frameOf(cr)], [1, three[2]]);
  const pv = tiler.slotPreview(desk, L, cr, three[0].x + 100, three[0].y + 100);
  const two = tileRects('halves', plan(L).work, 2, 0.5);
  check('case: the preview shows the whole left half', pv && pv.rect, two[0]);
  tiler.dropHint(cr, 'DP-1', three[0].x + 100, three[0].y + 100);
  sdk.wm.move(cr, frameOf(cr).x + 20, frameOf(cr).y - 200);
  for (let i = 0; i < 3; i++) await flush();
  check('case: chrome takes the whole left half, no free space left', [frameOf(cr), frameOf(mo), (tiler.tiled.get().holes['DP-1'] || []).length, tiler.tiled.get().counts['DP-1']],
    [two[0], two[1], 0, 2]);
  sdk.wm.focus(fz);
  for (let i = 0; i < 3; i++) await flush();
  check('case: the minimized one brought back swaps with chrome', [frameOf(fz), !!sdk.wm.get(cr).minimized], [two[0], true]);
  for (const id of [fz, mo, cr]) sdk.wm.close(id, true);
  tiler.setLayout(desk, 'DP-1', 'free');
  for (let i = 0; i < 3; i++) await flush();
}

// Add widget: the catalog opens over a display; a widget takes a block of the display's grid, in the activity in view,
// keeps it (another can't take it), changes size, follows its activity, and is removed
{
  const wp = await import('../../sdk/widgetpicker.jsx');
  const grid = await import('../../sdk/widgetgrid.js');
  const shownT = (v) => [...texts].filter(([id, x]) => x === v && attached(id)).length;
  wp.widgetPicker.set({ display: R });
  await flush();
  check('widgets: the catalog opens', shownT(sdk.t('shell.widgets.title')), 1);
  check('widgets: its kinds', ['video', 'timer', 'recent', 'updates', 'launcher', 'vpn', 'weather', 'clock', 'system', 'calendar', 'nowplaying', 'readaloud', 'network', 'disks', 'temps', 'reminders', 'clipboard', 'claude'].every((k) => shownT(sdk.t(`shell.widgets.${k}.name`)) >= 1), true);
  // the search (name and description, any accent or case) and the categories
  const kindOf = (type) => sdk.widgetKinds().find((k) => k.type === type);
  check('widgets: search by name and description, accents and case ignored',
    [wp.matches(kindOf('clock'), sdk.t('shell.widgets.clock.name').toUpperCase()), wp.matches(kindOf('clock'), 'zzqx'), wp.matches(kindOf('clock'), ''),
      wp.fold('Calendário Ñandú'), wp.matches(kindOf('system'), sdk.t('shell.widgets.system.desc').split(' ')[0])], [true, false, true, 'calendario nandu', true]);
  check('widgets: every kind has a category, none falls to "other"', sdk.widgetKinds().filter((k) => wp.categoryOf(k.type) === 'other').map((k) => k.type), []);
  check('widgets: the panel sits centred in the work area', (() => {
    const A = sdk.workAreaOf(R);
    return [...styles].some(([id, st]) => attached(id) && st.position === 'absolute' && st.height > 300 && Math.abs(st.left + st.width / 2 - (A.x + A.w / 2)) <= 1 && Math.abs(st.top + st.height / 2 - (A.y + A.h / 2)) <= 1);
  })(), true);
  wp.widgetPicker.set(null);
  const g = grid.gridOf(sdk.workAreaOf(R));
  const at = (w) => { const r = grid.rectOf(g, w.col, w.row, w.size); return [...styles].some(([id, st]) => st.left === r.x && st.top === r.y && st.width === r.w && st.height === r.h && attached(id)); };
  // the empty state's card shows only on a display with nothing at all: a widget is something
  const visible = (id) => { for (let n = id; n; n = parents.get(n)) if (styles.get(n)?.display === 'none') return false; return attached(id); };
  const cardOn = (D) => [...texts].some(([id, v]) => {
    if (v !== sdk.t('shell.ws.empty.title') || !visible(id)) return false;
    for (let n = parents.get(id); n; n = parents.get(n)) { const st = styles.get(n); if (st?.position === 'absolute' && st.left >= D.x && st.left < D.x + D.w) return true; }
    return false;
  });
  {
    const dv = sdk.addDeskWidget('devices', 'm', R);
    for (let i = 0; i < 3; i++) await flush();
    check('widgets: Devices with nothing that has a battery says so', shownT(sdk.t('shell.widgets.devices.none')), 1);
    sdk.removeDeskWidget(dv);
    for (let i = 0; i < 3; i++) await flush();
  }
  const c1 = sdk.addDeskWidget('clock', 's', R);
  const c2 = sdk.addDeskWidget('calendar', 'l', R);
  for (let i = 0; i < 2; i++) await flush();
  const list = () => sdk.settings.get().deskWidgets || [];
  const w1 = list().find((w) => w.id === c1), w2 = list().find((w) => w.id === c2);
  check('widgets: the first at the top right of the grid', [w1.col, w1.row, w1.dk, w1.desk], [g.cols - 1, 0, 'HDMI-A-1', desk]);
  check('widgets: the second beside it, its block reserved', [w2.col, w2.row], [g.cols - 3, 0]);
  check('widgets: both drawn on their blocks', [at(w1), at(w2)], [true, true]);
  check('widgets: no timer every second', intervals(1000), 0);
  check('widgets: resized where it fits', [sdk.resizeDeskWidget(c1, 'l'), list().find((w) => w.id === c1).size], [true, 'l']);
  const r1 = list().find((w) => w.id === c1);
  check('widgets: never over another one', grid.occupancy([r1]).size + grid.occupancy([w2]).size, grid.occupancy([r1, w2]).size);
  const nd = sdk.newDesktop ? null : null; void nd;
  const other = sdk.desktopList.get().find((x) => x !== desk);
  if (other != null) {
    sdk.activeDesktop.set(other);
    for (let i = 0; i < 2; i++) await flush();
    check('widgets: another activity shows none of them', [at(r1), at(w2)], [false, false]);
    sdk.activeDesktop.set(desk);
    for (let i = 0; i < 2; i++) await flush();
    check('widgets: back, they are there', [at(r1), at(w2)], [true, true]);
  }
  // dragged onto the other widget: it lands on the nearest free block, never over it
  {
    const ww = list().find((w) => w.id === c1);
    const r = grid.rectOf(g, ww.col, ww.row, ww.size);
    const card = [...styles].find(([id, st]) => st.left === r.x && st.top === r.y && st.width === r.w && attached(id))?.[0];
    const target = grid.rectOf(g, w2.col, w2.row, 'l');
    globalThis.__lordEvent('down', card, r.x + 20, r.y + 20, 0);
    globalThis.__lordEvent('move', card, r.x + 60, r.y + 30, 0);
    globalThis.__lordEvent('move', card, target.x + 20, target.y + 20, 0);
    await flush();
    globalThis.__lordEvent('up', card, target.x + 20, target.y + 20, 0);
    for (let i = 0; i < 2; i++) await flush();
    const moved = list().find((w) => w.id === c1), still = list().find((w) => w.id === c2);
    check('widgets: dragged over another, it lands on a free block', grid.occupancy([moved, still]).size, grid.occupancy([moved]).size + grid.occupancy([still]).size);
    check('widgets: the other one kept its block', [still.col, still.row], [w2.col, w2.row]);
    check('widgets: it did move', [moved.col, moved.row].join() !== [ww.col, ww.row].join(), true);
  }
  // right-clicked at the display's edge: its menu opens inwards, inside that display (not onto the next monitor)
  {
    const ww = list().find((w) => w.id === c1);
    const r = grid.rectOf(g, ww.col, ww.row, ww.size);
    const card = [...styles].filter(([id, st]) => st.left === r.x && st.top === r.y && st.width === r.w && attached(id)).pop()?.[0];   // the card, inside its host
    globalThis.__lordEvent('down', card, r.x + r.w - 4, r.y + 10, 2);
    globalThis.__lordEvent('up', card, r.x + r.w - 4, r.y + 10, 2);
    for (let i = 0; i < 2; i++) await flush();
    const frame = [...styles].find(([id, st]) => st.left === R.x && st.top === R.y && st.width === R.w && st.height === R.h && st.pointerEvents === 'box-none' && attached(id));
    const box = [...styles].find(([id, st]) => st.shadowBlur === 30 && st.shadowOffsetY === 10 && attached(id))?.[1];
    check('widget menu: laid inside its own display', [!!frame, !!box && box.left >= 4 && box.left + box.width <= R.w - 4], [true, true]);
    globalThis.__lordEvent('down', null, L.x + 5, L.y + 400, 0);
    globalThis.__lordEvent('up', null, L.x + 5, L.y + 400, 0);
    for (let i = 0; i < 2; i++) await flush();
    check('widget menu: a click outside closes it', [...styles].some(([id, st]) => st.shadowBlur === 30 && st.shadowOffsetY === 10 && attached(id)), false);
  }
  // a clock kept at the old 4×2 size shows as the 4×1 row of cities
  {
    sdk.setDeskWidget(c1, { size: 'xl' });
    for (let i = 0; i < 2; i++) await flush();
    const ww = list().find((w) => w.id === c1);
    const r = grid.rectOf(g, 0, 0, 'w');
    check('widgets: the old extra large clock is the wide one', [...styles].some(([id, st]) => st.width === r.w && st.height === r.h && attached(id)), true);
  }
  for (const id of [c1, c2]) sdk.removeDeskWidget(id);
  for (let i = 0; i < 2; i++) await flush();
  check('widgets: removed', list().length, 0);
  // in an activity with nothing open on the right display: the card, gone once a widget is there, back after
  {
    const other = sdk.desktopList.get().find((x) => x !== desk && !sdk.windows.get().some((w) => w.desktop === x && sdk.displayFor(w)?.id === R.id));
    if (other != null) {
      sdk.activeDesktop.set(other);
      for (let i = 0; i < 2; i++) await flush();
      check('empty card: on a display with nothing', cardOn(R), true);
      const cw = sdk.addDeskWidget('clock', 's', R);
      for (let i = 0; i < 3; i++) await flush();
      check('empty card: none over a widget', cardOn(R), false);
      sdk.removeDeskWidget(cw);
      for (let i = 0; i < 3; i++) await flush();
      check('empty card: back once the widget is gone', cardOn(R), true);
      sdk.activeDesktop.set(desk);
      for (let i = 0; i < 2; i++) await flush();
    } else check('empty card: an activity to try it in', false, true);
  }
  // the header's "+" is a menu: a window, a widget, a web app
  {
    // the header's buttons (28 px high, radius 9): the layout one is wide (10 px sides), "+" is the next one
    const hb = (pad) => [...styles].filter(([id, st]) => st.height === 28 && st.borderRadius === 9 && st.paddingLeft === pad && visible(id)).map(([id]) => id).sort((a, b) => a - b);
    const wide = hb(10), narrow = hb(7);
    const plus = wide.map((w) => narrow.find((n) => n > w && parents.get(n) === parents.get(w))).filter(Boolean);
    globalThis.__lordEvent('down', plus[0], 0, 0, 0); globalThis.__lordEvent('up', plus[0], 0, 0, 0);
    for (let i = 0; i < 2; i++) await flush();
    check('add menu: one "+" per display, its three entries', [plus.length >= 1, ['shell.ws.layout.add', 'shell.widgets.add', 'shell.ws.add.webapp'].every((k) => shownT(sdk.t(k)) >= 1)], [true, true]);
    globalThis.__lordEvent('down', null, L.x + 5, L.y + 600, 0); globalThis.__lordEvent('up', null, L.x + 5, L.y + 600, 0);
    for (let i = 0; i < 2; i++) await flush();
  }
}

// weather: off until the person turns it on from the widget; nothing is asked of the internet before
{
  const { counts } = globalThis.__lordTest;
  const shownW = (v) => [...texts].filter(([id, x]) => x === v && attached(id)).length;
  const id = sdk.addDeskWidget('weather', 'm', L);
  for (let i = 0; i < 2; i++) await flush();
  check('weather: off, it offers to turn on and asks nothing', [shownW(sdk.t('shell.widgets.weather.turnOn')), counts.wx], [1, 0]);
  sdk.settings.set({ weather: { on: true } });
  for (let i = 0; i < 2; i++) await flush();
  check('weather: on, it asks for the time zone\'s forecast', [counts.wx > 0, shownW(sdk.t('shell.widgets.weather.loading'))], [true, 1]);
  sdk.settings.set({ weather: { on: false } });
  sdk.removeDeskWidget(id);
  for (let i = 0; i < 2; i++) await flush();
}

// VPN: with one VPN set up, the widget drives it directly: its name and the switch (OFF while its interface is down)
{
  const shownV = (v) => [...texts].filter(([id, x]) => x === v && attached(id)).length;
  const id = sdk.addDeskWidget('vpn', 'm', L);
  for (let i = 0; i < 2; i++) await flush();
  check('vpn: the only VPN, its switch off', [shownV('office') >= 1, shownV(sdk.t('shell.widgets.vpn.off')) >= 1, shownV(sdk.t('shell.widgets.vpn.disconnected')) >= 1], [true, true, true]);
  sdk.removeDeskWidget(id);
  for (let i = 0; i < 2; i++) await flush();
}

// video: with no source yet it offers to set one; nothing is opened (no lord-stream) until there is an address
{
  const shownV = (v) => [...texts].filter(([id, x]) => x === v && attached(id)).length;
  const id = sdk.addDeskWidget('video', 'm', L);
  for (let i = 0; i < 2; i++) await flush();
  check('video: no source, it offers one', [shownV(sdk.t('shell.widgets.video.empty')), shownV(sdk.t('shell.widgets.video.setup'))], [1, 1]);
  sdk.removeDeskWidget(id);
  for (let i = 0; i < 2; i++) await flush();
}

// timer: idle, the ring shows the duration a tap starts (5:00), not a dash; the three presets on one row of pills
// sized by sdk/timerwidgetmodel.js (server/tests/timer_widget_test.mjs checks the fit in every language)
{
  const shownV = (v) => [...texts].filter(([id, x]) => x === v && attached(id)).length;
  for (const size of ['s', 'm']) {
    const id = sdk.addDeskWidget('timer', size, L);
    for (let i = 0; i < 2; i++) await flush();
    const presets = [1, 5, 25].map((n) => shownV(sdk.t('clock.timer.preset', { n })));
    check(`timer ${size}: idle shows 5:00 and the presets`, [shownV('5:00'), shownV('—'), presets], [1, 0, [1, 1, 1]]);
    if (size === 'm') check('timer m: the stopwatch beside it', shownV(sdk.t('shell.widgets.timer.stopwatch')), 1);
    sdk.removeDeskWidget(id);
    for (let i = 0; i < 2; i++) await flush();
  }
}

// every built-in widget renders in each of its sizes, and none adds a timer every second
{
  const grid = await import('../../sdk/widgetgrid.js');
  const ids = [];
  for (const k of sdk.widgetKinds().filter((x) => x.sizes)) for (const size of k.sizes) {
    for (const id of ids.splice(0)) sdk.removeDeskWidget(id);
    const id = sdk.addDeskWidget(k.type, size, L);
    ids.push(id);
    await flush();
    const w = (sdk.settings.get().deskWidgets || []).find((x) => x.id === id);
    // a pinned one (the sound wave starts at the bottom centre) is off the grid
    const r = w.opts?.pin ? grid.pinnedRect(sdk.workAreaOf(L), size, w.opts.pin) : grid.rectOf(grid.gridOf(sdk.workAreaOf(L)), w.col, w.row, size);
    const ok = [...styles].some(([nid, st]) => st.left === r.x && st.top === r.y && st.width === r.w && attached(nid));
    if (!ok) check(`widgets: ${k.type} ${size} on the desktop`, ok, true);
  }
  check('widgets: every kind in every size drew', true, true);
  check('widgets: still no timer every second', intervals(1000), 0);
  for (const id of ids) sdk.removeDeskWidget(id);
  for (let i = 0; i < 2; i++) await flush();
  check('widgets: all removed', (sdk.settings.get().deskWidgets || []).length, 0);
}

// the free layout's magnet, on the left display (free): the other window fills the other half, the shared edge follows
{
  const magnet = await import('../../themes/workspaces/magnet.js');
  const work = plan(L).work;
  const half = (zone) => sdk.snapRect(zone, sdk.workAreaOf(L));
  const m1 = sdk.wm.open('extra20', { frame: { x: L.x + 100, y: 200, w: 500, h: 400 } });
  const m2 = sdk.wm.open('extra21', { frame: { x: L.x + 300, y: 260, w: 500, h: 400 } });
  await flush();
  sdk.wm.snap(m1, 'left');
  await flush();
  check('magnet: the snapped window takes the left half', frameOf(m1), half('left'));
  check('magnet: the only other window fills the right half', [frameOf(m2).x, frameOf(m2).y, frameOf(m2).w], [half('right').x, half('right').y, half('right').w]);
  const l = frameOf(m1);
  sdk.wm.setFrame(m1, { ...l, w: l.w + 120 });
  await flush();
  check('magnet: the right window follows the shared edge', [frameOf(m2).x, frameOf(m2).x + frameOf(m2).w], [l.x + l.w + 120, work.x + work.w]);
  sdk.wm.move(m2, L.x + 200, 300);
  await flush();
  const after = frameOf(m2);
  sdk.wm.setFrame(m1, { ...frameOf(m1), w: l.w });
  await flush();
  check('magnet: moved by hand, the pair ends', frameOf(m2), after);
  const m3 = sdk.wm.open('extra22', { frame: { x: L.x + 500, y: 300, w: 400, h: 300 } });
  await flush();
  sdk.wm.snap(m3, 'right');
  await flush();
  const offer = magnet.assist.get();
  check('magnet: several windows, a choice for the left half', offer && [offer.side, offer.ids.slice(0, 2).sort()], ['left', [m1, m2].sort()]);
  magnet.choose(m2);
  await flush();
  check('magnet: the chosen window takes the left half', [frameOf(m2).x, frameOf(m2).w], [half('left').x, half('left').w]);
  check('magnet: the chooser closed', magnet.assist.get(), null);
  for (const id of [m1, m2, m3]) sdk.wm.close(id);
  await flush();
}

// one wallpaper per display (every theme, sdk/wallpapermodel.js): the right display gets its own picture
{
  const wallOf = (d) => [...props].filter(([id, p]) => typeof p.src === 'string' && attached(id) && styles.get(id)?.left === d.x && styles.get(id)?.width === d.w).map(([, p]) => p.src)[0] || '';
  const before = [L, M, R].map(wallOf);
  check('wallpaper: one picture for every display at first', new Set(before).size, 1);
  const other = sdk.lordosWallpapers.find((w) => w.id !== sdk.settings.get().wallpaper);
  sdk.settings.set(sdk.wallpaperPatch(sdk.settings.get(), other.id, R));
  await flush();
  const after = [L, M, R].map(wallOf);
  check('wallpaper: only the right display changed', [after[0] === before[0], after[1] === before[1], after[2] !== before[2]], [true, true, true]);
  sdk.settings.set(sdk.wallpaperPatch(sdk.settings.get(), sdk.settings.get().wallpaper, null));
  await flush();
  check('wallpaper: all displays again', [L, M, R].map(wallOf), before);
  // never chosen (null since 0.8.9): Lord Workspaces' own default, Cyberpunk City HD's panorama
  sdk.settings.set({ wallpaper: null, wallpapers: {} });
  await flush();
  check('wallpaper: the theme\'s default when none was chosen', [L, M, R].map(wallOf).every((src) => src.includes('lordos-cyberpunk-panorama')), true);
}

// the wallpaper slideshow (sdk/slideshow.jsx): off by default; on, each display its own picture of the category,
// decoded off the interface's thread ("thumb:" sources), and back to the still pictures when turned off
{
  const slides = () => [...props].filter(([id, p]) => typeof p.src === 'string' && p.src.startsWith('thumb:') && p.src.includes('/cyberpunk/') && attached(id)).map(([, p]) => p.src);
  check('slideshow: off by default', slides().length, 0);
  sdk.settings.set(sdk.slideshowPatch(sdk.settings.get(), { on: true, category: 'cyberpunk', perDisplay: true }));
  await flush();
  check('slideshow: a picture per display, three different ones', [slides().length, new Set(slides()).size], [3, 3]);
  // 1920×1080 plus the drift's 2 % margin on each side (1996×1124): the smallest file that covers it (the 4K one where
  // a picture has one, decoded down to that by the workers; else its 1920×1080 base, decoded at its own size)
  check('slideshow: at the displays\' size and margin', slides().every((src) => /^thumb:(1999:asset:wallpapers\/cyberpunk\/lordos-cyberpunk-[a-z-]+@3840x2160|1920:asset:wallpapers\/cyberpunk\/lordos-cyberpunk-[a-z-]+)\.jpg$/.test(src)), true);
  check('slideshow: turning it on keeps the interval\'s default (a minute)', sdk.settings.get().slideshow.every, 60);
  sdk.settings.set(sdk.slideshowPatch(sdk.settings.get(), { perDisplay: false }));
  await flush();
  check('slideshow: one picture for every display', [slides().length, new Set(slides()).size], [3, 1]);
  sdk.settings.set(sdk.slideshowPatch(sdk.settings.get(), { on: false }));
  await flush();
  check('slideshow: off, the still pictures again', slides().length, 0);
}

// the effects styles: Crystal draws its sheen, Opaque no blur at all
const blurred = () => [...styles].filter(([id, st]) => st.backdropBlur > 0 && attached(id)).length;
const sheens = () => [...styles].filter(([id, st]) => st.gradient && st.pointerEvents === 'none' && attached(id)).length;
check('crystal: blurred surfaces are few (spine, strip, status)', blurred() <= 3, true);
check('crystal: the sheen is drawn', sheens() > 0, true);
sdk.settings.set(sdk.themeOptionPatch(sdk.settings.get(), 'workspaces', 'surface', 'opaque'));
await flush();
check('opaque: nothing blurred', blurred(), 0);
check('opaque: no sheen', sheens(), 0);
sdk.settings.set({ effects: false, ...sdk.themeOptionPatch(sdk.settings.get(), 'workspaces', 'surface', 'crystal') });
await flush();
check('transparency off: opaque whatever the style', blurred(), 0);
sdk.settings.set({ effects: true, ...sdk.themeOptionPatch(sdk.settings.get(), 'workspaces', 'contrast', true) });
await flush();
check('high contrast: nothing blurred', blurred(), 0);
sdk.settings.set(sdk.themeOptionPatch(sdk.settings.get(), 'workspaces', 'contrast', false));
await flush();
check('idle again: still no timer every second', intervals(1000), 0);
// groups: another activity on the same desktop; its windows hide with the others' and come back, never in the tray
{
  const g = state.addGroup(desk, 'Mail');
  await flush();
  check('group: added and in view', [state.groupInView(desk), state.groupsOf(desk).length], [g, 1]);
  check('group: the activity\'s own windows hidden by the group, not in the tray', [[a, b, c].every((id) => sdk.wm.get(id).minimized), [a, b, c].every((id) => state.groupHidden.get().has(id))], [true, true]);
  state.showGroup(desk, null);
  await flush();
  check('group: the own again, its windows back', [a, b, c].every((id) => !sdk.wm.get(id).minimized), true);
  state.moveWindowToGroup(c, g);
  await flush();
  check('group: a window moved to the group leaves the view', [state.windowGroups.get().get(c), sdk.wm.get(c).minimized, state.groupHidden.get().has(c)], [g, true, true]);
  check('group: the chips in the strip', [...texts].some(([id, x]) => x === 'Mail' && attached(id)), true);
  state.showGroup(desk, g);
  await flush();
  check('group: its window shows, the others go', [sdk.wm.get(c).minimized, sdk.wm.get(a).minimized], [false, true]);
  state.removeGroup(desk, g);
  await flush();
  // after a group switch the tiler still tells a hand move from its own
  tiler.setLayout(desk, 'DP-1', 'halves');
  const h1 = sdk.wm.open('extra57', { frame: { x: L.x + 100, y: 200, w: 500, h: 400 } });
  for (let i = 0; i < 3; i++) await flush();
  sdk.wm.setFrame(h1, { x: L.x + 40, y: 60, w: 900, h: 900 });
  for (let i = 0; i < 3; i++) await flush();
  check('group: after a switch, a window moved by hand still floats', tiler.isFloating(h1), true);
  sdk.wm.close(h1, true);
  tiler.setLayout(desk, 'DP-1', 'free');
  await flush();
  check('group: removed, every window back in the activity', [state.groupsOf(desk).length, [a, b, c].every((id) => !sdk.wm.get(id).minimized), state.windowGroups.get().get(c), state.groupHidden.get().size], [0, true, null, 0]);
}

// launched from a display (its frame's + or Add window, its pinned apps, Search opened there): the window shows on it,
// whatever display the app's saved layout names or where it first appeared
{
  tiler.setLayout(desk, 'DP-1', 'halves');
  await flush();
  tiler.aimLaunch(L);
  const la = sdk.wm.open('extra2', { frame: { x: M.x + 200, y: 200, w: 500, h: 400 } });
  for (let i = 0; i < 3; i++) await flush();
  check('aim: a tiled display takes the new window', sdk.displayFor(frameOf(la))?.id, L.id);
  tiler.setLayout(desk, 'DP-1', 'free');
  await flush();
  tiler.aimLaunch(L);
  const lb = sdk.wm.open('extra3', { frame: { x: M.x + 300, y: 250, w: 500, h: 400 } });
  for (let i = 0; i < 3; i++) await flush();
  check('aim: a free display too, in its middle', sdk.displayFor(frameOf(lb))?.id, L.id);
  const lc = sdk.wm.open('extra4', { frame: { x: M.x + 300, y: 250, w: 500, h: 400 } });
  for (let i = 0; i < 3; i++) await flush();
  check('aim: only the launch it was for', sdk.displayFor(frameOf(lc))?.id, M.id);
  for (const id of [la, lb, lc]) sdk.wm.close(id, true);
  await flush();
}

// (0.8.9.5) the apps of each display (sdk/displayapps.js): an empty activity's card shows the display's quick launch and,
// once, its usual apps as a suggestion; nothing opens until it is accepted, then the apps open there in their usual
// tiles; "Don't suggest on this display" is kept for that display only
{
  const visibleNode = (id) => { for (let n = id; n; n = parents.get(n)) if (styles.get(n)?.display === 'none') return false; return attached(id); };
  const textOn = (D, v) => [...texts].some(([id, x]) => {
    if (x !== v || !visibleNode(id)) return false;
    for (let n = parents.get(id); n; n = parents.get(n)) { const st = styles.get(n); if (st?.position === 'absolute' && st.left >= D.x && st.left < D.x + D.w) return true; }
    return false;
  });
  const now = Date.now();
  sdk.settings.set({ displayApps: { v: 1, displays: {
    'DP-1': { apps: [{ id: 'calculator', n: 4, t: now }, { id: 'terminal', n: 2, t: now }], sets: [{ apps: ['extra10', 'extra11'], layout: 'halves', ratio: 0.5, n: 3, t: now }] },
    'HDMI-A-1': { apps: [{ id: 'store', n: 1, t: now }], sets: [{ apps: ['store'], n: 1, t: now }] },
  } } });
  const nd2 = state.newActivity();
  await flush();
  const usualT = sdk.t('shell.ws.usual.title'), quickT = sdk.t('shell.ws.quick.title');
  check('display apps: no suggestion before the moment with nothing open', textOn(L, usualT), false);
  await new Promise((res) => setTimeout(res, 1300));
  await flush();
  const opened = sdk.windows.get().length;
  check('display apps: the left display suggests its usual apps', textOn(L, usualT), true);
  check('display apps: a set seen once is no suggestion (right display)', textOn(R, usualT), false);
  check('display apps: each display its own quick launch', [textOn(L, quickT), textOn(R, quickT), textOn(M, quickT)], [true, true, false]);
  check('display apps: the quick launch keeps Add window', textOn(L, sdk.t('shell.ws.layout.add')), true);
  check('display apps: the card\'s controls in the bars\' keyboard', ['card:DP-1:open', 'card:DP-1:later', 'card:DP-1:never', 'card:DP-1:add', 'card:DP-1:q:calculator'].every((id) => bars.cardBarItems().some((x) => x.id === id)), true);
  await new Promise((res) => setTimeout(res, 300));
  await flush();
  check('display apps: nothing opens by itself', sdk.windows.get().length, opened);
  // accepted from the keyboard (⌃F2, then Enter on it)
  bars.enterBars('card:DP-1:open');
  check('display apps: the keyboard on Open', bars.barFocus.get(), 'card:DP-1:open');
  bars.barKey({ key: 'Enter' });
  for (let i = 0; i < 4; i++) await flush();
  const mine = sdk.windows.get().filter((w) => w.desktop === nd2);
  const fw = mine.find((w) => w.id.split('#')[0] === 'extra10'), tw = mine.find((w) => w.id.split('#')[0] === 'extra11');
  check('display apps: accepted, its apps open on that display', [!!fw, !!tw, fw && sdk.displayFor(frameOf(fw.id))?.id, tw && sdk.displayFor(frameOf(tw.id))?.id], [true, true, L.id, L.id]);
  check('display apps: in its usual layout and order', [tiler.layoutOf(nd2, 'DP-1').layout, fw && tw && frameOf(fw.id).x < frameOf(tw.id).x], ['halves', true]);
  check('display apps: offered once', [state.offered.get().has(`${nd2}|DP-1`), textOn(L, usualT)], [true, false]);
  // "Don't suggest on this display": that display, kept in the record
  const nd3 = state.newActivity();
  await new Promise((res) => setTimeout(res, 1300));
  await flush();
  check('display apps: a new activity suggests again', textOn(L, usualT), true);
  bars.enterBars('card:DP-1:never');
  bars.barKey({ key: ' ' });
  await flush();
  check('display apps: "Don\'t suggest" kept for that display only', [sdk.settings.get().displayApps.displays['DP-1'].quiet, !!sdk.settings.get().displayApps.displays['HDMI-A-1'].quiet, textOn(L, usualT), textOn(L, quickT)], [true, false, false, true]);
  // Settings → Clear forgets every display; off records nothing and shows nothing
  sdk.forgetDisplayApps();
  await flush();
  check('display apps: cleared', [sdk.displayAppsStored(sdk.settings.get()), textOn(L, quickT)], [false, false]);
  sdk.setDisplayAppsOn(false);
  check('display apps: off', [sdk.displayAppsOn(sdk.settings.get()), sdk.settings.get().displayApps.off], [false, true]);
  sdk.setDisplayAppsOn(true);
  for (const w of mine) sdk.wm.close(w.id, true);
  sdk.switchDesktop(desk);
  await flush();
  sdk.removeDesktopById(nd3);
  sdk.removeDesktopById(nd2);
  await flush();
}


// a launch from a display's bar: the app's windows go to THAT display and THAT activity, in its layout slot, wherever the
// app puts them and whatever activity is in view when they come. Google Chrome with profiles: its picker takes the
// place, closes, and the real browser window follows it there (model/launches.js); another app's window doesn't take it
{
  const key = 'linux:fakechrome';
  sdk.switchDesktop(desk);
  tiler.setLayout(desk, 'HDMI-A-1', 'halves');
  for (let i = 0; i < 3; i++) await flush();
  const here = sdk.windows.get().filter((w) => w.desktop === desk && sdk.displayFor(frameOf(w.id))?.id === R.id).map((w) => w.id);
  for (const id of here) sdk.wm.close(id, true);
  await flush();
  const first = sdk.wm.open('extra59', { frame: { x: R.x + 200, y: 200, w: 600, h: 400 } });
  for (let i = 0; i < 3; i++) await flush();
  const halves = tileRects('halves', plan(R).work, 2, 0.5);
  tiler.aimLaunch(R, 1, key);
  check('launch: pending for that display and activity', tiler.pendingLaunches().filter((p) => p.key === key).map((p) => [p.key, p.dk, p.desk]), [[key, 'HDMI-A-1', desk]]);
  // another app's window opens meanwhile on the main display: not taken
  const other = sdk.wm.open('linux', { instance: '9001', frame: { x: M.x + 100, y: 100, w: 500, h: 400 }, args: { surface: 9001, appId: 'gedit-like' } });
  // the profile picker, where Chrome puts it (the main display)
  const picker = sdk.wm.open('linux', { instance: '9002', frame: { x: M.x + 300, y: 200, w: 800, h: 600 }, args: { surface: 9002, appId: 'fakechrome' } });
  for (let i = 0; i < 3; i++) await flush();
  check('launch: another app\'s window stays where it opened', sdk.displayFor(frameOf(other))?.id, M.id);
  check('launch: the picker on the launch\'s display, in its slot', [sdk.displayFor(frameOf(picker))?.id, frameOf(picker)], [R.id, halves[1]]);
  // the person goes to another activity while choosing; the picker closes and the browser window opens there
  const nd = state.newActivity();
  for (let i = 0; i < 3; i++) await flush();
  sdk.wm.close(picker, true);
  await flush();
  const real = sdk.wm.open('linux', { instance: '9003', frame: { x: M.x + 50, y: 60, w: 1200, h: 800 }, args: { surface: 9003, appId: 'fakechrome' } });
  for (let i = 0; i < 3; i++) await flush();
  check('launch: the browser window goes to the launch\'s activity', sdk.wm.get(real)?.desktop, desk);
  sdk.switchDesktop(desk);
  for (let i = 0; i < 4; i++) await flush();
  check('launch: on the launch\'s display, in the picker\'s slot', [sdk.displayFor(frameOf(real))?.id, frameOf(real), frameOf(first)], [R.id, halves[1], halves[0]]);
  // a dialog of that window (transient for it): never placed by a launch, over its parent (sdk/wayland.js)
  check('launch: nothing pending after', tiler.pendingLaunches().filter((p) => p.key === key).length, 0);
  for (const id of [first, other, real]) sdk.wm.close(id, true);
  sdk.removeDesktopById(nd);
  tiler.setLayout(desk, 'HDMI-A-1', 'single');
  for (let i = 0; i < 2; i++) await flush();
}

process.exit(0);
