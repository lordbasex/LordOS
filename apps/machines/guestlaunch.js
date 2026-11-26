// guestlaunch.js — a machine's guest apps in this computer's launcher and search before the machine runs
// (docs/VIRTUALIZATION.md 6.12). Apps mode saves the guest's list in app.json (saveGuestApps, cleaned by
// sdk/guestappsmodel.js); this module lists the machines that are off as launcher entries (sdk/apps.js
// guestLauncherApps: the launcher, the Classic start menu and search all read it), each marked with the Machines mark and
// the machine's name. Choosing one asks the machine's window (launchAsks) to start it (its usual start: the memory
// budget, a bridged network's or a vGPU's preparation), turn apps mode on and, once the guest's own fresh list holds that
// exact token, launch it there (launchGuest: lord.vm.launch sends nothing but a token the guest listed). Nothing on this
// computer runs a guest's token or name.
import { createStore, guestLauncherApps, guestLauncherItems, cleanGuestApps, sameGuestApps, appsOff, vmMachines } from '@lordos/sdk';
import { readMeta, writeMeta, isProtectedMachine } from './meta.js';
import { stateView, unreadableOf, saveDelay } from './model.js';

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
  return writeMeta(dir, { guestApps: clean });
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
const keyOf = (l) => l.map((a) => `${a.id}\t${a.name}\t${a.machine}`).join('\n');
let lastKey = '';

// from the shell's list (sdk/vmlibrary.js vmMachines), never a list of its own: read again only when the library changed
// (apps mode's saved list is app.json in the machine's folder, a change too)
export function refreshGuestLauncher() {
  let items = [];
  if (has() && !appsOff().includes('machines')) {
    const list = vmMachines.get();
    items = guestLauncherItems(list, readMeta, {
      active: (m) => stateView(m.state, m.reason).active,
      locked: (m) => !!unreadableOf(m) || isProtectedMachine(m.dir, m),
    });
  }
  const k = keyOf(items);
  if (k === lastKey) return;
  lastKey = k;
  guestLauncherApps.set(items.map((it) => ({ ...it, launch: () => launchFromLauncher(it) })));
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

let unsub = null;
export function startGuestLauncher(openMachine) {
  opener = openMachine;
  if (unsub) return;
  refreshGuestLauncher();
  unsub = vmMachines.subscribe(refreshGuestLauncher);
}
