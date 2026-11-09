// sftp.js — the Terminal's Files mode, the part without a screen (docs/TERMINAL.md "Files mode"): the client of
// lord.sftp (server/src/term/sftpc.c; a request answers through poll()), the transfers queue with its speed history,
// and the pure helpers the view and server/tests/terminal_sftp_test.mjs use.
import { createStore } from '@lordos/sdk';

// ---- pure ----

// "rwxr-x---" from the permission bits; the kind's letter first: d, l or -
export function permString(perm, kind = '-') {
  const p = Number(perm) || 0;
  const bits = ['r', 'w', 'x'];
  let s = kind === 'd' ? 'd' : kind === 'l' ? 'l' : '-';
  for (let i = 8; i >= 0; i--) s += p & (1 << i) ? bits[(8 - i) % 3] : '-';
  return s;
}
// the 3x3 boxes of the Permissions sheet ↔ the octal number
export const permBoxes = (perm) => [0, 1, 2].map((who) => [0, 1, 2].map((what) => !!((Number(perm) || 0) & (1 << (8 - who * 3 - what)))));
export const boxesPerm = (boxes) => boxes.reduce((p, row, who) => row.reduce((q, on, what) => (on ? q | (1 << (8 - who * 3 - what)) : q), p), 0);
export const octal = (perm) => `0${((Number(perm) || 0) & 0o7777).toString(8).padStart(3, '0')}`;
export function parseOctal(s) {
  const t = String(s || '').trim();
  return /^0?[0-7]{3,4}$/.test(t) ? parseInt(t, 8) & 0o7777 : null;
}

// paths on the server: always absolute, '/'-separated
export const parentOf = (p) => { const s = String(p || '/').replace(/\/+$/, ''); const i = s.lastIndexOf('/'); return i <= 0 ? '/' : s.slice(0, i); };
export const joinPath = (dir, name) => `${String(dir || '/').replace(/\/+$/, '')}/${name}`;
export const baseName = (p) => String(p || '').replace(/\/+$/, '').split('/').pop() || '/';
// a name typed by the user for a new folder or a rename: one path segment
export const goodName = (n) => { const s = String(n || ''); return !!s.trim() && s.length <= 255 && !/[/\x00-\x1f\x7f]/.test(s) && s !== '.' && s !== '..'; };

// folders first, then by name (numbers in order), hidden ones last only when shown
// names compared without case, runs of digits as numbers ("a2" before "a10"); no Intl in lord-server's QuickJS
export function naturalCompare(x, y) {
  const re = /(\d+)|(\D+)/g;
  const a = String(x).toLowerCase().match(re) || [], b = String(y).toLowerCase().match(re) || [];
  for (let i = 0; i < Math.min(a.length, b.length); i++) {
    const da = /^\d/.test(a[i]), db = /^\d/.test(b[i]);
    if (da && db) { const d = Number(a[i]) - Number(b[i]); if (d) return d; }
    else if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
  }
  return a.length - b.length || (String(x) < String(y) ? -1 : String(x) > String(y) ? 1 : 0);
}
export function sortEntries(list, by = 'name', desc = false) {
  const coll = { compare: naturalCompare };
  const key = { name: (a, b) => coll.compare(a.name, b.name), size: (a, b) => (a.size || 0) - (b.size || 0) || coll.compare(a.name, b.name),
    mtime: (a, b) => (a.mtime || 0) - (b.mtime || 0) || coll.compare(a.name, b.name) }[by] || ((a, b) => coll.compare(a.name, b.name));
  return [...(list || [])].sort((a, b) => {
    const da = a.kind === 'd' || a.dir, db = b.kind === 'd' || b.dir;
    if (da !== db) return da ? -1 : 1;
    const c = key(a, b);
    return desc ? -c : c;
  });
}
export const visible = (list, hidden, filter) => {
  const f = String(filter || '').toLowerCase();
  return (list || []).filter((e) => (hidden || !e.name.startsWith('.')) && (!f || e.name.toLowerCase().includes(f)));
};

// 1536 → "1.5 KB"
export function size(n) {
  const v = Number(n) || 0;
  if (v < 1024) return `${v} B`;
  const u = ['KB', 'MB', 'GB', 'TB'];
  let x = v, i = -1;
  do { x /= 1024; i++; } while (x >= 1024 && i < u.length - 1);
  return `${x >= 100 ? Math.round(x) : x.toFixed(1)} ${u[i]}`;
}
export const rate = (bps) => `${size(bps)}/s`;

