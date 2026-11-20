// model.js — Machines' pure logic (docs/VIRTUALIZATION.md, "Phase 1 implementation", The app): what a machine's
// state means on screen, what the New machine sheet accepts, and where a point of the window falls on the guest's
// picture. No engine calls here, so server/tests/vm_model_test.mjs runs it under node.

// lord-vm's states and reasons (server/src/vm/vmproto.h, VS_* and VR_*) → what the app shows. `tone`: 'run' (green),
// 'pause' (amber), 'off' (gray), 'busy' (accent), 'bad' (red). The text key is under `machines.state.*`, and the
// reason, when it's worth saying, under `machines.why.*`.
import { compareNames } from '../../sdk/collate.js';
const BAD_REASONS = ['qemu-failed', 'qemu-signal', 'out-of-memory', 'start-failed', 'guest-panic', 'supervisor-lost', 'forced', 'resume-failed'];
export function stateView(state, reason) {
  const r = reason || 'none';
  switch (state) {
    case 'running': return { key: 'running', tone: 'run', active: true, why: null };
    case 'paused': return { key: 'paused', tone: 'pause', active: true, why: null };
    case 'starting': return { key: 'starting', tone: 'busy', active: true, why: null };
    case 'stopping': return { key: r === 'suspended' ? 'suspending' : 'stopping', tone: 'busy', active: true, why: null };
    case 'crashed': return { key: 'crashed', tone: 'bad', active: false, why: r === 'none' ? 'qemu-failed' : r };
    default:
      // a machine whose supervisor died is shown as off, with the reason: lord-vm recovers it at the next start
      if (r === 'supervisor-lost') return { key: 'recovered', tone: 'bad', active: false, why: r };
      // its memory is in suspend.state: the next start resumes where it was (6.1)
      if (r === 'suspended') return { key: 'suspended', tone: 'pause', active: false, why: null };
      return { key: 'stopped', tone: 'off', active: false, why: BAD_REASONS.includes(r) ? r : null };
  }
}

// which actions a state allows (the list's buttons and the window's menu)
export function actionsFor(state) {
  const on = state === 'running' || state === 'paused';
  return {
    start: state === 'stopped' || state === 'crashed' || !state,
    open: on || state === 'starting',
    shutdown: state === 'running',
    poweroff: on || state === 'starting' || state === 'stopping',
    pause: state === 'running',
    resume: state === 'paused',
    reset: on,
    suspend: on,
    keys: state === 'running',
    remove: state === 'stopped' || state === 'crashed' || !state,
  };
}

// ---- the New machine sheet ----

export const LIMITS = { vcpusMax: 64, memMin: 256, memMax: 1024 * 1024, diskMin: 1, diskMax: 16384, nameMax: 64 };

const utf8Len = (s) => { let n = 0; for (const ch of s) { const c = ch.codePointAt(0); n += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; } return n; };

// defaults for this computer (lord.vm.host()): its own architecture, the recommended vCPUs (all but one, at most 4 —
// a Pi 5 gets 3), 2 GB or what fits, 20 GB of disk
export function defaultsFor(host) {
  const arch = host?.arch || 'amd64';
  const vcpus = Math.max(1, Math.min(4, host?.vcpusRecommended || 1));
  const memMax = host?.memMaxMb || 2048;
  const memoryMb = Math.max(LIMITS.memMin, Math.min(2048, memMax >= 1024 ? Math.floor(memMax / 256) * 256 : memMax));
  return { name: '', arch, vcpus, memoryMb, diskGb: 20, source: 'import', path: '', rootPassword: '' };
}

// The sheet's draft → { ok, errors: { field: key }, warnings: [key], emulated }. Error and warning keys are under
// `machines.new.err.*` / `machines.new.warn.*`; lord-vm checks everything again.
export function validateDraft(d, host, names = []) {
  const errors = {}, warnings = [];
  const name = (d.name || '').trim();
  if (!name) errors.name = 'nameEmpty';
  else if (utf8Len(name) > LIMITS.nameMax || /[\u0000-\u001f\u007f]/.test(name)) errors.name = 'nameBad';
  else if (names.some((n) => n.toLowerCase() === name.toLowerCase())) errors.name = 'nameTaken';
  const emulated = !!host?.arch && d.arch !== host.arch;
  if (d.arch !== 'amd64' && d.arch !== 'arm64') errors.arch = 'arch';
  const cpus = host?.cpus || LIMITS.vcpusMax;
  if (!Number.isInteger(d.vcpus) || d.vcpus < 1 || d.vcpus > Math.min(cpus, LIMITS.vcpusMax)) errors.vcpus = 'vcpus';
  else if (host?.vcpusRecommended && d.vcpus > host.vcpusRecommended) warnings.push('vcpus');
  const memMax = host?.memMaxMb || LIMITS.memMax;
  if (!Number.isInteger(d.memoryMb) || d.memoryMb < LIMITS.memMin || d.memoryMb > Math.min(memMax, LIMITS.memMax)) errors.memoryMb = 'memory';
  if (!Number.isInteger(d.diskGb) || d.diskGb < LIMITS.diskMin || d.diskGb > LIMITS.diskMax) errors.diskGb = 'disk';
  const path = d.path || '';
  if (!path) errors.path = d.source === 'iso' ? 'isoMissing' : 'imageMissing';
  else if (d.source === 'iso' && !/\.iso$/i.test(path)) errors.path = 'isoKind';
  else if (d.source !== 'iso' && !/\.(qcow2|img|raw)$/i.test(path)) errors.path = 'imageKind';
  if (emulated) warnings.push('emulated');
  else if (host && !host.kvm) warnings.push('noKvm');
  return { ok: Object.keys(errors).length === 0, errors, warnings, emulated };
}

