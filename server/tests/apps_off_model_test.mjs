// apps_off_model_test.mjs — deactivated LordOS apps (sdk/appsoffmodel.js; Store → Installed, docs/PACKAGES.md):
// the core and deactivatable lists against apps/, the appsOff setting, the launcher/Dock/search filter, the
// default-app fallback, Files' "Open With", Machines' running VMs, lord-maild's pauses and the PIM alarms.
//
//   node tests/apps_off_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const M = await import(path.join(here, '../../sdk/appsoffmodel.js'));
const {
  DEACTIVATABLE, CORE_APPS, canDeactivate, isCoreApp, normalizeOff, isOff, withOff, launchable, nativeOfDesktop,
  usableDefault, pickDefault, openersAvailable, vmActive, mailPause, alarmApp, alarmAllowed,
} = M;

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

// ---- the lists ----
const appsDir = path.join(here, '../../apps');
const builtIn = fs.readdirSync(appsDir).filter((d) => fs.existsSync(path.join(appsDir, d, 'manifest.json')))
  .map((d) => JSON.parse(fs.readFileSync(path.join(appsDir, d, 'manifest.json'), 'utf8')).id).sort();
eq('every built-in app is either core or deactivatable', builtIn, [...DEACTIVATABLE, ...CORE_APPS].sort());
eq('no app is in both lists', DEACTIVATABLE.filter((id) => CORE_APPS.includes(id)), []);
eq('21 deactivatable', DEACTIVATABLE.length, 21);
eq('the lists are frozen', [Object.isFrozen(DEACTIVATABLE), Object.isFrozen(CORE_APPS)], [true, true]);
for (const id of ['files', 'settings', 'store', 'terminal', 'monitor', 'logs', 'disks', 'sysinfo', 'forcequit', 'screenshots', 'help',
  'passwords', 'editor', 'about', 'appabout', 'installer', 'sysprobe']) {
  eq(`${id} is core`, [isCoreApp(id), canDeactivate(id)], [true, false]);
}
for (const id of ['remote-desk', 'machines', 'databases', 'imager', 'scanner', 'recorder', 'camera', 'characters', 'calculator', 'clock',
  'calendar', 'contacts', 'mail', 'notes', 'reminders', 'photos', 'player', 'pdf', 'fonts', 'cleaner', 'backup']) {
  eq(`${id} can be deactivated`, [canDeactivate(id), isCoreApp(id)], [true, false]);
}
eq('an unknown id is neither', [canDeactivate('firefox'), isCoreApp('firefox'), canDeactivate(''), canDeactivate(undefined)], [false, false, false, false]);

// ---- the setting (settings.json → appsOff) ----
eq('missing → none', normalizeOff(undefined), []);
eq('not a list → none', normalizeOff('mail'), []);
eq('a hand-edited file never turns off a core app', normalizeOff(['files', 'store', 'mail', 'settings']), ['mail']);
eq('unknown ids and duplicates dropped, stable order', normalizeOff(['photos', 'mail', 'photos', 'nope', 42, null]), ['mail', 'photos']);
eq('withOff adds once', withOff(['mail'], 'mail', true), ['mail']);
eq('withOff adds in order', withOff(['photos'], 'calendar', true), ['calendar', 'photos']);
eq('withOff removes', withOff(['calendar', 'photos'], 'calendar', false), ['photos']);
eq('withOff refuses a core app', withOff([], 'files', true), []);
eq('withOff from nothing', withOff(null, 'pdf', true), ['pdf']);
eq('isOff', [isOff(['mail'], 'mail'), isOff(['mail'], 'photos'), isOff(['files'], 'files'), isOff(null, 'mail')], [true, false, false, false]);

// ---- launcher, Dock and search (useLauncherApps → launchable) ----
const apps = [
  { id: 'files', kind: 'lordos' }, { id: 'mail', kind: 'lordos' }, { id: 'photos', kind: 'lordos' },
  { id: 'desktop:mail', kind: 'linux' }, { id: 'mail', kind: 'package' }, { id: 'web:mail', kind: 'web' },
];
eq('nothing off: the same list', launchable(apps, []) === apps, true);
eq('a deactivated built-in app is left out', launchable(apps, ['mail']).map((a) => `${a.kind}:${a.id}`),
  ['lordos:files', 'lordos:photos', 'linux:desktop:mail', 'package:mail', 'web:web:mail']);
