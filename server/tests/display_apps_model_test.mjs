// node server/tests/display_apps_model_test.mjs — the apps used on each display (sdk/displayappsmodel.js): the record
// is small and pruned, the quick launch ranks by fading use, and the usual set is only suggested once seen twice,
// never where the person said no, and never while the record is off.
import assert from 'node:assert/strict';
import {
  cleanDisplayApps, noteApp, noteSet, quickApps, suggestionFor, quietDisplay, clearDisplayApps, setDisplayAppsOff, displayAppsEmpty,
  recordable, MAX_APPS, MAX_SETS, MAX_DISPLAYS, MAX_SET_APPS, QUICK_APPS, FORGET_DAYS, HALF_LIFE_DAYS,
} from '../../sdk/displayappsmodel.js';

const DAY = 86400000;
const T0 = Date.UTC(2026, 10, 22, 9);

// anything wrong cleans to an empty record
for (const raw of [null, undefined, 3, 'x', [], { displays: [] }, { displays: { 'bad key!': { apps: [{ id: 'mail', n: 1, t: 1 }] } } }]) {
  assert.deepEqual(cleanDisplayApps(raw), { v: 1, displays: {} });
}
// unrecordable ids (a guest's window, a window with no .desktop) and junk entries are dropped
const dirty = cleanDisplayApps({ displays: { 'DP-2': { apps: [{ id: 'vm:1:x', n: 2, t: 1 }, { id: 'linux:foo', n: 1 }, { id: 'mail', n: -4, t: 'no' }, { id: 'mail', n: 9 }, null],
  sets: [{ apps: ['mail', 'mail', 'vm:2:y'], layout: 'Bad Layout', ratio: 7, n: 2, t: 5 }, { apps: [] }, { apps: ['mail'] }], quiet: 'yes' } } });
assert.deepEqual(dirty.displays['DP-2'], { apps: [{ id: 'mail', n: 1, t: 0 }], sets: [{ apps: ['mail'], n: 2, t: 5 }] });
assert.equal(recordable('vm:1:a'), false);
assert.equal(recordable('linux:x'), false);
assert.equal(recordable('desktop:firefox.desktop'), true);

// apps per display, keyed by connector: one display's apps are not another's
let r = cleanDisplayApps(null);
r = noteApp(r, 'DP-2', 'mail', T0);
r = noteApp(r, 'DP-2', 'terminal', T0 + 1000);
r = noteApp(r, 'DP-2', 'mail', T0 + 2000);
r = noteApp(r, 'HDMI-A-1', 'music', T0 + 3000);
assert.deepEqual(quickApps(r, 'DP-2', T0 + 5000), ['mail', 'terminal']);
assert.deepEqual(quickApps(r, 'HDMI-A-1', T0 + 5000), ['music']);
assert.deepEqual(quickApps(r, 'DP-3', T0 + 5000), []);
// at most QUICK_APPS, only installed ones
for (let i = 0; i < 9; i++) r = noteApp(r, 'DP-2', `app${i}`, T0 + 10000 + i);
assert.equal(quickApps(r, 'DP-2', T0 + 20000).length, QUICK_APPS);
assert.deepEqual(quickApps(r, 'DP-2', T0 + 20000, { have: (id) => id === 'terminal' }), ['terminal']);
// pruned: at most MAX_APPS per display, the newest kept even when it ranks last
for (let i = 0; i < 30; i++) r = noteApp(r, 'DP-2', `more${i}`, T0 + 30000 + i);
assert.equal(r.displays['DP-2'].apps.length, MAX_APPS);
assert.equal(r.displays['DP-2'].apps[0].id, 'more29');
assert.ok(r.displays['DP-2'].apps.some((a) => a.id === 'mail'), 'the most used stays');

// use fades: a once-used app of today beats one used 3 times two months ago
let f = cleanDisplayApps(null);
for (let i = 0; i < 3; i++) f = noteApp(f, 'DP-1', 'old', T0 - 60 * DAY + i);
f = noteApp(f, 'DP-1', 'new', T0);
assert.deepEqual(quickApps(f, 'DP-1', T0 + 1), ['new', 'old']);
assert.ok(HALF_LIFE_DAYS < 60);
// not seen for FORGET_DAYS: no longer offered, and dropped at the next write
assert.deepEqual(quickApps(f, 'DP-1', T0 + (FORGET_DAYS + 1) * DAY), []);
f = noteApp(f, 'DP-1', 'x', T0 - 60 * DAY + FORGET_DAYS * DAY + DAY);
assert.ok(!f.displays['DP-1'].apps.some((a) => a.id === 'old'));