// QEMU's own memory beside the guest's (machine.h, VM_OVERHEAD_*): what a machine's cgroup limit adds
export const OVERHEAD_MB = { kvm: 384, tcg: 640 };
export const memLimit = (m) => (m?.memoryMb || 0) + (m?.accel === 'tcg' ? OVERHEAD_MB.tcg : OVERHEAD_MB.kvm);

// At start (3.7, vm_budget_fits): the machines already running plus this one, each with QEMU's overhead, within what
// this computer lets machines use (host.memMaxMb: RAM minus the desktop's floor). One machine alone always may start
// (the sheet already kept its memory within memMaxMb).
export function budgetCheck(list, m, host) {
  const running = (list || []).filter((x) => x.dir !== m.dir && stateView(x.state).active).reduce((a, x) => a + memLimit(x), 0);
  const need = memLimit(m), total = host?.memMaxMb || 0;
  return { ok: running === 0 || running + need <= total, need, free: Math.max(0, total - running) };
}

// what lord.vm.create takes
export function createRequest(d) {
  const r = { name: d.name.trim(), arch: d.arch, vcpus: d.vcpus, memoryMb: d.memoryMb, diskGb: d.diskGb, network: d.network === 'isolated' ? 'isolated' : 'shared' };
  if (d.source === 'iso') r.iso = d.path; else r.import = d.path;
  if (d.source !== 'iso' && d.rootPassword) r.rootPassword = d.rootPassword;
  if (d.shareClipboard) r.shareClipboard = true;
  if (d.graphics === '3d') r.graphics = '3d';
  return r;
}

// 3D graphics (6.10): why a machine can't have them here ('' it can). Machines with the windows device set never do
// (no stable 3D driver for that system), nor arm64 machines for now; otherwise host.no3d says ("arch", "gpu", "modules",
// "node")
export function no3dReason(host, windows, arch = 'amd64') {
  if (windows) return 'windows';
  if (arch !== 'amd64') return 'arch';
  return host ? host.no3d || '' : 'gpu';
}
// what a profile's graphics give a machine here: 3D only when it can have them
export function gfxFor(gfx, host, windows, arch = 'amd64') { return gfx === '3d' && !no3dReason(host, windows, arch) ? '3d' : '2d'; }
// a running machine with 3D (VM_INFO's VMS_RENDER_NODE): it can't be suspended or keep snapshots with its memory
export const VMS_RENDER_NODE = 512;
export function runs3d(st) { return !!st && ((st.sandbox | 0) & VMS_RENDER_NODE) !== 0; }

// the packages Machines still needs for the architectures in use (lord.vm.host().missing), as one list
// what only some machines need: the TPM (swtpm) for Windows 11 (4.3); asked for when a Windows machine is made, or on
// the requirements page, never as a banner for everyone
const OPTIONAL = ['swtpm'];
export function missingFor(host, arches, { windows = false } = {}) {
  const out = [];
  for (const a of arches) for (const p of (host?.missing?.[a] || '').split(' ').filter(Boolean)) if (!out.includes(p) && (windows || !OPTIONAL.includes(p))) out.push(p);
  return out;
}

// ---- the picture in the window ----

// The picture w × h shown in a box (x, y, bw, bh), keeping its shape: the rectangle it fills and its scale.
export function letterbox(box, w, h) {
  const [x, y, bw, bh] = box;
  if (!(w > 0 && h > 0 && bw > 0 && bh > 0)) return { x, y, w: 0, h: 0, k: 0 };
  const k = Math.min(bw / w, bh / h);
  const dw = w * k, dh = h * k;
  return { x: x + (bw - dw) / 2, y: y + (bh - dh) / 2, w: dw, h: dh, k };
}

// a pointer position in the window → picture coordinates, clamped; null on an empty picture
export function toPicture(px, py, box, w, h) {
  const lb = letterbox(box, w, h);
  if (!lb.k) return null;
  const cx = Math.floor((px - lb.x) / lb.k), cy = Math.floor((py - lb.y) / lb.k);
  return [Math.max(0, Math.min(w - 1, cx)), Math.max(0, Math.min(h - 1, cy))];
}

// the size the guest is asked for when the window changes: the box in whole pixels, even, at least 320 × 200 and at
// most 8192; null when it hasn't really changed (a pixel or two of rounding)
export function guestSize(box, last) {
  const w = Math.max(320, Math.min(8192, Math.floor(box[2]) & ~1)), h = Math.max(200, Math.min(8192, Math.floor(box[3]) & ~1));
  if (last && Math.abs(last[0] - w) < 4 && Math.abs(last[1] - h) < 4) return null;
  return [w, h];
}

