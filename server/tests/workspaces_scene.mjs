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

// moved by hand: out of its tile, floating; the others take the room
sdk.wm.move(c, M.x + 50, 200);
await flush();
check('moved by hand: it floats', tiler.isFloating(c), true);
check('the others re-tile as two', [a, b].map(frameOf), tileRects('split', plan(M).work, 2, 2 / 3));
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

// the padlock of one display: its windows can't be moved, resized, minimized, maximized, snapped or closed by the person,
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
  sdk.wm.move(k1, L.x + 600, 500); sdk.wm.resize(k1, 300, 200); sdk.wm.minimize(k1); sdk.wm.toggleMaximize(k1); sdk.wm.snap(k1, 'left'); sdk.wm.snapKey(k1, 'right'); sdk.wm.center(k1);
  await flush();
  check('lock: move, resize, minimize, maximize, snap and center refused', [frameOf(k1), !!sdk.wm.get(k1).minimized, !!sdk.wm.get(k1).maximized], [f0, false, false]);
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
check('idle again: still no timer every second', intervals(1000), 0);
process.exit(0);
