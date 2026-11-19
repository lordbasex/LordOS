// keyboard_default_test.mjs — (0.8.8.6, #55) the keyboard of a user who never chose one: the computer's
// (/etc/default/keyboard through lord.keyboard.system(), US without it), never Latin American because a
// default said so; and settings migration 4, which drops the saved 'latam' default. Bundles the real
// sdk/keyboard.js and sdk/settings.js with esbuild over a stub '@lordos/runtime'. Needs node_modules (esbuild).
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
const nodePaths = [path.join(root, 'node_modules'), ...(process.env.NODE_PATH || '').split(':').filter(Boolean)];
let esbuild = null;
for (const p of nodePaths) { try { esbuild = createRequire(path.join(p, 'x.js'))('esbuild'); break; } catch (e) { /* next */ } }
if (!esbuild) { console.log('needs node_modules (esbuild)'); process.exit(1); }

const entry = `
import { keyboard, systemKeyboards } from './sdk/keyboard.js';
import { settings, migrateSettings } from './sdk/settings.js';
globalThis.__out = { keyboard, systemKeyboards, settings, migrateSettings };`;
const stub = {
  name: 'runtime-stub',
  setup(b) {
    b.onResolve({ filter: /^@lordos\/runtime$/ }, () => ({ path: 'runtime', namespace: 'stub' }));
    b.onLoad({ filter: /.*/, namespace: 'stub' }, () => ({ contents: 'export const setLayout = (l) => { globalThis.__fallback = l; };', loader: 'js' }));
  },
};

