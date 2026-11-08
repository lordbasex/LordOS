// What the bar shows (sdk/menubarmodel.js, issue #58): the choices' defaults, the settings patch, the outputs a sound
// panel offers and the month calendar's cells. node server/tests/menubar_model_test.mjs
import { MENUBAR_DEFAULTS, menuBarPrefs, menuBarPatch, cleanerItemShown, outputChoices, monthCells, weekStartFor } from '../../sdk/menubarmodel.js';

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
console.log(`menubar-model-test: ${ok} ok`);