// the display's refresh rate for VD_RATE: lord.display.list() modes (Hz, or mHz from some drivers), 60 by default
export function refreshRate(displays) {
  let best = 0;
  for (const d of Array.isArray(displays) ? displays : []) {
    let r = Number(d?.mode?.refresh) || 0;
    if (r > 1000) r = Math.round(r / 1000);
    if (r > best) best = r;
  }
  return best >= 1 && best <= 240 ? best : 60;
}

// wheel: the engine's delta → guest steps (+ up), at most 32 a message. A touchpad (precise) sends pixels, added up
// until they make a step of 40 px; a mouse wheel's every notch is one step. → [steps, what's left over]
export function wheelSteps(acc, deltaY, precise) {
  if (!precise) return [deltaY ? -Math.sign(deltaY) * Math.max(1, Math.min(32, Math.round(Math.abs(deltaY) / 120) || 1)) : 0, 0];
  const total = acc - deltaY;
  const n = Math.trunc(total / 40);
  return [Math.max(-32, Math.min(32, n)), total - n * 40];
}

// Ctrl+Alt+Delete as evdev codes: press in order, release in reverse
export const CTRL_ALT_DEL = [29, 56, 111];

// the window's title: "<name> — <state>"
export const windowTitle = (name, stateText) => (stateText ? `${name} — ${stateText}` : name);

// a folder name for display, from the machine's dir (".../Machines/Debian.lordvm" → "Debian")
export const folderOf = (dir) => (dir || '').split('/').pop().replace(/\.lordvm$/, '');

// ---- the Control Center (Main) ----

// Systems recognized from an ISO's volume label or a file's name: an id for the card's mark, the name shown. Our own
// marks (ui.jsx OsMark), never a project's logo.
const SYSTEMS = [
  ['lordos', /lordos/i, 'LordOS'], ['debian', /debian|\bd-i\b/i, 'Debian'], ['ubuntu', /ubuntu|kubuntu|xubuntu|lubuntu/i, 'Ubuntu'],
  ['fedora', /fedora/i, 'Fedora'], ['mint', /\bmint\b|linuxmint/i, 'Mint'], ['arch', /\barch(linux)?\b|archlinux/i, 'Arch'],
  ['alpine', /alpine/i, 'Alpine'], ['opensuse', /suse/i, 'openSUSE'], ['kali', /kali/i, 'Kali'], ['rocky', /rocky/i, 'Rocky'],
  ['alma', /alma/i, 'Alma'], ['centos', /centos/i, 'CentOS'], ['freebsd', /freebsd/i, 'FreeBSD'], ['raspios', /raspios|raspberry/i, 'Pi OS'],
  // Microsoft's own volume labels (CCCOMA_X64FRE_…, CENA_X64FREE_…, CPBA_A64FRE_…) and its file names (Win11_…)
  ['windows', /\bWin1[01]_|\b(CCCOMA|CENA|CCSA|CPBA)_(X64|A64)FREE?_/i, 'Windows'],
];
export function systemOf(...texts) {
  const s = texts.filter(Boolean).join(' ');
  for (const [id, re, name] of SYSTEMS) if (re.test(s)) return { id, name };
  return { id: 'other', name: '' };
}
// a version in a file name or label: "debian-13.1.0-amd64" → "13.1"
export function versionOf(text) {
  const m = String(text || '').match(/(?:^|[^0-9.])(\d{1,2}(?:\.\d{1,2})?)(?:\.\d+)?(?=[^0-9.]|$)/);
  return m ? m[1] : '';
}
// the architecture a file is built for, from its name (the assistant picks it; '' when the name doesn't say)
export function archOf(name) {
  const s = String(name || '').toLowerCase();
  if (/aarch64|arm64/.test(s)) return 'arm64';
  if (/amd64|x86[_-]64|x64\b/.test(s)) return 'amd64';
  return '';
}
// a machine name from a system and version, unique among names
export function suggestName(sys, version, names = []) {
  const base = [sys?.name || 'Machine', version].filter(Boolean).join(' ');
  if (!names.some((n) => n.toLowerCase() === base.toLowerCase())) return base;
  for (let i = 2; i < 100; i++) { const c = `${base} ${i}`; if (!names.some((n) => n.toLowerCase() === c.toLowerCase())) return c; }
  return base;
}

