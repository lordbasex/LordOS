// storagemodel.js — the pure logic of a machine's storage choices (docs/VIRTUALIZATION.md 6.2 disposable sessions, 6.4
// raw disks and Export, 3.6 the kept UEFI code, 8 encrypted machines). No lord.* here: server/tests/vm_storage_model_test.mjs.

// what a key's state asks of the app before a machine (or group) can start, be copied or exported (lord.vm.keyState):
// go on, wait, unlock the session's keyring (the system's own prompt), ask the person for the recovery key, or stop
export function keyStep(state) {
  switch (state) {
    case 'none': case 'ready': return 'go';
    case 'pending': return 'wait';
    case 'locked': return 'unlock';
    case 'missing': return 'recovery';
    default: return 'fail';
  }
}

// a recovery key as typed or pasted: spaces, dashes and line breaks ignored, any case; '' when it isn't 64 hex digits
export function normalizeRecoveryKey(text) {
  const s = String(text || '').replace(/[\s-]/g, '').toLowerCase();
  return /^[0-9a-f]{64}$/.test(s) ? s : '';
}
// shown (in Passwords, or an export's note) in eight groups of eight, the way it's easiest to copy by hand
export function formatRecoveryKey(hex) {
  const s = normalizeRecoveryKey(hex);
  return s ? s.match(/.{8}/g).join('-') : '';
}

// the disposable session's three choices, in Configure's order; machine.json "disposable" (lord.vm.list's "off" when none)
export const DISPOSABLE = ['off', 'memory', 'disk'];
export const disposableOf = (m) => (DISPOSABLE.includes(m?.disposable) ? m.disposable : 'off');
// a disposable session can be chosen for any machine but a group's member (its group has the switch, 19.7)
export const disposableAllowed = (m) => !!m && !m.group;
// what a session in memory may hold before the guest pauses: the machine's memory, at least 1 GB (lordvm.c)
export const disposableCapMb = (m) => Math.max(1024, m?.memoryMb || 0);

// Export (6.4): which formats a machine can go out as, and why one can't. An encrypted machine goes only as .lordvm (an
// OVA is read by other apps, which can't open its disks); a running machine, a group's member, a saved session: none
export function exportRefusal(m, format, { running = false } = {}) {
  if (!m) return 'gone';
  if (running) return 'running';
  if (m.group) return 'member';
  if (m.suspended) return 'suspended';
  if (format === 'ova' && m.encrypted) return 'encryptedOva';
  return '';
}
// the name the save dialog offers: the machine's name with what a file name can't hold replaced, and the extension
export function exportName(m, format) {
  const base = String(m?.name || 'Machine').replace(/[/\\:*?"<>|\u0000-\u001f]+/g, '-').replace(/^[.\s-]+|[\s.-]+$/g, '').slice(0, 60) || 'Machine';
  return `${base}.${format === 'ova' ? 'ova' : 'lordvm'}`;
}
// the destination as lord-vm wants it: the extension added when the person left it out
export function exportTarget(path, format) {
  const ext = format === 'ova' ? '.ova' : '.lordvm';
  if (!path || typeof path !== 'string' || !path.startsWith('/')) return '';
  return path.toLowerCase().endsWith(ext) ? path : `${path}${ext}`;
}
// lord-vm's progress lines ({"progress":P}) and its last line ({"exported":…} or {"imported":…}) out of a job's text
export function jobProgress(out) {
  let pct = null, last = null;
  for (const line of String(out || '').split('\n')) {
    const l = line.trim();
    if (!l.startsWith('{')) continue;
    try {
      const j = JSON.parse(l);
      if (typeof j.progress === 'number') pct = j.progress;
      if (j.exported || j.imported) last = j;
    } catch (e) { /* not ours */ }
  }
  return { pct, last };
}
// lord-vm's reason on a failed job: its last "lord-vm: …" line
export const jobError = (out) => String(out || '').trim().split('\n').filter((l) => l.startsWith('lord-vm:')).pop()?.replace(/^lord-vm:\s*/, '') || '';

// the Security pane's line about the disks (8): encrypted, and where the key is; or not, and that it's chosen at creation
export function encryptionNote(m) {
  if (!m) return '';
  return m.encrypted ? 'encrypted' : m.diskFormat === 'raw' ? 'rawPlain' : 'plain';
}
// the library card's small marks for these choices, in order
export function storageBadges(m) {
  const out = [];
  if (m?.encrypted) out.push('encrypted');
  if (disposableOf(m) !== 'off') out.push('disposable');
  if (m?.diskFormat === 'raw') out.push('raw');
  return out;
}
