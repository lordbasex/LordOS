// vm_list_scene.js — the theme tests/vm_list_headless.sh loads: lord.vm.list() over a made-up library of machine folders,
// timed, and lord-server's events for the shell (__lordVm, ui/js_vm.c). The first call reads the folders; the next ones
// must come from lord-server's cache (under 1 ms each). Then, step by step with the shell script (it waits for each
// "VMLIST <step>" line): a machine.json renamed and a new machine folder must bring __lordVm('library') and show at the
// next call; a run.json saying "running" under a live "lord-vm" must bring __lordVm('state') with that folder active, and
// that process's end (it writes nothing) another with none. Prints "VMLIST …" lines and "VMLIST done".
const say = (s) => console.log(`VMLIST ${s}`);
const time = (f) => { const t = performance.now(); const r = f(); return [performance.now() - t, r]; };
const median = (a) => { const s = [...a].sort((x, y) => x - y); return s[Math.floor(s.length / 2)]; };
const want = +(globalThis.VMLIST_N || 0);
let libraryEvents = 0;
const states = [];
globalThis.__lordVm = (what, json) => {
  if (what === 'library') libraryEvents++;
  if (what === 'state') { try { states.push(JSON.parse(json)); } catch (e) { say(`FAIL a state that isn't JSON: ${json}`); } }
};
const until = (ok, ms, then, fail) => {
  const t0 = Date.now();
  const look = () => {
    if (ok()) { then(Date.now() - t0); return; }
    if (Date.now() - t0 > ms) { say(`FAIL ${fail()}`); say('done'); return; }
    setTimeout(look, 25);
  };
  look();
};

const [cold, first] = time(() => lord.vm.list());
say(`cold ${cold.toFixed(2)} ms, ${first.length} machines`);
if (first.length !== want) say(`FAIL listed ${first.length}, want ${want}`);
if (first.some((m) => m.error)) say(`FAIL an error: ${JSON.stringify(first.find((m) => m.error))}`);
const warm = [];
for (let i = 0; i < 200; i++) warm.push(time(() => lord.vm.list())[0]);
say(`cached median ${median(warm).toFixed(3)} ms, max ${Math.max(...warm).toFixed(3)} ms`);
if (median(warm) >= 1) say('FAIL cached list not under 1 ms');
if (JSON.stringify(lord.vm.list()) !== JSON.stringify(first)) say('FAIL the cached list differs from the first');

// the first state comes by itself (a new JS context gets one), with nothing active
until(() => states.length > 0, 5000, () => {
  const s0 = states[states.length - 1];
  if (!Array.isArray(s0.active) || s0.active.length || !Array.isArray(s0.apps) || s0.apps.length) say(`FAIL first state: ${JSON.stringify(s0)}`);
  else say('first state: nothing active');
  const lib0 = libraryEvents;
  say('READY');                    // the script renames a machine and adds one
  let reread = 0;
  until(() => libraryEvents > lib0 && (() => { const [t, l] = time(() => lord.vm.list()); reread = t; return l.some((m) => m.name === 'Renamed') && l.length === want + 1; })(), 20000, (ms) => {
    const [again] = time(() => lord.vm.list());
    say(`change seen after ${ms} ms (library events ${libraryEvents - lib0}); read again (firmware hashed already) in ${reread.toFixed(2)} ms, then cached in ${again.toFixed(3)} ms`);
    const n0 = states.length;
    say('START');                  // the script runs a "lord-vm" and writes M5's run.json
    until(() => states.length > n0 && states[states.length - 1].active.some((d) => d.endsWith('/M5.lordvm')), 20000, (ms2) => {
      say(`started: state event after ${ms2} ms`);
      const n1 = states.length;
      say('KILL');                 // the script kills it: run.json still says running
      until(() => states.length > n1 && states[states.length - 1].active.length === 0, 20000, (ms3) => {
        say(`stopped (its lord-vm gone, nothing written): state event after ${ms3} ms`);
        if (lord.vm.list().find((m) => m.dir.endsWith('/M5.lordvm'))?.state !== 'stopped') say('FAIL the list still says running');
        say('done');
      }, () => `no state event for the lord-vm's end: ${JSON.stringify(states.slice(n1))}`);
    }, () => `no state event for the start: ${JSON.stringify(states.slice(n0))}`);
  }, () => `change not seen: ${libraryEvents - lib0} library events, ${lord.vm.list().map((m) => m.name).join(',')}`);
}, () => 'no first state event');
