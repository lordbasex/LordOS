// vm_list_scene.js — the theme tests/vm_list_headless.sh loads: lord.vm.list() over a made-up library of machine folders
// (no machine runs), timed. The first call reads the folders; the next ones must come from lord-server's cache (under
// 1 ms each); a change the shell script makes to a machine.json (its name) and a new machine folder must show at the next
// call, and listVersion must say that something changed. Prints "VMLIST …" lines and "VMLIST done".
const say = (s) => console.log(`VMLIST ${s}`);
const time = (f) => { const t = performance.now(); const r = f(); return [performance.now() - t, r]; };
const median = (a) => { const s = [...a].sort((x, y) => x - y); return s[Math.floor(s.length / 2)]; };
const want = +(globalThis.VMLIST_N || 0);
const [cold, first] = time(() => lord.vm.list());
say(`cold ${cold.toFixed(2)} ms, ${first.length} machines`);
if (first.length !== want) say(`FAIL listed ${first.length}, want ${want}`);
if (first.some((m) => m.error)) say(`FAIL an error: ${JSON.stringify(first.find((m) => m.error))}`);
const warm = [];
for (let i = 0; i < 200; i++) warm.push(time(() => lord.vm.list())[0]);
say(`cached median ${median(warm).toFixed(3)} ms, max ${Math.max(...warm).toFixed(3)} ms`);
if (median(warm) >= 1) say('FAIL cached list not under 1 ms');
const same = JSON.stringify(lord.vm.list()) === JSON.stringify(first);
if (!same) say('FAIL the cached list differs from the first');
const v0 = typeof lord.vm.listVersion === 'function' ? lord.vm.listVersion() : -1;
if (v0 < 0) say('FAIL no listVersion');
if (lord.vm.listVersion() !== v0) say('FAIL listVersion moved with nothing changed');
// the shell script renames a machine and adds one when it sees READY
say('READY');
const t0 = Date.now();
let lastRead = 0;
const look = () => {
  const [ms, l] = time(() => lord.vm.list());
  if (ms > 0.5) lastRead = ms;
  if (l.some((m) => m.name === 'Renamed') && l.length === want + 1) {
    say(`change seen after ${Date.now() - t0} ms; read again (firmware hashed already) in ${lastRead.toFixed(2)} ms`);
    if (lord.vm.listVersion() === v0) say('FAIL listVersion did not move');
    say('done');
    return;
  }
  if (Date.now() - t0 > 20000) { say(`FAIL change not seen: ${l.length} machines, names ${l.map((m) => m.name).join(',')}`); say('done'); return; }
  setTimeout(look, 50);
};
setTimeout(look, 50);