eq('only LordOS apps are filtered; a core id in the list changes nothing', launchable(apps, ['files']).length, apps.length);
eq('search sees the same list (several off)', launchable(apps, ['mail', 'photos']).filter((a) => a.kind === 'lordos').map((a) => a.id), ['files']);

// ---- default apps (Settings → Default Apps, Files' double click) ----
eq('nativeOfDesktop', [nativeOfDesktop('lordos-pdf.desktop'), nativeOfDesktop('org.gnome.Evince.desktop'), nativeOfDesktop(null), nativeOfDesktop('lordos-../x.desktop')],
  ['pdf', null, null, null]);
eq('a default that is on is used', usableDefault('lordos-pdf.desktop', []), 'lordos-pdf.desktop');
eq('a deactivated default is not', usableDefault('lordos-pdf.desktop', ['pdf']), null);
eq('a Linux default is never touched', usableDefault('org.gnome.Evince.desktop', ['pdf']), 'org.gnome.Evince.desktop');
eq('no default', usableDefault('', ['pdf']), null);
eq('falls back to another capable app', pickDefault(['lordos-photos.desktop', 'org.gnome.Loupe.desktop'], ['photos']), 'org.gnome.Loupe.desktop');
eq('the user choice first when on', pickDefault(['lordos-photos.desktop', 'org.gnome.Loupe.desktop'], []), 'lordos-photos.desktop');
eq('only deactivated apps: none (Files offers to activate)', pickDefault(['lordos-pdf.desktop', null, ''], ['pdf']), null);
eq('a core default stays even if listed', pickDefault(['lordos-editor.desktop'], ['editor']), 'lordos-editor.desktop');

// ---- Files: "Open With" ----
const openers = [{ id: 'photos' }, { id: 'editor' }, { id: 'desktop:org.gnome.Loupe' }];
eq('openers with nothing off', openersAvailable(openers, []).list.map((o) => o.id), ['photos', 'editor', 'desktop:org.gnome.Loupe']);
eq('a deactivated app leaves Open With', openersAvailable(openers, ['photos']), { list: [{ id: 'editor' }, { id: 'desktop:org.gnome.Loupe' }], off: ['photos'] });
eq('only it opened the file: nothing left, and who to activate', openersAvailable([{ id: 'pdf' }], ['pdf']), { list: [], off: ['pdf'] });
eq('no openers', openersAvailable(undefined, ['pdf']), { list: [], off: [] });

// ---- Machines: never with a machine running ----
eq('no machines', vmActive([]), false);
eq('stopped and crashed ones', vmActive([{ state: 'stopped' }, { state: 'crashed' }, {}]), false);
for (const s of ['running', 'paused', 'starting', 'stopping']) eq(`a ${s} machine blocks it`, vmActive([{ state: 'stopped' }, { state: s }]), true);
eq('nothing listed', vmActive(null), false);

// ---- lord-maild: what it leaves alone ----
eq('all on', mailPause([]), { mailPaused: false, davPaused: false });
eq('Mail off: its accounts', mailPause(['mail']), { mailPaused: true, davPaused: false });
eq('Calendar alone off: the DAV accounts keep syncing (Contacts, Reminders)', mailPause(['calendar']), { mailPaused: false, davPaused: false });
eq('Calendar, Contacts and Reminders off: the DAV accounts too', mailPause(['calendar', 'contacts', 'reminders']), { mailPaused: false, davPaused: true });
eq('all four', mailPause(['mail', 'calendar', 'contacts', 'reminders']), { mailPaused: true, davPaused: true });

// ---- Calendar and Reminders alarms ----
eq('an event is Calendar\'s, a to-do Reminders\'', [alarmApp({ kind: 'event' }), alarmApp({ kind: 'todo' }), alarmApp({})], ['calendar', 'reminders', 'calendar']);
eq('Calendar off: its alarms don\'t ring', alarmAllowed({ kind: 'event' }, ['calendar']), false);
eq('Calendar off: a to-do still rings', alarmAllowed({ kind: 'todo' }, ['calendar']), true);
eq('Reminders off: its to-dos don\'t ring', alarmAllowed({ kind: 'todo' }, ['reminders']), false);

console.log(bad ? `apps-off-model: ${bad} of ${n} failed` : `apps-off-model: ${n}/${n} ok`);
process.exit(bad ? 1 : 0);
