// flathub_model_test.mjs — the Store's Flathub section as plain data (sdk/flathubmodel.js, docs/FLATPAK.md): metadata
// from `flatpak remote-info --show-metadata` (real outputs from Flathub, 2026-11-19), the permission rows and LordOS's
// changes, the Chromium/Electron apps marked "not yet supported", the catalog filter, `flatpak list`, progress lines;
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
eq('prefs', [M.prefsOf(null), M.prefsOf({ on: 1, showAll: 0, appstreamAt: '5' })], [{ on: false, showAll: false, appstreamAt: 0 }, { on: true, showAll: false, appstreamAt: 5 }]);

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
  const missing = [...[...perms].map((p) => `perm.${p}`), ...[...changes].map((c) => `change.${c}`), 'note.obsVideos'].filter((k) => typeof cat2[k] !== 'string' && typeof cat2[k] !== 'object');
  eq(`rows translated in ${lang}`, missing, []);
}

console.log(bad ? `flathub-model: ${bad} of ${n} failed` : `flathub-model: ${n}/${n} ok`);
process.exit(bad ? 1 : 0);
