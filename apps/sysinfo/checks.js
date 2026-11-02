// checks.js — System Information's Checks tab (docs/BENCHMARK.md, "Checks: configured versus
// effective"): each claim LordOS makes, checked on this machine, at one of two levels, never claiming
// the stronger one without the test:
//   configured  the mechanism is present (a kernel feature, a service running, an owner);
//   effective   something was tried and gave the right answer;
//   measured    a number, shown as it is (no pass or fail in this version).
// Part 1 (0.8.5) has only what lord-server can see itself; the probe app that tries what must fail
// (sandbox, firewall) is part 2. The thresholds are constants here: this file is bundled into the
// theme, which is signed with the release key (code-manifest.json), so they are signed too.
//
// evaluate(ctx) → rows { id, paper, level, state, note, vars }, state 'pass' | 'warn' | 'fail' |
// 'info' | 'unknown'. ctx: { report: lord.sysinfo.report(), facts: lord.sysinfo.facts(),
// code: codeStatus(), remote: remote.status() or null, stats: lord.stats() or null }. Pure: tested
// by server/tests/sysinfo_model_test.mjs.

export const LIMITS = {
  landlockAbi: 6,               // scoping of abstract sockets and signals (docs/FIREWALL.md)
  remotePort: 5673,
};

const row = (id, paper, level, state, note = null, vars = {}) => ({ id, paper, level, state, note, vars });

export function evaluate(ctx) {
  const c = ctx || {};
  const r = c.report || {}, f = c.facts || {}, sw = r.software || {}, s = r.session || {};
  const out = [];

  // lord-server runs as the user, never as root (§2.2)
  if (f.uid == null || f.uid < 0) out.push(row('serverUser', '§2.2', 'configured', 'unknown'));
  else out.push(row('serverUser', '§2.2', 'configured', f.uid > 0 && f.euid > 0 ? 'pass' : 'fail', f.uid > 0 && f.euid > 0 ? 'serverUserOk' : 'serverUserRoot', { uid: f.uid }));

  // Landlock (§6.4): the ABI the launcher needs for restricted apps
  const ll = sw.landlockAbi;
  if (ll == null) out.push(row('landlock', '§6.4', 'configured', 'unknown'));
  else if (ll >= LIMITS.landlockAbi) out.push(row('landlock', '§6.4', 'configured', 'pass', 'landlockOk', { abi: ll }));
  else if (ll > 0) out.push(row('landlock', '§6.4', 'configured', 'warn', 'landlockOld', { abi: ll, min: LIMITS.landlockAbi }));
  else out.push(row('landlock', '§6.4', 'configured', 'fail', 'landlockNone'));

  // the per-app firewall (§6.6): lord-netperm running, and the kernel pieces its programs need
  const kc = sw.kernelConfig || {};
  const services = sw.services || {};
  if (services['lord-netperm'] == null) out.push(row('netperm', '§6.6', 'configured', 'unknown'));
  else if (!services['lord-netperm']) out.push(row('netperm', '§6.6', 'configured', 'fail', 'netpermOff'));
  else if (kc.CONFIG_CGROUP_BPF === 'n' || !sw.bpfLsm) out.push(row('netperm', '§6.6', 'configured', 'warn', !sw.bpfLsm ? 'netpermNoLsm' : 'netpermNoCgroupBpf'));
  else out.push(row('netperm', '§6.6', 'configured', 'pass', 'netpermOk'));

  // LordOS's own code (§6.9): every bundle loaded this session matched the signed manifest
  const code = c.code || {};
  if (code.refused && code.refused.length) out.push(row('code', '§6.9', 'effective', 'fail', 'codeRefused', { count: code.refused.length }));
  else if (code.state === 'verified') out.push(row('code', '§6.9', 'effective', 'pass', 'codeVerified', { count: code.files || 0 }));
  else if (code.state === 'development') out.push(row('code', '§6.9', 'effective', 'warn', 'codeDevelopment'));
  else out.push(row('code', '§6.9', 'effective', 'unknown'));

  // the remote desktop (§11.4): off means nothing listens on its port (read from /proc/net)
  const listening = f.remoteListening;
  const on = c.remote ? !!c.remote.enabled : null;
  if (listening == null) out.push(row('remoteClosed', '§11.4', 'effective', 'unknown'));
  else if (on === true) out.push(row('remoteClosed', '§11.4', 'effective', 'info', listening ? 'remoteOnListening' : 'remoteOnNotListening', { port: LIMITS.remotePort }));
  else out.push(row('remoteClosed', '§11.4', 'effective', listening ? 'fail' : 'pass', listening ? 'remoteOffListening' : 'remoteOffClosed', { port: LIMITS.remotePort }));

  // the session's cgroup tree belongs to root (§11.4): an app can't leave its scope
  // (only LordOS's own tree counts: a lord-server started from a terminal sits in that terminal's cgroup)
  if (f.leafRoot == null || f.parentRoot == null) out.push(row('cgroups', '§11.4', 'configured', 'unknown'));
  else if (!/\/lordos-session-\d+\/server$/.test(f.cgroup || '')) out.push(row('cgroups', '§11.4', 'configured', 'info', 'cgroupsNotSession'));
  else out.push(row('cgroups', '§11.4', 'configured', f.leafRoot && f.parentRoot ? 'pass' : 'fail',
    f.leafRoot && f.parentRoot ? 'cgroupsOk' : 'cgroupsUser', { scopes: f.appScopes || 0 }));

  // the GPU composes when there is one (§7.3)
  if (s.composition === 'gpu') out.push(row('gpu', '§7.3', 'configured', 'pass', 'gpuOn', { renderer: s.renderer || '' }));
  else if (['headless', 'no-display', 'capture'].includes(s.composition)) out.push(row('gpu', '§7.3', 'configured', 'info', `gpuNot.${s.composition}`));
  else out.push(row('gpu', '§7.3', 'configured', 'warn', `gpuNot.${s.gpuInit || s.composition || 'unknown'}`));

  // memory (§7.1): what this session uses now; the idle measurement belongs to LordBench (0.8.6)
  const st = c.stats || {};
  if (st.rssKB) out.push(row('memory', '§7.1', 'measured', 'info', 'memoryNow', { mb: Math.round(st.rssKB / 1024) }));

  return out;
}

