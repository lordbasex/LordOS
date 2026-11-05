// model.js — Machines' pure logic (docs/VIRTUALIZATION.md, "Phase 1 implementation", The app): what a machine's
// state means on screen, what the New machine sheet accepts, and where a point of the window falls on the guest's
// picture. No engine calls here, so server/tests/vm_model_test.mjs runs it under node.

// lord-vm's states and reasons (server/src/vm/vmproto.h, VS_* and VR_*) → what the app shows. `tone`: 'run' (green),
// 'pause' (amber), 'off' (gray), 'busy' (accent), 'bad' (red). The text key is under `machines.state.*`, and the
// reason, when it's worth saying, under `machines.why.*`.
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

// Profiles (5.6): only values shown in Configure, scaled to this computer (all but one core at most, memMaxMb), and
// nothing shared by any of them. Design wants 3D, which comes later: it runs 2D and says so.
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

// What closing a machine's window does (app.json onClose): ask (the default), suspend, shutdown or keep (Keep Running)
export const CLOSE_CHOICES = ['ask', 'suspend', 'shutdown', 'keep'];
export const closeChoice = (meta) => (CLOSE_CHOICES.includes(meta?.onClose) ? meta.onClose : 'ask');

// the card picture's path, versioned by its time so the engine loads it again when it changes ('' when none)
export const thumbSrc = (dir, mtime) => (dir && mtime ? `${dir}/thumb.png#${Math.floor(mtime)}` : '');

// ---- the guest agent (6.17) ----

// lord-vm's capability bits (vmproto.h VAC_*) → the popover's list: what works now, and what doesn't and why
export const AGENT_CAPS = { shutdown: 1, time: 2, exec: 4, files: 8, journal: 16, clipboard: 32, awake: 64 };
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
    ['folders', 'later'], ['apps', 'later'],
  ];
}
// the agent's light: gray (off or not installed), amber (waiting for it), green (connected)
export const agentTone = (a, configured) => (!configured ? 'off' : a?.state === 'connected' ? 'run' : 'pause');
// which way the clipboard may go
export const clipTo = (c) => c === 'both' || c === 'to-guest';
export const clipFrom = (c) => c === 'both' || c === 'from-guest';

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
    for (const k of (kids.get(m.id) || []).sort((a, z) => a.name.localeCompare(z.name))) add(k, level + 1, seen);
  };
  const seen = new Set();
  for (const m of tops.sort((a, z) => a.name.localeCompare(z.name))) add(m, 0, seen);
  for (const b of bases) {
    const names = ms.filter((m) => m.base === b.path).map((m) => m.name);
    rows.push({ key: b.path, base: true, path: b.path, size: b.bytes, users: b.users, trash: b.trash, names, level: 0 });
  }
  return rows;
}

// the volume's bar: machines (their folders and shared bases), disc images in use, machines in the Trash, other files
export function storageBar(st) {
  const total = st?.total || 0;
  if (!(total > 0)) return [];
  const machines = (st.machines || []).reduce((a, m) => a + (m.total || 0), 0) + (st.bases || []).reduce((a, b) => a + (b.bytes || 0), 0);
  const used = Math.max(0, total - (st.free || 0));
  const parts = [['machines', machines], ['isos', st.isos || 0], ['trash', st.trash || 0]];
  const other = Math.max(0, used - parts.reduce((a, x) => a + x[1], 0));
  return [...parts, ['other', other]].map(([key, bytes]) => ({ key, bytes, frac: Math.min(1, bytes / total) }));
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
