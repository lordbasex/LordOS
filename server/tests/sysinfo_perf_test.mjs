// sysinfo_perf_test.mjs — LordBench's model (apps/sysinfo/perf.js, 0.8.6): ratios against the reference
// machines, category means, the stability and compatibility verdicts, comparisons with the previous
// result and the conditions that differ. Run by tests/run_suites.sh (needs node).
import { CATEGORIES, DESKTOP_CATEGORIES, SANDBOX_TRIES, ENGINE_IDS, desktopVerdict, REFERENCES, ratio, categoryRatios, comparableReferences, stability, compatibilityVerdict, conditionDiffs, changes, sustainedSeries, refreshBudget, fileName, piFirmware } from '../../apps/sysinfo/perf.js';

let n = 0, bad = 0;
const eq = (what, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${what}\n    got  ${g}\n    want ${w}`); }
};
const near = (what, got, want) => eq(what, got != null && Math.abs(got - want) < 1e-9, true);

const res = (over = {}) => ({
  lordbench: 1, mode: 'full', status: 'done', stopReason: '', threads: 4, live: false, gpuKind: 'hardware', lordos: '0.8.6', kernel: 'k',
  quiet: { quiet: true, busyPct: 2 }, before: { power: 'mains', governor: 'schedutil' }, after: { power: 'mains' }, storage: { kind: 'nvme' },
  session: { composition: 'gpu', displays: [{ mode: { w: 1920, h: 1080, refresh: 60000 } }, { mode: { w: 2560, h: 1440, refresh: 144000 } }] },
  results: { 'cpu.sha256.1': { value: 200 }, 'cpu.matmul.1': { value: 8 }, 'mem.latency': { value: 45, lowerBetter: true },
    'sustained.start': { value: 50 }, 'sustained.end': { value: 49 } },
  skipped: {}, ...over,
});
const ref = { lordbench: 1, results: { 'cpu.sha256.1': { value: 100 }, 'cpu.matmul.1': { value: 2 }, 'mem.latency': { value: 90 } } };

// every measurement of a peak run (lord-bench's and lord-media's) is in exactly one category
const ids = CATEGORIES.flatMap((c) => c.ids);
eq('32 measurements, no repeats', [ids.length, new Set(ids).size], [32, 32]);
eq('references have every measurement or say it was not measured', REFERENCES.every((r) => ids.every((id) => (r.results[id] && r.results[id].value > 0) !== (r.notMeasured || []).includes(id))), true);

near('×2 SHA', ratio(res(), ref, 'cpu.sha256.1'), 2);
// the engine's own workloads compare only within one LordOS version
const engRef = { lordbench: 1, results: { 'cpu.ui.1': { value: 20, lordos: '0.8.6' }, 'cpu.sha256.1': { value: 50 } } };
const engRes = (v) => res({ lordos: v, results: { 'cpu.ui.1': { value: 40 }, 'cpu.sha256.1': { value: 100 } } });
near('engine id, same version', ratio(engRes('0.8.6'), engRef, 'cpu.ui.1'), 2);
eq('engine id, another version: no ×N', ratio(engRes('0.8.7'), engRef, 'cpu.ui.1'), null);
near('other ids ignore the version', ratio(engRes('0.8.7'), engRef, 'cpu.sha256.1'), 2);
eq('every engine id of a reference carries its version', REFERENCES.every((r) => ENGINE_IDS.every((id) => !r.results[id] || typeof r.results[id].lordos === 'string')), true);
near('latency: lower is better', ratio(res(), ref, 'mem.latency'), 2);
eq('missing on one side', ratio(res(), ref, 'gpu.scene'), null);
const cr = categoryRatios(res(), ref);
near('CPU: geometric mean of ×2 and ×4', cr.cpu, Math.sqrt(8));
eq('no GPU numbers: null', cr.gpu, null);
eq('only the same LordBench version', comparableReferences({ lordbench: 99 }).length, 0);

// stability
eq('clean run passes', stability(res()), { state: 'pass', notes: [] });
eq('failed', stability(res({ status: 'failed', stopReason: 'helper' })).state, 'fail');
eq('thermal stop', stability(res({ status: 'stopped', stopReason: 'thermal' })).notes[0].id, 'stoppedThermal');
eq('throttle counters grew', stability(res({ before: { throttleEvents: 2 }, after: { throttleEvents: 5 } })).notes, [{ id: 'throttleSeen', vars: { count: 3 } }]);
eq('Pi throttling and under-voltage', stability(res({ after: { rpiThrottled: 0x50005 } })).notes.map((x) => x.id), ['piThrottled', 'underVoltage']);
eq('Pi: only past events', stability(res({ after: { rpiThrottled: 0x50000 } })).state, 'pass');
// the Pi firmware during the run (piFirmware), which replaces the end's bits when present
const pi5 = (over) => ({ flags: false, voltageAlarm: true, underVoltage: false, tempLimit: false, tempLimitC: 80, ...over });
eq('Pi 5: nothing during the run', stability(res({ piFirmware: pi5({}) })), { state: 'pass', notes: [] });
eq('Pi 5: under-voltage during the run', stability(res({ piFirmware: pi5({ underVoltage: true }) })).notes.map((x) => x.id), ['underVoltage']);
eq('Pi 5: the firmware temperature limit', stability(res({ piFirmware: pi5({ tempLimit: true }) })).notes, [{ id: 'piTempLimit', vars: { temp: 80 } }]);
eq('Pi: capped with flags, limit not said twice', stability(res({ piFirmware: { flags: true, voltageAlarm: false, underVoltage: true, capped: true, throttled: false, softLimit: false, tempLimit: true, tempLimitC: 80 } })).notes.map((x) => x.id),
  ['piThrottled', 'underVoltage']);
eq('Pi: piFirmware wins over the end\'s bits', stability(res({ after: { rpiThrottled: 0x5 }, piFirmware: pi5({}) })).state, 'pass');
eq('piFirmware events', piFirmware(res({ piFirmware: { flags: true, voltageAlarm: true, underVoltage: true, capped: false, throttled: 'yes', softLimit: true, tempLimit: false, tempLimitC: 80 } })),
  { flags: true, voltageAlarm: true, tempLimitC: 80, events: ['underVoltage', 'softLimit'] });
eq('piFirmware: not a Pi', piFirmware(res()), null);
eq('sustained dropped 20 %', stability(res({ results: { 'sustained.start': { value: 50 }, 'sustained.end': { value: 40 } } })).notes, [{ id: 'sustainedDrop', vars: { pct: 20 } }]);
eq('not quiet', stability(res({ quiet: { quiet: false, busyPct: 37.4 } })).notes, [{ id: 'notQuiet', vars: { pct: 37 } }]);
// the helper-sandbox self-test
const tries = (over = {}) => SANDBOX_TRIES.map((id) => ({ id, denied: over[id] == null ? true : over[id], errno: 1 }));
eq('every try refused: said, still a pass', stability(res({ sandboxTries: tries() })), { state: 'pass', notes: [{ id: 'sandboxHeld', vars: { count: 6 }, info: true }] });
eq('a try that worked fails the run', stability(res({ sandboxTries: tries({ helperExec: false }) })), { state: 'fail', notes: [{ id: 'sandboxLeaks', vars: { ids: ['helperExec'] } }] });
eq('a missing try counts as not refused', stability(res({ sandboxTries: tries().filter((t) => t.id !== 'helperRead') })).notes[0].vars.ids, ['helperRead']);
// without Landlock (mask SB_SECCOMP|SB_NOPRIV|SB_NOCAPS, no SB_LANDLOCK_FS) the file tries only warn
const noLl = { sandbox: 8 | 16 | 32, sandboxTries: tries({ helperNewFile: false, helperRead: false }) };
eq('no Landlock: the file tries warn', stability(res(noLl)), { state: 'warn', notes: [{ id: 'sandboxPartial', vars: { ids: ['helperNewFile', 'helperRead'] } }] });
eq('no Landlock: exec still fails', stability(res({ ...noLl, sandboxTries: tries({ helperNewFile: false, helperExec: false }) })).state, 'fail');
eq('with Landlock: a file try fails', stability(res({ sandbox: 2 | 8 | 16 | 32, sandboxTries: tries({ helperRead: false }) })).state, 'fail');
eq('no Landlock: a missing file try still fails', stability(res({ sandbox: 8, sandboxTries: tries().filter((t) => t.id !== 'helperRead') })).state, 'fail');
eq('older results have no self-test', stability(res()).notes, []);
eq('a measurement failed', stability(res({ skipped: { 'gpu.scene': 'failed', 'disk.seqread': 'mode' } })).notes, [{ id: 'measureFailed', vars: { count: 1 } }]);

eq('compatibility: limits warn', compatibilityVerdict([{ state: 'works' }, { state: 'limits' }, { state: 'absent' }]),
  { state: 'warn', count: { works: 1, limits: 1, no: 0, absent: 1, unchecked: 0 } });
eq('compatibility: nothing', compatibilityVerdict([]).state, 'unknown');

// comparisons
eq('same conditions', conditionDiffs(res(), res()), []);
eq('battery and governor differ', conditionDiffs(res({ before: { power: 'battery', governor: 'powersave' } }), res()),
  [{ id: 'power', now: 'battery', before: 'mains' }, { id: 'governor', now: 'powersave', before: 'schedutil' }]);
const ch = changes(res({ results: { 'cpu.sha256.1': { value: 220 }, 'mem.latency': { value: 50, lowerBetter: true } } }), res());
near('+10 %', ch['cpu.sha256.1'], 0.1);
near('latency got worse', ch['mem.latency'], 45 / 50 - 1);
eq('another mode: no comparison', changes(res({ mode: 'quick' }), res()), null);
eq('another LordBench: no comparison', changes(res({ lordbench: 2 }), res()), null);

eq('samples, bad ones dropped', sustainedSeries({ sustained: { samples: [[10, 19.8, null], [20, -1, 50], 'x', [30, 19.7, 61.5]] } }), [[10, 19.8, null], [30, 19.7, 61.5]]);
near('the slowest display sets the budget', refreshBudget(res()), 1000 / 60);
eq('no displays', refreshBudget(res({ session: {} })), null);
const at = Date.UTC(2026, 9, 27, 12) / 1000;
eq('file name', fileName(at, () => false), 'lordbench-2026-10-27.json');
eq('file name taken', fileName(at, (x) => x === 'lordbench-2026-10-27.json'), 'lordbench-2026-10-27-2.json');

// the desktop run
const desk = (results, budgetMs = 1000 / 60) => ({ mode: 'desktop', frames: { budgetMs }, results });
const v = (value) => ({ value });
eq('no frames: unknown', desktopVerdict(desk({})).state, 'unknown');
eq('no budget: unknown', desktopVerdict({ results: { 'comp.p99': v(3) } }).state, 'unknown');
let dv = desktopVerdict(desk({ 'comp.p99': v(4), 'comp.missed': v(0), 'desk.p99': v(9), 'desk.missed': v(1.5) }));
eq('both fit', dv.state, 'pass');
eq('both fit: notes', dv.notes.map((x) => x.id), ['compFits', 'deskFits']);
dv = desktopVerdict(desk({ 'comp.p99': v(4), 'comp.missed': v(0), 'desk.p99': v(21), 'desk.missed': v(3) }));
eq('p99 over the budget under load', [dv.state, dv.notes.map((x) => x.id)], ['warn', ['compFits', 'deskOver']]);
dv = desktopVerdict(desk({ 'comp.p99': v(4), 'comp.missed': v(8), 'desk.p99': v(5), 'desk.missed': v(40) }));
eq('late frames', [dv.state, dv.notes.map((x) => x.id)], ['fail', ['compFits', 'compMissed', 'deskFits', 'deskMissedMany']]);
dv = desktopVerdict(desk({ 'comp.p99': v(4), 'comp.missed': v(0), 'comp.in50': v(20), 'comp.in99': v(30), 'desk.p99': v(9), 'desk.missed': v(1), 'desk.in50': v(35), 'desk.in99': v(60) }));
eq('pointer latency: within three frames alone, over under load', [dv.state, dv.notes.map((x) => x.id)], ['warn', ['compFits', 'compInputOk', 'deskFits', 'deskInputSlow']]);
eq('pointer latency notes carry their numbers', dv.notes[3].vars, { in50: 35, in99: 60 });
eq('no pointer latency without both numbers', desktopVerdict(desk({ 'comp.p99': v(4), 'comp.in99': v(90) })).notes.map((x) => x.id), ['compFits']);
eq('a 144 Hz budget is tighter', desktopVerdict(desk({ 'comp.p99': v(9), 'comp.missed': v(0) }, 1000 / 144)).state, 'warn');
eq('every desktop id is a server measurement', DESKTOP_CATEGORIES.flatMap((c) => c.ids).every((id) => /^(comp|desk)\./.test(id)), true);
eq('video is a peak category', CATEGORIES.some((c) => c.id === 'video' && c.ids.length === 8), true);
const dch = changes(desk({ 'desk.p99': { value: 8, lowerBetter: true } }), desk({ 'desk.p99': { value: 10, lowerBetter: true } }));
near('a faster frame is better', dch['desk.p99'], 10 / 8 - 1);

console.log(`sysinfo-perf-test: ${n - bad} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