let n = 0, bad = 0;
const eq = (what, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${what}\n    got  ${g}\n    want ${w}`); }
};

// A fresh session: settings.json as `file`, /etc/default/keyboard as `system`.
async function session(file, system) {
  const sets = [];
  globalThis.lord = {
    settings: { load: () => file, save: () => true },
    keyboard: {
      current: () => ({ xkb: true }), system: () => system,
      set: (l, v) => { sets.push([l, v]); return true; }, setGroup: () => {},
    },
  };
  const r = await esbuild.build({ stdin: { contents: entry, resolveDir: root, loader: 'js' }, bundle: true, write: false, format: 'iife', plugins: [stub], logLevel: 'silent' });
  new Function(r.outputFiles[0].text)();
  return { ...globalThis.__out, sets };
}

const { systemKeyboards, migrateSettings } = await session('', { layouts: 'us', variants: '' });
eq('system: one layout', systemKeyboards({ layouts: 'gb', variants: '' }), [{ layout: 'gb', variant: '' }]);
eq('system: two layouts with variants', systemKeyboards({ layouts: 'us,latam', variants: ',deadtilde' }),
  [{ layout: 'us', variant: '' }, { layout: 'latam', variant: 'deadtilde' }]);
eq('system: none is US', systemKeyboards({ layouts: '', variants: '' }), [{ layout: 'us', variant: '' }]);
eq('system: an older server without system() is US', systemKeyboards(undefined), [{ layout: 'us', variant: '' }]);

// a new user on a computer set to UK English (install.sh over Debian): the session types UK English
let s = await session('', { layouts: 'gb', variants: '' });
eq('new user: the computer\'s layout', s.keyboard.active(), { layout: 'gb', variant: '' });
eq('new user: applied to the server', s.sets[0], ['gb', '']);
// what 0.8.8.5 saved for that user: the 'latam' default with every other setting
s = await session(JSON.stringify({ schema: 3, keyboard: 'latam', keyboards: null, mode: 'dark' }), { layouts: 'us', variants: '' });
eq('the saved latam default is dropped', s.keyboard.active(), { layout: 'us', variant: '' });
eq('and the server gets US', s.sets[0], ['us', '']);
// a chosen keyboard always wins
s = await session(JSON.stringify({ schema: 3, keyboard: 'latam', keyboards: [{ layout: 'latam', variant: '' }] }), { layouts: 'us', variants: '' });
eq('a chosen list is kept', s.keyboard.active(), { layout: 'latam', variant: '' });
s = await session(JSON.stringify({ schema: 4, keyboard: 'br' }), { layouts: 'us', variants: '' });
eq('a pre-M5 choice other than the default is kept', s.keyboard.active(), { layout: 'br', variant: '' });

eq('migration 4: latam without a list', migrateSettings({ schema: 3, keyboard: 'latam', keyboards: null }), { schema: 9, keyboard: null, keyboards: null });
eq('migration 4: latam with a list', migrateSettings({ schema: 3, keyboard: 'latam', keyboards: [{ layout: 'latam' }] }).keyboard, 'latam');
eq('migration 4: runs once', migrateSettings({ schema: 4, keyboard: 'latam', keyboards: null }).keyboard, 'latam');

// migration 5 (0.8.9): the saved Lumo Celestial default becomes the theme's default (null; Lord Workspaces shows its own)
eq('migration 5: the saved default', migrateSettings({ schema: 4, wallpaper: 'lumo-celestial' }), { schema: 9, wallpaper: null });
eq('migration 5: another picture stays', migrateSettings({ schema: 4, wallpaper: 'crystal-crown' }).wallpaper, 'crystal-crown');
eq('migration 5: a picture of the user\'s own stays', migrateSettings({ schema: 4, wallpaper: 'image:0123456789abcdef' }).wallpaper, 'image:0123456789abcdef');
eq('migration 7: the saved default accent', migrateSettings({ schema: 6, accent: 'azul' }), { schema: 9, accent: null });
eq('migration 7: a chosen accent stays', migrateSettings({ schema: 6, accent: 'verde' }).accent, 'verde');
// migration 9 (0.8.9.1, #193): each weather widget keeps the city it showed (the shared one) as its own
eq('migration 9: the shared city becomes each widget\'s own', migrateSettings({ schema: 8, weather: { on: true, city: { id: 3002280, name: 'Le Rouret', cc: 'FR' } },
  deskWidgets: [{ id: 'w', type: 'weather', opts: {} }] }).deskWidgets[0].opts.city, { id: 3002280, name: 'Le Rouret', cc: 'FR' });
eq('migration 5: runs once (Celestial chosen again later)', migrateSettings({ schema: 5, wallpaper: 'lumo-celestial' }).wallpaper, 'lumo-celestial');
eq('migration 5: a display\'s own picture stays', migrateSettings({ schema: 4, wallpaper: 'lumo-celestial', wallpapers: { 'DP-1': 'lumo-celestial' } }).wallpapers, { 'DP-1': 'lumo-celestial' });
s = await session('', { layouts: 'us', variants: '' });
eq('new user: no wallpaper chosen (the theme\'s default)', s.settings.get().wallpaper, null);

// migration 8 (0.8.9.1, #201): Lord Flow's ⌃ arrows are off by default, so apps get ⌃← ⌃→ for moving by word
eq('migration 8: the saved default (on) turns off', migrateSettings({ schema: 7, desktopArrows: true }), { schema: 9, desktopArrows: false });
eq('migration 8: turned off stays off', migrateSettings({ schema: 7, desktopArrows: false }).desktopArrows, false);
eq('migration 8: runs once (turned on again later)', migrateSettings({ schema: 8, desktopArrows: true }).desktopArrows, true);
eq('new user: ⌃ arrows off', s.settings.get().desktopArrows, false);
s = await session(JSON.stringify({ schema: 7, desktopArrows: true, mode: 'dark' }), { layouts: 'us', variants: '' });
eq('0.8.9 user with the saved default: off after the update', s.settings.get().desktopArrows, false);
s = await session(JSON.stringify({ schema: 8, desktopArrows: true }), { layouts: 'us', variants: '' });
eq('a user who turned them on after the update keeps them', s.settings.get().desktopArrows, true);
s = await session(JSON.stringify({ schema: 6 }), { layouts: 'us', variants: '' });
eq('a file without the key gets the new default', s.settings.get().desktopArrows, false);

console.log(bad ? `keyboard-default: ${bad} of ${n} failed` : `keyboard-default: ${n}/${n} ok`);
process.exit(bad ? 1 : 0);
