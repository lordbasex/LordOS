// model.js — Machines' pure logic (docs/VIRTUALIZATION.md, "Phase 1 implementation", The app): what a machine's
// state means on screen, what the New machine sheet accepts, and where a point of the window falls on the guest's
// picture. No engine calls here, so server/tests/vm_model_test.mjs runs it under node.

// lord-vm's states and reasons (server/src/vm/vmproto.h, VS_* and VR_*) → what the app shows. `tone`: 'run' (green),
// 'pause' (amber), 'off' (gray), 'busy' (accent), 'bad' (red). The text key is under `machines.state.*`, and the
// reason, when it's worth saying, under `machines.why.*`.
import { compareNames } from '../../sdk/collate.js';
const BAD_REASONS = ['qemu-failed', 'qemu-signal', 'qemu-3d-crash', 'out-of-memory', 'start-failed', 'guest-panic', 'supervisor-lost', 'forced', 'resume-failed'];
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

// a stop the machine's Graphics 2D avoids: QEMU's 3D display aborts when a guest resets its virtio-gpu-gl (QEMU issue
// 4918, docs/VIRTUALIZATION.md "Graphics 3D"); the window offers "Use 2D Graphics" (lord.vm.set) before Start Again
export function offers2d(why) { return why === 'qemu-3d-crash'; }

// a run that ended with the guest's own restart, which QEMU couldn't do in place: Machines starts the machine again.
// lord-vm says stopped/'reset' only then: a machine with a graphics card (every reset ends QEMU, docs/MACHINES-GPU.md
// Mode A), or with 3D graphics when the guest's tools said it restarts and QEMU then aborted (QEMU issue 4918,
// vmlog_3d_abort_end). A restart that aborts again right after the start comes back as 'qemu-3d-crash' instead
export function restartsAgain(state, reason) { return state === 'stopped' && reason === 'reset'; }

// the off card's hint: after the 3D abort with no tools in the guest to say why ('qemu-3d-stop': most likely its own
// shutdown or restart), what happened and that a restart needs Start; otherwise the usual one
export function offHint(reason) { return reason === 'qemu-3d-stop' ? 'machines.win.stopped3dHint' : 'machines.win.playHint'; }