// Profiles (5.6): only values shown in Configure, scaled to this computer (all but one core at most, memMaxMb), and
// nothing shared by any of them. Design wants 3D: a Linux machine gets it where this computer can give it (gfxFor),
// others run 2D and say so.
export const PROFILES = ['productivity', 'development', 'testing', 'design', 'light'];
const PROFILE = {
  //            cores (share of host), memory (MB cap, share of RAM), disk GB, network, graphics
  productivity: { cpu: 1 / 2, mem: [8192, 1 / 3], disk: 32, network: 'shared', gfx: '2d' },
  development: { cpu: 3 / 4, mem: [16384, 1 / 2], disk: 64, network: 'shared', gfx: '2d' },
  testing: { cpu: 1 / 2, mem: [4096, 1], disk: 32, network: 'isolated', gfx: '2d' },
  design: { cpu: 3 / 4, mem: [16384, 1 / 2], disk: 64, network: 'shared', gfx: '3d' },
  light: { cpu: 0, mem: [3072, 1 / 2], disk: 24, network: 'shared', gfx: '2d' },
};
export function profileSize(p, host) {
  const d = PROFILE[p] || PROFILE.productivity;
  const rec = Math.max(1, host?.vcpusRecommended || 1), cpus = host?.cpus || rec + 1;
  const vcpus = p === 'light' ? Math.min(2, rec) : Math.max(1, Math.min(rec, Math.floor(cpus * d.cpu) || 1));
  const ram = host?.ramMb || 4096, memMax = host?.memMaxMb || 2048;
  const want = Math.min(d.mem[0], Math.floor(ram * d.mem[1]));
  const memoryMb = Math.max(LIMITS.memMin, Math.floor(Math.min(want, memMax) / 256) * 256 || LIMITS.memMin);
  return { vcpus, memoryMb, diskGb: d.disk, network: d.network, gfx: d.gfx };
}
// a profile's values a machine has now ('' when it matches none: changed by hand since)
export function profileOf(m, host, saved) {
  if (saved && PROFILES.includes(saved)) {
    const s = profileSize(saved, host);
    if (s.vcpus === m.vcpus && s.memoryMb === m.memoryMb && s.network === m.network) return saved;
  }
  return '';
}

// Card numbers from two samples of lord.vm.stats(dir) (cgroup counters): CPU % of the machine's vCPUs, memory in
// bytes, disk activity since the last sample. null when the machine isn't running or there's only one sample.
export function usage(prev, cur, vcpus) {
  if (!cur || !(cur.at > 0) || !(cur.cpuUsec >= 0)) return null;
  const out = { memBytes: cur.memBytes >= 0 ? cur.memBytes : null, cpu: null, disk: false };
  if (prev && prev.at > 0 && cur.at > prev.at && prev.cpuUsec >= 0) {
    const ms = cur.at - prev.at;
    out.cpu = Math.max(0, Math.min(100, ((cur.cpuUsec - prev.cpuUsec) / (ms * 1000 * Math.max(1, vcpus || 1))) * 100));
    out.disk = cur.ioBytes > prev.ioBytes;
  }
  return out;
}

// The three bars at the top: machines running, the memory they use against this computer's, and free disk space
// where machines live. Each { value: [a, b], frac } or null where there's no real number.
export function summaryOf(list, usages, host) {
  const n = (list || []).length;
  const running = (list || []).filter((m) => stateView(m.state).active);
  const mem = running.reduce((a, m) => a + (usages?.[m.dir]?.memBytes || 0), 0);
  const ram = (host?.ramMb || 0) * 1048576;
  const free = host?.libFreeBytes, total = host?.libTotalBytes;
  return {
    running: { a: running.length, b: n, frac: n ? running.length / n : 0 },
    memory: ram ? { a: mem, b: ram, frac: Math.min(1, mem / ram) } : null,
    free: free >= 0 && total > 0 ? { a: free, b: total, frac: Math.min(1, free / total) } : null,
  };
}

// What a card says it's doing: lord-vm's state, a saved session, or running with no window of this app (Keep Running,
// Start Without Window). `windowed`: a window of this app shows it.
export function doingOf(m, windowed = true) {
  const v = stateView(m?.state, m?.reason);
  if (m?.suspended && !v.active) return 'suspended';
  if (v.key === 'stopped' && !v.why) return 'shutDown';
  if (v.key === 'running' && !windowed) return 'noWindow';
  return v.key;
}

// The library's view (settings.machinesView, 0.8.8.7): pictures or a list, the order, which machines show
export const LIB_VIEWS = ['grid', 'list'];
export const LIB_SORTS = ['name', 'state', 'last', 'disk'];
export const LIB_SHOWS = ['all', 'running', 'off'];
export function libView(v) {
  const o = v && typeof v === 'object' ? v : {};
  return {
    view: LIB_VIEWS.includes(o.view) ? o.view : 'grid',
    sort: LIB_SORTS.includes(o.sort) ? o.sort : 'name',
    show: LIB_SHOWS.includes(o.show) ? o.show : 'all',
  };
}
// the machines a search and the Show filter leave: the query matches the name, the system or the architecture
export function filterMachines(list, { query, show } = {}) {
  const q = (query || '').trim().toLowerCase();
  return (list || []).filter((m) => {
    const on = stateView(m.state, m.reason).active;
    if (show === 'running' && !on) return false;
    if (show === 'off' && on) return false;
    if (!q) return true;
    const sys = m.meta?.osName || systemOf(m.name).name || '';
    return [m.name, sys, m.arch].some((s) => (s || '').toLowerCase().includes(q));
  });
}
// in order: by name, running first, last used first (the card picture is written while it runs), or the most disk first;
// ties by name
export function sortMachines(list, sort) {
  const byName = (a, b) => compareNames(a.name || '', b.name || '');
  const rank = (m) => { const v = stateView(m.state, m.reason); return v.key === 'running' ? 0 : v.active ? 1 : m.suspended ? 2 : 3; };
  const key = {
    state: (a, b) => rank(a) - rank(b),
    last: (a, b) => (b.thumbTime || 0) - (a.thumbTime || 0),
    disk: (a, b) => (b.diskBytes || 0) - (a.diskBytes || 0),
  }[sort];
  return [...(list || [])].sort((a, b) => (key && key(a, b)) || byName(a, b));
}