// The free name for a download that shouldn't replace: "name (2).ext", "name (3).ext"…
export function freeName(name, taken) {
  if (!taken.has(name)) return name;
  const m = /^(.*?)(\.[^.]{1,10})?$/.exec(name);
  for (let i = 2; i < 1000; i++) { const n = `${m[1]} (${i})${m[2] || ''}`; if (!taken.has(n)) return n; }
  return name;
}

// The speed graph: bytes per second for uploads and downloads, the last `n` seconds, from the jobs' totals over time
export function speedSeries(samples, n = 60, now = Date.now()) {
  const out = Array.from({ length: n }, () => ({ up: 0, down: 0 }));
  for (const s of samples || []) {
    const k = n - 1 - Math.floor((now - s.at) / 1000);
    if (k >= 0 && k < n) out[k][s.kind === 'up' ? 'up' : 'down'] += s.bytes;
  }
  return out;
}

// ---- the client ----

const has = () => typeof lord !== 'undefined' && lord.sftp && typeof lord.sftp.request === 'function';
export const available = () => { try { return has() && lord.sftp.available(); } catch (e) { return false; } };

const pending = new Map();                    // request id → { resolve, reject }
let timer = null;
const errOf = (d) => Object.assign(new Error((d && d.message) || (d && d.error) || 'failed'), { code: (d && d.error) || 'failed', detail: d && d.message });

// transfers: [{ job, kind: 'up'|'down', name, from, to, conn, done, total, state: 'run'|'done'|'error'|'cancelled', error, at }]
export const transfers = createStore([]);
export const closedConns = createStore(0);    // goes up when a connection ended on its own (the ssh closed)
let samples = [];                             // { at, kind, bytes } for the speed graph
export const samplesOf = () => samples;

function pump() {
  let items = [];
  try { items = lord.sftp.poll(); } catch (e) { items = []; }
  for (const it of items) {
    if (it.type === 'evt') { onEvent(it.data || {}); continue; }
    const p = pending.get(it.id);
    if (!p) continue;
    pending.delete(it.id);
    if (it.type === 'ok') p.resolve(it.data || {}); else p.reject(errOf(it.data));
  }
  const running = transfers.get().some((x) => x.state === 'run');
  if (!pending.size && !running) { clearInterval(timer); timer = null; }
}
const ensure = () => { if (!timer) timer = setInterval(pump, 100); };

// events of a job not in the list yet (a small file can finish before its request's answer is read): applied when it is added
const early = new Map();
function onEvent(d) {
  if (d.ev === 'closed') { closedConns.set((n) => n + 1); return; }
  if (!d.job) return;
  if (!transfers.get().some((x) => x.job === d.job)) { early.set(d.job, [...(early.get(d.job) || []), d].slice(-8)); return; }
  transfers.set((list) => list.map((x) => {
    if (x.job !== d.job) return x;
    if (d.ev === 'progress') {
      const delta = Math.max(0, (d.done || 0) - (x.done || 0));
      if (delta) samples.push({ at: Date.now(), kind: x.kind, bytes: delta });
      return { ...x, done: d.done || 0, total: d.total || x.total };
    }
    if (d.ev === 'done') return { ...x, state: 'done', done: d.bytes ?? x.total, total: d.bytes ?? x.total };
    if (d.ev === 'error') return { ...x, state: d.error === 'cancelled' ? 'cancelled' : 'error', error: d.error, detail: d.message };
    return x;
  }));
  const cut = Date.now() - 61000;
  if (samples.length > 4000 || (samples[0] && samples[0].at < cut)) samples = samples.filter((s) => s.at >= cut);
}

export function request(op, args = {}) {
  if (!has()) return Promise.reject(errOf({ error: 'unavailable' }));
  let id;
  try { id = lord.sftp.request(JSON.stringify({ op, ...args })); } catch (e) { return Promise.reject(errOf({ error: 'bad' })); }
  if (id < 0) return Promise.reject(errOf({ error: 'unavailable' }));
  ensure();
  return new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
}

// a transfer: queued in `transfers`, its progress from the events
export async function transfer(kind, conn, args, label) {
  const r = await request(kind === 'up' ? 'put' : 'get', { conn, ...args });
  transfers.set((list) => [...list.filter((x) => x.state === 'run' || Date.now() - x.at < 10 * 60000).slice(-49),
    { job: r.job, kind, name: label, conn, done: 0, total: r.size || 0, state: 'run', at: Date.now(), args }]);
  const before = early.get(r.job);
  early.delete(r.job);
  for (const d of before || []) onEvent(d);
  ensure();
  return r.job;
}
export const cancel = (job) => request('cancel', { job }).catch(() => {});
export const clearFinished = () => transfers.set((list) => list.filter((x) => x.state === 'run'));
