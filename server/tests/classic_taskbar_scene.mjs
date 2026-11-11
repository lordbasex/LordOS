// classic_taskbar_scene.mjs — the scene of tests/classic_taskbar_test.mjs (not run by itself), 0.8.8.6: Lord
// Classic's taskbar on every display. The whole theme (themes/classic/index.jsx) through runtime/index.js and the
// real SDK, over a stub `lord` (the bundle's banner) with three monitors side by side, the main one in the middle.
// It checks a taskbar per display, each display's work area, one clock timer and one network poll for all the
// bars, the start menu, quick settings and notifications opening on the display whose bar was clicked (and the
// power menu's Lock still reaching the session there, #48), a pin dropped on one bar, the setting turned off,
// mirrored displays, a monitor unplugged and plugged back, and (#111) more pins than a bar holds. Prints one line per check: "ok <name>" or
// "FAIL <name> <detail>".

// the timers alive, by interval (installed before anything starts one)
const live = new Map();
const realSet = globalThis.setInterval, realClear = globalThis.clearInterval;
globalThis.setInterval = (fn, ms, ...a) => { const id = realSet(fn, ms, ...a); live.set(id, ms); return id; };
globalThis.clearInterval = (id) => { live.delete(id); realClear(id); };
const timers = (ms) => [...live.values()].filter((v) => v === ms).length;

const sessionCalls = [];
globalThis.lord.session = { managed: () => true, lock: () => { sessionCalls.push('lock'); return true; }, switchUser: () => true };

const React = (await import('react')).default;
const rt = await import('@lordos/runtime');
const sdk = await import('@lordos/sdk');
await import('../../themes/classic/index.jsx');
const { startBox } = await import('../../themes/classic/components/StartMenu.jsx');

const { texts, styles, parents, removed, layouts, counts } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, got, want) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  console.log(g === w ? `ok ${name}` : `FAIL ${name} got ${g}, want ${w}`);
};
const attached = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return parents.has(id); };
// x, y: where the press is (a press off every bar and outside an open panel closes the panel)
const click = async (id, x = 10, y = 10) => {
  globalThis.__lordEvent('down', id, x, y, 0); await flush();
  globalThis.__lordEvent('up', id, x, y, 0); await flush();
};

// three monitors, the main one in the middle and taller
const L = { id: 41, name: 'DP-1', x: 0, y: 0, w: 1920, h: 1080, scale: 100, main: false };
const M = { id: 42, name: 'DP-2', x: 1920, y: 0, w: 2560, h: 1440, scale: 100, main: true };
const R = { id: 43, name: 'HDMI-A-1', x: 4480, y: 0, w: 1920, h: 1080, scale: 100, main: false };
const plug = async (list, mirror = false) => {
  lord.screen.displays = list;
  lord.screen.mirror = mirror;
  lord.screen.width = Math.max(...list.map((d) => d.x + d.w));
  lord.screen.height = Math.max(...list.map((d) => d.y + d.h));
  globalThis.__lordScreen();
  await flush();
};

// the shell as startShell would render it: again on every screen change (sdk/index.js LocaleRoot does the same)
function Shell() {
  const [rev, set] = React.useState(0);
  React.useEffect(() => rt.onScreenChange(() => set((v) => v + 1)), []);
  return React.cloneElement(globalThis.__shellElement, { screenRev: rev });
}
await plug([L, M, R]);
rt.render(h(Shell));
await flush();

// the bars (a bottom taskbar: 48 px tall, a border on top) and the display each one is on (its parent's place)
const bars = () => [...styles].filter(([id, s]) => s.height === 48 && s.borderTopWidth === 1 && s.position === 'absolute' && attached(id))
  .map(([id]) => ({ id, x: styles.get(parents.get(id)).left })).sort((a, b) => a.x - b.x);
const barOf = (id) => { for (let n = id; n; n = parents.get(n)) { const b = bars().find((x) => x.id === n); if (b) return b.x; } return null; };
// the clock's date on each bar, in display order
const date = sdk.formatDate(new Date(), 'numeric');
const dates = () => [...texts].filter(([id, v]) => v === date && attached(id)).map(([id]) => parents.get(id)).sort((a, b) => barOf(a) - barOf(b));
// the first 40 × 40 button of a bar: the start button
const startOf = (x) => [...styles].filter(([id, s]) => s.width === 40 && s.height === 40 && s.borderRadius === 6 && attached(id) && barOf(id) === x)
  .map(([id]) => id).sort((a, b) => a - b)[0];