// What closing a machine's window does (app.json onClose): ask (the default), suspend, shutdown or keep (Keep Running)
export const CLOSE_CHOICES = ['ask', 'suspend', 'shutdown', 'keep'];
export const closeChoice = (meta) => (CLOSE_CHOICES.includes(meta?.onClose) ? meta.onClose : 'ask');

// the card picture's path, versioned by its time so the engine loads it again when it changes ('' when none)
export const thumbSrc = (dir, mtime) => (dir && mtime ? `${dir}/thumb.png#${Math.floor(mtime)}` : '');

// ---- the guest agent (6.17) ----

// lord-vm's capability bits (vmproto.h VAC_*) → the popover's list: what works now, and what doesn't and why
// transfer: the guest has the transfer folder (6.7.1) mounted; transferSetup: a Windows guest that gets it once WinFsp is
// installed (Set Up the Transfer Folder)
export const AGENT_CAPS = { shutdown: 1, time: 2, exec: 4, files: 8, journal: 16, clipboard: 32, awake: 64, transfer: 128, transferSetup: 256 };
export function agentLines(agent, cfg) {
  const caps = agent?.caps || 0, on = agent?.state === 'connected';
  const clip = cfg?.clipboard || 'off';
  return [
    ['shell', 'works'],                                       // the serial console needs no agent
    ['logs', on && caps & AGENT_CAPS.journal ? 'works' : 'needsAgent'],
    ['shutdown', on && caps & AGENT_CAPS.shutdown ? 'works' : 'acpi'],
    ['time', on && caps & AGENT_CAPS.time ? 'works' : 'needsAgent'],
    ['awake', on && caps & AGENT_CAPS.awake ? 'works' : 'needsAgent'],
    ['clipboard', clip === 'off' ? 'off' : caps & AGENT_CAPS.clipboard ? 'works' : 'needsHelper'],
    ['resolution', 'works'],
    ['folders', cfg?.shares?.length || (cfg?.transfer && caps & AGENT_CAPS.transfer) ? 'works' : 'none'], ['apps', 'later'],
  ];
}
// the agent's light: gray (off or not installed), amber (waiting for it), green (connected)
export const agentTone = (a, configured) => (!configured ? 'off' : a?.state === 'connected' ? 'run' : 'pause');

// ---- files dropped on the window: copied into the guest's Downloads by its agent (lord.vm.sendFile) ----
// Why a drop can't be taken now (the hint says it before the files are let go of), or null: 'off' (not running),
// 'paused', 'agentOff' (Configure), 'agentWaiting' (not connected yet), 'agentCant' (it can't write files or run the
// script that puts them in place). Files only go in: nothing is dragged out of a guest without a helper inside it.
export function dropBlocker(st, agentOn) {
  if (!st || (st.state !== 'running' && st.state !== 'paused')) return 'off';
  if (st.state === 'paused') return 'paused';
  if (!agentOn) return 'agentOff';
  const g = st.agent;
  if (!g || g.state !== 'connected') return 'agentWaiting';
  return g.caps & AGENT_CAPS.files && g.caps & AGENT_CAPS.exec ? null : 'agentCant';
}
// the line under the picture while files go in (lord.vm.files): the first one still copying, the whole percentage by
// size, how many wait after it, whether the first goes through the transfer folder; null when none is copying
export function copyLine(list) {
  const going = (list || []).filter((f) => f && f.state === 'copying').sort((a, b) => a.id - b.id);
  if (!going.length) return null;
  const total = going.reduce((a, f) => a + Math.max(0, f.size || 0), 0);
  const pct = total > 0 ? Math.floor(going.reduce((a, f) => a + Math.max(0, f.size || 0) * Math.min(100, Math.max(0, f.pct || 0)), 0) / total)
    : Math.floor(going.reduce((a, f) => a + Math.min(100, Math.max(0, f.pct || 0)), 0) / going.length);
  return { name: going[0].name, pct: Math.min(99, pct), waiting: going.length - 1, fast: going[0].via === 'transfer' };
}

