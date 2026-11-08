// What the bar shows (sdk/menubarmodel.js, issue #58): the choices' defaults, the settings patch, the outputs a sound
// panel offers and the month calendar's cells. node server/tests/menubar_model_test.mjs
import { MENUBAR_DEFAULTS, menuBarPrefs, menuBarPatch, cleanerItemShown, outputChoices, monthCells, weekStartFor,
  menuBarEveryDisplay, barDisplays, hasBar, displayIdFor, activeBarId, pinBar, focusDisplayId, panelLeft, inBar, insideDisplay } from '../../sdk/menubarmodel.js';
import { sharedTicker } from '../../sdk/ticker.js';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };

// a settings.json from before 0.8.8.6 shows everything, as before
eq('no menuBar', menuBarPrefs({}), MENUBAR_DEFAULTS);
eq('null settings', menuBarPrefs(null), MENUBAR_DEFAULTS);
eq('menuBar not an object', menuBarPrefs({ menuBar: 'off' }), MENUBAR_DEFAULTS);
eq('one off', menuBarPrefs({ menuBar: { osk: false } }).osk, false);
eq('others stay on', menuBarPrefs({ menuBar: { osk: false } }).keyboard, true);
eq('only booleans count', menuBarPrefs({ menuBar: { bell: 0, calendar: 'no' } }).bell, true);
eq('unknown keys are dropped', Object.keys(menuBarPrefs({ menuBar: { foo: false } })), Object.keys(MENUBAR_DEFAULTS));

// the patch keeps the other choices and touches nothing else
eq('patch', menuBarPatch({ mode: 'dark', menuBar: { osk: false } }, 'keyboard', false), { menuBar: { osk: false, keyboard: false } });
eq('patch from nothing', menuBarPatch({}, 'ccWifi', false), { menuBar: { ccWifi: false } });
eq('patch unknown key', menuBarPatch({}, 'nope', false), {});
// the crown is Cleaner's own key, shared with Cleaner → Settings
eq('cleaner patch', menuBarPatch({ cleaner: { lastScan: 1 } }, 'cleaner', false), { cleaner: { lastScan: 1, menuItem: false } });
eq('cleaner shown by default', cleanerItemShown({}), true);
eq('cleaner hidden', cleanerItemShown({ cleaner: { menuItem: false } }), false);

// outputs: the default one marked, the description as the label
const st = { sinks: [
  { index: 31, name: 'alsa_output.pci.analog', description: 'Built-in Audio', default: false, level: 0.4 },
  { index: 52, name: 'bluez_output.AA_BB', description: 'Headset', default: true, level: 0.8 },
  { index: 60, name: 'hdmi', description: '', default: false },
] };
eq('outputs', outputChoices(st), [
  { index: 31, name: 'alsa_output.pci.analog', label: 'Built-in Audio', default: false },
  { index: 52, name: 'bluez_output.AA_BB', label: 'Headset', default: true },
  { index: 60, name: 'hdmi', label: 'hdmi', default: false },
]);
eq('no default said: the first', outputChoices({ sinks: [{ index: 1, name: 'a' }, { index: 2, name: 'b' }] }).map((x) => x.default), [true, false]);
eq('no sound', outputChoices({ connected: false }), []);

// month cells: November 2026 starts on a Sunday and has 30 days
const nov = monthCells(2026, 10, 0);
eq('sunday first: no lead', nov.slice(0, 2), [1, 2]);
eq('whole weeks', nov.length % 7, 0);
eq('30 days', nov.filter(Boolean).length, 30);
const novMon = monthCells(2026, 10, 1);
eq('monday first: six blanks', novMon.slice(0, 7), [null, null, null, null, null, null, 1]);
eq('february 2028 (leap)', monthCells(2028, 1, 1).filter(Boolean).length, 29);
eq('week start en', weekStartFor('en-US'), 0);
eq('week start es', weekStartFor('es'), 1);
eq('week start unknown', weekStartFor(undefined), 0);

// ---- a bar on every display (0.8.8.6) ----
// three monitors side by side, the main one in the middle (as on the maintainer's RX 580)
const L = { id: 41, x: 0, y: 0, w: 1920, h: 1080 }, M = { id: 42, x: 1920, y: 0, w: 2560, h: 1440, main: true }, R = { id: 43, x: 4480, y: 0, w: 1920, h: 1080 };
const three = [L, M, R];
eq('every display: on without the key', menuBarEveryDisplay({}), true);
eq('every display: on without settings', menuBarEveryDisplay(null), true);
eq('every display: kept on', menuBarEveryDisplay({ menuBarAllDisplays: true }), true);
eq('every display: the user turned it off', menuBarEveryDisplay({ menuBarAllDisplays: false }), false);
eq('every display: patch', menuBarPatch({ menuBar: { osk: false } }, 'everyDisplay', false), { menuBarAllDisplays: false });
eq('bars: every display', barDisplays(three, true, false).map((d) => d.id), [41, 42, 43]);
eq('bars: off, the main one', barDisplays(three, false, false).map((d) => d.id), [42]);
eq('bars: mirrored, one', barDisplays(three, true, true).map((d) => d.id), [42]);
eq('bars: no main flag, the first', barDisplays([{ id: 1, x: 0, y: 0, w: 10, h: 10 }, { id: 2, x: 10, y: 0, w: 10, h: 10 }], false, false).map((d) => d.id), [1]);
eq('bars: no displays', barDisplays([], true, false), []);
eq('hasBar: secondary, on', hasBar(L, true, false), true);
eq('hasBar: secondary, off', hasBar(L, false, false), false);
eq('hasBar: main, off', hasBar(M, false, false), true);
eq('hasBar: secondary, mirrored', hasBar(R, true, true), false);

