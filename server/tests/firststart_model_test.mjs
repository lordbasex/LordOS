// firststart_model_test.mjs — the first-start desktop chooser's logic (sdk/firststartmodel.js, 0.8.9): who sees it (a new
// account yes, someone who already used LordOS no, the live system no), the cards' random order, the state that survives
// the live theme switch, and Back / forward between the steps.
//
//   node tests/firststart_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const M = await import(path.join(here, '../../sdk/firststartmodel.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

const ALL = ['default', 'classic', 'workspaces'];
const installed = { live: false };

// ---- who sees it ----
// what sdk/settings.js load() gives: the defaults, then the file, then the migrations from the file's schema
const DEFAULTS = { theme: null, sessionSaved: null, desktopChosen: false, firstStart: null };
const load = (file) => (file == null ? { ...DEFAULTS, schema: 6 } : (file.schema || 0) < 6 ? { ...M.markExistingUser({ ...DEFAULTS, ...file }), schema: 6 } : { ...DEFAULTS, ...file });

eq('a new account without a settings file sees it', M.shouldShow(load(null), installed, ALL), true);
eq('the installer\'s settings file (language, keyboard) is a new account', M.shouldShow(load({ language: 'es', keyboards: [{ layout: 'latam', variant: '' }], keyboardIndex: 0 }), installed, ALL), true);
eq('Settings → Users\' file (language only) is a new account', M.shouldShow(load({ language: 'pt' }), installed, ALL), true);
eq('the OVA\'s first-boot file (schema 1) is a new account', M.shouldShow(load({ schema: 1, language: 'en', mode: 'dark' }), installed, ALL), true);
eq('an existing user (the tour was shown) is migrated: never', M.shouldShow(load({ schema: 4, welcomeShown: true }), installed, ALL), false);
eq('an existing user (a session saved its windows) is migrated', M.shouldShow(load({ schema: 4, sessionSaved: { windows: [], clean: true } }), installed, ALL), false);
eq('an existing user who chose a theme is migrated', M.shouldShow(load({ schema: 2, theme: 'classic' }), installed, ALL), false);
eq('the migration sets the flag', load({ schema: 4, welcomeShown: true }).desktopChosen, true);
eq('the migration leaves a new account alone', load({ language: 'es' }).desktopChosen, false);
eq('a file already at schema 6 is not migrated again', M.shouldShow(load({ schema: 6, welcomeShown: true }), installed, ALL), true);
eq('finished: never again', M.shouldShow({ ...DEFAULTS, desktopChosen: true }, installed, ALL), false);
eq('the live system: never', M.shouldShow(load(null), { live: true }, ALL), false);
eq('the OVA\'s first boot (the installer runs): never', M.shouldShow(load(null), { live: false, firstAccount: true }, ALL), false);
eq('no system information: never', M.shouldShow(load(null), null, ALL), false);
eq('one desktop installed: nothing to choose', M.shouldShow(load(null), installed, ['default']), false);

// ---- the random order ----
const sorted = (a) => [...a].sort();
const orders = new Set();
for (let seed = 1; seed <= 60; seed++) {
  const o = M.shuffle(ALL, M.seeded(seed));
  if (JSON.stringify(sorted(o)) !== JSON.stringify(sorted(ALL))) { bad++; console.log(`✗ seed ${seed}: not a permutation ${o}`); }
  orders.add(o.join(','));
}
n++;
eq('every order turns up across seeds', orders.size, 6);
eq('the same seed, the same order', M.shuffle(ALL, M.seeded(42)), M.shuffle(ALL, M.seeded(42)));
eq('a seed gives a different order from another', M.shuffle(ALL, M.seeded(1)).join() !== M.shuffle(ALL, M.seeded(2)).join() || M.shuffle(ALL, M.seeded(3)).join() !== M.shuffle(ALL, M.seeded(1)).join(), true);
const firstPlace = { default: 0, classic: 0, workspaces: 0 };
for (let seed = 1; seed <= 3000; seed++) firstPlace[M.shuffle(ALL, M.seeded(seed * 7919))[0]]++;
eq('none comes first by design (each 25-42 % of 3000)', Object.values(firstPlace).every((k) => k > 750 && k < 1250), true);
const s0 = M.startState(ALL, M.seeded(9));
eq('it starts on step 1 with nothing chosen', [s0.step, s0.pick], [1, null]);
eq('only the installed desktops are offered', sorted(M.startState(['classic', 'default'], M.seeded(3)).order), ['classic', 'default']);