// ---- the transfer folder (6.7.1): <library>/Transfer/<name>, the guest's LordOS folder or drive ----
// What the machine's window says of it: 'off' (Configure), 'ready' (the guest has it mounted), 'setup' (a Windows guest
// that needs WinFsp: Set Up the Transfer Folder), 'settingUp' (that is running), 'waiting' (the machine isn't running,
// its agent hasn't answered, or the guest hasn't mounted it yet). cfg: lord.vm.list()'s row; st: the session's poll
export function transferState(cfg, st) {
  if (!cfg?.transfer) return 'off';
  const g = st?.agent;
  if (!st || (st.state !== 'running' && st.state !== 'paused') || !g || g.state !== 'connected') return 'waiting';
  if (g.transfer === 1) return 'settingUp';
  if (g.caps & AGENT_CAPS.transfer) return 'ready';
  if (g.caps & AGENT_CAPS.transferSetup) return 'setup';
  return 'waiting';
}
// the note under the drop hint: how the files will go ('fast' through the transfer folder, 'agent' because it isn't
// ready yet, 'off' when the machine has none)
export function dropWay(cfg, st) {
  const x = transferState(cfg, st);
  return x === 'ready' ? 'fast' : x === 'off' ? 'off' : 'agent';
}
// WinFsp's installer for a Windows guest's transfer folder: { id, name, sha, size } of the catalog's pinned file, or null
export function winfspFor(cat, arch) {
  const s = (cat?.systems || []).find((x) => x?.id === 'winfsp');
  const f = s?.arch?.[arch]?.pinned?.[0];
  return f ? { id: 'winfsp', name: f.name, sha: f.sha256, size: f.bytes } : null;
}
// Set Up the Transfer Folder runs by itself once, after an unattended Windows install (app.json transferSetup), when the
// guest says it needs WinFsp
export const autoTransferSetup = (meta, state) => !!meta?.transferSetup && state === 'setup';
// the folder of a path in the guest, Linux's or Windows' ('' for a bare name)
export function guestFolder(p) {
  const s = String(p || '');
  const i = Math.max(s.lastIndexOf('/'), s.lastIndexOf('\\'));
  return i > 0 ? s.slice(0, i) : i === 0 ? s.slice(0, 1) : '';
}
// Follow the window, not followed: the guest still shows another size 4 s after it was asked for one (a display
// driver that takes sizes only when the system starts). asked = [w, h] (null: nothing asked), got = the picture's size
export function followStuck(asked, got, askedAt, now) {
  if (!asked || !got || !(got[0] > 0)) return false;
  if (Math.abs(asked[0] - got[0]) <= 8 && Math.abs(asked[1] - got[1]) <= 8) return false;
  return now - askedAt > 4000;
}
// a shared folder's mount tag from its name (6.7): letters, digits, '-' and '_', at most 32, not one already used
export function shareTag(name, used = []) {
  const base = String(name || '').normalize('NFD').replace(/[\u0300-\u036f]/g, '').replace(/[^A-Za-z0-9_-]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 28) || 'share';
  const taken = new Set(used.map((x) => x.toLowerCase()));
  if (!taken.has(base.toLowerCase())) return base;
  for (let i = 2; i < 100; i++) { const t = `${base}-${i}`; if (!taken.has(t.toLowerCase())) return t; }
  return base;
}
// the command that mounts it in a Linux guest
export const mountCommand = (tag) => `sudo mkdir -p /mnt/${tag} && sudo mount -t virtiofs ${tag} /mnt/${tag}`;
// which way the clipboard may go
export const clipTo = (c) => c === 'both' || c === 'to-guest';
export const clipFrom = (c) => c === 'both' || c === 'from-guest';

// ---- sound and microphone (6.9) ----
// A machine's choices (machine.json schema 6; an older machine reads as sound on at full volume, microphone off)
export function soundOf(m) {
  const v = Number(m?.volume);
  return { sound: m?.sound !== false, volume: Number.isFinite(v) ? Math.max(0, Math.min(100, Math.round(v))) : 100, microphone: m?.microphone === true };
}
// heard: the sound is on and not at zero
export const audible = (s) => !!s && s.sound && s.volume > 0;
// Configure's one-line summary: which keys of machines.cfg.sum.* and their values
export function soundSummary(m) {
  const s = soundOf(m);
  return { key: audible(s) ? 'machines.cfg.sum.soundOn' : 'machines.cfg.sum.soundOff', volume: s.volume,
    mic: s.microphone ? 'machines.cfg.sum.micOn' : 'machines.cfg.sum.micOff' };
}
// the toolbar's speaker: a click mutes, or turns the sound back on (at 50 when it was at zero)
export function muteToggle(s) {
  const c = soundOf(s);
  if (audible(c)) return { sound: false };
  return c.volume > 0 ? { sound: true } : { sound: true, volume: 50 };
}
// lord.vm.poll's sound.mic: 'live' shows the microphone in use (the bar's light and the menu bar's indicator); 'wait'
// asks LordOS's microphone permission once per window while the guest records
export const micLive = (snd) => snd?.mic === 'live';
export const micAsk = (snd, asked) => snd?.mic === 'wait' && !asked;

// ---- snapshots (6.2) ----

// a new snapshot's suggested name: "Snapshot 3", the first number not in use
export function snapName(list, base) {
  const names = new Set((list || []).map((s) => s.name.toLowerCase()));
  for (let i = (list || []).length + 1; i < 1000; i++) { const n = `${base} ${i}`; if (!names.has(n.toLowerCase())) return n; }
  return base;
}
// does the snapshot hold the machine's memory (and how much)
export const snapSize = (s) => (s?.memory && s.vmstate > 0 ? s.vmstate : 0);

// ---- clones (6.3) ----

// How lord-vm will copy the disk, from lord.vm.cloneInfo: 'reflink' (shared until changed), 'linked' (ext4: a frozen
// base and an overlay for each), 'copy' (a full copy), or 'blocked' (a Clone of a machine with a saved session, which
// holds its identity). fromSnapshot: a Clone of one snapshot is always converted.
// a Clone's name inside the guest (6.3): one DNS label, as lord-vm's vm_hostname_valid and vm_hostname_from_name
export const hostnameValid = (h) => typeof h === 'string' && h.length >= 1 && h.length <= 63 && /^[a-z0-9]([a-z0-9-]*[a-z0-9])?$/.test(h);
export function hostnameFrom(name) {
  const base = String(name || '').toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 63).replace(/-+$/, '');
  return base || 'machine';
}
export const cloneHostname = (name) => `${hostnameFrom(name).slice(0, 57).replace(/-+$/, '')}-clone`;

