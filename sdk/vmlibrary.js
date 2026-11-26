// vmlibrary.js — the shell's view of Machines' library (docs/VIRTUALIZATION.md, "The list's cache"), for what runs outside
// the Machines window: the launcher's guest apps, a group's link, the battery notice and "Pause while I'm away",
// Sentinela, Monitor's machine names.
//
//   vmMachines           store: the last lord.vm.list() ([] before the first)
//   vmAnyActive(list)    a machine starting, running, paused or stopping
//   refreshVmMachines()  reads the list now (after an action of the caller's own)
//
// No polling while nothing runs: the list is read once when the session starts and then only when lord-server says the
// library changed (__lordVm('library'): its inotify watch over ~/Machines, server/src/vm/vmlistcache.h; a machine.json
// or run.json written, a machine folder added or removed; a start, a stop and apps mode's saved list are all such
// writes), a burst coalesced into one read. While a machine is active the list is read again every ACTIVE_MS too: a
// lord-vm that dies writes nothing. Without a library (~/Machines not made yet) nothing is read; if lord-server can't
// watch it (no inotify left), the list is read every UNWATCHED_MS. The pure part (when to read) is vmLibraryPlan,
// tested in server/tests/vmlibrary_test.mjs.
import { createStore } from './store.js';

export const ACTIVE_MS = 5000;
export const UNWATCHED_MS = 30000;
const COALESCE_MS = 200;
const ACTIVE = new Set(['starting', 'running', 'paused', 'stopping']);

export const vmMachines = createStore([]);
export const vmAnyActive = (list) => (Array.isArray(list) ? list : []).some((m) => m && !m.error && ACTIVE.has(m.state));

// When to read the list again, from what lord.vm.listWatch() said (1 watched, 0 no library, -1 can't watch) and the
// list now: ms, or 0 for "only when the library says it changed"
export function vmLibraryPlan(watch, list) {
  if (vmAnyActive(list)) return ACTIVE_MS;
  if (watch === 1) return 0;
  return UNWATCHED_MS;            // no library yet (only listWatch is asked then) or no watch (the list is read)
}

const api = () => (typeof lord !== 'undefined' && lord.vm && typeof lord.vm.list === 'function' ? lord.vm : null);
let started = false, timer = null, soon = null, watch = 0;

function watchState() {
  const v = api();
  if (!v || typeof v.listWatch !== 'function') return -1;
  try { const w = v.listWatch(); return w === true ? 1 : w === false ? -1 : +w || 0; } catch (e) { return -1; }
}

export function refreshVmMachines() {
  const v = api();
  if (!v) return vmMachines.get();
  if (soon) { clearTimeout(soon); soon = null; }
  watch = watchState();
  let list = [];
  if (watch !== 0) { try { list = v.list() || []; } catch (e) { list = vmMachines.get(); } }
  const before = vmMachines.get();
  if (JSON.stringify(before) !== JSON.stringify(list)) vmMachines.set(list);
  plan();
  return list;
}

function plan() {
  if (timer) { clearTimeout(timer); timer = null; }
  const ms = vmLibraryPlan(watch, vmMachines.get());
  if (!ms) return;
  timer = setTimeout(() => {
    timer = null;
    // no library yet: only whether there is one now (a realpath), never the list
    if (watch === 0 && watchState() === 0) { plan(); return; }
    refreshVmMachines();
  }, ms);
}

globalThis.__lordVm = (what) => {
  if (what !== 'library' || !started) return;
  if (soon) return;
  soon = setTimeout(() => { soon = null; refreshVmMachines(); }, COALESCE_MS);
};

// once, with the session (index.js); nothing in a guest-less greeter or an isolated app
export function startVmLibrary() {
  if (started || !api() || typeof lord.greeter !== 'undefined') return;
  started = true;
  refreshVmMachines();
}