// ---- the state across the live theme switch (settings.json, the user's file) ----
const st2 = { step: 2, pick: 'classic', order: ['workspaces', 'classic', 'default'], pos: { x: 1500.4, y: 400 } };
const reloaded = M.cleanState(JSON.parse(JSON.stringify(st2)), ALL);
eq('step 2 survives a reload', reloaded, { step: 2, pick: 'classic', order: ['workspaces', 'classic', 'default'], pos: { x: 1500, y: 400 } });
eq('step 3 survives a reload', M.cleanState({ ...st2, step: 3 }, ALL).step, 3);
eq('a step past 1 without a pick goes back to 1', M.cleanState({ ...st2, pick: null }, ALL).step, 1);
eq('an unknown pick is dropped', M.cleanState({ ...st2, pick: 'evil' }, ALL), { step: 1, pick: null, order: st2.order, pos: { x: 1500, y: 400 } });
eq('an order that isn\'t the installed desktops: start over', M.cleanState({ ...st2, order: ['classic', 'classic', 'default'] }, ALL), null);
eq('a short order: start over', M.cleanState({ ...st2, order: ['classic'] }, ALL), null);
eq('a desktop uninstalled since: start over', M.cleanState(st2, ['default', 'classic']), null);
eq('garbage: start over', [M.cleanState(null, ALL), M.cleanState([], ALL), M.cleanState('x', ALL)], [null, null, null]);
eq('an odd step reads as 1', M.cleanState({ ...st2, step: 7 }, ALL).step, 1);
eq('a bad position is dropped', M.cleanState({ ...st2, pos: { x: 'a', y: 1 } }, ALL).pos, null);

// ---- Back and forward ----
let s = M.startState(ALL, M.seeded(5));
eq('step 1: no pick, no next', [M.canNext(s), M.next(s).step], [false, 1]);
s = M.choose(s, 'workspaces');
eq('a pick enables next', M.canNext(s), true);
eq('choosing something not offered changes nothing', M.choose(s, 'greeter').pick, 'workspaces');
s = M.next(s);
eq('1 → 2', s.step, 2);
eq('step 2 switches to the pick', M.themeToApply(s, 'default'), 'workspaces');
eq('no switch when the pick is already the current theme', M.themeToApply(s, 'workspaces'), null);
eq('step 1 never switches', M.themeToApply(M.back(s), 'default'), null);
s = M.choose(s, 'classic');
eq('another name on step 2 switches live', M.themeToApply(s, 'workspaces'), 'classic');
eq('2 → 1 keeps the pick', [M.back(s).step, M.back(s).pick], [1, 'classic']);
s = M.next(s);
eq('2 → 3', s.step, 3);
eq('3 → 2', M.back(s).step, 2);
eq('3 is the last: next stays (Start finishes)', M.next(s).step, 3);
eq('1 has no back', M.back(M.startState(ALL)).step, 1);

// ---- the floating box ----
const area = { x: 0, y: 28, w: 1920, h: 980 };      // Flow: below the menu bar, above the Dock
const size = { w: M.FLOAT_W, h: 186 };
const p0 = M.floatPlace(null, area, size);
eq('it opens at the end of the work area', p0.x + size.w <= area.x + area.w && p0.x > area.x + area.w / 2, true);
eq('about mid-height, inside the work area', p0.y > area.y + area.h * 0.3 && p0.y + size.h < area.y + area.h, true);
eq('a dragged position is kept', M.floatPlace({ x: 300, y: 300 }, area, size), { x: 300, y: 300 });
eq('dragged off the display it stays inside the work area', M.clampFloat({ x: -500, y: 5000 }, area, size), { x: 16, y: area.y + area.h - size.h - 16 });
const ws = { x: 96, y: 60, w: 1812, h: 1008 };      // Workspaces: right of the spine, below the band
eq('never over the Workspaces spine', M.clampFloat({ x: 0, y: 0 }, ws, size), { x: 112, y: 76 });
const tiny = { x: 0, y: 0, w: 300, h: 150 };
eq('a work area smaller than the box: its corner', M.clampFloat({ x: 50, y: 50 }, tiny, size), { x: 0, y: 0 });

console.log(`firststart-model: ${n} checks, ${bad} failures`);
process.exit(bad ? 1 : 0);
