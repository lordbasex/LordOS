// guestlaunch.js — a machine's guest apps in this computer's launcher and search before the machine runs
// (docs/VIRTUALIZATION.md 6.12). Apps mode saves the guest's list in app.json (saveGuestApps, cleaned by
// sdk/guestappsmodel.js); this module lists the machines that are off as launcher entries (sdk/apps.js
// guestLauncherApps: the launcher, the Classic start menu and search all read it), each marked with the Machines mark and
// the machine's name. Choosing one asks the machine's window (launchAsks) to start it (its usual start: the memory
// budget, a bridged network's or a vGPU's preparation), turn apps mode on and, once the guest's own fresh list holds that
// exact token, launch it there (launchGuest: lord.vm.launch sends nothing but a token the guest listed). Nothing on this
// computer runs a guest's token or name.
import { createStore, guestLauncherApps, guestLauncherItems, cleanGuestApps, sameGuestApps, appsOff, settings, vmState, vmLibraryChanged, keptGuestItems, shownGuestItems } from '@lordos/sdk';
import { readMeta, writeMeta, isProtectedMachine } from './meta.js';
import { unreadableOf, saveDelay } from './model.js';

// dir → { token, name, at }: a launch the launcher asked for, taken by the machine's window (vmwindow.jsx)
export const launchAsks = createStore({});

// apps mode's fresh list → app.json, only when it changed (and never an empty one: a guest that lists nothing yet keeps
// what it listed last), at most once per SAVE_APPS_MS for a machine (model.js saveDelay): a guest that alternates two
// lists would otherwise write the disk many times a second (an SD card on the Pi). The latest list waits its turn.
const saves = new Map();     // dir → { at: the last write, apps: the list waiting, timer }
function writeApps(dir, apps) {
  const clean = cleanGuestApps(apps);
  if (!dir || !clean.length) return false;
  if (sameGuestApps(readMeta(dir).guestApps, clean)) return false;
  const ok = writeMeta(dir, { guestApps: clean });
  if (ok) refreshGuestLauncher();
  return ok;
}
export function saveGuestApps(dir, apps) {
  if (!dir) return false;
  const s = saves.get(dir) || { at: 0, apps: null, timer: null };
  saves.set(dir, s);
  const now = Date.now(), wait = saveDelay(s.at, now);
  if (!wait && !s.timer) {
    const wrote = writeApps(dir, apps);
    if (wrote) s.at = now;
    return wrote;
  }
  s.apps = apps;
  if (!s.timer) s.timer = setTimeout(() => { s.timer = null; const a = s.apps; s.apps = null; if (writeApps(dir, a)) s.at = Date.now(); }, Math.max(wait, 1));
  return false;
}

const has = () => typeof lord !== 'undefined' && lord.vm && typeof lord.vm.list === 'function';
let opener = null;           // library.jsx's openMachine, given at the start (no import cycle)
const keyOf = (l) => l.map((a) => `${a.id}\t${a.name}\t${a.machine}\t${a.dir}`).join('\n');
let lastKey = '';

// The launcher's entries without asking Machines (docs/VIRTUALIZATION.md, "The shell and the machines"): the entries of
// every machine that shows its apps are kept in settings.json (guestLauncherKept) and shown without the ones that run
// (vmState, from lord-server's events). The list is read again (lord.vm.list, then each app.json) only when it can have
// changed: apps mode saved a new list (saveGuestApps), Configure's switch (refreshGuestLauncher from there), or the
// library changed while there are entries (a machine renamed or removed); once at the first login that has nothing kept.
function show() {
  let items = [];
  if (has() && !appsOff().includes('machines')) items = shownGuestItems(keptGuestItems(settings.get().guestLauncherKept) || [], vmState.get().active);
  const k = keyOf(items);
  if (k === lastKey) return;
  lastKey = k;
  guestLauncherApps.set(items.map((it) => ({ ...it, launch: () => launchFromLauncher(it) })));
}

export function refreshGuestLauncher() {
  if (!has()) return;
  let list = [];
  try { list = lord.vm.list() || []; } catch (e) { return; }
  const all = guestLauncherItems(list, readMeta, {
    locked: (m) => !!unreadableOf(m) || isProtectedMachine(m.dir, m),
  });
  const kept = keptGuestItems(settings.get().guestLauncherKept);
  if (!kept || keyOf(kept) !== keyOf(all)) settings.set({ guestLauncherKept: all });
  show();
}

// the machine by its id (a rename keeps it), started in its window, the app asked for
export function launchFromLauncher(it) {
  if (!has() || !opener || !it?.mid || !it.token) return false;
  let m = null;
  try { m = (lord.vm.list() || []).find((x) => x.id === it.mid) || null; } catch (e) { m = null; }
  if (!m || unreadableOf(m) || isProtectedMachine(m.dir, m)) return false;
  launchAsks.set((x) => ({ ...x, [m.dir]: { token: it.token, name: it.name, at: Date.now() } }));
  opener(m);
  return true;
}

export const dropLaunchAsk = (dir) => launchAsks.set((x) => { if (!x[dir]) return x; const n = { ...x }; delete n[dir]; return n; });

let started = false;
export function startGuestLauncher(openMachine) {
  opener = openMachine;
  if (started) return;
  started = true;
  if (keptGuestItems(settings.get().guestLauncherKept) === null) refreshGuestLauncher();   // never read: once
  else show();
  vmState.subscribe(show);
  settings.subscribe(show);           // Machines deactivated or back; the kept entries written
  vmLibraryChanged.subscribe(() => { if ((keptGuestItems(settings.get().guestLauncherKept) || []).length) refreshGuestLauncher(); });
}
