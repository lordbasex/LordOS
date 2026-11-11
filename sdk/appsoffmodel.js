// appsoffmodel.js — deactivated LordOS apps (Store → Installed, "Deactivate"; docs/PACKAGES.md,
// "Deactivating LordOS apps"): which built-in apps a person may turn off, and the pure rules the shell
// applies to them. No `lord`, no React: node tests import it (server/tests/apps_off_model_test.mjs).
//
// A deactivated app stays installed (it is part of the lordos package and verified with it), but it is
// left out of the launcher, the Dock, search, "Open With" and the default-app choices, it can't be
// started, and its background work stops. The ids live in settings.json → appsOff (sdk/appsoff.js).

// The built-in apps a person may deactivate.
export const DEACTIVATABLE = Object.freeze([
  'remote-desk', 'machines', 'databases', 'imager', 'scanner', 'recorder', 'camera', 'characters', 'calculator', 'clock',
  'calendar', 'contacts', 'mail', 'notes', 'reminders', 'photos', 'player', 'pdf', 'fonts', 'cleaner', 'backup',
]);
// The core: LordOS needs them to manage itself (files, settings, the Store that turns apps back on, the
// terminal, the system tools, the keyring) or they are part of another app's window. Never deactivated.
export const CORE_APPS = Object.freeze([
  'files', 'settings', 'store', 'terminal', 'monitor', 'logs', 'disks', 'sysinfo', 'forcequit', 'screenshots', 'help',
  'passwords', 'editor', 'about', 'appabout', 'installer', 'sysprobe',
]);

export const canDeactivate = (id) => DEACTIVATABLE.includes(id);
export const isCoreApp = (id) => CORE_APPS.includes(id);

// settings.appsOff as read from the file: only deactivatable ids, once each, in a stable order (a hand-edited
// file can never turn off a core app).
export function normalizeOff(raw) {
  if (!Array.isArray(raw)) return [];
  return DEACTIVATABLE.filter((id) => raw.includes(id));
}
export const isOff = (list, id) => canDeactivate(id) && Array.isArray(list) && list.includes(id);
// The list with id turned off (off = true) or back on.
export function withOff(list, id, off) {
  const cur = normalizeOff(list);
  if (!canDeactivate(id)) return cur;
  return off ? normalizeOff([...cur, id]) : cur.filter((x) => x !== id);
}

// The launcher, the Dock and search: a deactivated built-in app is left out. Only LordOS's own apps
// (kind 'lordos'); a package, a Linux app or a web app with the same id is never touched.
export function launchable(apps, list) {
  const off = normalizeOff(list);
  if (!off.length) return apps;
  return apps.filter((a) => !(a && a.kind === 'lordos' && off.includes(a.id)));
}

// "lordos-<app>.desktop" (sdk/defaults.js NATIVE) → the app's id, or null.
export const nativeOfDesktop = (desktopId) => (desktopId && /^lordos-[a-z0-9_-]+\.desktop$/.test(desktopId) ? desktopId.slice(7, -8) : null);
// A default-app id that may still be used: null when it names a deactivated LordOS app.
export const usableDefault = (desktopId, list) => (desktopId && isOff(list, nativeOfDesktop(desktopId)) ? null : desktopId || null);
// The first usable one of a type's candidates, in order (the user's choice, the system's default, LordOS's
// own app, the first installed app that declares the type): another capable app when the default is
// deactivated, or null when only deactivated apps could open it.
export function pickDefault(candidates, list) {
  for (const c of candidates || []) { const u = usableDefault(c, list); if (u) return u; }
  return null;
}

// Files' "Open With": openers whose id is a LordOS app ('pdf', 'photos'…) are dropped when that app is
// deactivated. off: the ones dropped, so a file nothing else opens can offer to activate one.
export function openersAvailable(openers, list) {
  const kept = [], off = [];
  for (const o of openers || []) (isOff(list, o && o.id) ? off : kept).push(o);
  return { list: kept, off: off.map((o) => o.id) };
}

// Machines can't be deactivated while a machine runs: its window would go and the machine with it
// (lord.vm.list states, apps/machines/model.js stateView: these are "active").
const VM_ACTIVE = ['running', 'paused', 'starting', 'stopping'];
export const vmActive = (machines) => (machines || []).some((m) => m && VM_ACTIVE.includes(m.state));

// What lord-maild should leave alone (its config: server/src/mail/maild.c accounts_apply_config): the
// mail accounts with Mail off; the calendar-and-contacts (DAV) accounts when Calendar, Contacts and
// Reminders are all off.
export function mailPause(list) {
  return { mailPaused: isOff(list, 'mail'), davPaused: ['calendar', 'contacts', 'reminders'].every((id) => isOff(list, id)) };
}

// A calendar or reminder alarm rings only while its app is on (sdk/calendar.js ring).
export const alarmApp = (alarm) => (alarm && alarm.kind === 'todo' ? 'reminders' : 'calendar');
export const alarmAllowed = (alarm, list) => !isOff(list, alarmApp(alarm));