const panelAt = (width) => [...styles].filter(([id, s]) => s.width === width && s.left != null && s.position === 'absolute' && attached(id)).map(([, s]) => s);

check('a taskbar on each of the three displays', bars().map((b) => b.x), [0, 1920, 4480]);
check('a clock on each bar', dates().length, 3);
check('every display\'s work area leaves out its bar', [L, M, R].map((d) => sdk.workAreaOf(d)),
  [{ x: 0, y: 0, w: 1920, h: 1032 }, { x: 1920, y: 0, w: 2560, h: 1392 }, { x: 4480, y: 0, w: 1920, h: 1032 }]);
check('one clock timer for three bars', timers(1000), 1);
check('one network poll for three bars', timers(5000), 1);
const polls = counts.netPolls;

// the notifications' panel opens on the display whose clock was clicked, at that bar's tray end
const onBarOf = (d) => [d.x + d.w - 100, d.y + d.h - 20];        // a point on that display's bar (its tray end)
await click(dates()[2], ...onBarOf(R));
const nc = panelAt(380);
check('notifications: one panel', nc.length, 1);
check('notifications: on the right display, at its tray', nc[0]?.left, R.x + R.w - 12 - 380);
// another bar's clock moves it to that display (the press on a bar doesn't close it first)
await click(dates()[0], ...onBarOf(L));
check('notifications: the left bar\'s clock moves it to the left display', panelAt(380).map((s) => s.left), [L.x + L.w - 12 - 380]);
await click(dates()[0], ...onBarOf(L));
check('notifications: the same clock again closes it', panelAt(380).length, 0);

// the start menu opens on the right display, above its bar
await click(startOf(R.x), R.x + 900, R.h - 20);
const box = startBox('bottom', 'center', R);
check('start menu: on the right display', panelAt(640).map((s) => [s.left, s.top]), [[box.left, box.top]]);
check('start menu: inside that display', box.left >= R.x && box.left + box.width <= R.x + R.w && box.top + box.height <= R.y + R.h - 48, true);
// the power menu there (#48): a click on Lock reaches the session
const power = [...styles].filter(([id, s]) => s.width === 40 && s.height === 40 && s.borderRadius === 6 && attached(id) && barOf(id) == null)
  .map(([id]) => id).pop();
check('start menu: the power button is there', !!power, true);
await click(power, R.x + 600, 1000);
const lockLabel = sdk.t('shell.menu.lock');
const lockRow = [...texts].filter(([id, v]) => v === lockLabel && attached(id)).map(([id]) => parents.get(id)).pop();
check('start menu: the power menu shows Lock', !!lockRow, true);
const menuBox = [...styles].filter(([id, s]) => s.width === 220 && s.left != null && attached(id)).map(([, s]) => s)[0];
check('start menu: the power menu stays on the right display', !!menuBox && menuBox.left >= R.x && menuBox.left + 220 <= R.x + R.w, true);
await click(lockRow);
check('start menu: Lock reaches lord.session.lock', sessionCalls.join(','), 'lock');
check('start menu: closed after the click', panelAt(640).length, 0);

// a pin dragged from the start menu and dropped on the right display's bar: only that bar shows where it lands
for (const b of bars()) {
  const d = [L, M, R].find((x) => x.x === b.x);
  layouts.set(b.id, [d.x, d.y + d.h - 48, d.w, 48]);
}
sdk.beginDockDrag({ id: 'calculator', Icon: null, x: R.x + 300, y: 500, source: 'launcher' });
globalThis.__lordEvent('move', 0, R.x + 900, R.h - 20, 0); await flush();
check('drag: over the right display\'s bar', sdk.dockDrag.get()?.over?.bar, R.id);
const lines = () => [...styles].filter(([id, s]) => s.width === 2 && s.top === 8 && attached(id)).map(([id]) => barOf(id));
check('drag: the drop line on that bar alone', lines(), [R.x]);
globalThis.__lordEvent('move', 0, L.x + 900, L.h - 20, 0); await flush();
check('drag: over the left display\'s bar', [sdk.dockDrag.get()?.over?.bar, ...lines()], [L.id, L.x]);
globalThis.__lordEvent('up', 0, L.x + 900, L.h - 20, 0); await flush();
check('drag: dropped on a bar, the app is pinned', sdk.dock.isPinned('calculator'), true);

