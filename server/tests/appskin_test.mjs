// appskin_test.mjs — style packs and app skins (sdk/appskinmodel.js, apps/files/skinmodel.js; docs/THEMES-GUIDE.md
// "Style packs" and "App skins"): what a theme's pack and skins keep after checking, the person's switch per part, what
// lord-server is told, and the view model Files hands a skin. Files with every region stubbed: files_skin_scene.mjs.
//
//   node tests/appskin_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.join(here, '..', '..');
const M = await import(path.join(root, 'sdk/appskinmodel.js'));
const F = await import(path.join(root, 'apps/files/skinmodel.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};
const ok = (name, v) => eq(name, !!v, true);

// ---- skins ----
const A = () => null, B = () => null;
const skins = M.cleanSkins({ files: { toolbar: A, sidebar: 'x', nope: B, itemView: B }, notes: { toolbar: A }, files2: null });
eq('only known apps and regions, only components', Object.keys(skins), ['files']);
eq('files keeps toolbar and itemView', Object.keys(skins.files), ['toolbar', 'itemView']);
eq('no skins from junk', M.cleanSkins('x'), {});
ok('skinFor: the component', M.skinFor(skins, {}, 'files', 'toolbar') === A);
eq('skinFor: layouts off', M.skinFor(skins, { themeParts: { layouts: false } }, 'files', 'toolbar'), null);
eq('skinFor: a region without one', M.skinFor(skins, {}, 'files', 'statusBar'), null);
eq('every Files region is named', M.APP_REGIONS.files, ['toolbar', 'addressBar', 'sidebar', 'itemView', 'statusBar', 'contextMenu']);

// ---- parts ----
eq('parts default on', M.STYLE_PARTS.map((p) => M.partOn({}, p)), [true, true, true, true, true]);
eq('a part off', M.partOn({ themeParts: { icons: false } }, 'icons'), false);
eq('partPatch keeps the others', M.partPatch({ themeParts: { icons: false } }, 'sounds', false), { themeParts: { icons: false, sounds: false } });

// ---- tunes ----
eq('a tune', M.cleanTune([[523.25, 120], [0, 40], [784, 200]]), [[523.25, 120], [0, 40], [784, 200]]);
eq('too many notes', M.cleanTune(Array(9).fill([440, 50])), null);
eq('too long in all', M.cleanTune([[440, 1000], [440, 1000], [440, 600]]), null);
eq('a note too high', M.cleanTune([[9000, 100]]), null);
eq('a note too short', M.cleanTune([[440, 5]]), null);
eq('not numbers', M.cleanTune([['440', 100]]), null);
eq('empty', M.cleanTune([]), null);
// the same limits as lord-server's (server/src/sound/snd.h)
const snd = fs.readFileSync(path.join(root, 'server/src/sound/snd.h'), 'utf8');
eq('MAX_NOTES matches snd.h', Number(/SND_SCHEME_NOTES (\d+)/.exec(snd)[1]), M.MAX_NOTES);
eq('MAX_TUNE_MS matches snd.h', Number(/SND_SCHEME_MAX_MS (\d+)/.exec(snd)[1]), M.MAX_TUNE_MS);
const sndc = fs.readFileSync(path.join(root, 'server/src/sound/snd.c'), 'utf8');
ok('every sound event is one snd_set_scheme knows', M.SOUND_EVENTS.every((e) => sndc.includes(`"${e}"`)));

// ---- cursors, fonts, icons ----
eq('cursors', M.cleanCursors({ fill: '#FFFFFF', outline: '#000000' }), { fill: '#ffffff', outline: '#000000' });
eq('cursors: a bad colour', M.cleanCursors({ fill: 'white', outline: '#000000' }), null);
eq('a family', M.cleanFamily('DejaVu Sans'), 'DejaVu Sans');
eq('a family with a path', M.cleanFamily('../../x'), null);
eq('an image icon', M.cleanIcon('assets/icons/folder.png'), 'assets/icons/folder.png');
eq('an icon outside assets', M.cleanIcon('/etc/x.png'), null);
eq('an icon going up', M.cleanIcon('assets/../x.png'), null);
eq('an icon that is a URL', M.cleanIcon('https://x/y.png'), null);
const Comp = () => null;
const pack = M.cleanStylePack({
  icons: { apps: { files: Comp, 'bad name!': Comp }, files: { folder: 'assets/f.png', text: 7 }, toolbar: { back: Comp, launch: Comp }, other: { x: Comp } },
  cursors: { fill: '#ffffff', outline: '#000000' },
  sounds: { startup: [[523, 100]], notify: [[1, 1]], boom: [[440, 100]] },
  fonts: { family: 'DejaVu Sans' },
});
eq('pack icons kept', [Object.keys(pack.icons.apps), Object.keys(pack.icons.files), Object.keys(pack.icons.toolbar)], [['files'], ['folder'], ['back']]);
eq('pack sounds kept', Object.keys(pack.sounds), ['startup']);
eq('pack parts', M.packParts(pack, skins), ['icons', 'cursors', 'sounds', 'fonts', 'layouts']);
eq('no pack, no parts', M.packParts(M.cleanStylePack(null), {}), []);
ok('packIcon', M.packIcon(pack, {}, 'toolbar', 'back') === Comp);
eq('packIcon with icons off', M.packIcon(pack, { themeParts: { icons: false } }, 'toolbar', 'back'), null);
eq('packIcon: not an own property', M.packIcon(pack, {}, 'files', 'constructor'), null);
eq('effects', M.packEffects(pack, {}), { cursors: { fill: '#ffffff', outline: '#000000' }, sounds: { startup: [[523, 100]], notify: null, error: null, emptyTrash: null }, font: 'DejaVu Sans' });
eq('effects with everything off', M.packEffects(pack, { themeParts: { cursors: false, sounds: false, fonts: false } }),
  { cursors: null, sounds: { startup: null, notify: null, error: null, emptyTrash: null }, font: null });

// ---- freezing ----
const fn = () => 1;
const model = M.freezeModel({ a: { b: [1, { c: 2 }] }, act: { go: fn } });
ok('frozen deep', Object.isFrozen(model) && Object.isFrozen(model.a.b[1]) && Object.isFrozen(model.act));
ok('functions kept', model.act.go === fn && !Object.isFrozen(fn));

// ---- Files' view model ----
const entries = [
  { name: 'Docs', path: '/h/Docs', dir: true, size: 0, mtime: 5 },
  { name: 'a.txt', path: '/h/a.txt', dir: false, size: 12, mtime: 6, secret: 'internal' },
];
const vm = F.filesViewModel({
  loc: { type: 'dir', path: '/h' }, title: 'Home', crumbs: [{ label: 'Home', folder: true }], readOnly: false, writable: true,
  canBack: true, canForward: false, canUp: false, view: 'icons', views: ['icons', 'list'], sort: { by: 'name', desc: false }, zoom: 0, query: '',
  items: entries, selected: new Set(['/h/a.txt']), cut: new Set(), renaming: null, places: [{ id: 'home', label: 'Home', kind: 'folder', selected: true, extra: 1 }],
  statusText: '1 of 2 selected', canPaste: false, FileIcon: fn,
}, { open: fn });
eq('items are copies with only what a skin needs', Object.keys(vm.items[1]), ['key', 'name', 'label', 'dir', 'size', 'mtime', 'kind', 'ext', 'selected', 'cut', 'renaming']);
eq('kinds', vm.items.map((x) => x.kind), ['folder', 'text']);
eq('selection', [vm.selection.count, vm.selection.keys], [1, ['/h/a.txt']]);
eq('details of the selected item', vm.details.name, 'a.txt');
eq('places keep their own fields only', Object.keys(vm.places[0]), ['id', 'label', 'kind', 'selected']);
eq('history', vm.history, { back: true, forward: false, up: false });
ok('the actions are Files\'', vm.actions.open === fn);

// ---- the address bar ----
eq('address ~', F.addressPath('~', '/home/u', '/x'), '/home/u');
eq('address ~/Docs', F.addressPath('~/Docs/', '/home/u', '/x'), '/home/u/Docs');
eq('address relative', F.addressPath('b/c', '/home/u', '/home/u/a'), '/home/u/a/b/c');
eq('address ..', F.addressPath('/home/u/../../etc', '/home/u', null), '/etc');
eq('address above /', F.addressPath('/../..', '/home/u', null), '/');
eq('address empty', F.addressPath('  ', '/home/u', '/'), null);
eq('address relative without a folder', F.addressPath('x', '/home/u', null), null);
eq('address with a newline', F.addressPath('/a\nb', '/home/u', null), null);

console.log(`appskin-test: ${n} checks, ${bad} failures`);
process.exit(bad ? 1 : 0);