export function cloneMethod(info, kind, fromSnapshot = false) {
  if (kind === 'clone' && info?.suspended) return 'blocked';
  if (fromSnapshot) return 'copy';
  if (info?.reflink) return 'reflink';
  if (kind === 'clone' && !info?.snapshots && !info?.suspended && !info?.linked) return 'linked';
  return kind === 'duplicate' ? 'duplicate' : 'copy';
}

// ---- the Storage window (6.16) ----

// lord.vm.storage() → the list's rows: each machine, its clones under it (app.json cloneOf), then the shared bases.
// { key, name, level, size, frees, machine } or { key, base: true, path, size, users, trash, names }
export function storageRows(st) {
  const ms = [...(st?.machines || [])].map((m) => ({ ...m, savedBytes: m.state }));
  const byId = new Map(ms.map((m) => [m.id, m]));
  const kids = new Map();
  const tops = [];
  for (const m of ms) {
    const parent = m.meta?.cloneOf && byId.has(m.meta.cloneOf) && m.meta.cloneOf !== m.id ? m.meta.cloneOf : null;
    if (parent) { if (!kids.has(parent)) kids.set(parent, []); kids.get(parent).push(m); } else tops.push(m);
  }
  const rows = [];
  const bases = st?.bases || [];
  const add = (m, level, seen) => {
    if (seen.has(m.id) || level > 6) return;
    seen.add(m.id);
    const b = m.base ? bases.find((x) => x.path === m.base) : null;
    rows.push({ key: m.dir, name: m.name, level, size: m.total, frees: m.frees, machine: { ...m, savedBytes: m.state }, baseBytes: b?.bytes || 0, baseUsers: b ? b.users + b.trash : 0 });
    for (const k of (kids.get(m.id) || []).sort((a, z) => compareNames(a.name, z.name))) add(k, level + 1, seen);
  };
  const seen = new Set();
  for (const m of tops.sort((a, z) => compareNames(a.name, z.name))) add(m, 0, seen);
  for (const b of bases) {
    const names = ms.filter((m) => m.base === b.path).map((m) => m.name);
    rows.push({ key: b.path, base: true, path: b.path, size: b.bytes, users: b.users, trash: b.trash, names, level: 0 });
  }
  return rows;
}

// the volume's bar: machines (their folders and shared bases), disc images in use, the systems downloaded by the
// assistant (images: catalog.js downloadedImages; one in use is counted there, not twice), machines in the Trash, other
// files. The downloads only show when there are some.
export function storageBar(st, images = []) {
  const total = st?.total || 0;
  if (!(total > 0)) return [];
  const machines = (st.machines || []).reduce((a, m) => a + (m.total || 0), 0) + (st.bases || []).reduce((a, b) => a + (b.bytes || 0), 0);
  const used = Math.max(0, total - (st.free || 0));
  const dl = images.reduce((a, x) => a + (x.size || 0), 0);
  const inUse = images.reduce((a, x) => a + (x.users?.length ? x.size || 0 : 0), 0);
  const parts = [['machines', machines], ['isos', Math.max(0, (st.isos || 0) - inUse)], ...(images.length ? [['downloads', dl]] : []), ['trash', st.trash || 0]];
  const other = Math.max(0, used - parts.reduce((a, x) => a + x[1], 0));
  // an empty slice isn't shown (no "Disc images 0 MB")
  return [...parts, ['other', other]].filter(([, bytes]) => bytes > 0).map(([key, bytes]) => ({ key, bytes, frac: Math.min(1, bytes / total) }));
}

// ---- the resolution menu (5.3) ----

// sizes a guest can be asked for, those that fit this display (the window's picture never needs to be larger), and the
// guest's current one when it isn't in the list
const SIZES = [[1024, 768], [1280, 720], [1280, 800], [1366, 768], [1440, 900], [1600, 900], [1680, 1050], [1920, 1080], [1920, 1200], [2560, 1440], [3840, 2160]];
export function resolutions(screenW, screenH, cur) {
  const fit = SIZES.filter(([w, h]) => w <= (screenW || 1920) && h <= (screenH || 1080));
  const list = fit.length ? fit : [SIZES[0]];
  if (cur && cur[0] > 0 && !list.some(([w, h]) => w === cur[0] && h === cur[1])) list.push([cur[0], cur[1]]);
  return list.sort((a, b) => a[0] * a[1] - b[0] * b[1]);
}