// ---- part 2: the Sandbox Probe (apps/sysprobe, server/src/app/probe.c) ----
// result: lord.sysinfo.probe() → { probe: { v, sandbox, tries: [{ id, denied, errno }] },
//   proc: { noNewPrivs, seccomp, capEff, cgroup, memoryMax }, at }.

const KERNEL = ['fsHome', 'fsSystem', 'cgroupEscape', 'netInet', 'netUnix', 'ptrace', 'userns', 'mount', 'exec'];
const HELPER = ['helperSocket', 'helperExec', 'helperFork', 'helperData'];
const BROKER = ['brokerFsEscape', 'brokerClipboard', 'brokerFido', 'brokerMail'];

export function evaluateProbe(result) {
  const p = (result && result.probe) || {}, proc = (result && result.proc) || {};
  const tries = Array.isArray(p.tries) ? p.tries : [];
  const by = Object.fromEntries(tries.filter((t) => t && typeof t.id === 'string').map((t) => [t.id, t]));
  // what got through: a try that must fail and didn't, or one that never ran
  const leaks = (ids) => ids.filter((id) => !by[id] || by[id].denied !== true);
  const out = [];
  const ownOk = by.ownData && by.ownData.denied === false;

  const k = leaks(KERNEL);
  const procOk = proc.noNewPrivs === '1' && proc.seccomp === '2' && /^0+$/.test(proc.capEff || '');
  if (!ownOk) out.push(row('appSandbox', '§6.4', 'effective', 'unknown', 'probeBroken'));
  else if (k.length) out.push(row('appSandbox', '§6.4', 'effective', 'fail', 'sandboxLeaks', { list: k.join(', ') }));
  else if (!procOk) out.push(row('appSandbox', '§6.4', 'effective', 'warn', 'sandboxProc', { nnp: proc.noNewPrivs || '?', seccomp: proc.seccomp || '?' }));
  else out.push(row('appSandbox', '§6.4', 'effective', 'pass', 'sandboxOk', { count: KERNEL.length }));

  if (by.helperRan) out.push(row('helperSandbox', '§6.7', 'effective', 'unknown', 'helperNotRun'));
  else {
    const h = leaks(HELPER);
    out.push(row('helperSandbox', '§6.7', 'effective', h.length ? 'fail' : 'pass', h.length ? 'sandboxLeaks' : 'helperOk', h.length ? { list: h.join(', ') } : { count: HELPER.length }));
  }

  const b = leaks(BROKER);
  out.push(row('broker', '§6.5', 'effective', b.length ? 'fail' : 'pass', b.length ? 'sandboxLeaks' : 'brokerOk', b.length ? { list: b.join(', ') } : { count: BROKER.length }));

  // its own scope with a memory cap (§6.4: memory.max 512 MB)
  const scoped = /\/app-lordos-lpk-sysprobe\.scope$/.test(proc.cgroup || '');
  const mb = /^\d+$/.test(proc.memoryMax || '') ? Math.round(+proc.memoryMax / 1048576) : null;
  if (!proc.cgroup) out.push(row('appScope', '§6.4', 'configured', 'unknown'));
  else out.push(row('appScope', '§6.4', 'configured', scoped && mb ? 'pass' : 'warn', scoped ? (mb ? 'scopeOk' : 'scopeNoCap') : 'scopeMissing', { mb: mb || 0 }));
  return out;
}