// sets: suggested only once seen MIN_SEEN times; the latest order and layout of the same apps win
let s = cleanDisplayApps(null);
s = noteSet(s, 'DP-2', { apps: ['mail', 'terminal', 'desktop:chrome.desktop'], layout: 'three', ratio: 0.333 }, T0);
assert.equal(suggestionFor(s, 'DP-2', T0 + 1), null, 'once is not "as usual"');
s = noteSet(s, 'DP-2', { apps: ['terminal', 'mail', 'desktop:chrome.desktop'], layout: 'split', ratio: 0.6 }, T0 + DAY);
assert.deepEqual(suggestionFor(s, 'DP-2', T0 + DAY + 1), { apps: ['terminal', 'mail', 'desktop:chrome.desktop'], layout: 'split', ratio: 0.6 });
assert.equal(s.displays['DP-2'].sets.length, 1);
// a set without the apps no longer installed; none left: no suggestion
assert.deepEqual(suggestionFor(s, 'DP-2', T0 + DAY + 1, { have: (id) => id !== 'mail' }).apps, ['terminal', 'desktop:chrome.desktop']);
assert.equal(suggestionFor(s, 'DP-2', T0 + DAY + 1, { have: () => false }), null);
// another display has its own
assert.equal(suggestionFor(s, 'HDMI-A-1', T0 + DAY + 1), null);
// the most frequent set wins over a newer one seen less
s = noteSet(s, 'DP-2', { apps: ['music', 'notes'] }, T0 + 2 * DAY);
s = noteSet(s, 'DP-2', { apps: ['notes', 'music'] }, T0 + 2 * DAY + 5);
s = noteSet(s, 'DP-2', { apps: ['mail', 'terminal', 'desktop:chrome.desktop'] }, T0 + 2 * DAY + 9);
assert.deepEqual(suggestionFor(s, 'DP-2', T0 + 2 * DAY + 10).apps, ['mail', 'terminal', 'desktop:chrome.desktop']);
assert.equal(suggestionFor(s, 'DP-2', T0 + 2 * DAY + 10).layout, null, 'the latest layout (none) of that set');
// at most MAX_SET_APPS apps per set, MAX_SETS sets per display
s = noteSet(s, 'DP-2', { apps: Array.from({ length: 10 }, (_, i) => `a${i}`) }, T0 + 3 * DAY);
assert.equal(s.displays['DP-2'].sets[0].apps.length, MAX_SET_APPS);
for (let i = 0; i < 20; i++) s = noteSet(s, 'DP-2', { apps: [`solo${i}`] }, T0 + 4 * DAY + i);
assert.equal(s.displays['DP-2'].sets.length, MAX_SETS);
assert.equal(s.displays['DP-2'].sets[0].apps[0], 'solo19');

// "Don't suggest on this display": that display only, and the quick launch stays
const q = quietDisplay(noteSet(noteSet(noteSet(s, 'DP-3', { apps: ['mail'] }, T0), 'DP-3', { apps: ['mail'] }, T0 + 1), 'DP-3', { apps: ['mail'] }, T0 + 2), 'DP-2');
assert.equal(suggestionFor(q, 'DP-2', T0 + 4 * DAY + 100), null);
assert.deepEqual(suggestionFor(q, 'DP-3', T0 + 3).apps, ['mail']);
assert.equal(cleanDisplayApps(q).displays['DP-2'].quiet, true);

// at most MAX_DISPLAYS: the one used longest ago goes, never the one just written
let m = cleanDisplayApps(null);
for (let i = 0; i < MAX_DISPLAYS + 3; i++) m = noteApp(m, `DP-${i}`, 'mail', T0 + i);
assert.equal(Object.keys(m.displays).length, MAX_DISPLAYS);
assert.ok(!m.displays['DP-0'] && m.displays[`DP-${MAX_DISPLAYS + 2}`]);
m = noteApp(m, 'DP-0', 'mail', T0 - DAY);
assert.ok(m.displays['DP-0'], 'the display just written stays even when it is the oldest');

// off: nothing is recorded or offered, and turning it off forgets; clearing forgets every display (quiet ones too)
const off = setDisplayAppsOff(q, true);
assert.equal(displayAppsEmpty(off), true);
assert.deepEqual(noteApp(off, 'DP-2', 'mail', T0), { v: 1, off: true, displays: {} });
assert.deepEqual(noteSet(off, 'DP-2', { apps: ['mail'] }, T0), { v: 1, off: true, displays: {} });
assert.deepEqual(quickApps({ ...q, off: true }, 'DP-2', T0), []);
assert.equal(suggestionFor({ ...q, off: true }, 'DP-3', T0 + 3), null);
assert.deepEqual(setDisplayAppsOff(off, false), { v: 1, displays: {} });
assert.deepEqual(setDisplayAppsOff(q, false), cleanDisplayApps(q), 'turning on what is on keeps the record');
assert.deepEqual(clearDisplayApps(q), { v: 1, displays: {} });
assert.deepEqual(clearDisplayApps(off), { v: 1, off: true, displays: {} });
assert.equal(displayAppsEmpty(q), false);

// small: a busy record of every display stays a few KB in the settings file
let big = cleanDisplayApps(null);
for (let d = 0; d < 12; d++) for (let i = 0; i < 40; i++) {
  big = noteApp(big, `DP-${d}`, `desktop:some-long-application-name-${i}.desktop`, T0 + d * 100 + i);
  big = noteSet(big, `DP-${d}`, { apps: [`desktop:some-long-application-name-${i}.desktop`, `desktop:other-${i % 7}.desktop`], layout: 'split', ratio: 0.5 }, T0 + d * 100 + i);
}
assert.ok(JSON.stringify(big).length < 16000, `record too big: ${JSON.stringify(big).length}`);
assert.deepEqual(cleanDisplayApps(JSON.parse(JSON.stringify(big))), big, 'cleaning a clean record keeps it');
console.log('display_apps_model_test: ok');
