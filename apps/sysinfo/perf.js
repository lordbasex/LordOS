// LordBench's model (0.8.6, docs/BENCHMARK.md "0.8.6 — LordBench"): the measurements by category, how a
// result compares with the reference machines (×N of each, never one score), the three verdicts
// (performance, compatibility, stability), the desktop run's verdict against the refresh budget, and what
// changed against the previous result. Pure
// functions over the result JSON lord.sysinfo.bench writes (server/src/bench/lbrun.c); tested in
// server/tests/sysinfo_perf_test.mjs.
import { REFERENCES } from './references.js';

export const CATEGORIES = [
  { id: 'cpu', ids: ['cpu.sha256.1', 'cpu.sha256.n', 'cpu.matmul.1', 'cpu.matmul.n', 'cpu.xz.1', 'cpu.xz.n', 'cpu.js.1', 'cpu.js.n',
    'cpu.ui.1', 'cpu.ui.n', 'cpu.shape.1', 'cpu.shape.n'] },
  { id: 'memory', ids: ['mem.copy', 'mem.triad', 'mem.latency'] },
  { id: 'gpu', ids: ['gpu.scene', 'gpu.fill', 'gpu.upload', 'gpu.layers'] },
  { id: 'storage', ids: ['disk.seqwrite', 'disk.seqread', 'disk.random4k'] },
  { id: 'sustained', ids: ['sustained.start', 'sustained.end'] },
  { id: 'video', ids: ['video.h264.1080', 'video.hevc.1080', 'video.vp9.1080', 'video.av1.1080', 'video.h264.2160', 'video.hevc.2160', 'video.enc.h264', 'video.enc.vp8'] },
];

// the desktop run (mode 'desktop'): the compositor alone, then with every core loaded at the lowest priority
export const DESKTOP_CATEGORIES = [
  { id: 'compositor', ids: ['comp.fps', 'comp.p50', 'comp.p99', 'comp.missed', 'comp.in50', 'comp.in99'] },
  { id: 'desktop', ids: ['desk.fps', 'desk.p50', 'desk.p99', 'desk.missed', 'desk.in50', 'desk.in99', 'desk.work'] },
];
// the helper-sandbox self-test lord-bench makes after locking itself in (server/src/bench/lbproto.h, LB_SBX_*):
// every one of these must be refused
export const SANDBOX_TRIES = ['helperSocket', 'helperInet', 'helperExec', 'helperFork', 'helperNewFile', 'helperRead'];
// the tries only Landlock refuses, and its bit in the sandbox mask (server/src/sec/sandbox.h, SB_LANDLOCK_FS)
export const LANDLOCK_TRIES = ['helperNewFile', 'helperRead'];
const SB_LANDLOCK_FS = 2;
export const MISSED_WARN = 5;           // % of frames shown late before the verdict warns
export const MISSED_FAIL = 25;
export const INPUT_WARN_FRAMES = 3;     // pointer-to-screen p99 above this many refresh periods warns

export { REFERENCES };

// the measurements that time LordOS's own engine code (layout, paint, shaping) with the machine: a reference
// value of one carries the LordOS version it was measured with ("lordos"), and is used only for a result of that
// same version, so an engine change (or a new interface font) never shows up as a faster or slower machine
export const ENGINE_IDS = ['cpu.ui.1', 'cpu.ui.n', 'cpu.shape.1', 'cpu.shape.n'];

// ×N of the reference for one measurement (latency: the reference's time over ours); null if either
// is missing, or if the reference value belongs to another LordOS version (ENGINE_IDS)
export function ratio(res, ref, id) {
  const a = res && res.results && res.results[id], b = ref && ref.results && ref.results[id];
  if (!a || !b || !(a.value > 0) || !(b.value > 0)) return null;
  if (b.lordos != null && b.lordos !== res.lordos) return null;
  return a.lowerBetter ? b.value / a.value : a.value / b.value;
}

// per category, the geometric mean of the ratios it has; null when none
export function categoryRatios(res, ref) {
  const out = {};
  for (const c of CATEGORIES) {
    const rs = c.ids.map((id) => ratio(res, ref, id)).filter((x) => x != null);
    out[c.id] = rs.length ? Math.exp(rs.reduce((s, x) => s + Math.log(x), 0) / rs.length) : null;
  }
  return out;
}

// the references a result can be compared with: the same LordBench version
export function comparableReferences(res) {
  return REFERENCES.filter((r) => res && r.lordbench === res.lordbench);
}

// ---- verdicts ----

