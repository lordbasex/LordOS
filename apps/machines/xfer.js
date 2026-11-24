// xfer.js — the machine window's file copy panel (docs/VIRTUALIZATION.md 6.21): files on their way into the guest (drops,
// lord.vm.files) and files the guest sends with "Send to LordOS" (lord.vm.received, into the machine's quarantine folder),
// one list, with speed and time left. Pure, so it is tested in server/tests/vm_xfer_test.mjs.
//
// A row: { key, dir: 'in' (into the guest) | 'out' (from the guest), id, name, size, done (bytes), state: 'running' |
// 'paused' | 'done' | 'failed', why ('' or one of cancelled, badHash, failed, noSpace, later, or a drop's message), path
// (a received file once kept), exec (runnable in the guest), speed (bytes/s, 0 unknown), left (s, -1 unknown), until (ms:
// a finished row stays shown until then) }

export const XFER_KEEP_MS = 8000;           // a finished row stays this long
const WINDOW_MS = 4000;                     // the speed is measured over this much of the past

// lord.vm.files()'s entry as a row's fields
const fromDrop = (f) => ({
  key: `in:${f.id}`, dir: 'in', id: f.id, name: String(f.name || ''), size: Math.max(0, Number(f.size) || 0),
  done: Math.round(Math.max(0, Number(f.size) || 0) * Math.min(100, Math.max(0, Number(f.pct) || 0)) / 100),
  state: f.state === 'copying' ? 'running' : f.state === 'done' ? 'done' : 'failed',
  why: f.state === 'failed' ? (/cancelled/.test(String(f.message || '')) ? 'cancelled' : String(f.message || '')) : '',
  path: '', exec: false, fast: f.via === 'transfer', open: !!f.open, place: !!f.place,
});
// lord.vm.received()'s entry as a row's fields
const fromRecv = (r) => ({
  key: `out:${r.id}`, dir: 'out', id: r.id, name: String(r.name || ''), size: Math.max(0, Number(r.size) || 0),
  done: Math.max(0, Math.min(Number(r.got) || 0, Number(r.size) || 0)),
  state: r.state === 'receiving' ? 'running' : ['paused', 'done', 'failed'].includes(r.state) ? r.state : 'failed',
  why: r.state === 'failed' ? String(r.code || 'failed') : r.state === 'paused' ? 'later' : '',
  path: r.state === 'done' ? String(r.path || '') : '', exec: !!r.exec, folder: String(r.folder || ''), marked: !!r.marked,
});

// the next list: prev's rows updated with what came now (drops and receives may be partial updates: a list holds only the
// entries the server still has, finished ones once), finished rows kept XFER_KEEP_MS, speed from the last WINDOW_MS
export function xferStep(prev, { drops, recvs } = {}, now = Date.now()) {
  const by = new Map((prev || []).map((r) => [r.key, r]));
  const seen = new Set();
  const add = (n) => {
    const o = by.get(n.key);
    const samples = [...(o?.samples || []), [now, n.done]].filter(([t]) => now - t <= WINDOW_MS);
    const first = samples[0], last = samples[samples.length - 1];
    const speed = samples.length > 1 && last[0] > first[0] ? Math.max(0, (last[1] - first[1]) * 1000 / (last[0] - first[0])) : o?.speed || 0;
    const left = n.state === 'running' && speed > 0 ? Math.ceil((n.size - n.done) / speed) : -1;
    const fin = n.state === 'done' || n.state === 'failed';
    by.set(n.key, { ...o, ...n, samples, speed: n.state === 'running' ? speed : 0, left, until: fin ? (o?.until || now + XFER_KEEP_MS) : 0, t0: o?.t0 || now });
    seen.add(n.key);
  };
  if (Array.isArray(drops)) for (const f of drops) if (f && f.id) add(fromDrop(f));
  if (Array.isArray(recvs)) for (const r of recvs) if (r && r.id) add(fromRecv(r));
  return [...by.values()].filter((r) => !r.until || r.until > now).sort((a, b) => a.t0 - b.t0 || (a.key < b.key ? -1 : 1));
}

// the rows the panel shows now (finished ones until their time); the panel hides when none
export const xferShown = (rows, now = Date.now()) => (rows || []).filter((r) => !r.until || r.until > now);
// the panel's own state: rows still running (a Cancel each), the next time it must look again (a finished row going)
export const xferBusy = (rows) => (rows || []).some((r) => r.state === 'running' || r.state === 'paused');
export function xferNextExpiry(rows, now = Date.now()) {
  const t = (rows || []).filter((r) => r.until > now).map((r) => r.until);
  return t.length ? Math.min(...t) - now : -1;
}

// "1.2 MB of 48 MB · 85 MB/s · 3 s left"'s parts, for the language's line: { done, size, speed, left } (sizes as fmt
// formats them; left in whole seconds, minutes past 90 s); null parts are left out
export function xferFigures(r, fmt) {
  const f = typeof fmt === 'function' ? fmt : (n) => String(n);
  return {
    done: f(r.done || 0), size: f(r.size || 0),
    speed: r.state === 'running' && r.speed > 0 ? f(Math.round(r.speed)) : null,
    left: r.state === 'running' && r.left >= 0 ? (r.left > 90 ? { min: Math.ceil(r.left / 60) } : { s: Math.max(1, r.left) }) : null,
  };
}

// A received file whose name says it would run here (a script, an installer, a program, a desktop entry) or that ran in
// the guest: Files and this panel never open it by itself (docs/VIRTUALIZATION.md 6.21, "quarantine")
const RUNNABLE = /\.(sh|bash|zsh|fish|csh|ksh|run|bin|appimage|deb|rpm|flatpak|flatpakref|snap|desktop|py|pl|rb|js|mjs|jar|exe|msi|bat|cmd|com|scr|ps1|vbs|lnk|elf|so|out|x86_64|aarch64)$/i;
export const runnableName = (name) => RUNNABLE.test(String(name || ''));
export const xferRunnable = (r) => !!r && (r.exec || runnableName(r.name));
