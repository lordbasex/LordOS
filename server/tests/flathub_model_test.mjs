// flathub_model_test.mjs — the Store's Flathub section as plain data (sdk/flathubmodel.js, docs/FLATPAK.md): metadata
// from `flatpak remote-info --show-metadata` (real outputs from Flathub, 2026-11-19), the permission rows and LordOS's
// changes, the Chromium/Electron apps marked "not yet supported", the catalog filter, `flatpak list`, progress lines; the
// home's collections, categories and subcategories on a recorded piece of Flathub's real catalog and on hostile entries;
// and every permission and change key in every language.
//
//   node tests/flathub_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.join(here, '..', '..');
const M = await import(path.join(root, 'sdk/flathubmodel.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

const lines = (s) => s.split('\n');
const marktext = lines(`[Application]
name=com.github.marktext.marktext
runtime=org.freedesktop.Platform/x86_64/26.08
sdk=org.freedesktop.Sdk/x86_64/26.08
base=app/org.electronjs.Electron2.BaseApp/x86_64/26.08
command=marktext

[Context]
shared=ipc;network;
sockets=wayland;x11;
devices=dri;
filesystems=/tmp;xdg-run/keyring;host;

[Session Bus Policy]
com.canonical.AppMenu.Registrar=talk
org.freedesktop.Notifications=talk
org.freedesktop.secrets=talk

[Extension com.github.marktext.marktext.Debug]
directory=lib/debug`);
const calculator = lines(`[Application]
name=org.gnome.Calculator
runtime=org.gnome.Platform/x86_64/51
command=gnome-calculator

[Context]
shared=ipc;network;
sockets=fallback-x11;wayland;
devices=dri;

[X-DConf]
migrate-path=/org/gnome/calculator/`);
const obs = lines(`[Application]
name=com.obsproject.Studio
runtime=org.freedesktop.Platform/x86_64/26.08
command=obs

[Context]
shared=ipc;network;
sockets=fallback-x11;pulseaudio;wayland;
devices=all;
filesystems=xdg-run/pipewire-0;host;

[Session Bus Policy]
org.kde.StatusNotifierWatcher=talk
org.freedesktop.Flatpak=talk
org.freedesktop.Notifications=talk
org.a11y.Bus=talk

[Environment]
VST_PATH=/app/extensions/Plugins/vst`);

// ---- metadata ----
const mt = M.parseMetadata(marktext);
eq('runtime', mt.runtime, 'org.freedesktop.Platform/x86_64/26.08');
eq('context lists', mt.context, { shared: ['ipc', 'network'], sockets: ['wayland', 'x11'], devices: ['dri'], features: [], filesystems: ['/tmp', 'xdg-run/keyring', 'host'] });
eq('session bus', mt.session, { 'com.canonical.AppMenu.Registrar': 'talk', 'org.freedesktop.Notifications': 'talk', 'org.freedesktop.secrets': 'talk' });
// Chromium and Electron apps are not set apart any more (their sub-sandboxes are served, docs/FLATPAK.md E9)
eq('no sub-sandbox marking left', [M.needsSubsandbox, M.SUBSANDBOX_BASES], [undefined, undefined]);
eq('the base is still read', M.parseMetadata(['[Application]', 'base=app/org.chromium.Chromium.BaseApp/x86_64/24.08']).base, 'app/org.chromium.Chromium.BaseApp/x86_64/24.08');
// LordOS's Spawn portal is a portal like the others: never shown as "running programs outside its sandbox"
eq('portal.Flatpak alone: no spawn row', M.permissionRows(M.parseMetadata(['[Session Bus Policy]', 'org.freedesktop.portal.Flatpak=talk'])), []);
eq('org.freedesktop.Flatpak still is', M.permissionRows(M.parseMetadata(['[Session Bus Policy]', 'org.freedesktop.Flatpak=talk'])).map((r) => r.perm), ['spawn']);
eq('empty and junk', M.parseMetadata(['', 'junk', '=x', '[Context', 'shared=network']).context.shared, []);

// ---- permission rows ----
const keys = (rows) => rows.map((r) => `${r.perm}:${r.change}:${r.level}`);
eq('marktext rows', keys(M.permissionRows(mt)), ['filesHost:files:limited', 'filesOther:null:info', 'network:network:info', 'x11:x11:limited', 'secrets:never:never']);
eq('marktext other files', M.permissionRows(mt)[1].args, { list: '/tmp, xdg-run/keyring' });
eq('calculator rows', keys(M.permissionRows(M.parseMetadata(calculator))), ['network:network:info']);
eq('obs rows', keys(M.permissionRows(M.parseMetadata(obs))), ['filesHost:files:limited', 'filesOther:null:info', 'network:network:info', 'sound:sound:info', 'devicesAll:devices:info', 'spawn:never:never']);
const big = M.parseMetadata(lines(`[Context]
shared=network;
sockets=session-bus;system-bus;ssh-auth;gpg-agent;cups;pcsc;wayland;inherit-wayland-socket;
devices=usb;input;kvm;dri;
features=bluetooth;devel;multiarch;weird;
filesystems=home:ro;xdg-download;~/Games:create;!xdg-documents;
unknown=x;
[Session Bus Policy]
org.freedesktop.systemd1=talk
org.freedesktop.portal.Flatpak=talk
org.freedesktop.Flatpak=talk
org.example.Other=talk
org.example.Gone=none
org.freedesktop.portal.Desktop=talk
[System Bus Policy]
org.freedesktop.UPower=talk`));
eq('everything rows', keys(M.permissionRows(big)), [
  'filesHome:files:limited', 'filesOther:null:info', 'network:network:info', 'usb:devices:info', 'input:null:info', 'kvm:null:info',
  'bluetooth:devices:info', 'devel:null:info', 'printing:null:info', 'ssh:null:info', 'gpg:null:info', 'smartcard:null:info',
  'sessionBus:never:never', 'systemBus:never:never', 'systemd:never:never', 'spawn:never:never', 'talks:talks:limited', 'system:null:info', 'other:null:info']);
const rowOf = (rows, p) => rows.find((r) => r.perm === p);
eq('other places, :create dropped, negations skipped', rowOf(M.permissionRows(big), 'filesOther').args, { list: 'xdg-download, ~/Games' });
eq('talks: the other apps only', rowOf(M.permissionRows(big), 'talks').args, { list: 'org.example.Other', count: 1 });
eq('unknown values', rowOf(M.permissionRows(big), 'other').args, { list: 'weird' });
eq('no permissions', M.permissionRows(M.parseMetadata(['[Context]', 'sockets=wayland;fallback-x11;', 'devices=dri;'])), []);
eq('home read-only is still the home', keys(M.permissionRows(M.parseMetadata(['[Context]', 'filesystems=home:ro;']))), ['filesHome:files:limited']);
eq('host-etc is the computer', keys(M.permissionRows(M.parseMetadata(['[Context]', 'filesystems=host-etc;']))), ['filesHost:files:limited']);
eq('rows of null', M.permissionRows(null), []);

// ---- display and toolkit (real metadata above; Flatpak 1.16's fallback-x11 rule) ----
eq('display: marktext wayland+x11', M.displayOf(mt), 'both');
eq('display: calculator fallback', M.displayOf(M.parseMetadata(calculator)), 'waylandFallback');
eq('display: obs fallback', M.displayOf(M.parseMetadata(obs)), 'waylandFallback');
const sock = (v) => M.displayOf(M.parseMetadata(['[Context]', `sockets=${v}`]));
eq('display: by sockets', ['wayland;', 'x11;', 'fallback-x11;', 'x11;fallback-x11;wayland;', 'pulseaudio;', '!x11;wayland;', 'wayland;!x11;x11;'].map(sock),
  ['wayland', 'x11', 'x11', 'waylandFallback', 'none', 'wayland', 'both']);
eq('display: no metadata', M.displayOf(null), null);
eq('toolkit: electron', M.toolkitOf(mt), { kind: 'electron', runtime: 'org.freedesktop.Platform', version: '26.08' });
eq('toolkit: gtk', M.toolkitOf(M.parseMetadata(calculator)), { kind: 'gtk', runtime: 'org.gnome.Platform', version: '51' });
eq('toolkit: its own', M.toolkitOf(M.parseMetadata(obs)), { kind: 'other', runtime: 'org.freedesktop.Platform', version: '26.08' });
const tk = (...l) => M.toolkitOf(M.parseMetadata(['[Application]', ...l])).kind;
eq('toolkit: by runtime and base', [
  tk('runtime=org.kde.Platform/x86_64/6.9'), tk('runtime=org.kde.Platform/x86_64/6.9', 'base=app/io.qt.qtwebengine.BaseApp/x86_64/6.9'),
  tk('runtime=org.freedesktop.Platform/x86_64/24.08', 'base=app/org.chromium.Chromium.BaseApp/x86_64/24.08'),
  tk('runtime=io.elementary.Platform/x86_64/8'), tk('runtime=org.freedesktop.Platform/x86_64/24.08', 'base=app/io.qt.PySide.BaseApp/x86_64/6.8'), tk(),
  tk('runtime=org.freedesktop.Platform/x86_64/19.08', 'base=app/io.atom.electron.BaseApp/x86_64/stable')],
  ['qt', 'qt', 'chromium', 'gtk', 'qt', 'other', 'electron']);
eq('toolkit: no metadata', M.toolkitOf(null), null);

// ---- ids, refs ----
eq('ids', ['org.gnome.Calculator', 'com.example.App-x', 'io.github.a_b.c', 'org.x', 'org.-a.b', 'com.ex-ample.App', '1org.a.b', `org.a.${'b'.repeat(60)}`].map(M.idOk),
  [true, true, true, false, false, false, false, false]);
eq('ref', M.refOf('org.gnome.Calculator', 'stable'), 'app/org.gnome.Calculator/x86_64/stable');
eq('ref refused', [M.refOf('org.x', 'stable'), M.refOf('org.a.b', '../x'), M.refOf('org.a.b', '..')], [null, null, null]);

// ---- catalog ----
const cat = [
  { id: 'org.gnome.Calculator', name: 'Calculadora', summary: 'Cálculos', developer: 'GNOME', categories: 'Utility;Calculator;', verified: true },
  { id: 'org.example.Zeta', name: 'Zeta', summary: 'calc helper', developer: 'Z', categories: 'Utility;', verified: false },
  { id: 'org.example.Alpha', name: 'alpha', summary: 'A game', developer: 'A', categories: 'Game;', verified: true },
  { id: 'org.example.Calcium', name: 'Calcium', summary: 'Chemistry', developer: 'C', categories: 'Science;Education;', verified: true },
];
const ids = (l) => l.map((a) => a.id.split('.').pop());
eq('verified only, by name', ids(M.filterCatalog(cat)), ['Alpha', 'Calcium', 'Calculator']);
eq('show all', ids(M.filterCatalog(cat, { showAll: true })), ['Alpha', 'Calcium', 'Calculator', 'Zeta']);
eq('category', ids(M.filterCatalog(cat, { category: 'Utility', showAll: true })), ['Calculator', 'Zeta']);
eq('category is a whole name', ids(M.filterCatalog(cat, { category: 'Calc' })), []);
eq('search: accents folded, name starts first', ids(M.filterCatalog(cat, { query: 'calc', showAll: true })), ['Calcium', 'Calculator', 'Zeta']);
eq('search: every word', ids(M.filterCatalog(cat, { query: 'calc  gnome' })), ['Calculator']);
eq('search by id', ids(M.filterCatalog(cat, { query: 'org.example.alpha' })), ['Alpha']);
eq('counts verified', M.categoryCounts(cat, false), { Utility: 1, Calculator: 1, Game: 1, Science: 1, Education: 1 });
eq('counts all', M.categoryCounts(cat, true).Utility, 2);
eq('catalog langs', ['es-AR', 'zh-Hant', 'zh-Hans', 'pt', 'x y', ''].map(M.catalogLang), ['es-AR', 'zh_TW', 'zh_CN', 'pt', 'en', 'en']);

// ---- flatpak list, progress, staleness, prefs ----
eq('list', M.parseList(['Application ID\tBranch\tVersion\tOrigin', 'org.gnome.Calculator\tstable\t48.1\tflathub', 'cc.nift.nsm\tstable\t\tflathub', 'bad line', 'org.x\tstable']),
  [{ id: 'org.gnome.Calculator', branch: 'stable', version: '48.1', origin: 'flathub' }, { id: 'cc.nift.nsm', branch: 'stable', version: '', origin: 'flathub' }]);
eq('progress', ['Installing 1/3… ████ 45%  1.2 MB/s', '100%', 'Installation complete.', 'x 450% y', 'build 5%x', 'Installing 2/2…', 'Updating 3/4: app', 'x 5/0', 'app/org.a.B/x86_64/stable'].map(M.progressOf), [15, 100, null, null, null, 50, 50, null, null]);
const day = 24 * 3600e3, now = 1_800_000_000_000;
eq('stale', [M.appstreamStale(0, now), M.appstreamStale(now - day - 1, now), M.appstreamStale(now - 1000, now), M.appstreamStale(now + day, now)], [true, true, false, true]);
eq('prefs', [M.prefsOf(null), M.prefsOf({ on: 1, showAll: 0, appstreamAt: '5' })],
  [{ on: false, showAll: false, appstreamAt: 0, rankings: true }, { on: true, showAll: false, appstreamAt: 5, rankings: true }]);
eq('prefs: rankings off only when set off', [M.prefsOf({ rankings: false }).rankings, M.prefsOf({ rankings: 0 }).rankings, M.prefsOf({ rankings: true }).rankings], [false, true, true]);

// a note on an app's page: OBS Studio's recordings go to Videos (the home is read-only in a Flatpak app's view)
eq('note: OBS', M.appNote('com.obsproject.Studio'), 'obsVideos');
eq('note: none', [M.appNote('org.gnome.Calculator'), M.appNote('toString'), M.appNote(null)], [null, null, null]);

// ---- every row's text in every language ----
const perms = new Set(), changes = new Set();
for (const md of [mt, M.parseMetadata(obs), big]) for (const r of M.permissionRows(md)) { perms.add(r.perm); if (r.change) changes.add(r.change); }
const dir = path.join(root, 'assets/locales');
for (const lang of fs.readdirSync(dir).filter((d) => fs.statSync(path.join(dir, d)).isDirectory())) {
  const f = path.join(dir, lang, 'flathub.json');
  const cat2 = fs.existsSync(f) ? JSON.parse(fs.readFileSync(f, 'utf8')) : {};
  const look = ['look.title', 'look.display', 'look.toolkit', 'look.runtime',
    ...['wayland', 'waylandFallback', 'both', 'x11', 'none'].flatMap((d) => [`display.${d}`, `display.${d}Hint`]),
    ...['gtk', 'qt', 'electron', 'chromium', 'other'].map((k) => `toolkit.${k}`)];
  const missing = [...[...perms].map((p) => `perm.${p}`), ...[...changes].map((c) => `change.${c}`), 'note.obsVideos', ...look].filter((k) => typeof cat2[k] !== 'string' && typeof cat2[k] !== 'object');
  eq(`rows translated in ${lang}`, missing, []);
}

// ---- the license row ----
{
  const L = M.licenseView;
  eq('license: proprietary with its address', L('LicenseRef-proprietary=https://gitlab.com/warlordsoftwares/4ktube/-/raw/main/LICENSE'),
    { kind: 'proprietary', text: '', url: 'https://gitlab.com/warlordsoftwares/4ktube/-/raw/main/LICENSE' });
  eq('license: proprietary alone', L('LicenseRef-proprietary'), { kind: 'proprietary', text: '', url: null });
  eq('license: proprietary with a plain-http, spaced or odd address: no link',
    [L('LicenseRef-proprietary=http://x.org/l').url, L('LicenseRef-proprietary=https://x.org/a b').url, L('LicenseRef-proprietary=javascript:alert(1)').url,
      L('LicenseRef-proprietary=https://x.org/' + 'a'.repeat(600)).url, L('LicenseRef-proprietary=https:///x').url], [null, null, null, null, null]);
  eq('license: one SPDX id links to its page', L('GPL-3.0-or-later'), { kind: 'spdx', text: 'GPL-3.0-or-later', url: 'https://spdx.org/licenses/GPL-3.0-or-later.html' });
  eq('license: an expression shows as given, no one page', L('MIT AND Apache-2.0'), { kind: 'spdx', text: 'MIT AND Apache-2.0', url: null });
  eq('license: another LicenseRef has no page', L('LicenseRef-Freeware').url, null);
  eq('license: nothing', [L(''), L(null)], [{ kind: null, text: '', url: null }, { kind: null, text: '', url: null }]);
}

// an app's description: paragraphs, shut to about six lines, open whole
{
  const V = M.descriptionView;
  eq('no description', V(''), { paras: [], long: false });
  eq('no description (null)', V(null), { paras: [], long: false });
  eq('short: whole, nothing to open', V('One.\n\nTwo.'), { paras: [{ text: 'One.', clamp: 0 }, { text: 'Two.', clamp: 0 }], long: false });
  eq('blank paragraphs dropped', V('\n\nOne.\n\n\n\n  \n\nTwo.\n'), { paras: [{ text: 'One.', clamp: 0 }, { text: 'Two.', clamp: 0 }], long: false });
  const list = 'Features:\n\n• a\n• b\n• c\n• d\n• e\n\nThe end.';
  eq('a list counts a line per item; shut cuts the paragraph that overflows', V(list),
    { paras: [{ text: 'Features:', clamp: 0 }, { text: '• a\n• b\n• c\n• d\n• e', clamp: 0 }], long: true });
  eq('open: every paragraph', V(list, { open: true }).paras.length, 3);
  const longP = 'x'.repeat(90 * 8);
  eq('one long paragraph clamped to six lines', V(longP), { paras: [{ text: longP, clamp: 6 }], long: true });
  eq('cut to the lines left', V(`a\n\n${longP}`).paras.map((p) => p.clamp), [0, 5]);
  eq('characters, not UTF-16 units', V('😀'.repeat(90 * 6)).long, false);
  eq('custom line budget', V('a\n\nb\n\nc', { lines: 2 }), { paras: [{ text: 'a', clamp: 0 }, { text: 'b', clamp: 0 }], long: true });
}

// ---- the home: collections, categories, subcategories ----
// tests/data/flathub/catalog-2026-11-22.json: 38 apps of Flathub's real catalog as lord.flatpak.catalog() gives them
// (read from Flathub's appstream.xml of 2026-11-22; public data only)
{
  const real = JSON.parse(fs.readFileSync(path.join(here, 'data/flathub/catalog-2026-11-22.json'), 'utf8'));
  const at = Date.UTC(2026, 10, 22) + 12 * 3600e3;            // noon 2026-11-22 (the newest release, OBS's, is that day)
  eq('fixture: real shape', real.every((a) => M.idOk(a.id) && M.branchOk(a.branch) && Number.isInteger(a.updated) && typeof a.verified === 'boolean'), true);
  const upd = M.recentlyUpdated(real, { now: at });
  eq('recently updated: verified only, newest first', upd.slice(0, 4).map((a) => a.id),
    ['com.obsproject.Studio', 'com.discordapp.Discord', 'org.mozilla.firefox', 'org.gnome.Evince']);
  eq('recently updated: no unverified while showAll is off', upd.some((a) => !a.verified), false);
  eq('recently updated: showAll lets the unverified in', M.recentlyUpdated(real, { now: at, showAll: true })[1].id, 'org.signal.Signal');
  eq('recently updated: every app has a date (the real catalog)', upd.length, real.filter((a) => a.verified).length);
  eq('recently updated in a category', M.recentlyUpdated(real, { now: at, category: 'Game' }).map((a) => a.id),
    ['net.pcsx2.PCSX2', 'org.libretro.RetroArch', 'net.supertuxkart.SuperTuxKart']);
  eq('spotlight: newest with an icon and summary', M.spotlight(real, { now: at }).id, 'com.obsproject.Studio');
  eq('verified collection only with showAll', [M.collection(real, 'verified', { now: at }).length, M.collection(real, 'verified', { now: at, showAll: true }).length],
    [0, real.filter((a) => a.verified).length]);
  eq('home rows', M.homeRows(real, { now: at }).map((r) => r.key), ['updated']);
  eq('home rows with showAll', M.homeRows(real, { now: at, showAll: true }).map((r) => r.key), ['updated', 'verified']);
  eq('unknown collection', [M.collection(real, 'nope', { now: at }), M.collectionOk('nope'), M.collectionOk('__proto__'), M.collectionOk('updated'), M.collectionOk('popular')],
    [[], false, false, true, true]);
  eq('rankings absent: their rows are too', [M.collection(real, 'popular', { now: at }), M.collection(real, 'week', { now: at, ranks: {} })], [[], []]);
  eq('rankings absent: the banner is the latest release', M.banner(real, { now: at }), { kind: 'latest', app: M.spotlight(real, { now: at }) });

  // ---- the rankings (E10) on Flathub's recorded answers of the same day (tests/data/flathub/api-2026-11-22) ----
  const api = (f) => JSON.parse(fs.readFileSync(path.join(here, 'data/flathub/api-2026-11-22', `${f}.json`), 'utf8'));
  const raw = {
    popular: api('popular').hits.map((x) => x.app_id), trending: api('trending').hits.map((x) => x.app_id),
    added: api('recently-added').hits.map((x) => x.app_id), updated: api('recently-updated').hits.map((x) => x.app_id),
    day: [api('app-of-the-day').app_id], week: api('apps-of-the-week').apps.sort((a, b) => a.position - b.position).map((x) => x.app_id), busy: false,
  };
  const ranks = M.ranksOf(raw);
  eq('ranks: the lists as they came', [ranks.popular.length, ranks.trending.length, ranks.day, ranks.week.length, 'busy' in ranks], [250, 40, ['nl.andreasknoben.Laser'], 5, false]);
  const pop = M.collection(real, 'popular', { now: at, ranks });
  eq('popular: Flathub\'s order, only apps in the catalog, verified only', pop.slice(0, 4).map((a) => a.id),
    ['org.mozilla.firefox', 'com.discordapp.Discord', 'com.github.tchx84.Flatseal', 'org.telegram.desktop']);
  eq('popular: no unverified app while the switch is off', pop.every((a) => a.verified), true);
  eq('popular with every app', M.collection(real, 'popular', { now: at, ranks, showAll: true }).slice(0, 4).map((a) => a.id),
    ['org.mozilla.firefox', 'com.discordapp.Discord', 'org.videolan.VLC', 'com.valvesoftware.Steam']);
  eq('popular: names and icons are the catalog\'s', pop[0], real.find((a) => a.id === 'org.mozilla.firefox'));
  eq('trending, added: none of them in this piece of the catalog', [M.collection(real, 'trending', { now: at, ranks }), M.collection(real, 'added', { now: at, ranks })], [[], []]);
  eq('updated: Flathub\'s has 1 app here, under RANK_UPDATED_MIN: the catalog\'s', M.collection(real, 'updated', { now: at, ranks }).map((a) => a.id),
    M.recentlyUpdated(real, { now: at }).map((a) => a.id));
  const many = { updated: ['org.gnome.Evince', 'org.mozilla.firefox', 'com.discordapp.Discord', 'com.obsproject.Studio', 'org.x.NotInCatalog'] };
  eq('updated: Flathub\'s when it has enough', M.collection(real, 'updated', { now: at, ranks: many }).map((a) => a.id),
    ['org.gnome.Evince', 'org.mozilla.firefox', 'com.discordapp.Discord', 'com.obsproject.Studio']);
  eq('app of the day not in the catalog: the latest release', M.banner(real, { now: at, ranks }).kind, 'latest');
  eq('app of the day in the catalog', M.banner(real, { now: at, ranks: { day: ['org.gnome.Evince'] } }), { kind: 'day', app: real.find((a) => a.id === 'org.gnome.Evince') });
  const unv = real.find((a) => !a.verified);
  eq('an unverified app of the day only with the switch', [M.banner(real, { now: at, ranks: { day: [unv.id] } }).kind, M.banner(real, { now: at, showAll: true, ranks: { day: [unv.id] } }).kind], ['latest', 'day']);
  eq('home rows with the rankings', M.homeRows(real, { now: at, ranks: { ...ranks, week: ['com.obsproject.Studio', 'org.gnome.Evince'] } }).map((r) => r.key), ['week', 'popular', 'updated']);
  eq('home rows with the rankings and every app', M.homeRows(real, { now: at, showAll: true, ranks }).map((r) => r.key), ['popular', 'updated', 'verified']);
  eq('subcategories of Game (verified)', M.subcategoryCounts(real, 'Game', false), { ArcadeGame: 1, Emulator: 2 });
  eq('subcategories of Game (all)', M.subcategoryCounts(real, 'Game', true), { ArcadeGame: 1, Emulator: 3 });
  eq('subcategories in SUBCATS order', Object.keys(M.subcategoryCounts(real, 'Graphics', true)), ['2DGraphics', 'Viewer', 'RasterGraphics', '3DGraphics', 'VectorGraphics']);
  eq('a subcategory needs its main category', M.subcategoryCounts(real, 'Utility', true).Viewer, undefined);
  eq('shown apps = the catalog list, not the categories\' sum', [M.shownCount(real, false), M.filterCatalog(real).length,
    Object.values(M.categoryCounts(real, false)).reduce((a, n) => a + n, 0) > M.shownCount(real, false)], [M.filterCatalog(real).length, M.filterCatalog(real).length, true]);
  eq('filter by subcategory', ids(M.filterCatalog(real, { category: 'Game', sub: 'Emulator', showAll: true })), ['dolphin-emu', 'PCSX2', 'RetroArch']);
  eq('subcategory checks', [M.subOk('Game', 'Emulator'), M.subOk('Game', 'Viewer'), M.subOk('toString', 'x'), M.subOk('__proto__', 'Audio'), M.subcatsOf('constructor')],
    [true, false, false, false, []]);
}
// hostile or broken catalog entries: never thrown on, never first
{
  const now = Date.UTC(2026, 10, 22);
  const s = Math.floor(now / 1000);
  const hostile = [
    { id: 'org.x.Future', name: 'Future', summary: 's', icon: 'i.png', verified: true, updated: s + 10 * 86400 },
    { id: 'org.x.Float', name: 'Float', verified: true, updated: s - 1.5 },
    { id: 'org.x.Str', name: 'Str', verified: true, updated: String(s) },
    { id: 'org.x.Neg', name: 'Neg', verified: true, updated: -5 },
    { id: 'org.x.Old', name: 'Old', verified: true, updated: 12345 },
    { id: 'org.x.NaN', name: 'NaN', verified: true, updated: NaN },
    { id: 'org.x.Inf', name: 'Inf', verified: true, updated: Infinity },
    { id: 'org.x.None', name: 'None', verified: true },
    { id: 'org.x.Ok', name: 'Ok', summary: '', icon: '', verified: true, updated: s - 100 },
    { id: 'org.x.Okay', name: 'Okay', summary: 'yes', icon: 'o.png', verified: true, updated: s - 200, categories: 'Game;Emulator;Emulator;' },
    { id: 'org.x.Unv', name: 'Unv', summary: 'x', icon: 'u.png', verified: false, updated: s },
    null, undefined, 5, 'org.x.String',
  ].filter((a) => a && typeof a === 'object');
  eq('hostile dates left out', M.recentlyUpdated(hostile, { now }).map((a) => a.name), ['Ok', 'Okay']);
  eq('a release up to a day ahead counts', M.updatedAt({ updated: s + 3600 }, now) > 0, true);
  eq('spotlight skips an app without icon or summary', M.spotlight(hostile, { now }).name, 'Okay');
  eq('spotlight: none', M.spotlight([], { now }), null);
  eq('a category listed twice counts once', M.subcategoryCounts(hostile, 'Game', false), { Emulator: 1 });
  eq('a main category listed twice counts once, as its page shows the app once',
    [M.categoryCounts([...hostile, { id: 'org.x.Twice', name: 'Twice', verified: true, categories: 'Graphics;Viewer;Graphics;' }], false).Graphics,
      M.filterCatalog([{ id: 'org.x.Twice', name: 'Twice', verified: true, categories: 'Graphics;Viewer;Graphics;' }], { category: 'Graphics' }).length], [1, 1]);
  eq('shown apps: each app once, whatever its categories', [M.shownCount(hostile, false), M.shownCount(hostile, true), M.shownCount(null, true), M.shownCount([null, 5], true)],
    [hostile.length - 1, hostile.length, 0, 0]);
  eq('verified apps: dated first, then by name', M.verifiedApps(hostile, { now }).map((a) => a.name).slice(0, 4), ['Ok', 'Okay', 'Float', 'Future']);
  eq('nothing at all', [M.recentlyUpdated(null), M.homeRows(undefined), M.subcategoryCounts(null, 'Game', true), M.collection(null, 'updated')], [[], [], {}, []]);
}
// hostile rankings: whatever lord.flatpak.ranks() gave, only known lists of good ids, no duplicates, at most 250
{
  eq('ranks: not an object', [M.ranksOf(null), M.ranksOf('popular'), M.ranksOf(5), M.ranksOf([['org.a.B']])], [{}, {}, {}, {}]);
  const r = M.ranksOf({ popular: ['org.a.B', 'org.a.B', '../../etc', 'org.ex ample.App', 7, null, { id: 'org.c.D' }, 'org.c.D', 'a.b', 'org.e-x.App', 'org.x.Y-z'],
    trending: 'org.a.B', added: { 0: 'org.a.B', length: 1 }, updated: [], day: ['bad id'], week: ['org.w.One', 'org.w.Two'],
    verified: ['org.a.B'], __proto__: { popular: ['org.evil.Proto'] }, busy: true });
  eq('ranks: bad ids, duplicates and other types dropped', r, { popular: ['org.a.B', 'org.c.D', 'org.x.Y-z'], week: ['org.w.One', 'org.w.Two'] });
  eq('ranks: own keys only', M.ranksOf(Object.create({ popular: ['org.evil.Proto'] })), {});
  const long = Array.from({ length: 400 }, (_, i) => `org.app.N${i}`);
  eq('ranks: at most 250', M.ranksOf({ popular: long }).popular.length, 250);
  const cat = [{ id: 'org.a.B', name: 'A', verified: true }, { id: 'org.c.D', name: 'C', verified: false }, null, 5, { id: 'bad id', verified: true },
    { id: 'org.a.B', name: 'A twice', verified: true }];
  eq('ranked apps: catalog entries only, first of a duplicate id', M.rankedApps(cat, ['org.c.D', 'org.a.B', 'org.z.Z', 'bad id', 'org.a.B'], { showAll: true }).map((a) => a.name), ['C', 'A']);
  eq('ranked apps: verified only', M.rankedApps(cat, ['org.c.D', 'org.a.B']).map((a) => a.name), ['A']);
  eq('ranked apps: no list', [M.rankedApps(cat, null), M.rankedApps(cat, 'org.a.B'), M.rankedApps(null, ['org.a.B'])], [[], [], []]);
  eq('app of the day: none', [M.appOfTheDay(cat, null), M.appOfTheDay(cat, {}), M.appOfTheDay(cat, { day: ['org.c.D'] })], [null, null, null]);
}
// every collection, subcategory and home text in every language
{
  const keys = ['verifiedShort', 'home.spotlight', 'home.categories', 'home.appOfTheDay', 'rankingsOn', 'rankingsOff', ...M.COLLECTIONS.map((k) => `coll.${k}`),
    ...[...new Set(Object.values(M.SUBCATS).flat())].map((s) => `sub.${s}`)];
  const dir = path.join(root, 'assets/locales');
  for (const lang of fs.readdirSync(dir).filter((d) => fs.statSync(path.join(dir, d)).isDirectory())) {
    const cat2 = JSON.parse(fs.readFileSync(path.join(dir, lang, 'flathub.json'), 'utf8'));
    eq(`home texts in ${lang}`, keys.filter((k) => typeof cat2[k] !== 'string' || !cat2[k].trim()), []);
  }
  eq('every main category has subcategories', M.MAIN_CATS.every((c) => M.subcatsOf(c).length > 0), true);
}

console.log(bad ? `flathub-model: ${bad} of ${n} failed` : `flathub-model: ${n}/${n} ok`);
process.exit(bad ? 1 : 0);
