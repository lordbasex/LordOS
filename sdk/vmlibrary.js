// vmlibrary.js — what the shell knows about Machines without asking (docs/VIRTUALIZATION.md, "The shell and the
// machines"): which machines run and which are in apps mode, as lord-server says it. For everything outside the Machines
// window: the launcher's guest apps, the battery notice and "Pause while I'm away", a group's link, Sentinela, the idle
// sleep's check, deactivating Machines.
//
//   vmState              store: { known, active: [dir…], apps: [dir…] } (known false until lord-server said it once)
//   vmAnyActive()        a machine is starting, running, paused or stopping
//   vmLibraryChanged     store: a number that moves when something in ~/Machines changed (a machine made, renamed,
//                        removed, its app.json or machine.json written)
//
// The rule (the maintainer's, 2026-11-26): the shell makes no lord.vm call at all while no machine runs, and nothing
// polls. lord-server sends __lordVm('state', json) when the set of active machines or of sessions in apps mode changes
// (its inotify watch over the library sees run.json renamed into place at every start and stop, a pidfd sees a lord-vm
// that died, apps mode is its own), and once to every new JS context; __lordVm('library') when the library changed.
// Whoever needs the list itself (lord.vm.list) reads it only from one of those events, or while a machine is active.
import { createStore } from './store.js';

export const vmState = createStore({ known: false, active: [], apps: [] });
export const vmLibraryChanged = createStore(0);
export const vmAnyActive = () => vmState.get().active.length > 0;

// what lord-server sent: { active: [string], apps: [string] }; anything else is ignored
export function parseVmState(json) {
  let v = null;
  try { v = typeof json === 'string' ? JSON.parse(json) : null; } catch (e) { v = null; }
  if (!v || typeof v !== 'object') return null;
  const dirs = (a) => (Array.isArray(a) ? a.filter((x) => typeof x === 'string' && x.startsWith('/')).slice(0, 256) : []);
  return { known: true, active: dirs(v.active), apps: dirs(v.apps) };
}

const same = (a, b) => a.known === b.known && a.active.join('\n') === b.active.join('\n') && a.apps.join('\n') === b.apps.join('\n');

globalThis.__lordVm = (what, json) => {
  if (what === 'library') { vmLibraryChanged.set((n) => n + 1); return; }
  if (what !== 'state') return;
  const v = parseVmState(json);
  if (v && !same(v, vmState.get())) vmState.set(v);
};