// ---- importing a machine of another virtualization app (6.22) ----
// The plan is lord-vm --import-plan's JSON (the last line that is an object); null when there's none yet.
export function parsePlan(out) {
  const lines = String(out || '').split('\n').filter((l) => l.startsWith('{'));
  for (let i = lines.length - 1; i >= 0; i--) { try { const p = JSON.parse(lines[i]); if (p && 'blockers' in p) return p; } catch (e) { /* not this one */ } }
  return null;
}
// an import's progress: the last {"progress":…} line, and the result once it's there
export function importProgress(out) {
  let pct = 0, disk = 0, disks = 0, done = null;
  for (const l of String(out || '').split('\n')) {
    if (!l.startsWith('{')) continue;
    try {
      const o = JSON.parse(l);
      if (typeof o.progress === 'number') { pct = o.progress; disk = o.disk; disks = o.disks; }
      if (o.imported) done = o;
    } catch (e) { /* a cut line */ }
  }
  return { pct: Math.max(0, Math.min(100, pct)), disk, disks, done };
}
// a name the library doesn't have yet: "Name", "Name 2", …
export function uniqueName(name, names = []) {
  const base = String(name || '').trim().slice(0, 60) || 'Imported machine';
  if (!names.includes(base)) return base;
  for (let k = 2; k < 100; k++) if (!names.includes(`${base} ${k}`)) return `${base} ${k}`;
  return base;
}
// what the plan sheet lists as not coming along, in its order; each is a machines.mig.note.<code> text
export const PLAN_NOTE_ORDER = ['snapshots-flattened', 'tpm', 'secure-boot-off', 'boot-entries', 'cdrom', 'shared-folders', 'usb', 'too-many-nics',
  'new-mac', 'new-uuid', 'serial-dropped', 'vcpus-cut', 'memory-cut'];
export function planNotes(plan) {
  const codes = (plan?.notes || []).map((n) => n.code);
  return PLAN_NOTE_ORDER.filter((c) => codes.includes(c));
}
// what stops the import: { code, text } in the order lord-vm found them
export const planBlockers = (plan) => (plan?.blockers || []).map((b) => ({ code: b.code, text: b.text || '' }));

// ---- the network (docs/VIRTUALIZATION.md 6.5; machine.json schema 6) ----
// The four modes, in the order Configure shows them. Shared and host-only go through passt (the forwarded ports apply);
// bridged puts the guest on one of this computer's wired cards (a macvtap the admin helper makes); isolated has none.
export const NET_MODES = ['shared', 'bridged', 'hostonly', 'isolated'];
export const PORTS_MAX = 32;
export const PORT_HOST_MIN = 1024;
export const usesPorts = (network) => network === 'shared' || network === 'hostonly';
// a rule as lord.vm.set takes it
export const portRule = (r) => ({ proto: r?.proto === 'udp' ? 'udp' : 'tcp', host: Number(r?.host) || 0, guest: Number(r?.guest) || 0,
  open: r?.open === 'lan' ? 'lan' : 'this-computer' });
// The rules' checks, the same as machine.c's vm_ports_check: [{ i, code }] for each wrong rule ('host', 'guest',
// 'duplicate', 'lanHostOnly'), and 'tooMany' at index -1. Empty: fine.
export function portProblems(ports, network) {
  const out = [];
  const list = ports || [];
  if (list.length > PORTS_MAX) out.push({ i: -1, code: 'tooMany' });
  const seen = new Set();
  list.forEach((r0, i) => {
    const r = portRule(r0);
    if (!Number.isInteger(r.host) || r.host < PORT_HOST_MIN || r.host > 65535) out.push({ i, code: 'host' });
    else if (!Number.isInteger(r.guest) || r.guest < 1 || r.guest > 65535) out.push({ i, code: 'guest' });
    else if (seen.has(`${r.proto}:${r.host}`)) out.push({ i, code: 'duplicate' });
    else if (r.open === 'lan' && network === 'hostonly') out.push({ i, code: 'lanHostOnly' });
    seen.add(`${r.proto}:${r.host}`);
  });
  return out;
}
// a new rule: SSH to the guest on the first free port from 2222 (tcp)
export function newPortRule(ports) {
  const used = new Set((ports || []).filter((r) => r.proto !== 'udp').map((r) => Number(r.host)));
  let host = 2222;
  while (used.has(host) && host < 65535) host++;
  return { proto: 'tcp', host, guest: 22, open: 'this-computer' };
}
// what changing the mode does to the rules: a host-only machine opens ports to this computer only
export const portsForMode = (ports, network) => (network === 'hostonly' ? (ports || []).map((r) => ({ ...portRule(r), open: 'this-computer' })) : (ports || []).map(portRule));
// digits typed into a port's field, as a number (0 while empty)
export const portInput = (text) => Number(String(text || '').replace(/\D/g, '').slice(0, 5)) || 0;
// the wired card a bridged machine joins by default: the one it had, else the first wired card that is up, else the first
export function defaultCard(cards, current) {
  const wired = (cards || []).filter((c) => c.kind === 'wired');
  if (current && wired.some((c) => c.name === current)) return current;
  return (wired.find((c) => c.up) || wired[0])?.name || '';
}
// vm-net-bridge's argument (vm_admin.h): "<id> <card> <mac>"
export const bridgeArg = (m) => (m?.id && m?.bridge && m?.mac ? `${m.id} ${m.bridge} ${m.mac}` : '');
// the summary line of Configure's Network section
export function netSummary(m) {
  const n = NET_MODES.includes(m?.network) ? m.network : 'shared';
  return { mode: n, card: n === 'bridged' ? m.bridge || '' : '', ports: usesPorts(n) ? (m.ports || []).length : 0,
    lan: usesPorts(n) && n !== 'hostonly' && (m.ports || []).some((r) => r.open === 'lan') };
}