// What part 2 doesn't check yet.
export const PENDING = ['netpermEffective'];
// Before the probe has run.
export const PROBE_CHECKS = ['appSandbox', 'helperSandbox', 'broker', 'appScope'];

// ---- the Test Bench's last report (lordos-testbench, ~/.local/state/lordos/testbench.json) ----
// The Wayland checklist (§3.4), the 3D renderer, whether frames are presented and input-to-screen latency.
export function evaluateBench(b) {
  if (!b || b.tool !== 'lordos-testbench' || !Array.isArray(b.checks)) return null;
  const out = [];
  const fails = b.checks.filter((c) => c && c.state === 'fail');
  const passes = b.checks.filter((c) => c && c.state === 'pass').length;
  out.push(row('wayland', '§3.4', 'effective', b.checks.length ? (fails.length ? 'fail' : 'pass') : 'unknown',
    fails.length ? 'benchFails' : 'benchPasses', { pass: passes, fail: fails.length, list: fails.map((c) => c.id).join(', ') }));
  const gl = b.gl || {};
  if (gl.renderer) out.push(row('renderer3d', '§7.3', 'effective', /SOFTWARE/.test(gl.kind || '') ? 'warn' : 'pass', 'benchRenderer', { renderer: gl.renderer, ms: Math.round((gl.drawMs || 0) * 100) / 100 }));
  else if (gl.error) out.push(row('renderer3d', '§7.3', 'effective', 'unknown', 'benchNo3d', { error: gl.error }));
  else out.push(row('renderer3d', '§7.3', 'effective', 'unknown', 'benchNo3dRun'));
  if (b.presentation) out.push(row('presented', '§7.2', 'measured', b.presented ? 'info' : 'warn', b.presented ? 'benchPresented' : 'benchNotPresented',
    { shown: b.presented || 0, discarded: b.discarded || 0, hz: Math.round((b.refreshHz || b.outputRefreshHz || 0) * 100) / 100, fps: Math.round((b.fps || 0) * 10) / 10 }));
  const lat = b.latency || {};
  if (lat.n) out.push(row('latency', '§7.2', 'measured', 'info', 'benchLatency', { ms: Math.round(lat.avgMs * 10) / 10, n: lat.n }));
  return out;
}
