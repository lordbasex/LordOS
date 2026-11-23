// store_lordos_test.mjs — Store → Apps LordOS, LordOS's own store (apps/store/lordosmodel.js): the catalog file
// (assets/store/lordos-apps.json) as the shop reads it, what a hand-edited entry may not do, which items are installed,
// the cards' action, the second column's categories (none while the list is short), the search, the Installed rows'
// ids; and that every entry of the shipped file has its two texts in every language.
//
//   node tests/store_lordos_test.mjs        (from server/; run_suites.sh runs it when node exists)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.join(here, '../..');
const M = await import(path.join(root, 'apps/store/lordosmodel.js'));
const DATA = JSON.parse(fs.readFileSync(path.join(root, 'assets/store/lordos-apps.json'), 'utf8'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

// the shipped list
const items = M.nativeItems(DATA);
eq('every shipped entry is read', items.length, DATA.apps.length);
eq('in the file\'s order', items.map((a) => a.id), ['studioCamera', 'diskFormats', 'webRuntime', 'voices']);
eq('Studio Camera first: its package, Open shows the Camera app', [items[0].kind, items[0].package, items[0].open], ['deb', 'lordos-studio-camera', { app: 'camera', args: null }]);
eq('Disk Formats keeps its catalog page (licenses first)', [items[1].kind, items[1].package], ['catalog', 'lordos-disk-formats']);
eq('the Web Runtime opens the Web apps section', items[2].open, { page: 'src:web' });
eq('the voices: Settings → AI, installed when a model is there', [items[3].kind, items[3].package, items[3].installed, items[3].open],
  ['settings', null, 'voices', { app: 'settings', args: { section: 'ai' } }]);
// a Disk Formats entry must still be in the catalog, with the drivers' page
const catalog = JSON.parse(fs.readFileSync(path.join(root, 'apt/catalog.json'), 'utf8'));
eq('every catalog entry is in apt/catalog.json', items.filter((a) => a.kind === 'catalog').every((a) => catalog.apps.some((c) => c.package === a.package)), true);

// what a hand-edited file may not do
const odd = M.nativeItems({ apps: [
  { id: 'ok', kind: 'deb', package: 'lordos-x' },
  { id: 'ok', kind: 'deb', package: 'lordos-y' },                    // the same id again
  { id: 'bad kind', kind: 'deb', package: 'lordos-z' },             // not an id the texts can use
  { id: 'script', kind: 'exec', package: 'lordos-z' },              // not a kind
  { id: 'noPkg', kind: 'deb' },
  { id: 'evilPkg', kind: 'deb', package: 'lordos-x; rm -rf /' },
  { id: 'setNoApp', kind: 'settings' },
  { id: 'setOk', kind: 'settings', open: { app: 'settings', args: { section: 'ai' } } },
  { id: 'badOpen', kind: 'deb', package: 'lordos-q', open: { app: '../../bin/sh' } },
  { id: 'pageOpen', kind: 'deb', package: 'lordos-r', open: { page: 'src:web' } },
  null, 'x',
] });
eq('only the well-formed entries', odd.map((a) => a.id), ['ok', 'setOk', 'badOpen', 'pageOpen']);
eq('a bad open target is dropped, not the entry', odd[2].open, null);
eq('no data, no items', [M.nativeItems(null), M.nativeItems({}), M.nativeItems({ apps: 'x' })], [[], [], []]);

// installed
const probe = { pkg: (p) => p === 'lordos-studio-camera', voices: () => true };
eq('installed: a package, the voices', M.installedItems(items, probe).map((a) => a.id), ['studioCamera', 'voices']);
eq('nothing without the index', M.installedItems(items, { pkg: () => false, voices: () => false }), []);
eq('no probe, nothing', M.isInstalled(items[0], null), false);

// the card's action
eq('actions', [M.actionOf(items[0], false), M.actionOf(items[0], true), M.actionOf({ open: null }, true)], ['get', 'open', 'view']);

// the second column
eq('four entries: no categories', M.nativeCats(items), { cats: [], counts: {} });
const six = [...items, { id: 'a', category: 'media' }, { id: 'b', category: 'games' }];
eq('six or more: each category once, in order, with counts', M.nativeCats(six), { cats: ['media', 'system', 'voice', 'games'], counts: { media: 2, system: 2, voice: 1, games: 1 } });
eq('a category keeps its entries', M.inCategory(six, 'system').map((a) => a.id), ['diskFormats', 'webRuntime']);
eq('no category keeps all', M.inCategory(items, null).length, items.length);

// the search, in the person's language
const text = (k) => ({ 'store.native.studioCamera.name': 'LordOS Studio Camera', 'store.native.studioCamera.summary': 'La cámara en todas las apps',
  'store.native.voices.name': 'Voces de LordOS' }[k] || '');
eq('by name', M.searchNative(items, 'studio', text).map((a) => a.id), ['studioCamera']);
eq('accents and case ignored', M.searchNative(items, 'CAMARA', text).map((a) => a.id), ['studioCamera']);
eq('by package', M.searchNative(items, 'lordos-web', text).map((a) => a.id), ['webRuntime']);
eq('every word', M.searchNative(items, 'voces lordos', text).map((a) => a.id), ['voices']);
eq('an empty query keeps all', M.searchNative(items, '  ', text).length, items.length);

// the Installed rows
eq('row ids', [M.rowId(items[0]), M.fromRowId('native:studioCamera'), M.fromRowId('files'), M.fromRowId(null)], ['native:studioCamera', 'studioCamera', null, null]);
eq('the row kind', M.NATIVE_KIND, 'native');

// the texts of every shipped entry, in every language
const locales = path.join(root, 'assets/locales');
const missing = [];
for (const l of fs.readdirSync(locales)) {
  const f = path.join(locales, l, 'store.json');
  if (!fs.existsSync(f)) continue;
  const cat = JSON.parse(fs.readFileSync(f, 'utf8'));
  for (const a of items) for (const k of [M.nameKey(a), M.summaryKey(a)]) if (!cat[k.replace(/^store\./, '')]) missing.push(`${l}:${k}`);
}
eq('every entry has its name and summary in every language', missing, []);

console.log(`store-lordos-test: ${n} checks, ${bad} failures`);
process.exit(bad ? 1 : 0);
