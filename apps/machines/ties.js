// ties.js — Machines and the rest of the system (docs/VIRTUALIZATION.md 6.14), started once with the session:
//   - Search (❖Space) finds machines by name; Enter opens the machine's window (it starts if it was off);
//   - the machines chosen in Configure → Startup and shutdown → "Start with the session" start without a window once the
//     desktop is up (settings.machinesAtLogin, in settings.json: apps can't write it, so none can add a machine to it);
//     every start goes through lord-server's resource budget, as any other;
//   - when the computer leaves the charger while machines run, a notice offers Pause All (a guest's load keeps the
//     computer busy and drains the battery);
//   - "Pause while I'm away" (app.json autoPause): after AWAY_SEC without input the machines that chose it pause, and
//     resume when the person is back (one the person paused stays paused).
// Nothing here reads the library while no machine runs (docs/VIRTUALIZATION.md, "The shell and the machines"): the battery
// and away checks tick only while lord-server says one is active (vmState), the session's starts read it only when some
// machine chose to start with the session, and search only when the person types.
// The decisions are model.js's pure functions (searchMachines, sessionStarts, batteryNotice, awayStep), tested in
// server/tests/vm_model_test.mjs.
import { registerSpotlightSource, settings, battery, notify, addListener, t as tr, vmState, vmAnyActive } from '@lordos/sdk';
import { openMachine } from './library.jsx';
import { startPrep, vmRefusal } from './ui.jsx';
import { readMeta } from './meta.js';
import { searchMachines, sessionStarts, batteryNotice, awayStep } from './model.js';

const has = () => typeof lord !== 'undefined' && !!lord.vm && typeof lord.vm.list === 'function';
const list = () => { try { return has() ? lord.vm.list() || [] : []; } catch (e) { return []; } };
const TICK_MS = 15000;
const START_AFTER_MS = 6000;
const START_GAP_MS = 4000;

let started = false;
export function startMachinesTies() {
  if (started) return;
  started = true;
  registerSpotlightSource('machines', (q, query, { fold }) => {
    if (!has() || q.length < 2) return [];
    return searchMachines(list(), q, fold).map(({ m, score }) => ({
      key: `machine:${m.id || m.dir}`, score, title: m.name, subtitle: tr('machines.title'), glyph: '▣',
      preview: { kind: 'text', text: tr('machines.search.open', { name: m.name }) }, run: () => openMachine(m),
    }));
  });
  // start with the session: one at a time, a moment apart, after the desktop is up
  setTimeout(() => {
    const want = settings.get().machinesAtLogin;
    if (!Array.isArray(want) || !want.length) return;
    const todo = sessionStarts(list(), want);
    const next = () => {
      const m = todo.shift();
      if (!m) return;
      startPrep(m).then(() => {
        if (!lord.vm.startDetached(m.dir)) notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.atLogin.failed', { name: m.name }), body: vmRefusal(''), sound: false });
        setTimeout(next, START_GAP_MS);
      }, () => setTimeout(next, START_GAP_MS));
    };
    next();
  }, START_AFTER_MS);
  // input, for "Pause while I'm away"
  let lastInput = Date.now();
  const touch = () => { lastInput = Date.now(); };
  addListener('keydown', touch);
  addListener('pointerdown', touch);
  addListener('pointermove', touch);
  const paused = new Set();
  let bmem = {};
  const tick = () => {
    if (!has()) return;
    const ms = list();
    const b = batteryNotice(battery.get(), ms, bmem);
    bmem = b.mem;
    if (b.notice) {
      notify({ app: 'machines', label: tr('machines.title'), title: tr('machines.battery.title', { count: b.notice }), body: tr('machines.battery.body'), sound: false,
        actions: [{ id: 'pause', label: tr('machines.battery.pauseAll'), run: () => { for (const m of list()) if (m.state === 'running') lord.vm.stop(m.dir, 'pause'); } }] });
    }
    // a machine paused here that something else resumed or stopped since is no longer this one's to resume
    for (const d of [...paused]) if (!ms.some((m) => m.dir === d && m.state === 'paused')) paused.delete(d);
    const step = awayStep((Date.now() - lastInput) / 1000, ms, readMeta, paused);
    for (const d of step.pause) { lord.vm.stop(d, 'pause'); paused.add(d); }
    for (const d of step.resume) { lord.vm.stop(d, 'resume'); paused.delete(d); }
  };
  // only while a machine is active: started and stopped by lord-server's state events (vmState)
  let timer = null;
  const follow = () => {
    if (vmAnyActive() && !timer) {
      if (!battery.get()?.discharging) bmem = {};          // on the charger meanwhile: the next unplug says it again
      timer = setInterval(tick, TICK_MS);
    } else if (!vmAnyActive() && timer) { clearInterval(timer); timer = null; paused.clear(); }
  };
  vmState.subscribe(follow);
  follow();
  // back from away: resume at once, not at the next tick
  addListener('pointerdown', () => {
    if (!paused.size || !has()) return;
    const step = awayStep(0, list(), readMeta, paused);
    for (const d of step.resume) { lord.vm.stop(d, 'resume'); paused.delete(d); }
  });
}
