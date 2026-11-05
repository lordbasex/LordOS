// model.js — Machines' pure logic (docs/VIRTUALIZATION.md, "Phase 1 implementation", The app): what a machine's
// state means on screen, what the New machine sheet accepts, and where a point of the window falls on the guest's
// picture. No engine calls here, so server/tests/vm_model_test.mjs runs it under node.

// lord-vm's states and reasons (server/src/vm/vmproto.h, VS_* and VR_*) → what the app shows. `tone`: 'run' (green),
// 'pause' (amber), 'off' (gray), 'busy' (accent), 'bad' (red). The text key is under `machines.state.*`, and the
// reason, when it's worth saying, under `machines.why.*`.
const BAD_REASONS = ['qemu-failed', 'qemu-signal', 'out-of-memory', 'start-failed', 'guest-panic', 'supervisor-lost', 'forced'];
export function stateView(state, reason) {
  const r = reason || 'none';
  switch (state) {
    case 'running': return { key: 'running', tone: 'run', active: true, why: null };
    case 'paused': return { key: 'paused', tone: 'pause', active: true, why: null };
    case 'starting': return { key: 'starting', tone: 'busy', active: true, why: null };
    case 'stopping': return { key: 'stopping', tone: 'busy', active: true, why: null };
    case 'crashed': return { key: 'crashed', tone: 'bad', active: false, why: r === 'none' ? 'qemu-failed' : r };
    default:
      // a machine whose supervisor died is shown as off, with the reason: lord-vm recovers it at the next start
      if (r === 'supervisor-lost') return { key: 'recovered', tone: 'bad', active: false, why: r };
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
  const r = { name: d.name.trim(), arch: d.arch, vcpus: d.vcpus, memoryMb: d.memoryMb, diskGb: d.diskGb };
  if (d.source === 'iso') r.iso = d.path; else r.import = d.path;
  if (d.source !== 'iso' && d.rootPassword) r.rootPassword = d.rootPassword;
  return r;
}

// the packages Machines still needs for the architectures in use (lord.vm.host().missing), as one list
export function missingFor(host, arches) {
  const out = [];
  for (const a of arches) for (const p of (host?.missing?.[a] || '').split(' ').filter(Boolean)) if (!out.includes(p)) out.push(p);
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

// Profiles in the assistant: what a machine gets, kept within this computer (all but one core, memMaxMb).
export const PROFILES = ['light', 'standard', 'development'];
export function profileSize(p, host) {
  const want = { light: [1, 1024, 16], standard: [2, 2048, 32], development: [4, 4096, 64] }[p] || [2, 2048, 32];
  const cpus = Math.max(1, Math.min(want[0], host?.vcpusRecommended || 1));
  const memMax = host?.memMaxMb || want[1];
  const mem = Math.max(LIMITS.memMin, Math.min(want[1], Math.floor(memMax / 256) * 256 || memMax));
  return { vcpus: cpus, memoryMb: mem, diskGb: want[2] };
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

// What a card says it's doing (no guest agent yet: lord-vm's state only)
export function doingOf(m) {
  const v = stateView(m?.state, m?.reason);
  if (v.key === 'stopped' && !v.why) return 'shutDown';
  return v.key;
}

// the card picture's path, versioned by its time so the engine loads it again when it changes ('' when none)
export const thumbSrc = (dir, mtime) => (dir && mtime ? `${dir}/thumb.png#${Math.floor(mtime)}` : '');