const throttled = (c) => c && c.rpiThrottled != null && (c.rpiThrottled & 0x7) !== 0;

// The Raspberry Pi firmware during the run (res.piFirmware, server/src/bench/lbrun.c and lbcond.h): what it
// reported between the start and the end, not since boot. `flags` says get_throttled was readable (capped,
// throttled and softLimit are known only then); `voltageAlarm` that rpi_volt's under-voltage alarm was; tempLimit
// is the hottest zone at or above the firmware's documented limit (tempLimitC). Results from before it have only
// the end's rpiThrottled bits.
export const PI_FIRMWARE_EVENTS = ['underVoltage', 'capped', 'throttled', 'softLimit', 'tempLimit'];
export function piFirmware(res) {
  const pf = res && res.piFirmware;
  if (!pf || typeof pf !== 'object') return null;
  return {
    flags: pf.flags === true, voltageAlarm: pf.voltageAlarm === true,
    tempLimitC: typeof pf.tempLimitC === 'number' ? pf.tempLimitC : null,
    events: PI_FIRMWARE_EVENTS.filter((id) => pf[id] === true),
  };
}

export function stability(res) {
  if (!res) return { state: 'unknown', notes: [] };
  const notes = [];
  let state = 'pass';
  const warn = (id, vars = {}) => { notes.push({ id, vars }); if (state === 'pass') state = 'warn'; };
  if (res.status === 'failed') { state = 'fail'; notes.push({ id: 'failed', vars: { why: res.stopReason || '' } }); }
  else if (res.status === 'stopped') warn(res.stopReason === 'thermal' ? 'stoppedThermal' : res.stopReason === 'throttled' ? 'stoppedThrottled' : 'stopped');
  const failed = Object.entries(res.skipped || {}).filter(([, why]) => why === 'failed').map(([id]) => id);
  if (failed.length) warn('measureFailed', { count: failed.length });
  const b = res.before || {}, a = res.after || {};
  if (b.throttleEvents != null && a.throttleEvents != null && a.throttleEvents > b.throttleEvents) warn('throttleSeen', { count: a.throttleEvents - b.throttleEvents });
  const pf = piFirmware(res);
  if (pf) {
    const ev = pf.events;
    if (ev.includes('capped') || ev.includes('throttled') || ev.includes('softLimit')) warn('piThrottled');
    else if (ev.includes('tempLimit')) warn('piTempLimit', { temp: pf.tempLimitC });
    if (ev.includes('underVoltage')) warn('underVoltage');
  } else {
    if (throttled(a)) warn('piThrottled');
    if (a.rpiThrottled != null && (a.rpiThrottled & 1)) warn('underVoltage');
  }
  const s = res.results || {};
  if (s['sustained.start'] && s['sustained.end']) {
    const drop = 1 - s['sustained.end'].value / s['sustained.start'].value;
    if (drop > 0.1) warn('sustainedDrop', { pct: Math.round(drop * 100) });
  }
  if (res.sustained && res.sustained.throttleAtS != null) warn('sustainedThrottle', { s: res.sustained.throttleAtS, temp: res.sustained.throttleTempC });
  if (res.quiet && !res.quiet.quiet) warn('notQuiet', { pct: Math.round(res.quiet.busyPct) });
  // the helper's own sandbox (sec.helper-sandbox): a try that wasn't refused, or one missing, fails the run;
  // all refused is said too (info: it doesn't change the state). Results from before it was built have none.
  // On a kernel without Landlock (the sandbox mask has no SB_LANDLOCK_FS) the two tries only Landlock refuses
  // are expected to work: that is the kernel's limit, already in the mask, so it warns instead of failing.
  // The notes carry ids (vars.ids); the page translates each one (sysinfo.perf.sbx.<id>).
  if (Array.isArray(res.sandboxTries)) {
    const by = Object.fromEntries(res.sandboxTries.filter((t) => t && typeof t.id === 'string').map((t) => [t.id, t]));
    const leaks = SANDBOX_TRIES.filter((id) => !by[id] || by[id].denied !== true);
    const noLandlock = typeof res.sandbox === 'number' && !(res.sandbox & SB_LANDLOCK_FS);
    const partial = noLandlock ? leaks.filter((id) => LANDLOCK_TRIES.includes(id) && by[id]) : [];
    const real = leaks.filter((id) => !partial.includes(id));
    if (real.length) { state = 'fail'; notes.push({ id: 'sandboxLeaks', vars: { ids: real } }); }
    if (partial.length) warn('sandboxPartial', { ids: partial });
    if (!leaks.length) notes.push({ id: 'sandboxHeld', vars: { count: SANDBOX_TRIES.length }, info: true });
  }
  return { state, notes };
}