eq('displayIdFor: inside the right one', displayIdFor(three, { x: 4600, y: 100, w: 800, h: 600 }), 43);
eq('displayIdFor: spanning, the bigger part', displayIdFor(three, { x: 1500, y: 100, w: 1000, h: 600 }), 42);
eq('displayIdFor: off every display, the nearest', displayIdFor(three, { x: 7000, y: 100, w: 100, h: 100 }), 43);
eq('displayIdFor: no rect, the main', displayIdFor(three, null), 42);
eq('focusDisplayId: none, the main', focusDisplayId(three, null), 42);

const bars = barDisplays(three, true, false);
const onRight = { id: 'files', rect: { x: 4600, y: 40, w: 900, h: 700 } };
eq('lit: the focused window\'s display', activeBarId(three, bars, onRight, null), 43);
eq('lit: nothing focused, the main', activeBarId(three, bars, null, null), 42);
eq('lit: only the main bar', activeBarId(three, barDisplays(three, false, false), onRight, null), 42);
const pin = pinBar(three, onRight, 41);
eq('pin: what it keeps', pin, { display: 41, focus: 'files', from: 43 });
eq('lit: the dimmed bar the user clicked', activeBarId(three, bars, onRight, pin), 41);
eq('lit: stale pin after the focus moves', activeBarId(three, bars, { id: 'terminal', rect: onRight.rect }, pin), 43);
eq('lit: stale pin after the window moves', activeBarId(three, bars, { id: 'files', rect: { x: 2000, y: 40, w: 900, h: 700 } }, pin), 42);
eq('lit: pin on an unplugged display', activeBarId(three, [L, M].filter((d) => d.id !== 41), onRight, pin), 42);
eq('lit: pin with nothing focused', activeBarId(three, bars, null, pinBar(three, null, 43)), 43);
eq('lit: no bars', activeBarId(three, [], onRight, null), null);

// panels under the item that opened them, inside that display
eq('panel: under an item on the right half', panelLeft([4480 + 1700, 60], 320, R), 4480 + 1760 - 320);
eq('panel: under an item on the left half', panelLeft([200, 40], 320, R), 4480 + 10);
eq('panel: an item near the right edge stays inside', panelLeft([6380, 20], 340, R), 4480 + 1920 - 340 - 10);
eq('panel: an item on the left display stays inside it', panelLeft([1880, 40], 340, L), 1920 - 340 - 10);
eq('panel: no item, the right edge as before', panelLeft(null, 320, M), 1920 + 2560 - 330);
eq('panel: wider than the display', panelLeft([10, 10], 2000, L), 10);
eq('panel: no area', panelLeft([10, 10], 300, null), null);
eq('inBar: on that bar', inBar(4500, 10, R, 28), true);
eq('inBar: below it', inBar(4500, 28, R, 28), false);
eq('inBar: another display\'s bar', inBar(100, 10, R, 28), false);
eq('menu: inside its display at the right edge', insideDisplay(1900, 30, 260, 300, L), { x: 1920 - 260 - 4, y: 30 });
eq('menu: taller than the display', insideDisplay(10, 500, 200, 2000, L), { x: 10, y: 4 });
eq('menu: no display', insideDisplay(5, 6, 1, 1, null), { x: 5, y: 6 });

// one timer for every bar's clock
{
  const live = new Set();
  let made = 0;
  const realSet = globalThis.setInterval, realClear = globalThis.clearInterval;
  globalThis.setInterval = () => { made++; const id = { n: made }; live.add(id); return id; };
  globalThis.clearInterval = (id) => { live.delete(id); };
  let n = 0;
  const tk = sharedTicker(() => `t${n}`, 1000);
  eq('ticker: read lazily', n === 0 && tk.get() === 't0', true);
  eq('ticker: no timer before a subscriber', tk.running(), false);
  let calls = 0;
  const offs = [1, 2, 3].map(() => tk.subscribe(() => { calls++; }));
  eq('ticker: three bars, one timer', [made, live.size], [1, 1]);
  n = 1;
  offs.slice(0, 2).forEach((f) => f());
  eq('ticker: still running with one bar', tk.running(), true);
  offs[2]();
  offs[2]();                                 // unsubscribing twice is harmless
  eq('ticker: stopped with no bars', [tk.running(), live.size], [false, 0]);
  const off = tk.subscribe(() => { calls++; });
  eq('ticker: a new bar reads the value at once', tk.get(), 't1');
  off();
  globalThis.setInterval = realSet; globalThis.clearInterval = realClear;
}
{
  // the change notification, with real timers
  let v = 0;
  const tk = sharedTicker(() => v, 5);
  const seen = [];
  const a = tk.subscribe(() => seen.push('a')), b = tk.subscribe(() => seen.push('b'));
  v = 1;
  await new Promise((res) => setTimeout(res, 30));
  a(); b();
  eq('ticker: every bar hears a change once', seen.slice(0, 2), ['a', 'b']);
  eq('ticker: unchanged values notify nobody', seen.length, 2);
}
console.log(`menubar-model-test: ${ok} ok`);
