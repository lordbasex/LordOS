// guestlaunch.js — a machine's guest apps in this computer's launcher and search before the machine runs
// (docs/VIRTUALIZATION.md 6.12). Apps mode saves the guest's list in app.json (saveGuestApps, cleaned by
// sdk/guestappsmodel.js); this module lists the machines that are off as launcher entries (sdk/apps.js
// guestLauncherApps: the launcher, the Classic start menu and search all read it), each marked with the Machines mark and
// the machine's name. Choosing one asks the machine's window (launchAsks) to start it (its usual start: the memory
// budget, a bridged network's or a vGPU's preparation), turn apps mode on and, once the guest's own fresh list holds that
// exact token, launch it there (launchGuest: lord.vm.launch sends nothing but a token the guest listed). Nothing on this
// computer runs a guest's token or name.
import { createStore, guestLauncherApps, guestLauncherItems, cleanGuestApps, sameGuestApps, appsOff } from '@lordos/sdk';
import { readMeta, writeMeta, isProtectedMachine } from './meta.js';
import { stateView, unreadableOf } from './model.js';

// dir → { token, name, at }: a launch the launcher asked for, taken by the machine's window (vmwindow.jsx)
export const launchAsks = createStore({});

// apps mode's fresh list → app.json, only when it changed (and never an empty one: a guest that lists nothing yet keeps
// what it listed last)
export function saveGuestApps(dir, apps) {
  const clean = cleanGuestApps(apps);
  if (!dir || !clean.length) return false;
  if (sameGuestApps(readMeta(dir).guestApps, clean)) return false;
  return writeMeta(dir, { guestApps: clean });
}

const has = () => typeof lord !== 'undefined' && lord.vm && typeof lord.vm.list === 'function';
let opener = null;           // library.jsx's openMachine, given at the start (no import cycle)
const keyOf = (l) => l.map((a) => `${a.id}\t${a.name}\t${a.machine}`).join('\n');
let lastKey = '';

export function refreshGuestLauncher() {
  let items = [];
  if (has() && !appsOff().includes('machines')) {
    let list = [];
    try { list = lord.vm.list() || []; } catch (e) { list = []; }
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

let timer = null;
export function startGuestLauncher(openMachine) {
  opener = openMachine;
  if (timer) return;
  refreshGuestLauncher();
  timer = setInterval(refreshGuestLauncher, 3000);
}