// The failure page (6.1): a cause more precise than lord-vm's reason when QEMU's own words name it (text: run.json's
// detail and the log's last lines). 'locked': another process holds the disk's lock (QEMU's image locking: "Failed to
// get "write" lock", "Is another process using the image"); 'disk-missing': a disk or ISO the machine names isn't there.
// Otherwise the reason itself.
export function failCause(why, text) {
  const t = String(text || '');
  if (!why) return why;
  if (/Failed to get "(write|read|shared|consistent read)" lock|Is another process using the image/i.test(t)) return 'locked';
  if (/Could not open '[^'\n]*': No such file or directory/i.test(t)) return 'disk-missing';
  return why;
}
// the fixes that fit a cause, beyond Start Again and Configure: 'less' (start with less memory: lessMb), 'snapshot'
// (go back to one: the machine has snapshots and stopped because of the system inside or the engine), in that order
export function failActions(cause, { memoryMb = 0, snaps = 0 } = {}) {
  const out = [];
  const less = Math.floor((memoryMb * 3) / 4 / 256) * 256;
  if (cause === 'out-of-memory' && less >= LESS_MIN_MB && less < memoryMb) out.push({ k: 'less', mb: less });
  if (snaps > 0 && ['guest-panic', 'qemu-failed', 'qemu-signal', 'out-of-memory', 'disk-missing'].includes(cause)) out.push({ k: 'snapshot' });
  return out;
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

// The resource budget (3.7) is decided by lord-server for every start path (lord.vm.budget, js_vm.c budget_ok: the
// window, Start Without Window, `machines start`, a group's members). When a start doesn't fit, budgetPlan says what
// the window offers instead (b: lord.vm.budget(dir)): lessMb, the memory this machine could start with now (whole
// 256 MB steps, at least 512 MB, less than it has; 0 when none fits), and suspend, up to three running machines whose
// suspend would free enough, the biggest first (a graphics card's or a 3D machine's can't be suspended; pausing frees
// nothing: a paused machine keeps its memory). Suspended, they come back where they were.
export const LESS_MIN_MB = 512;
export function budgetPlan(b, list, m) {
  if (!b || b.ok || !m) return { lessMb: 0, suspend: [] };
  const over = Math.max(0, (b.needMb || 0) - (m.memoryMb || 0));       // QEMU's share and a disposable session's
  const fit = Math.floor(((b.freeMb || 0) - over) / 256) * 256;
  const lessMb = fit >= LESS_MIN_MB && fit < (m.memoryMb || 0) ? fit : 0;
  const short = (b.needMb || 0) - (b.freeMb || 0);
  const suspend = (list || [])
    .filter((x) => x.dir !== m.dir && (x.state === 'running' || x.state === 'paused') && !x.gpu && x.graphics !== '3d' && memLimit(x) >= short)
    .sort((x, y) => memLimit(y) - memLimit(x))
    .slice(0, 3)
    .map((x) => ({ dir: x.dir, name: x.name, mb: memLimit(x) }));
  return { lessMb, suspend };
}
// the library's status line (3.7): memory the running machines hold of what machines may use here, and whether the
// computer is short of memory (PSI some avg10, percent; a warning from PRESSURE_WARN up)
export const PRESSURE_WARN = 10;
export function budgetLine(b) {
  if (!b || !(b.maxMb > 0)) return null;
  return { usedGb: Math.round((b.usedMb || 0) / 102.4) / 10, maxGb: Math.round(b.maxMb / 102.4) / 10, pressure: (b.pressure || 0) >= PRESSURE_WARN };
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
// "Weaker isolation on this kernel" (3.3): what of QEMU's jail this computer can't give. host.landlock is the kernel's
// Landlock ABI (0: none, so QEMU isn't kept to its folder); a running machine's VMS_* bits (st.sandbox, from its
// lord-vm) say what was really applied: Landlock on its files (VMS_LANDLOCK_FS) and its own namespaces (VMS_NAMESPACES).
// → [] or a list of 'landlock', 'namespaces'
export const VMS_LANDLOCK_FS = 1, VMS_NAMESPACES = 1024;
export function isolationGaps(host, st) {
  const out = [];
  const bits = st && st.sandbox > 0 ? st.sandbox | 0 : -1;
  if ((host && host.landlock === 0) || (bits >= 0 && !(bits & VMS_LANDLOCK_FS))) out.push('landlock');
  if (bits >= 0 && !(bits & VMS_NAMESPACES)) out.push('namespaces');
  return out;
}

// the packages Machines still needs for the architectures in use (lord.vm.host().missing), as one list
// what only some machines need: the TPM (swtpm) for Windows 11 (4.3); asked for when a Windows machine is made, or on
// the requirements page, never as a banner for everyone
const OPTIONAL = ['swtpm'];
// The Requirements page's rows (library.jsx): [id, status]; each id has machines.req.row.<id> and machines.req.use.<id>
// (an id "engine.amd64" uses "engine" with { arch })
export function reqRows(host, list, missing) {
  const have = (pk) => !missing.includes(pk);
  return [
    ['kvm', host.kvm ? 'ready' : host.kvmDevice ? 'notAllowed' : 'off'],
    ...list.map((a) => [`engine.${a}`, have(a === 'amd64' ? 'qemu-system-x86' : 'qemu-system-arm') ? 'ready' : 'missing']),
    ...list.map((a) => [`firmware.${a}`, have(a === 'amd64' ? 'ovmf' : 'qemu-efi-aarch64') ? 'ready' : 'missing']),
    ['display', have('qemu-system-modules-opengl') ? 'ready' : 'missing'],
    ['disk', have('qemu-utils') ? 'ready' : 'missing'],
    ['network', have('passt') ? 'ready' : 'missing'],
    ['folders', have('virtiofsd') ? 'ready' : 'missing'],
    ...(list.includes('amd64') ? [['tpm', have('swtpm') ? 'ready' : 'missing']] : []),
  ];
}
export const REQ_IDS = ['kvm', 'engine', 'firmware', 'display', 'disk', 'network', 'folders', 'tpm'];

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

// ---- the Keys panel: held keys, combinations and single keys sent straight to the guest ----
// evdev codes (linux/input-event-codes.h). Sent with lord.vm.key, they never pass through LordOS's shortcuts, so ❖P
// reaches a Windows guest even while LordOS keeps ❖ for itself.
export const KEY = {
  esc: 1, tab: 15, e: 18, r: 19, t: 20, p: 25, ctrl: 29, s: 31, d: 32, l: 38, shift: 42, alt: 56,
  f1: 59, f2: 60, f3: 61, f4: 62, f5: 63, f6: 64, f7: 65, f8: 66, f9: 67, f10: 68, f11: 87, f12: 88,
  prtsc: 99, home: 102, up: 103, pgup: 104, left: 105, right: 106, end: 107, down: 108, pgdn: 109, ins: 110, del: 111, meta: 125,
};
// the modifiers, left and right: a key the person types with one of these held is still "the next key" only once a
// key that isn't one is let go of
const MODIFIERS = new Set([29, 97, 42, 54, 56, 100, 125, 126]);
export const isModifier = (code) => MODIFIERS.has(code);
// the keys the panel can hold down, in order; meta is "Windows" or "Super" by the guest's system (keyName)
export const HELD_KEYS = ['ctrl', 'alt', 'shift', 'meta'];
const F = (n) => `f${n}`;
// [id, keys]: the id names what it does (machines.keys.do.<id>), the keys say how it is labelled (keyName)
const WIN_COMBOS = [
  ['cad', ['ctrl', 'alt', 'del']], ['start', ['meta']], ['project', ['meta', 'p']], ['desktop', ['meta', 'd']],
  ['explorer', ['meta', 'e']], ['run', ['meta', 'r']], ['lock', ['meta', 'l']], ['taskView', ['meta', 'tab']],
  ['switchApp', ['alt', 'tab']], ['closeWin', ['alt', 'f4']], ['taskMgr', ['ctrl', 'shift', 'esc']],
  ['moveLeft', ['meta', 'shift', 'left']], ['moveRight', ['meta', 'shift', 'right']], ['maximize', ['meta', 'up']],
  ['minimize', ['meta', 'down']], ['screenshot', ['prtsc']], ['snip', ['meta', 'shift', 's']],
];
const LINUX_COMBOS = [
  ['cadLinux', ['ctrl', 'alt', 'del']], ['activities', ['meta']], ['switchApp', ['alt', 'tab']], ['closeWin', ['alt', 'f4']],
  ['terminal', ['ctrl', 'alt', 't']], ['screenshot', ['prtsc']],
];
export const SINGLE_KEYS = ['esc', 'tab', 'ins', 'del', 'home', 'end', 'pgup', 'pgdn'];
export const F_KEYS = Array.from({ length: 12 }, (_, i) => F(i + 1));
// Linux's text consoles: Ctrl+Alt+F1 … F7
export const CONSOLE_KEYS = Array.from({ length: 7 }, (_, i) => ['ctrl', 'alt', F(i + 1)]);
const codesOf = (keys) => keys.map((k) => KEY[k]);
// the combinations for a guest: { id, keys: names, codes }
export function keyCombos(windows) {
  return (windows ? WIN_COMBOS : LINUX_COMBOS).map(([id, keys]) => ({ id, keys, codes: codesOf(keys) }));
}
// a key's label: letters and F keys as they are, arrows as arrows, the rest by name (names: t of machines.keys.<k>)
const ARROWS = { left: '←', right: '→', up: '↑', down: '↓' };
export function keyName(k, windows, names = (x) => x) {
  if (ARROWS[k]) return ARROWS[k];
  if (/^f\d+$/.test(k)) return k.toUpperCase();
  if (k.length === 1) return k.toUpperCase();
  return names(k === 'meta' ? (windows ? 'win' : 'super') : k);
}
export const comboLabel = (keys, windows, names) => keys.map((k) => keyName(k, windows, names)).join('+');

// Held keys (latched): down in the guest from the click until the next key the person types there or the next
// combination sent, then up in reverse. Each function returns the latched codes left and what to send, [[code, 1|0]].
export function latchToggle(latched, code) {
  return latched.includes(code) ? { latched: latched.filter((c) => c !== code), send: [[code, 0]] } : { latched: [...latched, code], send: [[code, 1]] };
}
export const latchRelease = (latched) => ({ latched: [], send: [...latched].reverse().map((c) => [c, 0]) });
// a key let go of in the guest (already sent): a latched one the person pressed and let go of themselves is up now;
// another modifier keeps them (latched Ctrl + Shift + T); any other key ends them
export function latchKeyUp(latched, code) {
  if (latched.includes(code)) return { latched: latched.filter((c) => c !== code), send: [] };
  if (!latched.length || isModifier(code)) return { latched, send: [] };
  return latchRelease(latched);
}
// a combination: its keys down in order (but those already held), up in reverse, then the held ones up
export function comboSend(codes, latched = []) {
  const own = codes.filter((c) => !latched.includes(c));
  return { latched: [], send: [...own.map((c) => [c, 1]), ...[...own].reverse().map((c) => [c, 0]), ...latchRelease(latched).send] };
}

// ---- the keyboard to the guest ----
// Every key of the session reaches every key listener (runtime addListener), so a window's own focus flag is not
// enough to send keys to a guest: it is from the window's last render, and it was once true for a full-screen machine
// and a Terminal at the same time. guestKeys asks owns() at each key: wm.hasKeyboard(winId), the window in front with
// no search, launcher, sheet or system prompt open. A key goes down in the guest only while owns() is true, and its
// release only when its press went there; the first key while owns() is false lets go of everything held in the guest
// (release(): VD_RELEASE_ALL, latched keys included), once, so nothing stays down and nothing typed elsewhere goes in.
//   guestKeys({ send(code, down), release(), owns() }) → { down(code) → bool, up(code) → bool, held() → codes }
export function guestKeys({ send, release, owns }) {
  const held = new Set();
  let lost = false;
  const lose = () => { held.clear(); if (!lost) { lost = true; release(); } };
  return {
    down(code) {
      if (!(code > 0)) return false;
      if (!owns()) { lose(); return false; }
      lost = false;
      held.add(code);
      send(code, 1);
      return true;
    },
    up(code) {
      if (!held.has(code)) return false;
      held.delete(code);
      if (!owns()) { lose(); return false; }
      send(code, 0);
      return true;
    },
    held: () => [...held],
  };
}
// Whether a window the guest opens or brings back in apps mode may take the front: only while the person is already
// in this machine (its window or one of its guest windows is in front, or nothing is). Otherwise it opens behind the
// window in front, so a guest's new window never takes the keys of a Terminal being typed in.
export function guestMayTakeFront(front, dir) {
  if (!front) return true;
  if (front.app !== 'machines' || !dir) return false;
  const a = front.args || {};
  return a.guestWin?.dir === dir || a.machine === dir;
}

// the window's title: "<name> — <state>"
export const windowTitle = (name, stateText) => (stateText ? `${name} — ${stateText}` : name);

// a folder name for display, from the machine's dir (".../Machines/Debian.lordvm" → "Debian")
export const folderOf = (dir) => (dir || '').split('/').pop().replace(/\.lordvm$/, '');

// ---- renaming a machine (docs/VIRTUALIZATION.md 6.26) ----
// The name is machine.json's; its folder and transfer folder follow it while it's off (lord-vm --rename / --set --name,
// lord.vm.renamePlan says what a name would do). A machine is the same machine by its id: its windows ('machines#vm-<id>',
// 'cfg-', 'snap-', 'log-') find it again under its new folder.
const RENAME_PROBLEMS = ['badName', 'nameTaken', 'folderTaken', 'transferTaken'];
// the uuid in a Machines window's id, or null
export function windowMachineId(winId) {
  const m = /^machines#(?:vm|cfg|snap|log)-([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})$/.exec(String(winId || ''));
  return m ? m[1] : null;
}
// the folder a window shows now: dir while the library still has it, else the one machine with its id (a Duplicate
// shares the id: two of them, and the window keeps dir)
export function followDir(dir, id, list) {
  if (!id || !Array.isArray(list) || list.some((m) => m && m.dir === dir)) return dir;
  const same = list.filter((m) => m && m.id === id && typeof m.dir === 'string');
  return same.length === 1 ? same[0].dir : dir;
}
// Configure → General's line under Nombre for the draft name, from lord.vm.renamePlan's answer: { tone: 'bad'|'dim',
// key, vars } or null, and whether Save may go. Running: only the name changes now. A name that isn't the machine's yet
// but whose folder stays is said by nothing at all.
export function renameNote(plan, { name, current, running } = {}) {
  const changed = typeof name === 'string' && name !== current;
  if (!changed || !plan) return { note: null, ok: true };
  if (!String(name).trim() || String(name).startsWith('.')) return { note: { tone: 'bad', key: 'machines.cfg.rn.badName', vars: {} }, ok: false };
  if (RENAME_PROBLEMS.includes(plan.problem)) {
    return { note: { tone: 'bad', key: `machines.cfg.rn.${plan.problem}`, vars: { name: plan.other || name, folder: plan.folder || '' } }, ok: false };
  }
  if (running) return { note: { tone: 'dim', key: plan.moves ? 'machines.cfg.rn.running' : 'machines.cfg.rn.runningSame', vars: { folder: plan.folder || '' } }, ok: true };
  return { note: plan.moves ? { tone: 'dim', key: 'machines.cfg.rn.folderWillBe', vars: { folder: plan.folder } } : null, ok: true };
}
// the Folder row's offer while the machine is off: its folder still says another name (renamed while it ran, or by
// hand) and that name is free: { folder } to offer Rename Folder, else null
export function folderOffer(plan, running) {
  return !running && plan && plan.moves && !plan.problem && plan.folder ? { folder: plan.folder } : null;
}
// what a rename (or a set with a name) did, from its job's output: lord-vm's JSON line { dir, folder: 'moved'|'same'|
// 'kept'|'later', why, problem }, or {} when there is none
export function renameOutcome(out) {
  const lines = String(out || '').split('\n').map((l) => l.trim()).filter((l) => l.startsWith('{'));
  for (let i = lines.length - 1; i >= 0; i--) {
    try {
      const j = JSON.parse(lines[i]);
      if (j && typeof j === 'object' && ('folder' in j || 'problem' in j)) {
        return { dir: typeof j.dir === 'string' ? j.dir : null, folder: typeof j.folder === 'string' ? j.folder : null, to: typeof j.to === 'string' ? j.to : null,
          why: typeof j.why === 'string' ? j.why : null, problem: typeof j.problem === 'string' ? j.problem : null,
          name: typeof j.name === 'string' ? j.name : null, other: typeof j.other === 'string' ? j.other : null };
      }
    } catch (e) { /* not JSON: lord-vm's words */ }
  }
  return {};
}

// ---- the Control Center (Main) ----

// Systems recognized from an ISO's volume label or a file's name: an id for the card's mark, the name shown. Our own
// marks (ui.jsx OsMark), never a project's logo.
const SYSTEMS = [
  ['lordos', /lordos/i, 'LordOS'], ['debian', /debian|\bd-i\b/i, 'Debian'], ['ubuntu', /ubuntu|kubuntu|xubuntu|lubuntu/i, 'Ubuntu'],
  ['fedora', /fedora/i, 'Fedora'], ['mint', /\bmint\b|linuxmint/i, 'Mint'], ['arch', /\barch(linux)?\b|archlinux/i, 'Arch'],
  ['alpine', /alpine/i, 'Alpine'], ['opensuse', /suse/i, 'openSUSE'], ['kali', /kali/i, 'Kali'], ['rocky', /rocky/i, 'Rocky'],
  ['alma', /alma/i, 'Alma'], ['centos', /centos/i, 'CentOS'], ['freebsd', /freebsd/i, 'FreeBSD'], ['raspios', /raspios|raspberry/i, 'Pi OS'],
  // a person's own Android ISO for PCs (Bliss OS, Android-x86, LineageOS): named only
  ['android', /android|blissos|\bbliss-v\d|lineage/i, 'Android'],
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
// an ISO's architecture (5.2): what its boot files say (lord.vm.isoInfo: the UEFI boot files' PE headers, their names, or
// El Torito's x86 BIOS entry) wins over its file name and label; { arch, from: 'boot' | 'name' | '', other } — `other`
// an architecture the boot files name that Machines doesn't run (riscv64), and when the boot files name two (amd64 and
// arm64 on one disc) the name picks between them
const RUNS = ['amd64', 'arm64'];
export function isoArch(info, file) {
  const boot = String(info?.arch || '');
  const named = archOf(file) || archOf(info?.label);
  if (info?.archFrom === 'boot' && RUNS.includes(boot)) return { arch: boot, from: 'boot', other: '' };
  if (info?.archFrom === 'boot' && boot) return { arch: '', from: 'boot', other: boot };
  const both = Array.isArray(info?.efiArchs) ? info.efiArchs.filter((a) => RUNS.includes(a)) : [];
  if (named && (both.length < 2 || both.includes(named))) return { arch: named, from: 'name', other: '' };
  return { arch: '', from: '', other: '' };
}
// The assistant's Detect step (5.2): what lord.vm.isoInfo read from the disc's boot files (vmiso.c: its label, the UEFI
// boot files' architectures, El Torito's BIOS and UEFI entries; isoArch decides the architecture) against this computer.
//   info: isoInfo's answer or null; file: its name; hostArch: 'amd64'|'arm64'
// → { arch, from: 'boot'|'name'|'host', starts: ['uefi'?, 'bios'?], conflict: the name says another architecture than the
//     boot files, emulated: arch isn't this computer's, both: the disc starts on amd64 and arm64 (the name or this
//     computer decides), unknownArch: the boot files name only another processor (i386, riscv64), efi: their architectures }
export function detectPlan(info, file, hostArch) {
  const byName = archOf(file) || archOf(info?.label);
  const efi = Array.isArray(info?.efiArchs) ? info.efiArchs.filter((x) => typeof x === 'string') : [];
  const ia = isoArch(info, file);
  const both = efi.includes('amd64') && efi.includes('arm64');
  const unknownArch = !!ia.other;
  const arch = ia.arch || hostArch;
  const from = ia.from === 'boot' && ia.arch ? 'boot' : ia.from === 'name' ? 'name' : 'host';
  const starts = [...(efi.length || info?.efi ? ['uefi'] : []), ...(info?.bios ? ['bios'] : [])];
  return { arch, from, starts, conflict: from === 'boot' && !!byName && byName !== arch, emulated: arch !== hostArch, both, unknownArch, efi };
}
// a file dropped on the assistant's first step (5.2): which tile takes it, by its name (an installer disc, a disk image,
// another app's machine as Migrate takes it), or null
export const dropSource = (name) => {
  const s = String(name || '');
  if (/\.iso$/i.test(s)) return 'iso';
  if (/\.(qcow2|img|raw)$/i.test(s)) return 'import';
  return /(\.(vmx|vbox|pvs|ovf|ova|vmdk|vdi|vhdx|vhd|hds)|^config\.pvs)$/i.test(s) ? 'migrate' : null;
};
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
// The card's agent and disk lights (5.1), while the machine runs: the agent's connection as lord-vm wrote it in run.json
// (agentState: connected, waiting, off) and the disk's activity between two reads (usage's disk: QEMU's cgroup I/O
// counter grew). Each light has a key for its words (the tooltip and the text beside it), never color alone.
// → [] when it isn't running, else [{ k: 'agent', tone, key }, { k: 'disk', tone, key }]; tone 'on' | 'wait' | 'off'
export function cardLights(m, u) {
  if (!m || !stateView(m.state, m.reason).active) return [];
  const ag = m.agentState === 'connected' || m.agentState === 'waiting' ? m.agentState : 'off';
  return [
    { k: 'agent', tone: ag === 'connected' ? 'on' : ag === 'waiting' ? 'wait' : 'off', key: `machines.light.agent.${ag}` },
    { k: 'disk', tone: u?.disk ? 'on' : 'off', key: u?.disk ? 'machines.light.disk.busy' : 'machines.light.disk.idle' },
  ];
}

// What this computer can run (5.7), from what the host reports (lord.vm.host(); gvt: lord.vm.gpus() lists a card that
// can be shared), never more than the hardware gives. Each row: k (its title's key), st ('native' | 'slow' | 'limited' |
// 'no': the words and color of section 2), note (its line's key), vars.
export function canRun(host, { gvt = false } = {}) {
  if (!host || !host.arch) return [];
  const a = host.arch, other = a === 'amd64' ? 'arm64' : 'amd64', kvm = !!host.kvm;
  const no3d = host.no3d || '';
  return [
    { k: 'machines.run.row.native', st: kvm ? 'native' : 'slow', note: kvm ? 'machines.run.note.native' : host.kvmDevice ? 'machines.run.note.kvmOff' : 'machines.run.note.noKvm', vars: { arch: a } },
    { k: 'machines.run.row.other', st: 'slow', note: 'machines.pi.note.amd64', vars: { arch: other } },
    { k: 'machines.win.run.row', st: a === 'amd64' ? (kvm ? 'native' : 'slow') : 'no', note: a === 'amd64' ? 'machines.win.run.note' : 'machines.win.run.noteArm', vars: {} },
    { k: 'machines.run.row.android', st: a === 'amd64' ? (kvm ? 'native' : 'slow') : 'no', note: a === 'amd64' ? 'machines.run.note.android' : 'machines.run.note.androidArm', vars: {} },
    { k: 'machines.pi.row.gpu', st: no3d ? 'limited' : 'native', note: no3d ? `machines.cfg.no3d.${['arch', 'gpu', 'modules', 'node'].includes(no3d) ? no3d : 'gpu'}` : 'machines.run.note.gpu', vars: {} },
    { k: 'machines.pi.row.passthrough', st: gvt ? 'limited' : 'no', note: gvt ? 'machines.run.note.gvt' : a === 'arm64' ? 'machines.pi.note.passthrough' : 'machines.run.note.noGvt', vars: {} },
    { k: 'machines.pi.row.nested', st: host.nested ? 'native' : 'no', note: host.nested ? 'machines.run.note.nested' : 'machines.pi.note.nested', vars: {} },
  ];
}
// the sensors' lines while machines run (5.7; lord.vm.sensors()): only the ones this computer has
export function sensorLines(s) {
  const out = [];
  if (!s) return out;
  if (s.tempC >= 0) out.push({ key: 'machines.run.temp', vars: { t: s.tempC }, warn: s.tempC >= 80 });
  if (s.fanRpm >= 0) out.push({ key: 'machines.run.fan', vars: { rpm: s.fanRpm }, warn: false });
  if (s.underVoltage === 0 || s.underVoltage === 1) out.push({ key: s.underVoltage ? 'machines.run.powerLow' : 'machines.run.power', vars: {}, warn: s.underVoltage === 1 });
  return out;
}

// Picture in Picture of a machine (5.4): a small floating view of its display at the bottom end corner of the display
// its window is on (d: { x, y, w, h }), the guest's shape kept (gw × gh; 16:10 when unknown), PIP_W wide or a quarter
// of the display if smaller, never under 240 × 135; `rtl`: the bottom left instead. Input stays off in it (no keys,
// no pointer: the view is to watch, a click brings the window back).
export const PIP_W = 400, PIP_MARGIN = 24, PIP_DOCK = 96;
export function pipFrame(d, gw, gh, rtl = false) {
  if (!d || !(d.w > 0) || !(d.h > 0)) return null;
  const ratio = gw > 0 && gh > 0 ? gh / gw : 10 / 16;
  const w = Math.max(240, Math.min(PIP_W, Math.round(d.w / 4)));
  const h = Math.max(135, Math.round(w * ratio));
  const x = rtl ? d.x + PIP_MARGIN : d.x + d.w - w - PIP_MARGIN;
  return { x, y: Math.max(d.y, d.y + d.h - h - PIP_DOCK), w, h };
}
// Actual Size (5.3): the window's frame so the picture is the guest's display pixel for pixel (bar above it), within the
// display's work area; null when the guest's size isn't known
export function actualFrame(gw, gh, bar, area) {
  if (!(gw > 0) || !(gh > 0) || !area) return null;
  return { w: Math.min(gw, area.w), h: Math.min(gh + (bar || 0), area.h) };
}
// View → Scale (5.3): the window's frame so the picture is the guest's display at k times its size (bar above it); when
// that doesn't fit the display's work area, the largest frame of the same shape that does (fits: false then)
export const SCALES = [0.5, 0.75, 1.25, 1.5, 2];
export function scaleFrame(gw, gh, k, bar, area) {
  if (!(gw > 0) || !(gh > 0) || !(k > 0) || !area) return null;
  const b = bar || 0;
  let w = Math.round(gw * k), h = Math.round(gh * k);
  const fit = Math.min(1, area.w / w, (area.h - b) / h);
  if (fit < 1) { w = Math.floor(w * fit); h = Math.floor(h * fit); }
  return { w: Math.max(320, w), h: Math.max(200, h) + b, fits: fit >= 1 };
}
// The window keeps the guest's shape (5.3) while the resolution is fixed: after the person resizes it, the frame that
// keeps its width and gives the picture box the guest's proportions (or, when that is taller than the work area, the
// tallest that fits, narrower). frame and box: { w, h } of the window and of the picture; null when it is already within
// 2 pixels (or nothing is known)
export function shapeFrame(frame, box, gw, gh, area) {
  if (!frame || !box || !(gw > 0) || !(gh > 0) || !(box.w > 0) || !(box.h > 0)) return null;
  const want = Math.round(box.w * gh / gw);
  if (Math.abs(want - box.h) <= 2) return null;
  let w = frame.w, h = frame.h + (want - box.h);
  if (area && h > area.h) {
    const ph = box.h + (area.h - frame.h);
    if (ph < 100) return null;
    w = frame.w - box.w + Math.round(ph * gw / gh);
    h = area.h;
  }
  if (Math.abs(w - frame.w) <= 2 && Math.abs(h - frame.h) <= 2) return null;
  return { w, h };
}
// The bar's device dots (5.3): one per device the guest can use now, each { k, key } (machines.dot.<k> its words); a
// dot is shown only while the device is there, so what the guest has is always in sight. USB and the camera come from the
// devices' own backend (caps from lord.vm.deviceCaps, when it is there): shown only once it says they are supported.
export function deviceDots({ iso, heard, mic, cableOut, network, shares, transfer, caps } = {}) {
  const out = [];
  if (network && network !== 'isolated') out.push({ k: cableOut ? 'netOut' : 'net' });
  if (iso) out.push({ k: 'cd' });
  if (heard) out.push({ k: 'sound' });
  if (mic) out.push({ k: 'mic' });
  if ((shares | 0) > 0 || transfer) out.push({ k: 'folders' });
  if (caps?.usb && (caps.usbAttached | 0) > 0) out.push({ k: 'usb' });
  if (caps?.camera && caps.cameraOn) out.push({ k: 'camera' });
  return out;
}
// Devices → Network: can the cable be pulled? Only the shared, host-only and bridged networks of a running machine that
// isn't a group's member (lord-vm refuses the rest too)
export const cableOffered = (cfg, running) => !!running && !cfg?.group && ['shared', 'hostonly', 'bridged'].includes(cfg?.network);
// The Info menu (5.3): what this machine uses now, from two lord.vm.stats reads (model.js usage) and its sandbox
// (isolationGaps); each line a key and its values
export function infoLines(m, u, gaps) {
  const out = [];
  if (u) {
    out.push({ key: 'machines.info.cpu', vars: { v: u.cpu != null ? Math.round(u.cpu) : '—' } });
    out.push({ key: 'machines.info.memory', vars: { a: u.memBytes ? Math.round(u.memBytes / 1048576) : '—', b: m?.memoryMb || 0 } });
    out.push({ key: u.disk ? 'machines.light.disk.busy' : 'machines.light.disk.idle', vars: {} });
  }
  out.push({ key: (gaps || []).length ? 'machines.info.weaker' : 'machines.info.isolated', vars: {} });
  return out;
}

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

// ---- vCPU recommendation (docs/VIRTUALIZATION.md, "vCPU recommendation") ----
// lord-vm keeps what a machine's vCPUs used in <dir>/cpu-usage.json (server/src/vm/vmcpu.h): one window per 5 minutes
// of running time, [end (Unix s), seconds, vCPUs, average, busiest minute] (both in hundredths of a core). The app
// suggests a count from them; it never changes the machine by itself.
export const VCPU_ADVICE = {
  minSecs: 2 * 3600,        // at least two hours of running time at the current count
  maxAgeSecs: 30 * 86400,   // windows of the last 30 days
  low: 0.3,                 // fewer: 95 % of the time under 30 % of the vCPUs
  high: 0.85,               // more: half the time or more at 85 % of them or over
  target: 0.6,              // a fewer count keeps the busy minutes at about 60 % of it
  floor: 2,                 // never suggests fewer than two
};

// cpu-usage.json's text → its windows [{ t, secs, vcpus, avg, peak }] (cores, not hundredths); [] when it isn't one
export function cpuWindows(text) {
  let j = null;
  try { j = JSON.parse(text); } catch (e) { return []; }
  if (!j || j.format !== 1 || !Array.isArray(j.windows)) return [];
  const ok = (x) => Number.isFinite(x) && x >= 0;
  return j.windows.filter((w) => Array.isArray(w) && w.length === 5 && w.every(ok) && w[1] > 0 && Number.isInteger(w[2]) && w[2] >= 1 && w[3] <= w[2] * 100 && w[4] <= w[2] * 100)
    .map(([t, secs, vcpus, avg, peak]) => ({ t, secs, vcpus, avg: avg / 100, peak: Math.max(avg, peak) / 100 }));
}

// the value under which `frac` of the time falls (windows weighted by their seconds)
function weightedQuantile(ws, key, frac) {
  const s = [...ws].sort((a, b) => a[key] - b[key]);
  const total = s.reduce((a, w) => a + w.secs, 0);
  let acc = 0;
  for (const w of s) { acc += w.secs; if (acc >= total * frac) return w[key]; }
  return s.length ? s[s.length - 1][key] : 0;
}

// The most vCPUs a suggestion gives one machine here: this computer's processors less a reserve for the desktop
// (one in eight, at least one), never more than lord-vm recommends
export function vcpuCeiling(host) {
  const cpus = Math.max(1, host?.cpus | 0 || 1);
  const rec = Math.max(1, host?.vcpusRecommended | 0 || cpus - 1 || 1);
  return Math.max(1, Math.min(rec, cpus - Math.max(1, Math.ceil(cpus / 8))));
}

// A new machine's processors when its profile asks for "half": half of this computer's physical cores, six at most
// (a guest rarely uses more, and every vCPU is a thread the host schedules), never more than cpuCap
export function vcpuDefault(host, cap) {
  const cores = Math.max(1, host?.cores | 0 || Math.floor((host?.cpus | 0 || 2) / 2) || 1);
  return Math.max(1, Math.min(cap ?? 64, 6, Math.floor(cores / 2) || 1));
}

// What a machine used and the count suggested from it, or null when there's nothing to say:
//   { kind: 'fewer'|'more', n, avg (cores on average), p95, peak (cores), hours (of running time measured), vcpus }
// Only windows at the machine's current count, of the last 30 days, weighted by their length. Fewer: when 95 % of the
// time it stayed under 30 % of its vCPUs, enough for its busy minutes (95th percentile) at about 60 %, at least two. More:
// when half the time or more it used 85 % or over (a busy spell now and then, a build, doesn't count), half again (at
// least one more) up to vcpuCeiling.
export function vcpuAdvice(windows, vcpus, host, now = Date.now() / 1000) {
  const n0 = vcpus | 0;
  if (n0 < 1 || !Array.isArray(windows)) return null;
  const ws = windows.filter((w) => w.vcpus === n0 && w.t <= now + 86400 && now - w.t <= VCPU_ADVICE.maxAgeSecs);
  const secs = ws.reduce((a, w) => a + w.secs, 0);
  if (secs < VCPU_ADVICE.minSecs) return null;
  const avg = ws.reduce((a, w) => a + w.avg * w.secs, 0) / secs;
  const p50 = weightedQuantile(ws, 'avg', 0.5), p95 = weightedQuantile(ws, 'avg', 0.95), peak = weightedQuantile(ws, 'peak', 0.95);
  const base = { avg, p95, peak, hours: secs / 3600, vcpus: n0 };
  const ceil = vcpuCeiling(host);
  if (p50 >= VCPU_ADVICE.high * n0 && n0 < ceil) return { ...base, kind: 'more', n: Math.min(ceil, n0 + Math.max(1, Math.ceil(n0 / 2))) };
  if (p95 < VCPU_ADVICE.low * n0) {
    const n = Math.max(VCPU_ADVICE.floor, Math.ceil(peak / VCPU_ADVICE.target - 1e-9));
    if (n < n0) return { ...base, kind: 'fewer', n };
  }
  return null;
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

// The library's view (settings.machinesView, 0.8.8.7; the chips of the 0.8.9 redesign): pictures or a list, the order,
// which machines show (all, running, Linux, the other system's); 'off' of before is read as all
export const LIB_VIEWS = ['grid', 'list'];
export const LIB_SORTS = ['name', 'state', 'last', 'disk'];
export const LIB_SHOWS = ['all', 'running', 'linux', 'windows', 'templates'];
export function libView(v) {
  const o = v && typeof v === 'object' ? v : {};
  return {
    view: LIB_VIEWS.includes(o.view) ? o.view : 'grid',
    sort: LIB_SORTS.includes(o.sort) ? o.sort : 'name',
    show: LIB_SHOWS.includes(o.show) ? o.show : 'all',
  };
}
// The library's grid for a window this wide (narrow: useNarrow): as many cards as fit at 300 px or more, the New
// machine card first, each picture about half as tall as wide. From the window's width in the window store, known from
// the first frame, never from the root's layout (not measured yet then: one column until the next read, 2026-11-23)
export function libGrid(width, narrow) {
  const pad = narrow ? 16 : 24, gap = 20;
  const cw = Math.max(0, (Number(width) || 0) - pad * 2);
  const cols = Math.max(1, Math.floor((cw + gap) / (300 + gap)));
  const cardW = Math.floor((cw - gap * (cols - 1)) / cols);
  return { pad, gap, cols, cardW, picH: Math.max(120, Math.round(cardW * 0.48)) };
}
// What the library showed the last time (settings.machinesSeen: { count, shown }, 2026-11-23): the machines there were
// and the cards the chip left. Its first frame, before lord.vm.list has answered, draws that many placeholders (at most
// SKELETON_MAX; SKELETON_DEFAULT when never seen) in the view's layout, or the empty library at once when there were none
export const SKELETON_MAX = 6;
export const SKELETON_DEFAULT = 2;
export function libSeen(v) {
  const o = v && typeof v === 'object' ? v : {};
  const n = (x) => (Number.isInteger(x) && x >= 0 && x <= 9999 ? x : -1);
  return { count: n(o.count), shown: n(o.shown) };
}
export function skeletonPlan(seen) {
  const s = libSeen(seen);
  if (s.count === 0) return { empty: true, cards: 0 };
  const n = s.shown >= 0 ? s.shown : s.count >= 0 ? s.count : SKELETON_DEFAULT;
  return { empty: false, cards: Math.min(SKELETON_MAX, n) };
}
// the next machinesSeen once the list is in, or null when it hasn't changed (a search doesn't count: never kept)
export function seenNext(seen, count, shown, query) {
  if ((query || '').trim() || !Number.isInteger(count) || !Number.isInteger(shown)) return null;
  const s = libSeen(seen);
  return s.count === count && s.shown === shown ? null : { count, shown };
}
// the systems a machine may run, as the chips group them: 'windows', 'linux' or 'other'
const LINUX = ['lordos', 'debian', 'ubuntu', 'fedora', 'mint', 'arch', 'alpine', 'opensuse', 'kali', 'rocky', 'alma', 'centos', 'raspios', 'linux'];
export function familyOf(m) {
  if (isWindowsMachine(m) || m?.meta?.os === 'windows') return 'windows';
  const id = m?.meta?.os || systemOf(m?.meta?.osName, m?.name).id;
  if (id === 'windows') return 'windows';
  return LINUX.includes(id) || m?.meta?.family === 'linux' ? 'linux' : 'other';
}
// templates (5.1, app.json template): machines kept as they are, to make new ones from (a clone); their own chip
const shows = (m, show) => (show === 'templates' ? !!m.meta?.template : show === 'running' ? stateView(m.state, m.reason).active : show === 'linux' || show === 'windows' ? familyOf(m) === show
  : show === 'off' ? !stateView(m.state, m.reason).active : true);
// A machine's tags (Configure → General, app.json): trimmed, no control characters, 1 to 24 characters each, at most 8,
// each once (case-insensitively), in the order given. "#tag" in the library's search finds them exactly.
export const TAGS_MAX = 8, TAG_LEN = 24;
export function tagsOf(list) {
  const out = [];
  for (const x of Array.isArray(list) ? list : []) {
    if (typeof x !== 'string') continue;
    const t = x.replace(/[\u0000-\u001f\u007f]/g, '').replace(/\s+/g, ' ').trim().replace(/^#+/, '');
    if (!t || [...t].length > TAG_LEN || out.some((o) => o.toLowerCase() === t.toLowerCase())) continue;
    out.push(t);
    if (out.length === TAGS_MAX) break;
  }
  return out;
}
// "work, test" as typed in Configure → tags
export const parseTags = (text) => tagsOf(String(text || '').split(/[,\u060c\uff0c\u3001]/));
// the machines a search and the chip leave: the query matches the name, the system, the architecture or a tag;
// "#name" only a tag of that name
export function filterMachines(list, { query, show } = {}) {
  const q = (query || '').trim().toLowerCase();
  return (list || []).filter((m) => {
    if (!shows(m, show)) return false;
    if (!q) return true;
    const tags = (m.meta?.tags || []).map((x) => x.toLowerCase());
    if (q.startsWith('#') && q.length > 1) return tags.includes(q.slice(1));
    const sys = m.meta?.osName || systemOf(m.name).name || '';
    return [m.name, sys, m.arch, ...tags].some((s) => (s || '').toLowerCase().includes(q));
  });
}
// how many machines each chip shows (the search applies to all of them)
export function chipCounts(list, query) {
  const out = {};
  for (const k of LIB_SHOWS) out[k] = filterMachines(list, { query, show: k }).length;
  return out;
}
// A card's one line of numbers (the approved mock "Main"): "10 CPU · 8 GB · 64 GB · Bridged · Intel card" as parts —
// [{ k: key under machines.lib.spec, n }] the app words; 3D and a graphics card only when the machine has them
export function specParts(m) {
  const out = [{ k: 'cpu', n: m?.vcpus | 0 }, { k: 'mem', mb: m?.memoryMb | 0 }, { k: 'disk', n: m?.diskGb | 0 },
    { k: `net.${NET_MODES.includes(m?.network) ? m.network : 'shared'}` }];
  if (m?.gpu) out.push({ k: 'gpu' });
  else if (m?.graphics === '3d') out.push({ k: '3d' });
  return out;
}
// What a list row says beside a machine that isn't running: since when it's off (its card picture is written while it
// runs), that it resumes, or its profile. → { k, n } (n: minutes, hours or days ago)
export function restLine(m, now = Date.now()) {
  if (m?.suspended) return { k: 'resumes' };
  const t = (m?.thumbTime || 0) * (m?.thumbTime > 1e12 ? 1 : 1000);
  if (t > 0 && t <= now) {
    const min = Math.floor((now - t) / 60000);
    if (min < 60) return { k: 'lastMin', n: Math.max(1, min) };
    if (min < 48 * 60) return { k: 'lastHours', n: Math.floor(min / 60) };
    return { k: 'lastDays', n: Math.floor(min / 1440) };
  }
  return m?.meta?.profile ? { k: 'profile', id: m.meta.profile } : null;
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

// A protected machine (docs/VIRTUALIZATION.md 19.5, docs/PRIVACY.md "Protected windows"): the anonymity profile, as the
// Whonix members' machine.json profile_lock says it, or as the machine was made with it (app.json profile), or a
// member of a Whonix group. Its window and its apps-mode windows open protected (a neutral fill with a lock in every
// capture of the screen), lord.vm.protect keeps its picture from leaving as a picture of its own (no card picture,
// screenshot, recording or snapshot picture), and its card never shows a thumb.png.
export const ANONYMITY_PROFILE = 'anonymity';
export const machineProtected = (m) => !!m && (m.profileLock === ANONYMITY_PROFILE || m.meta?.profile === ANONYMITY_PROFILE || m.groupKind === 'whonix' || m.group?.kind === 'whonix');
// The one question every call site asks (meta.js isProtectedMachine): lord.vm.list()'s entry for the machine (machine.json:
// profile_lock, its group) and its app.json meta together, or a window of it that is already protected (home: the
// machine's own window, so its apps-mode windows can never be less protected than it). Each part alone is enough.
export const machineProtectedOf = (entry, meta, homeProtected = false) =>
  !!homeProtected || machineProtected({ ...(entry && typeof entry === 'object' ? entry : null), meta: meta || entry?.meta || {} });

// ---- the guest agent (6.17) ----

// lord-vm's capability bits (vmproto.h VAC_*) → the popover's list: what works now, and what doesn't and why
// transfer: the guest has the transfer folder (6.7.1) mounted; transferSetup: a Windows guest that gets it once WinFsp is
// installed (Set Up the Transfer Folder); clipNoTool: the Linux guest's helper runs but the guest has no clipboard tool
// (xclip or wl-clipboard: lordos-guest's hello said "noclip")
// shell: the tools start the agent's shell (6.18, tools 8 / w12); links, notify: they pass the guest's links and
// notifications on, and this machine takes them (6.12); eventlog: a Windows guest's event logs come in (6.19)
export const AGENT_CAPS = { shutdown: 1, time: 2, exec: 4, files: 8, journal: 16, clipboard: 32, awake: 64, display: 128, windows: 256, transfer: 512, transferSetup: 1024, clipNoTool: 2048, clipLazy: 4096, open: 8192, place: 16384, extra: 32768, extraOff: 65536, shell: 131072, links: 262144, notify: 524288, eventlog: 1048576, toolsDisc: 2097152, send: 4194304 };
// a Windows guest (the system qemu-guest-agent reported): its clipboard and its display's size need the LordOS tools
// (tools/guest-win), installed from the agent's popover; a Linux guest's size follows QEMU's own display by itself
export const guestWindows = (agent) => /^windows/i.test(agent?.system || '');
export function agentLines(agent, cfg) {
  const caps = agent?.caps || 0, on = agent?.state === 'connected';
  const clip = cfg?.clipboard || 'off';
  const win = guestWindows(agent);
  return [
    ['shell', 'works'],                                       // the serial console needs no agent
    // the agent's shell (6.18): the signed-in person's own shell, with the LordOS tools 8 / w12
    ['agentShell', on && caps & AGENT_CAPS.shell ? 'works' : on ? 'needsTools' : 'needsAgent'],
    // the guest's journal (systemd) or a Windows guest's event logs (6.19); keeping the guest awake is systemd's
    ['logs', on && caps & (win ? AGENT_CAPS.eventlog : AGENT_CAPS.journal) ? 'works' : 'needsAgent'],
    ['shutdown', on && caps & AGENT_CAPS.shutdown ? 'works' : 'acpi'],
    ['time', on && caps & AGENT_CAPS.time ? 'works' : 'needsAgent'],
    win ? null : ['awake', on && caps & AGENT_CAPS.awake ? 'works' : 'needsAgent'],
    ['clipboard', clipState(agent, cfg)],
    ['resolution', !win || caps & AGENT_CAPS.display ? 'works' : 'needsTools'],
    ['folders', cfg?.shares?.length || (cfg?.transfer && caps & AGENT_CAPS.transfer) ? 'works' : 'none'], ['apps', appsBlocker(agent, cfg) || 'works'],
    // links and notifications from the guest (6.12): off unless Configure turned them on
    ['links', !cfg?.guestLinks ? 'off' : on && caps & AGENT_CAPS.links ? 'works' : on ? 'needsTools' : 'needsAgent'],
    ['notify', !cfg?.guestNotify ? 'off' : on && caps & AGENT_CAPS.notify ? 'works' : on ? 'needsTools' : 'needsAgent'],
    // files from the guest (6.21, "Send to LordOS"): on unless Configure turned them off; the tools 10 / w13
    ['sendFiles', cfg?.guestFiles === false ? 'off' : on && caps & AGENT_CAPS.send ? 'works' : on ? 'needsTools' : 'needsAgent'],
  ].filter(Boolean);
}
// the clipboard's line: 'off' (Configure), 'works', 'needsXclip' (the helper runs but the guest has no clipboard tool),
// 'needsTools' (a Windows guest without the LordOS tools), 'needsHelper' (a Linux guest without lordos-guest)
export function clipState(agent, cfg) {
  const caps = agent?.caps || 0;
  if ((cfg?.clipboard || 'off') === 'off') return 'off';
  if (caps & AGENT_CAPS.clipboard) return 'works';
  if (caps & AGENT_CAPS.clipNoTool) return 'needsXclip';
  return guestWindows(agent) ? 'needsTools' : 'needsHelper';
}
// The LordOS Guest Tools disc (6.17, lordvm.c VAG_DISC): for a Linux guest without a usable qemu-guest-agent.
// → null (nothing to offer), 'insert' (the popover offers "Install the Agent from a Disc") or 'guide' (the disc is in the
// drive: the steps to run in the machine). Never for a protected (anonymity) machine, a Windows guest, a machine that is
// off or whose agent is off, nor when qemu-guest-agent answers and lets Install copy files and run its script.
export function discOffer(agent, cfg, running, isProtected) {
  if (!cfg?.agent || !running || isProtected) return null;
  const caps = agent?.caps || 0;
  if (caps & AGENT_CAPS.toolsDisc) return 'guide';
  if (guestWindows(agent)) return null;
  // it answered earlier in this run of the guest and stopped (6.17, red): the agent is there, not a disc's matter
  if (agent?.lost) return null;
  if (agent?.state === 'connected' && (caps & AGENT_CAPS.exec) && (caps & AGENT_CAPS.files)) return null;
  return 'insert';
}
// what to run in the machine (install.sh on the disc, volume LORDOS_TOOLS): where desktops mount it, and by hand
export const DISC_LABEL = 'LORDOS_TOOLS';
export const discCommands = () => [
  { key: 'debian', cmd: `sh /media/$USER/${DISC_LABEL}/install.sh` },
  { key: 'others', cmd: `sh /run/media/$USER/${DISC_LABEL}/install.sh` },
  { key: 'manual', cmd: 'sudo mount -r /dev/sr0 /mnt && sh /mnt/install.sh' },
];
// Copy is offered only where it can reach the machine: its clipboard goes toward it (Configure) and a helper there
// takes it. Before the agent is installed it usually can't: the command is shown to be typed
export const discCanCopy = (agent, cfg) => clipTo(cfg?.clipboard || 'off') && !!((agent?.caps || 0) & AGENT_CAPS.clipboard);
// the agent's popover says what to install in the guest: connected, the clipboard on, the helper there without a tool
export const clipNeedsTool = (agent, cfg) => agent?.state === 'connected' && clipState(agent, cfg) === 'needsXclip';
// The LordOS tools this LordOS ships: lordos-guest-win's VERSION (tools/guest-win) and lordos-guest's (tools/guest);
// vm_model_test checks them against the sources. A connected guest's tools say theirs in their hello (agent.tools)
export const TOOLS_SHIPPED = { windows: 'w13', linux: '10' };
const toolsVer = (v) => /^(w?)([0-9]{1,6})$/.exec(String(v || ''));
// the guest's tools are older than the ones shipped: the agent's popover offers Update, the bar's button gets a dot.
// "w<n>" is the Windows tools', "<n>" the Linux helper's; anything else (none yet, a newer format) isn't older
export function toolsOutdated(agent, shipped = TOOLS_SHIPPED) {
  if (agent?.state !== 'connected') return false;
  const have = toolsVer(agent.tools);
  if (!have) return false;
  const want = toolsVer(have[1] ? shipped?.windows : shipped?.linux);
  return !!want && want[1] === have[1] && Number(have[2]) < Number(want[2]);
}
// the version this LordOS ships for that guest's tools when theirs is older ('' when it isn't, or nothing is known)
export function toolsWant(agent, shipped = TOOLS_SHIPPED) {
  if (!toolsOutdated(agent, shipped)) return '';
  return toolsVer(agent.tools)[1] ? String(shipped.windows) : String(shipped.linux);
}
// the popover's line about the LordOS tools, apart from qemu-guest-agent's version: null (not connected, or no tools said
// hello), { key: 'current', have } (the ones shipped, or newer), { key: 'old', have, want }, or { key: 'other', have } (a
// version this LordOS can't compare)
export function toolsLine(agent, shipped = TOOLS_SHIPPED) {
  if (agent?.state !== 'connected' || !agent.tools) return null;
  const have = String(agent.tools);
  const want = toolsWant(agent, shipped);
  if (want) return { key: 'old', have, want };
  return toolsVer(have) ? { key: 'current', have } : { key: 'other', have };
}

// ---- the LordOS tools brought up to date by themselves (docs/VIRTUALIZATION.md "Keeping the LordOS tools up to date") ----
// When a machine's agent is connected and its tools are older than the ones shipped, Machines asks for the same install
// as the popover's button (lord.vm.agent 'update', which lord-vm logs), unless the person turned it off: the machine's
// Configure → Agent (app.json toolsAuto) or, for machines that don't say, Machines settings (machinesDefaults.toolsAuto).
// Only when it's safe: the agent connected for TOOLS_SETTLE_MS and nothing on its way (files copying, the transfer
// folder's setup, an app of apps mode opening, an install the person asked for). At most TOOLS_TRIES attempts per run of
// the machine and version, TOOLS_BACKOFF_MS apart; an attempt without an answer in TOOLS_WAIT_MS failed. Off, or given
// up: one notification per run and version, with Update. Before the first attempt for a version, a snapshot of the
// machine (6.2, "Auto-snapshot before a guest's tools upgrade"; with its memory, as it runs) when `snap` says the
// machine can have one: act 'snapshot', once per run and version; the install follows once it's done or refused.
export const TOOLS_SETTLE_MS = 20000;
export const TOOLS_BACKOFF_MS = [2 * 60000, 10 * 60000];
export const TOOLS_TRIES = 3;
export const TOOLS_WAIT_MS = 10 * 60000;
export const TOOLS_AUTO_IDLE = { want: '', since: 0, tries: 0, next: 0, pending: 0, done: 0, told: '', snapped: '' };
// may the update be preceded by a snapshot: snapshots exist for it (not raw disks, not a disposable session, not 3D or a
// graphics card while it runs: lord-vm refuses those), and there's room for one more
export const toolsSnapOk = ({ raw, disposable, gl, count, max = 64 } = {}) => !raw && !disposable && !gl && (count || 0) < max;
// the machine's choice, else the default for all of them (on unless turned off)
export const toolsAutoOn = (meta, defaults) => (typeof meta?.toolsAuto === 'boolean' ? meta.toolsAuto : defaults?.toolsAuto !== false);
// an attempt that failed: the next one after its backoff, none after the last
export function toolsAutoFailed(s, now) {
  const k = Math.min(Math.max(s.tries - 1, 0), TOOLS_BACKOFF_MS.length - 1);
  return { ...s, pending: 0, next: s.tries >= TOOLS_TRIES ? Infinity : now + TOOLS_BACKOFF_MS[k] };
}
// One step at every poll of a running machine: s (TOOLS_AUTO_IDLE for a new run), the poll's agent, on (toolsAutoOn),
// busy (something on its way, see above), now. → { s, act: null | 'install' | 'notify', why: 'off' | 'failed' (with
// notify), result: 'ok' | 'failed' (an attempt of ours ended) }. After 'install' the caller sends it and, when
// lord-server refuses it at once, calls toolsAutoFailed.
export function toolsAutoStep(s0, { agent, on, busy, now, snap = false }, shipped = TOOLS_SHIPPED) {
  let s = { ...TOOLS_AUTO_IDLE, ...(s0 || {}) };
  const inst = agent?.install || 0;
  // our attempt on its way: how it ended. lord.vm.agent sets the session's install to 1 as it sends, so a 2 or 3 after
  // it is its answer; 0 (the window opened again on the machine, a new session) waits for TOOLS_WAIT_MS
  if (s.pending) {
    if (inst === 2) return { s: { ...s, pending: 0, done: now }, act: null, result: 'ok' };
    if (inst !== 3 && now - s.pending <= TOOLS_WAIT_MS) return { s, act: null };
    s = toolsAutoFailed(s, now);
    if (s.tries < TOOLS_TRIES || s.told === s.want) return { s, act: null, result: 'failed' };
    return { s: { ...s, told: s.want }, act: 'notify', why: 'failed', result: 'failed' };
  }
  if (agent?.state !== 'connected') return { s: s.since ? { ...s, since: 0 } : s, act: null };
  const want = toolsWant(agent, shipped);
  if (!want) return { s, act: null };
  // another version to bring (the shipped one changed): a fresh count
  if (want !== s.want) s = { ...TOOLS_AUTO_IDLE, want, since: s.since };
  if (!s.since) s = { ...s, since: now };
  if (now - s.since < TOOLS_SETTLE_MS) return { s, act: null };
  if (!on) return s.told === want ? { s, act: null } : { s: { ...s, told: want }, act: 'notify', why: 'off' };
  // installed, and the tools still say the older version a while later: said once, never tried again in this run
  if (s.done) {
    if (s.told === want || now - s.done < TOOLS_SETTLE_MS * 3) return { s, act: null };
    return { s: { ...s, told: want }, act: 'notify', why: 'failed' };
  }
  if (s.tries >= TOOLS_TRIES || now < s.next || busy || inst === 1) return { s, act: null };
  if (snap && !s.tries && s.snapped !== want) return { s: { ...s, snapped: want }, act: 'snapshot' };
  return { s: { ...s, tries: s.tries + 1, pending: now }, act: 'install' };
}
// the agent's light: gray (off or not installed), amber (waiting for it), green (connected)
// red ('bad'): the agent answered in this run of the guest and stopped answering while it runs (lost), or installing
// the LordOS tools failed (install 3)
export const agentTone = (a, configured) => (!configured ? 'off' : a?.state === 'connected' ? (a.install === 3 ? 'bad' : 'run') : a?.lost ? 'bad' : 'pause');

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
  return { name: midName(going[0].name), pct: Math.min(99, pct), waiting: going.length - 1, fast: going[0].via === 'transfer' };
}
// a long file name shortened in the middle, so the copy's percentage after it stays in view and the end of the name
// (its extension, a version) is kept: at most MAX characters (code points, so no emoji is cut in half)
export function midName(name, max = 36) {
  const c = Array.from(String(name || ''));
  if (c.length <= max) return c.join('');
  const tail = Math.max(8, Math.floor((max - 1) / 3));
  return c.slice(0, max - 1 - tail).join('') + '…' + c.slice(c.length - tail).join('');
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
// The clipboard's gate (6.8): the machine is in use while its window is in front, or, in apps mode, one of its guest's
// windows; lord-server holds the gate (lord.vm.clipboardGate) with a few seconds' grace after it goes to the background
export const clipInUse = (sid, focused, guestFocused) => !!sid && !!(focused || guestFocused);
// A guest that read or set the clipboard while not in use (lord-server refused it: poll's clipBlockedRead and
// clipBlockedWrite count them) is told to the person once per burst: again only after CLIP_BURST_MS without one, or
// CLIP_RENOTIFY_MS after the last notice. seen: what was looked at last ({} the first time: the counts are taken as
// they are, nothing told). → { seen, notify: ['read' | 'write'] }
export const CLIP_BURST_MS = 30000;
export const CLIP_RENOTIFY_MS = 300000;
export function clipBlocked(seen, read, write, now) {
  const next = { ...(seen || {}) };
  const notify = [];
  for (const [k, n] of [['read', read | 0], ['write', write | 0]]) {
    const at = `${k}At`, told = `${k}Told`;
    if (next[k] === undefined || n < next[k]) { next[k] = n; continue; }
    if (n === next[k]) continue;
    const quiet = next[at] === undefined || now - next[at] >= CLIP_BURST_MS;
    const stale = next[told] === undefined || now - next[told] >= CLIP_RENOTIFY_MS;
    if (quiet || stale) { notify.push(k); next[told] = now; }
    next[at] = now;
    next[k] = n;
  }
  return { seen: next, notify };
}

// "Ask before each paste" (machine.json clipboard_ask, 6.8): the bar over the machine's picture. Two slots: `in` (a
// paste into the machine: the guest's request for an offer, { gen }, or, with a helper that can't ask, the host's
// clipboard about to be pushed, { key }) and `out` (what the machine copied, { seq }); the bar shows `in` first. One
// answer covers every request of the same offer (a program asking for several forms of one copy is asked once), and
// the same push again. An `in` not answered in CLIP_ASK_IN_MS (the guest stops waiting) is a no; an `out` is ignored
// after CLIP_ASK_OUT_MS. Each function returns { state, answer? } where answer is what to tell lord-server at once.
export const CLIP_ASK_IN_MS = 25000;
export const CLIP_ASK_OUT_MS = 60000;
export const clipAskEmpty = () => ({ in: null, out: null, gen: 0, genYes: false, key: '', keyYes: false });
export function clipAskRequest(st, gen, now) {
  if (!(gen > 0)) return { state: st };
  if (st.gen === gen) return { state: st, answer: { gen, yes: st.genYes } };
  if (st.in?.gen === gen) return { state: st };
  return { state: { ...st, in: { gen, at: now } } };
}
export function clipAskPush(st, key, now) {
  if (!key) return { state: st };
  if (st.key === key) return { state: st, answer: { key, yes: st.keyYes } };
  if (st.in?.key === key) return { state: st };
  if (st.in?.gen) return { state: st };           // a guest's request is waiting: it goes first
  return { state: { ...st, in: { key, at: now } } };
}
export function clipAskCopied(st, seq, now) {
  if (!seq || st.out?.seq === seq) return { state: st };
  return { state: { ...st, out: { seq, at: now } } };
}
// the person's answer to the slot shown (the bar's): in first, else out
export function clipAskAnswer(st, yes) {
  if (st.in) {
    const x = st.in, n = { ...st, in: null };
    if (x.gen) { n.gen = x.gen; n.genYes = !!yes; return { state: n, answer: { gen: x.gen, yes: !!yes } }; }
    n.key = x.key; n.keyYes = !!yes;
    return { state: n, answer: { key: x.key, yes: !!yes } };
  }
  if (st.out) return { state: { ...st, out: null }, answer: { seq: st.out.seq, yes: !!yes } };
  return { state: st };
}
export function clipAskTick(st, now) {
  if (st.in && now - st.in.at >= CLIP_ASK_IN_MS) {
    const r = clipAskAnswer({ ...st, out: null }, false);
    return { state: { ...r.state, out: st.out }, answer: r.answer };
  }
  if (!st.in && st.out && now - st.out.at >= CLIP_ASK_OUT_MS) return { state: { ...st, out: null }, answer: { seq: st.out.seq, yes: false } };
  return { state: st };
}
export const clipAskShown = (st) => (st.in ? (st.in.gen ? 'in' : 'push') : st.out ? 'out' : null);
// the bar's preview line: a single line of at most max characters (text, or rich text's own text), or the picture's size
export function clipPreviewLine(pv, max = 80) {
  if (!pv) return { kind: '', text: '' };
  if (pv.kind === 'image') return { kind: 'image', w: pv.w | 0, h: pv.h | 0 };
  const one = String(pv.text || '').replace(/[\u0000-\u001f\u007f]+/g, ' ').replace(/\s+/g, ' ').trim();
  const cps = [...one];
  return { kind: pv.kind || 'text', text: cps.length > max ? `${cps.slice(0, max - 1).join('')}…` : one };
}
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
// ---- the sound's output and input (6.9, machine.json schema 8) ----
// What a machine chose, a node.name each ('' the system's default, which follows Settings → Sound): the library's list
// says soundOut and soundIn, a window's lord.vm.poll says sound.output and sound.input
export function soundDevs(m) {
  const str = (x) => (typeof x === 'string' ? x : '');
  return { output: str(m?.soundOut ?? m?.output), input: str(m?.soundIn ?? m?.input) };
}
// One menu's choices: the system's default first (with the device it is now, when known), then every device as
// Settings → Sound lists it (lord.sound's sinks, or its microphones), the chosen one checked. A chosen device that
// isn't there (unplugged) stays in the list, checked and marked missing: its sound goes to the default meanwhile.
// [{ name, label, checked, isDefault?, current?, missing? }]; label null for the default (the app names it)
export function soundDevChoices(list, chosen) {
  const devs = (Array.isArray(list) ? list : []).filter((d) => d && typeof d.name === 'string' && d.name);
  const c = typeof chosen === 'string' ? chosen : '';
  const def = devs.find((d) => d.default);
  const out = [{ name: '', label: null, isDefault: true, current: def ? def.description || def.name : '', checked: !c }];
  for (const d of devs) out.push({ name: d.name, label: d.description || d.name, checked: d.name === c });
  if (c && !devs.some((d) => d.name === c)) out.push({ name: c, label: c, missing: true, checked: true });
  return out;
}
// the chosen entry of soundDevChoices (what a menu button or summary shows)
export const soundDevChosen = (list, chosen) => soundDevChoices(list, chosen).find((x) => x.checked);

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
// A Windows guest's computer name (NetBIOS, as lord-vm's vm_winname_valid): at most 15, and also a valid hostname here
// (lord-vm takes it as the Clone's hostname and passes it on unchanged when it fits), not only digits. The Clone's
// default fits: the machine's name cut to 9, then "-clone"
export const WIN_NAME_MAX = 15;
export const winNameValid = (h) => hostnameValid(h) && h.length <= WIN_NAME_MAX && /[a-z-]/.test(h);
export const cloneWinName = (name) => `${hostnameFrom(name).slice(0, WIN_NAME_MAX - 6).replace(/-+$/, '')}-clone`;
// A machine whose machine.json this LordOS can't read (lord.vm.list's error): "newer" (a newer LordOS wrote it) or
// "invalid"; its numbers aren't known and it can't start
export const unreadableOf = (m) => (m?.error === 'newer' || m?.error === 'invalid' ? m.error : null);
// a template (5.1) doesn't start from the library: it stays as it was made, and New Machine from Template clones it
export const actionsOf = (m) => (unreadableOf(m) || (m?.meta?.template && !stateView(m.state, m.reason).active) ? { ...actionsFor(m.state), start: false } : actionsFor(m?.state));
export const isWindowsMachine = (m) => !!m && (!!m.windowsDevices || m.devices === 'windows');

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

// "Delete Old Snapshots" (6.16): every snapshot but the newest (lord.vm.snaps' list is in the order they were taken; a
// created time decides when two say otherwise). Nothing with one or none
export function oldSnapshots(list) {
  const xs = (Array.isArray(list) ? list : []).filter((s) => s && typeof s.tag === 'string');
  if (xs.length < 2) return [];
  const t = (s) => { const v = Date.parse(s.created || ''); return Number.isFinite(v) ? v : 0; };
  const newest = xs.reduce((a, s, i) => (t(s) >= t(xs[a]) ? i : a), 0);
  return xs.filter((s, i) => i !== newest);
}

// "Remove Unused" (6.16): the downloads no machine or group uses and that aren't a stopped download (catalog.js
// downloadedImages' entries); one in a running machine's drive is used
export const unusedImages = (imgs) => (Array.isArray(imgs) ? imgs : []).filter((x) => x && !x.partial && !(x.users || []).length && !(x.running || []).length);

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
// The machine's window around a start (6.5: a bridged machine's network is made by the admin helper first, which takes a
// few seconds): 'starting' while a start is on its way (even over an earlier run's state, which would say Off), 'off' with
// no session and a state, 'starting' with neither yet or with a session whose first state hasn't come, else 'session'
// (the session's own state decides)
export function startPhase(sid, st, starting) {
  if (!st || (!sid && starting)) return 'starting';
  if (!sid) return 'off';
  return 'session';
}
// a Play that reaches the window (its own button, or the library's while the window is open) starts the machine only when
// the window has no session, no start on its way and isn't closing
export const playStarts = (sid, starting, closing) => !sid && !starting && !closing;

// vm-net-bridge's argument (vm_admin.h): "<id> <card> <mac>"
export const bridgeArg = (m) => (m?.id && m?.bridge && m?.mac ? `${m.id} ${m.bridge} ${m.mac}` : '');

// ---- a graphics card for a machine (docs/MACHINES-GPU.md, Mode A: Intel GVT-g) ----

// machine.json's gpu (schema 11): none, the iGPU's largest share, or a share that fits the machine's picture
export const GPU_CHOICES = ['', 'intel-whole', 'intel-shared'];
const INTEL = 0x8086;
// lord.vm.gpus()'s Intel iGPU that isn't the desktop's and runs with GVT-g on (it lists mdev types), or null
export function gvtGpu(r) {
  return (r?.gpus || []).find((g) => g && g.vendor === INTEL && !g.desktop && g.gvtOn) || null;
}
// one that could be shared but isn't on yet (Settings → Graphics cards turns it on), or null
export function gvtSpare(r) {
  return (r?.gpus || []).find((g) => g && g.vendor === INTEL && !g.desktop && g.gvtCapable && !g.gvtOn) || null;
}
// "8086:9bc5": the PCI ids, the only honest name there is
const hex4 = (v) => ((v >>> 0) & 0xffff).toString(16).padStart(4, '0');
export const gpuIds = (g) => (g ? `${hex4(g.vendor)}:${hex4(g.device)}` : '');
// the picture a shared type must show: the machine's fixed resolution, or 1920 × 1080 when it follows the window
function wantSize(resolution) {
  const m = /^([0-9]{2,5})x([0-9]{2,5})$/.exec(String(resolution || ''));
  return m ? [Number(m[1]), Number(m[2])] : [1920, 1080];
}
// the mdev type a choice uses on that GPU: whole, the largest (by its memory, then its share of time); shared, the
// smallest that shows the machine's picture, else the largest. free: only types with an instance available (the
// shared choice then takes the next one up). null when none
export function gvtType(gpu, choice, resolution, free = false) {
  if (choice !== 'intel-whole' && choice !== 'intel-shared') return null;
  const up = (gpu?.types || []).filter((t) => t && t.name)
    .sort((a, b) => (a.highMb | 0) - (b.highMb | 0) || (a.weight | 0) - (b.weight | 0));
  if (!up.length) return null;
  const ok = (t) => !free || (t.available | 0) > 0;
  const largest = up[up.length - 1];
  // the whole iGPU is only ever the largest type: a smaller one isn't "whole"
  if (choice === 'intel-whole') return ok(largest) ? largest : null;
  const [w, h] = wantSize(resolution);
  return up.find((t) => ok(t) && (t.maxW | 0) >= w && (t.maxH | 0) >= h) || (ok(largest) ? largest : null);
}
// vm-gvt-create's argument (AGENTS.md "Machines' graphics cards"): "<machine id> <mdev type>"
export const gvtArg = (m, type) => (m?.id && type?.name ? `${m.id} ${type.name}` : '');
// a machine that can't be suspended or keep snapshots with its memory: 3D (virgl) running, or a graphics card
export const noSuspend = (st, gpu) => !!gpu || runs3d(st);
// a machine with a graphics card has its memory locked while it runs (VFIO): its memory plus QEMU's 512 MB, in MB
export const MEMLOCK_EXTRA_MB = 512;
export const memlockNeed = (m) => ((m?.memoryMb | 0) > 0 ? (m.memoryMb | 0) + MEMLOCK_EXTRA_MB : 0);
// whether that fits "Memory machines may lock" (lord.vm.gpus().memlock.capMb); true when the cap isn't known
export const memlockFits = (m, memlock) => !(memlock && memlock.capMb > 0) || memlockNeed(m) <= memlock.capMb;
// the largest memory (MB, a multiple of step) a machine with a graphics card may have under that cap; Infinity when unknown
export const memlockMaxMemory = (memlock, step = 256) => (memlock && memlock.capMb > 0
  ? Math.floor(Math.max(0, memlock.capMb - MEMLOCK_EXTRA_MB) / step) * step : Infinity);
// the summary line of Configure's Network section
export function netSummary(m) {
  const n = NET_MODES.includes(m?.network) ? m.network : 'shared';
  return { mode: n, card: n === 'bridged' ? m.bridge || '' : '', ports: usesPorts(n) ? (m.ports || []).length : 0,
    lan: usesPorts(n) && n !== 'hostonly' && (m.ports || []).some((r) => r.open === 'lan') };
}

// Why the guest tools or the clipboard helper weren't installed (VAG_INSTALL's answer, lord-vm's English words): the
// translated line's key under machines.agent.err, and lord-vm's own text as a detail when it says more than the key
const INSTALL_ERRORS = [
  [/aren't on this computer|isn't available for this machine's architecture/, 'missing'],
  [/^busy$|agent is busy/, 'busy'],
  [/can't write files or run commands/, 'noExec'],
  [/took too long/, 'timeout'],
  [/didn't make a private folder|didn't open the file/, 'noFolder'],
  [/started again|stopped answering/, 'gone'],
  [/script failed/, 'failed'],
];
export function installError(msg) {
  const text = String(msg || '').trim();
  const hit = INSTALL_ERRORS.find(([re]) => re.test(text));
  const key = hit ? hit[1] : 'other';
  // the words that add something: a script's output, qemu-guest-agent's error, anything not known
  const detail = key === 'failed' || key === 'other' ? text : '';
  return { key, detail };
}

// ---- apps mode (docs/VIRTUALIZATION.md 7.2): each window of the guest as its own LordOS window ----

// Why apps mode can't be turned on now, or null: 'needsAgent' (the machine's agent is off or not connected), 'needsTools'
// (the guest's helper doesn't list its windows: it needs LordOS or Windows in the machine, with the LordOS tools)
export function appsBlocker(agent, cfg) {
  if (!cfg?.agent || agent?.state !== 'connected') return 'needsAgent';
  return agent.caps & AGENT_CAPS.windows ? null : 'needsTools';
}

// The size the guest is asked for in apps mode: the display's, so its windows are never cut by a smaller screen (even,
// 320 × 200 to 8192 × 8192, as guestSize)
export const appsGuestSize = (d) => [Math.max(320, Math.min(8192, Math.round(d?.w || 0) & ~1)), Math.max(200, Math.min(8192, Math.round(d?.h || 0) & ~1))];

// Guest pixels → this computer's coordinates: the guest's screen (gw × gh) laid over the display d ({ x, y, w, h }), one
// scale for both axes (the smaller, so all of it fits); and back
export function guestScale(gw, gh, d) {
  if (!(gw > 0) || !(gh > 0) || !(d?.w > 0) || !(d?.h > 0)) return 1;
  return Math.min(d.w / gw, d.h / gh);
}
export const toHostRect = (r, k, d) => ({ x: Math.round(d.x + r.x * k), y: Math.round(d.y + r.y * k), w: Math.max(1, Math.round(r.w * k)), h: Math.max(1, Math.round(r.h * k)) });
export const toGuestRect = (r, k, d) => ({ x: Math.round((r.x - d.x) / k), y: Math.round((r.y - d.y) / k), w: Math.max(1, Math.round(r.w / k)), h: Math.max(1, Math.round(r.h / k)) });

// Apps mode with more than one display: the guest has one screen (gw × gh) laid over the main display d, but its LordOS
// windows can be anywhere, another monitor included (x ≥ the main display's width, or negative). A guest window asked
// to go outside its own screen is clamped, hidden or dropped from its list by the guest (Windows), and its LordOS window
// closed. So the place asked of the guest (ask) is always inside its screen, its size kept as far as it fits, and the
// LordOS window keeps where the person put it: off is how far it is from where the guest's window maps (0, 0 when it
// is over the guest's screen); the window then shows, and follows, the guest's window moved by off.
export function guestPlace(here, k, d, gw, gh) {
  const r = toGuestRect(here, k, d);
  const w = Math.max(1, Math.min(r.w, gw || r.w)), h = Math.max(1, Math.min(r.h, gh || r.h));
  const x = gw > 0 ? Math.max(0, Math.min(gw - w, r.x)) : r.x, y = gh > 0 ? Math.max(0, Math.min(gh - h, r.y)) : r.y;
  const ask = { x, y, w, h };
  const back = toHostRect(ask, k, d);
  const off = { x: here.x - back.x, y: here.y - back.y };
  return { ask, off: Math.abs(off.x) <= 2 && Math.abs(off.y) <= 2 ? null : off };
}
export const offsetRect = (r, off) => (r && off && (off.x || off.y) ? { ...r, x: r.x + off.x, y: r.y + off.y } : r);
// a guest window that went minimized right after this side moved it (the guest hid a window it thought off its screen):
// restored, not minimized here too
export const minimizedByUs = (askedAt, now, hostMinimized, wait = 3000) => !hostMinimized && !!askedAt && now - askedAt < wait;

// the part of the guest's picture a window shows (<MediaView crop>)
export const cropOf = (w) => [w.x, w.y, w.w, w.h];

// A point over a guest's window (layout box [x, y, w, h] of its picture, crop [x, y, w, h]) → the guest's absolute
// pointer position, inside its screen (gw × gh); null outside the box
export function cropPoint(px, py, box, crop, gw, gh) {
  if (!box || !crop || box[2] <= 0 || box[3] <= 0) return null;
  const lx = px - box[0], ly = py - box[1];
  if (lx < 0 || ly < 0 || lx >= box[2] || ly >= box[3]) return null;
  const x = Math.floor(crop[0] + lx * crop[2] / box[2]), y = Math.floor(crop[1] + ly * crop[3] / box[3]);
  return [Math.max(0, Math.min(gw - 1, x)), Math.max(0, Math.min(gh - 1, y))];
}

// A point while a button is held on a guest's window (dragging its own title bar, a selection): mapped as the window and
// its crop were when the button went down (anchor { box, crop }), never as they are now. The guest moves its window as
// the pointer goes and the LordOS window follows a moment later; mapped through the window as it is now, every move of
// the guest's window pushed the point further by as much again and the window ran away from the pointer. Outside the
// box too (the pointer may leave it during a drag); kept inside the guest's screen (gw × gh)
export function anchorPoint(px, py, anchor, gw, gh) {
  const box = anchor?.box, crop = anchor?.crop;
  if (!box || !crop || !(box[2] > 0) || !(box[3] > 0)) return null;
  const x = Math.floor(crop[0] + (px - box[0]) * crop[2] / box[2]), y = Math.floor(crop[1] + (py - box[1]) * crop[3] / box[3]);
  return [Math.max(0, Math.min(gw - 1, x)), Math.max(0, Math.min(gh - 1, y))];
}

// Apps mode: the guest's picture changed size (a change of resolution; the guest's displays moved around it): the window
// list the guest sent before it may be relative to the display as it was, and a crop of it shows part of one display,
// part of another, or black. Held (the windows show black, not a wrong part of the picture) until the guest's next list,
// unless a list came just before the size did (they cross), or for at most STALE_MS
export const STALE_MS = 1500;
export const cropsStale = (resizedAt, listedAt, now) => !!resizedAt && !(listedAt >= resizedAt - 400) && now - resizedAt < STALE_MS;

// Apps mode: a place asked of a guest's window (lord.vm.winAct move) that the guest didn't take, and won't: its window
// is still elsewhere (a smaller size than its minimum, a maximized window) a while after the ask. The LordOS window
// then goes back to where the guest's window is, or it would show the guest's picture squeezed into the wrong size.
export const guestKept = (asked, askedAt, got, now, wait = 1500) => !!asked && !!got && !nearRect(asked, got) && now - askedAt >= wait;

// Apps mode: where a guest window's picture (r, this computer's coordinates) goes so its LordOS frame (inset: the
// theme's chrome around the content) starts inside the work area (work: { x, y, w, h }), never under the menu bar or
// a bar at the start side; null when it fits as it is. The guest is then asked to move its window there too, so the
// window still shows its own part of the picture 1:1.
export function insideWork(r, inset, work) {
  if (!r || !work) return null;
  const i = inset || { l: 0, t: 0 };
  const x = Math.max(r.x, Math.round(work.x + (i.l || 0))), y = Math.max(r.y, Math.round(work.y + (i.t || 0)));
  return x === r.x && y === r.y ? null : { ...r, x, y };
}

// Files dropped on an apps mode window (7.2.1): one batch for the guest's tools (lord.vm.sendFile's place), at most
// PLACE_MAX files (the rest left out: left), the drop point in the guest's screen. batch ids go 1..32767 and around
export const PLACE_MAX = 8;
let placeSeq = 0;
export function placeBatch(files, win, pt, seq) {
  const list = (files || []).filter((f) => typeof f === 'string' && f.startsWith('/')).slice(0, PLACE_MAX);
  const batch = seq || (placeSeq = (placeSeq % 32767) + 1);
  const x = Math.max(0, Math.min(16384, Math.round(pt?.[0] || 0))), y = Math.max(0, Math.min(16384, Math.round(pt?.[1] || 0)));
  return { items: list.map((path, index) => ({ path, place: { win, x, y, batch, index, count: list.length } })), left: Math.max(0, (files || []).length - list.length) };
}

// Two rectangles the same within a few pixels (rounding between the two sides)
export const nearRect = (a, b, tol = 2) => !!a && !!b && Math.abs(a.x - b.x) <= tol && Math.abs(a.y - b.y) <= tol && Math.abs(a.w - b.w) <= tol && Math.abs(a.h - b.h) <= tol;

// A guest window drawn as a small frameless window on top (a menu, a tooltip: owned and topmost), not a framed one
export const isPopup = (w) => !!w && w.owner > 0 && w.topmost;

// The LordOS window's instance name for a guest window (wm.open('machines', { instance }))
export const guestInstance = (mid, id) => `ga-${String(mid || '').replace(/[^A-Za-z0-9-]/g, '')}-${Number(id) >>> 0}`;

// What changes between the guest's list and the LordOS windows open for it: open (new and not minimized, a sane size),
// close (gone from the list), minimize / restore (the guest's state changed), follow (the guest moved or resized it:
// the LordOS window follows, unless it's the place this side asked for). have: Map gid → { rect, minimized, asked }.
export function planWindows(list, have) {
  const open = [], close = [], follow = [], minimize = [], restore = [];
  const ids = new Set();
  for (const w of list || []) {
    ids.add(w.id);
    const h = have.get(w.id);
    if (!h) { if (!w.minimized && w.w >= 8 && w.h >= 8) open.push(w); continue; }
    if (w.minimized && !h.minimized) minimize.push(w);
    else if (!w.minimized && h.minimized) restore.push(w);
    if (!w.minimized && !nearRect(w, h.rect) && !(h.asked && nearRect(w, h.asked))) follow.push(w);
  }
  for (const gid of have.keys()) if (!ids.has(gid)) close.push(gid);
  return { open, close, follow, minimize, restore };
}

// The tracks (Map gid → { winId, icon }) whose icon is asked of lord.vm.winIcon now (appsmode.jsx): every one with a
// LordOS window but no icon yet, on each new list or new icons. Before 2026-11-22 the icons were asked only when the
// guest's icons changed, so a window opened from a later list (one minimized in the guest when apps mode began, or
// opened after the icons came, as on a re-attach) kept a letter tile in the Dock for good. iconsSeq: an icon already
// taken is asked again when the guest's icons changed since (an app that changed its icon)
export function iconsToAsk(tracks, iconsSeq) {
  const out = [];
  for (const x of tracks || []) if (x && x.winId && (!x.icon || x.iconSeq !== iconsSeq)) out.push(x);
  return out;
}

// The guest's apps for the Applications menu: by name, no repeats of a token
export function guestAppItems(apps) {
  const seen = new Set();
  return (apps || []).filter((a) => a && a.token && a.name && !seen.has(a.token) && seen.add(a.token)).sort((a, b) => a.name.localeCompare(b.name));
}

// Apps mode's Running list (the controller panel, the Applications menu): the guest's own windows, not its popups or the
// windows another one owns (a menu, a dialog: they come with their owner), nor a sliver no window opens for. Sorted by
// app, then title, then id: the guest's list is in stacking order, and a list that reordered on every click would jump.
export function runningWindows(list) {
  const out = [];
  for (const w of list || []) {
    if (!w || !Number.isFinite(w.id) || w.owner > 0 || isPopup(w)) continue;
    if (!w.minimized && !(w.w >= 8 && w.h >= 8)) continue;
    out.push({ id: w.id, title: String(w.title || w.app || ''), app: String(w.app || ''), minimized: !!w.minimized, focused: !!w.focused });
  }
  return out.sort((a, b) => a.app.localeCompare(b.app) || a.title.localeCompare(b.title) || a.id - b.id);
}
// what the Running rows show, as one string: menus are set again only when it changes, not on every move of a window
export const runningKey = (rows) => (rows || []).map((r) => `${r.id}\u0001${r.title}\u0001${r.minimized ? 1 : 0}${r.focused ? 1 : 0}`).join('\u0002');

// text as compared by a search: lower case, accents off ("Pequeño" matches "pequeno")
const folded = (s) => String(s || '').normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();
// The guest's apps that match a search (each word somewhere in the name), the ones whose name starts with it first; an
// empty search keeps them all, in their order
export function appsMatching(apps, query) {
  const words = folded(query).split(/\s+/).filter(Boolean);
  if (!words.length) return apps || [];
  const hits = [];
  for (const a of apps || []) {
    const n = folded(a?.name);
    if (!words.every((w) => n.includes(w))) continue;
    const rank = n.startsWith(words[0]) ? 0 : n.split(/[\s\-_.]+/).some((p) => p.startsWith(words[0])) ? 1 : 2;
    hits.push({ a, rank });
  }
  return hits.sort((x, y) => x.rank - y.rank).map((h) => h.a);
}
// the letter a guest app's row shows (the guest lists no icons for its apps): its first character, upper case
export const appInitial = (name) => (Array.from(String(name || '').trim())[0] || '?').toUpperCase();

// The guest's apps a launcher lists (the controller's panel, the status item's menu): the ones matching the search, each
// with its icon when the guest sent one (icons: token → pixels; none yet, so the initial tile shows instead)
export function launcherApps(apps, query, icons) {
  return appsMatching(apps, query).map((a) => ({ ...a, icon: (icons && a && icons[a.token]) || null }));
}

// The machines in apps mode, as their status items by the clock show them: one per machine with a session (guests: dir →
// what useAppsMode publishes), in name order so the items keep their places
export function appsStatusList(guests) {
  const out = [];
  for (const [dir, g] of Object.entries(guests || {})) if (g && g.sid) out.push({ dir, name: String(g.name || dir.split('/').pop() || '') });
  return out.sort((a, b) => a.name.localeCompare(b.name) || a.dir.localeCompare(b.dir));
}
// what to add and remove so the registered items (have: dirs) are the machines in apps mode now
export function appsStatusDiff(have, guests) {
  const now = appsStatusList(guests).map((x) => x.dir);
  const was = new Set(have || []);
  return { add: now.filter((d) => !was.has(d)), remove: [...was].filter((d) => !now.includes(d)) };
}
// the system a machine runs, for its mark (ui.jsx OsMark): the one kept in app.json, or guessed from its name
export const machineOsId = (meta, name) => meta?.os || systemOf(meta?.osName, name).id;

// Apps mode: the closes the guest refused (lord.vm.closeRefusals: its window runs as administrator in Windows, or the
// system refused) as guests[dir].closeRefused: gid → { reason: 'elevated' | 'other', at (ms since 1970) }; anything else
// the list holds is left out.
export function closeRefusedOf(list) {
  const out = {};
  for (const r of Array.isArray(list) ? list : []) {
    if (!r || !(Number.isInteger(r.gid) && r.gid > 0) || !(r.reason === 'elevated' || r.reason === 'other') || !Number.isFinite(r.at)) continue;
    out[r.gid] = { reason: r.reason, at: r.at };
  }
  return out;
}

// Launch feedback (the launcher's rows): an app opened in the guest may take seconds to show a window, or show it where
// the person doesn't look, so its row says "Opening…" and a click on it again is ignored until a new window of that app
// is listed or LAUNCH_WAIT_MS pass. The guest lists its apps by name and its windows by program file name, so a window
// belongs to an app when its program's name (without the extension) holds a word of the app's name (3 letters or more;
// "Microsoft Edge" → msedge.exe, "Word" → WINWORD.EXE) or its title holds the app's whole name.
export const LAUNCH_WAIT_MS = 5000;
export function windowOfApp(name, w) {
  const n = folded(name).trim();
  if (!n || !w) return false;
  const exe = folded(w.app).replace(/\.[a-z0-9]{1,4}$/, '');
  if (exe && n.split(/[^\p{L}\p{N}]+/u).some((p) => p.length >= 3 && exe.includes(p))) return true;
  return folded(w.title).includes(n);
}
// launches (token → { at, name, seen: the guest window ids listed when it was clicked }) with this one started
export function launchStart(launches, a, list, now) {
  return { ...(launches || {}), [a.token]: { at: now, name: String(a.name || ''), seen: (list || []).map((w) => w.id) } };
}
// the id of the new window that answered a launch, or 0
export function launchAnswered(entry, list) {
  if (!entry) return 0;
  const seen = new Set(entry.seen || []);
  const w = (list || []).find((x) => x && !seen.has(x.id) && !(x.owner > 0) && windowOfApp(entry.name, x));
  return w ? w.id : 0;
}
// whether an app's row still waits (shows "Opening…" and ignores clicks)
export const launchPending = (entry, list, now) => !!entry && now - entry.at < LAUNCH_WAIT_MS && !launchAnswered(entry, list);
// the launches still waiting (the others dropped)
export function launchPrune(launches, list, now) {
  const out = {};
  for (const [k, e] of Object.entries(launches || {})) if (launchPending(e, list, now)) out[k] = e;
  return out;
}

// Close feedback (Running's ×): the guest may refuse (Windows doesn't let a normal program close a window that runs as
// administrator). After ×, the row says "Closing…"; when the window is still listed CLOSE_WAIT_MS later, or the guest
// says it refused (refusal: { reason, at }, at ≥ the click), a note on the row says why. Gone from the list: nothing.
export const CLOSE_WAIT_MS = 3000;
export const closeStart = (closes, gid, now) => ({ ...(closes || {}), [gid]: { gid, at: now } });
// null, { state: 'closing' } or { state: 'refused', reason: 'elevated' | '' }
export function closeView(entry, list, refusal, now) {
  if (!entry) return null;
  if (!(list || []).some((w) => w && w.id === entry.gid)) return null;
  if (refusal && refusal.at >= entry.at) return { state: 'refused', reason: refusal.reason === 'elevated' ? 'elevated' : '' };
  return now - entry.at >= CLOSE_WAIT_MS ? { state: 'refused', reason: '' } : { state: 'closing' };
}
// the closes worth keeping: their window is still listed
export function closePrune(closes, list) {
  const ids = new Set((list || []).map((w) => w && w.id));
  const out = {};
  for (const [k, e] of Object.entries(closes || {})) if (ids.has(e.gid)) out[k] = e;
  return out;
}

// Where a machine starts from (Configure → Boot; machine.json's boot_order): "auto" (the system disk, then the CD/DVD
// drive) or the devices the firmware tries, in order, as lord-vm takes them ("network,disk").
export const BOOT_DEVS = ['disk', 'cd', 'network'];
// the order as the list Configure moves rows in: the chosen ones first, then the others (auto: the disk, the CD/DVD, the network)
export function bootList(order) {
  const got = typeof order === 'string' && order !== 'auto' ? order.split(',').filter((d, i, a) => BOOT_DEVS.includes(d) && a.indexOf(d) === i) : [];
  return [...got, ...BOOT_DEVS.filter((d) => !got.includes(d))];
}
// a row moved up (by -1) or down (by +1); out of range leaves the list as it was
export function bootMove(list, i, by) {
  const j = i + by;
  if (i < 0 || j < 0 || i >= list.length || j >= list.length) return list;
  const out = list.slice();
  [out[i], out[j]] = [out[j], out[i]];
  return out;
}
// the summary's words: null for auto, else the first device's key
export function bootSummary(order) {
  if (typeof order !== 'string' || order === 'auto' || !order) return null;
  return `machines.cfg.bootFirst.${bootList(order)[0]}`;
}

// ---- the `machines` command's session channel (cli.js; server/src/vm/vmcli.h) ----
// The command's exit statuses, also the answer's code (vmcli.h VMCLI_*)
export const CLI = { OK: 0, FAILED: 1, USAGE: 2, NOT_RUNNING: 3, NO_MACHINE: 4 };
// what a request does, from lord.vm.list(): { code, key, vars } to answer now (key under machinescli.*), with open: m when
// the machine's window opens first (openMachine: a start from there is Play's), or { detached: m } for a start without a
// window (startPrep, then startDetached). Only a machine of this library, by its folder exactly
export function cliPlan(req, list) {
  const m = (list || []).find((x) => x && typeof x.dir === 'string' && x.dir === req?.dir && !x.error);
  if (!m) return { code: CLI.NO_MACHINE, key: 'notInLibrary', vars: {} };
  const a = actionsFor(m.state);
  const vars = { name: m.name };
  const notNow = () => (a.open ? { code: CLI.OK, key: 'alreadyRunning', vars } : { code: CLI.FAILED, key: 'busy', vars });
  switch (req.verb) {
    case 'open': return a.open ? { code: CLI.OK, key: 'opened', vars, open: m } : { code: CLI.NOT_RUNNING, key: 'notRunning', vars };
    case 'start': return a.start ? { code: CLI.OK, key: 'startingWindow', vars, open: m } : notNow();
    case 'start-detached': return a.start ? { detached: m } : notNow();
    case 'release': return { code: CLI.OK, key: '', vars };    // reading the list gave back what a stopped machine held
    default: return { code: CLI.USAGE, key: '', vars };
  }
}
// lord-vm's own last word in a job's output ("lord-vm: …"), else the output
export const lordVmWhy = (out) => {
  const s = String(out || '').trim();
  return (s.split('\n').filter((l) => l.startsWith('lord-vm:')).pop()?.replace(/^lord-vm:\s*/, '') || s).slice(0, 400);
};

// A PopMenu's height (apps/machines/ui.jsx: 6 px padding and a 1 px edge on each side, 30 px a row, 11 a separator,
// about 24 an information row), for placing it before it's drawn
export const popMenuHeight = (items) => 14 + (items || []).filter(Boolean).reduce((s, it) => s + (it === '-' ? 11 : it.note ? 24 : 30), 0);

// Where a menu w × h opened at at ({ x, y }: its top-left on the desktop; above: the y its bottom goes to when it opens
// upwards, the button's top or the pointer) goes on the display b ({ x, y, w, h }, the one under at): below when it
// fits, else above, else as low as the display lets it; never past an edge (m from each). The window doesn't cut it:
// it's drawn over everything.
export function menuSpot(at, w, h, b, m = 8) {
  const bx = b?.x || 0, by = b?.y || 0, bw = b?.w || 0, bh = b?.h || 0;
  const left = Math.max(bx + m, Math.min(Math.round(at?.x || 0), bx + bw - w - m));
  const y = Math.round(at?.y || 0);
  if (y + h <= by + bh - m) return { left, top: Math.max(by + m, y) };
  const up = Math.round(at?.above ?? y) - h;
  return { left, top: up >= by + m ? up : Math.max(by + m, by + bh - h - m) };
}
// the display under a point (screen.displays), or the main one
export const displayUnder = (ds, x, y) => (ds || []).find((d) => x >= d.x && x < d.x + d.w && y >= d.y && y < d.y + d.h) || (ds || []).find((d) => d.main) || (ds || [])[0] || null;

// ---- machine groups (docs/VIRTUALIZATION.md 19.2, 19.9): a Tor gateway and a workstation started and stopped together ----
// the group's members as lord.vm.list() gives them (each with groupDir, role), keyed by role: { gateway, workstation }
export function groupMembers(group, list) {
  const out = {};
  for (const m of list || []) if (m && m.groupDir === group?.dir && typeof m.role === 'string' && !m.error && m.groupOk) out[m.role] = m;
  return out;
}
const RUNNING = (s) => s === 'running' || s === 'paused';
const ACTIVE = (s) => s === 'starting' || s === 'running' || s === 'paused' || s === 'stopping';
// one word for the whole group: 'running' (all members), 'off' (none), 'partial', or 'damaged' (a member missing or not
// this group's, or a base missing: 19.9, "A member folder, a base… missing or invalid")
export function groupState(group, list) {
  const ms = groupMembers(group, list), roles = (group?.members || []).map((x) => x.role);
  if (!roles.length || roles.some((r) => !ms[r])) return 'damaged';
  const on0 = roles.some((r) => ACTIVE(ms[r].state));
  if (!on0 && (group?.basesMissing || []).length) return 'damaged';
  const on = roles.filter((r) => ACTIVE(ms[r].state)).length;
  return on === 0 ? 'off' : on === roles.length && roles.every((r) => RUNNING(ms[r].state)) ? 'running' : 'partial';
}
// the next step of starting (action 'start') or stopping ('stop') the group, from the members' states now; called again
// after each step until it says done. Start: in group.json's start order (the gateway first), each member only once the
// one before it runs; the link once all run. Stop: in stop order (the workstation first), each once the one before it
// is off. { member, role, verb }: act on that member; { wait }: a member being started or stopped; { fail }: the plan
// stops (the gateway failed to start: the workstation isn't started, 19.9); { done } when there's nothing left.
export function groupPlan(group, list, action) {
  const ms = groupMembers(group, list);
  const order = [...(group?.members || [])].sort((a, b) => (action === 'stop' ? a.stop - b.stop : a.start - b.start));
  if (!order.length || order.some((x) => !ms[x.role])) return { fail: 'damaged' };
  // a base missing: a member off can't start (Repair makes it again first); one running keeps its open base
  if (action === 'start' && (group?.basesMissing || []).some((r) => ms[r] && !ACTIVE(ms[r].state))) return { fail: 'damaged' };
  for (const x of order) {
    const m = ms[x.role];
    if (action === 'stop') {
      if (m.state === 'stopped' || m.state === 'crashed' || !m.state) continue;
      if (m.state === 'stopping' || m.state === 'starting') return { wait: m };
      return { member: m, role: x.role, verb: 'shutdown' };
    }
    if (RUNNING(m.state)) continue;
    if (m.state === 'starting' || m.state === 'stopping') return { wait: m };
    return { member: m, role: x.role, verb: 'start' };
  }
  return action === 'stop' ? { done: true } : { done: true, link: true };
}
// a start the plan asked for ended without the member running (its state went back to stopped or crashed): the
// gateway's failure stops the group's start; the workstation's leaves the gateway running (19.9)
export function groupStartFailed(role) { return role === 'gateway' ? 'gatewayFailed' : 'workstationFailed'; }
// whether the group's link should be made now (lord.vm.link): both members run and their supervisors aren't the pair
// last linked. key: the members' lord-vm pids, so a member started anew (its QEMU gone, the old pair dead) is linked again
export function groupLinkKey(group, list) {
  const ms = groupMembers(group, list), roles = (group?.members || []).map((x) => x.role);
  if (roles.length < 2 || roles.some((r) => !ms[r] || !RUNNING(ms[r].state) || !(ms[r].vmPid > 0))) return '';
  return roles.map((r) => `${r}:${ms[r].vmPid}`).join(',');
}
// what a request of the `machines` command does with a group folder (vmcli.h: <dir> may end in .lordgroup): { code, key,
// vars } to answer now, { group, action: 'start', window } to run groupPlan, or { open: m } (the primary member's window)
export function groupCliPlan(req, groups, list) {
  const g = (groups || []).find((x) => x && x.dir === req?.dir && !x.error);
  if (!g) return { code: CLI.NO_MACHINE, key: 'notInLibrary', vars: {} };
  const vars = { name: g.name };
  const st = groupState(g, list);
  if (req?.verb === 'release') return { code: CLI.OK, key: '', vars };
  if (st === 'damaged') return { code: CLI.FAILED, key: 'groupDamaged', vars };
  const primary = Object.values(groupMembers(g, list)).find((m) => m.primary);
  switch (req.verb) {
    case 'open': return primary && RUNNING(primary.state) ? { code: CLI.OK, key: 'opened', vars, open: primary } : { code: CLI.NOT_RUNNING, key: 'notRunning', vars };
    case 'start': case 'start-detached':
      return st === 'running' ? { code: CLI.OK, key: 'alreadyRunning', vars } : { group: g, action: 'start', window: req.verb === 'start' };
    default: return { code: CLI.USAGE, key: '', vars };
  }
}

// ---- Machines and the rest of the system (docs/VIRTUALIZATION.md 6.14; ties.js) ----
// Search: machines whose name has every word of the query (q: lowercase, accents folded by the caller's fold), the
// name's start first; a group's members are found through their group's card, not one by one
export function searchMachines(list, q, fold = (s) => String(s || '').toLowerCase()) {
  const words = String(q || '').trim().split(/\s+/).filter(Boolean);
  if (!words.length) return [];
  const out = [];
  for (const m of list || []) {
    if (!m || m.error || m.groupDir) continue;
    const n = fold(m.name);
    if (!words.every((w) => n.includes(w))) continue;
    out.push({ m, score: n.startsWith(words[0]) ? 60 : 40 });
  }
  return out.sort((a, b) => b.score - a.score || String(a.m.name).localeCompare(String(b.m.name))).slice(0, 6);
}
// Start with the session: the machines settings.machinesAtLogin names by id (at most SESSION_START_MAX), off now and
// readable, a group's members left to their group; in the library's order. The list is in settings.json, which apps
// can't write (lord.fs's protected list): an app can't make a machine start at login.
export const SESSION_START_MAX = 8;
export function sessionStarts(list, ids) {
  const want = new Set((Array.isArray(ids) ? ids : []).filter((x) => typeof x === 'string').slice(0, SESSION_START_MAX));
  return (list || []).filter((m) => m && want.has(m.id) && !m.error && !m.groupDir && !stateView(m.state, m.reason).active);
}
// the ids after turning one machine on or off (at most the max, the newest kept)
export function atLoginToggle(ids, id, on) {
  const cur = (Array.isArray(ids) ? ids : []).filter((x) => typeof x === 'string' && x !== id);
  return (on ? [...cur, id] : cur).slice(-SESSION_START_MAX);
}
// On battery (bat: the battery store, lord.power.state()): once each time the computer leaves the charger while
// machines run, a notice with Pause All. mem: { was } between calls. → { notice: how many run, or 0; mem }
export function batteryNotice(bat, list, mem = {}) {
  const on = !!bat && !!bat.discharging;
  const running = (list || []).filter((m) => m && m.state === 'running').length;
  const notice = on && !mem.was && running > 0 ? running : 0;
  return { notice, mem: { was: on && (!!mem.was || running > 0) } };
}
// "Pause while I'm away" (Configure → Startup and shutdown; app.json autoPause): after AWAY_SEC without input the
// running machines that chose it pause; once input comes back, the ones this paused resume (a machine the person paused
// stays paused). paused: the dirs this paused. → { pause: [dirs], resume: [dirs] }
export const AWAY_SEC = 600;
export function awayStep(idleSec, list, metaOf, paused) {
  const mine = paused instanceof Set ? paused : new Set(paused || []);
  if (idleSec >= AWAY_SEC) {
    return { pause: (list || []).filter((m) => m && m.state === 'running' && !mine.has(m.dir) && metaOf(m.dir)?.autoPause === true).map((m) => m.dir), resume: [] };
  }
  return { pause: [], resume: (list || []).filter((m) => m && mine.has(m.dir) && m.state === 'paused').map((m) => m.dir) };
}

// Configure (5.5): which section each field lord-vm --set changes belongs to (the sidebar's dots: a section with a change
// not saved yet), and the sidebar's search over the sections' titles and summaries
export const FIELD_SECTION = {
  name: 'general', vcpus: 'cpu', memoryMb: 'cpu', cpuPin: 'cpu', cpuModel: 'cpu', perfProfile: 'cpu', resolution: 'display', graphics: 'display',
  gpu: 'display', pointer: 'display', captureFullscreen: 'display', network: 'network', bridge: 'network', ports: 'network',
  nicModel: 'network', diskGb: 'disks', iso: 'disks', shares: 'sharing', transfer: 'sharing', clipboard: 'sharing', agent: 'agent',
  bootOrder: 'boot', bootMenu: 'boot',
};
export function changedSections(draft, differs) {
  const out = new Set();
  for (const k of Object.keys(draft || {})) if (FIELD_SECTION[k] && differs(k, draft[k])) out.add(FIELD_SECTION[k]);
  return out;
}
export function sectionsMatching(secs, q, textOf, fold = (s) => String(s || '').toLowerCase()) {
  const words = fold(q).trim().split(/\s+/).filter(Boolean);
  if (!words.length) return secs;
  return secs.filter((k) => { const t = fold(textOf(k)); return words.every((w) => t.includes(w)); });
}

// ---- host USB devices (docs/VIRTUALIZATION.md 6.6) ----
// The Devices menu's rows from lord.vm.usbDevices() (this computer's devices) and the machine's poll (st.usb: what it holds,
// its controller, its lock, its auto-connect rules). A hub is never shown; a device another machine holds, or that lord-server
// won't offer (the session's own keyboard or mouse), is shown greyed with why. state: 'locked' (anonymity), 'nocontroller'
// (machine.json's usb off: Configure turns it on with the machine off), 'empty' or 'list'.
export const usbRule = (d) => `${d.vendor}:${d.product}${d.serial ? `:${d.serial}` : ''}`;
export const usbName = (d) => String(d?.model || d?.maker || `${d?.vendor || '????'}:${d?.product || '????'}`).slice(0, 48);
export function usbRows(devs, usb, sid) {
  if (!usb || usb.locked) return { state: 'locked', rows: [] };
  if (!usb.controller) return { state: 'nocontroller', rows: [] };
  const held = new Set((usb.devices || []).map((d) => `${d.bus}-${d.dev}`));
  const auto = usb.auto || [];
  const rows = (devs || []).filter((d) => d && d.why !== 'vm.usbHub').map((d) => {
    const on = held.has(`${d.bus}-${d.dev}`);
    const other = !on && d.holder > 0 && d.holder !== sid;
    const rule = usbRule(d), plain = `${d.vendor}:${d.product}`;
    return { bus: d.bus, dev: d.dev, name: usbName(d), on, why: d.why || (other ? 'vm.usbOtherMachine' : ''), camera: !!d.camera, audio: !!d.audio,
      storage: !!d.storage, rule, auto: auto.includes(rule) || auto.includes(plain), autoRule: auto.includes(rule) ? rule : auto.includes(plain) ? plain : rule };
  });
  return { state: rows.length ? 'list' : 'empty', rows };
}
// what the machine's USB devices capture now: the capture indicator's camera and microphone
export const usbCamLive = (usb) => !!(usb?.devices || []).some((d) => d.camera);
export const usbMicLive = (usb) => !!(usb?.devices || []).some((d) => d.audio);
// an attach refused for want of LordOS's permission: the one to ask for ('' none)
export const usbNeedsPerm = (code) => (code === 'vm.usbCameraPerm' ? 'camera' : code === 'vm.usbMicPerm' ? 'microphone' : '');
// Configure's list of auto-connect rules, with the device's name when it is plugged in now
export const usbAutoRows = (rules, devs) => (rules || []).map((r) => {
  const d = (devs || []).find((x) => usbRule(x) === r || `${x.vendor}:${x.product}` === r);
  return { rule: r, name: d ? usbName(d) : r };
});