// quick settings on the right display; unplugging it closes the panel and takes its bar away
const qsButton = [...styles].filter(([id, s]) => s.width === 64 && s.height === 40 && attached(id) && barOf(id) === R.x).map(([id]) => id)[0];
await click(qsButton, ...onBarOf(R));
check('quick settings: on the right display', panelAt(360).map((s) => s.left), [R.x + R.w - 12 - 360]);
await plug([L, M]);
check('unplugged: two taskbars', bars().map((b) => b.x), [0, 1920]);
check('unplugged: the panel on that display closed', panelAt(360).length, 0);
await plug([L, M, R]);
check('plugged back: three taskbars', bars().map((b) => b.x), [0, 1920, 4480]);
check('plugged back: still one clock timer and one network poll', [timers(1000), timers(5000)], [1, 1]);

// the setting off: the main display's bar alone, the others all work area
sdk.settings.set({ menuBarAllDisplays: false }); await flush();
check('setting off: one taskbar, on the main display', bars().map((b) => b.x), [1920]);
check('setting off: the other displays are all work area', [L, M].map((d) => sdk.workAreaOf(d)),
  [{ x: 0, y: 0, w: 1920, h: 1080 }, { x: 1920, y: 0, w: 2560, h: 1392 }]);
sdk.settings.set({ menuBarAllDisplays: true }); await flush();
check('setting on again: three taskbars', bars().length, 3);

// mirrored displays: one bar
await plug([{ ...L, x: 0 }, { ...M, x: 0, w: 1920, h: 1080 }], true);
check('mirrored: one taskbar', bars().length, 1);
await plug([L, M, R]);
// three bars drawn again (another alignment) read the network's last poll, not the network
const p0 = counts.netPolls;
sdk.settings.set({ taskbarAlign: 'start' }); await flush();
check('three bars re-rendered: the network is not read again', [bars().length, counts.netPolls - p0], [3, 0]);
// (the first render reads it, the timer's start reads it again: not once per bar)
check('and the bars appearing read it at most twice', polls <= 2, true);

// (#111) 60 more pins than a 1920 px bar holds: the app buttons shrink to 30 px and the rest go into "more apps", so
// the row ends before the tray (its length the bar measures from the last frame) and is no longer centered
const { taskbarFit, rowLength } = await import('../../themes/classic/taskbarfit.js');
sdk.settings.set({ taskbarAlign: 'center', dockPinned: ['files', 'terminal', 'store', 'settings', ...Array.from({ length: 60 }, (_, i) => `extra${i}`)].map((id) => `app:${id}`) });
await flush();
const trays = () => [...styles].filter(([id, s]) => s.flexDirection === 'row' && s.gap === 2 && s.alignItems === 'center' && attached(id)
  && styles.get(parents.get(id))?.flex === undefined && styles.get(parents.get(id))?.justifyContent === 'flex-end').map(([id]) => id);
// the trays laid out 300 px wide; the bars read that after they render again
for (const tr of trays()) layouts.set(tr, [0, 0, 300, 48]);
sdk.settings.set({ taskbarAlign: 'start' }); await flush();
sdk.settings.set({ taskbarAlign: 'center' }); await flush();
await new Promise((res) => setTimeout(res, 120)); await flush();
const appBtns = (x) => [...styles].filter(([id, s]) => s.width === 30 && s.height === 40 && s.borderRadius === 6 && attached(id) && barOf(id) === x);
const want = taskbarFit({ length: L.w, tray: 304, count: 64, center: true });
check('#111: the full bar\'s buttons shrink to 30 px, the rest in "more apps"', [want.size, want.more, appBtns(L.x).length], [30, true, want.shown + 1]);
const spacer = [...styles].filter(([id, s]) => s.width === want.spacer && s.height === undefined && attached(id) && barOf(id) === L.x).length;
check('#111: the row moves toward the start so it ends before the tray', [spacer, 6 + want.spacer + rowLength(want.shown, 30, true) <= L.w - 6 - 304], [1, true]);
const moreBtn = appBtns(L.x).map(([id]) => id).sort((a, b) => a - b).pop();
await click(moreBtn, L.x + 1000, L.h - 20);
const hiddenNames = Array.from({ length: 64 - want.shown }, (_, i) => `extra${60 - (64 - want.shown) + i}`);
const listed = [...texts].filter(([id, v]) => hiddenNames.includes(v) && attached(id)).length;
check('#111: "more apps" lists every app that is not on the bar', listed, hiddenNames.length);
process.exit(0);