// the Compatibility panel's rows (model.js compatibility()), counted by state
export function compatibilityVerdict(rows) {
  const count = { works: 0, limits: 0, no: 0, absent: 0, unchecked: 0 };
  for (const r of rows || []) if (count[r.state] != null) count[r.state]++;
  const state = count.no ? 'fail' : count.limits ? 'warn' : rows && rows.length ? 'pass' : 'unknown';
  return { state, count };
}

// the desktop verdict (docs/BENCHMARK.md, "Frame times against the refresh budget"): a phase passes when the
// 99th percentile of the time to build a frame fits the refresh period in use and few frames came late
export function desktopVerdict(res) {
  const r = (res && res.results) || {};
  const budget = res && res.frames && res.frames.budgetMs;
  if (!budget || (!r['comp.p99'] && !r['desk.p99'])) return { state: 'unknown', notes: [] };
  const notes = [];
  let state = 'pass';
  const worse = (s) => { if (s === 'fail' || (s === 'warn' && state === 'pass')) state = s; };
  for (const phase of ['comp', 'desk']) {
    const p99 = r[`${phase}.p99`], missed = r[`${phase}.missed`];
    if (!p99) continue;
    const vars = { p99: p99.value, budget, pct: missed ? missed.value : 0 };
    if (p99.value > budget) { notes.push({ id: `${phase}Over`, vars }); worse('warn'); }
    else notes.push({ id: `${phase}Fits`, vars });
    if (missed && missed.value > MISSED_FAIL) { notes.push({ id: `${phase}MissedMany`, vars }); worse('fail'); }
    else if (missed && missed.value > MISSED_WARN) { notes.push({ id: `${phase}Missed`, vars }); worse('warn'); }
    const in50 = r[`${phase}.in50`], in99 = r[`${phase}.in99`];
    if (in50 && in99) {                 // the synthetic pointer (bench/lbinput.h)
      const ivars = { in50: in50.value, in99: in99.value };
      if (in99.value > budget * INPUT_WARN_FRAMES) { notes.push({ id: `${phase}InputSlow`, vars: ivars }); worse('warn'); }
      else notes.push({ id: `${phase}InputOk`, vars: ivars });
    }
  }
  return { state, notes };
}

// ---- comparing two results ----

// conditions that change the numbers; listed next to a comparison when they differ
export function conditionDiffs(a, b) {
  if (!a || !b) return [];
  const pick = (r) => ({
    power: r.before && r.before.power, governor: r.before && r.before.governor, profile: r.before && r.before.platformProfile,
    gpuKind: r.gpuKind, storage: r.storage && r.storage.kind, threads: r.threads, live: r.live, lordos: r.lordos, kernel: r.kernel,
    composition: r.session && r.session.composition,
  });
  const x = pick(a), y = pick(b);
  return Object.keys(x).filter((k) => x[k] !== y[k] && (x[k] != null || y[k] != null)).map((k) => ({ id: k, now: x[k], before: y[k] }));
}

// change against the previous result, per measurement (+ is better), only with the same LordBench
// version and mode
export function changes(now, prev) {
  if (!now || !prev || now.lordbench !== prev.lordbench || now.mode !== prev.mode) return null;
  const out = {};
  for (const id of Object.keys(now.results || {})) {
    const r = ratio(now, prev, id);
    if (r != null) out[id] = r - 1;
  }
  return out;
}

// the sustained test's samples as [seconds, GFLOP/s, °C or null]
export function sustainedSeries(res) {
  return (res && res.sustained && Array.isArray(res.sustained.samples) ? res.sustained.samples : [])
    .filter((s) => Array.isArray(s) && s.length === 3 && s[1] >= 0);
}

// the refresh budget of the slowest active display, ms (for the compositor's frame times)
export function refreshBudget(res) {
  const ds = (res && res.session && res.session.displays) || [];
  const hz = ds.map((d) => (d.mode && d.mode.refresh ? d.mode.refresh / 1000 : 0)).filter((x) => x > 0);
  return hz.length ? 1000 / Math.min(...hz) : null;
}

export function fileName(at, taken) {
  const d = new Date((at || 0) * 1000);
  const p = (n) => String(n).padStart(2, '0');
  const base = `lordbench-${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`;
  for (let i = 1; i < 100; i++) {
    const name = i === 1 ? `${base}.json` : `${base}-${i}.json`;
    if (!taken(name)) return name;
  }
  return null;
}
