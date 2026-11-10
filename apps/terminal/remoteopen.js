// remoteopen.js — Open, Open With… and Edit for a file on the server (Files mode, docs/TERMINAL.md): the file comes down
// to a folder of its own under ~/.cache/lordos/terminal-remote and opens with the app chosen; with Edit, each save goes
// back up to the same path on the server. That is the only upload that replaces a file without asking: the person
// asked to edit that one. Watching ends when Files mode closes or the connection ends.
import { fs, openFileWithApps, t as T } from '@lordos/sdk';
import { mimesOf, appOpens } from '../../sdk/filekinds.js';
import { transfer, transfers, baseName, joinPath } from './sftp.js';

const CACHE = '.cache/lordos/terminal-remote';

// a transfer's end: its state once it isn't running ('done', 'error', 'cancelled')
const settled = (job) => new Promise((ok) => {
  const now = () => transfers.get().find((x) => x.job === job);
  const check = () => { const x = now(); if (x && x.state !== 'run') { off(); ok(x); return true; } return false; };
  const off = transfers.subscribe(check);
  check();
});

// the file on the server, here: its local path
export async function fetchRemote(conn, rpath) {
  const dir = `${fs.home()}/${CACHE}/${Date.now().toString(36)}${Math.floor(Math.random() * 1e6).toString(36)}`;
  await fs.mkdirAsync(`${fs.home()}/${CACHE}`).catch(() => {});
  await fs.mkdirAsync(dir);
  const dest = joinPath(dir, baseName(rpath));
  const r = await settled(await transfer('down', conn, { path: rpath, dest }, baseName(rpath)));
  if (r.state !== 'done') throw Object.assign(new Error(r.error || 'failed'), { code: r.error || 'failed' });
  return dest;
}

// who can open it: LordOS's own choice first (as Files' double click), then each Linux app that declares the type
export function openersFor(name, linuxApps) {
  const mimes = mimesOf({ name, dir: false }, typeof lord !== 'undefined' && lord.mime ? lord.mime.guess : null);
  const out = [{ id: '', label: T('terminal.files.openDefault'), run: (path) => openFileWithApps(path, name) }];
  for (const a of linuxApps || []) {
    if (a.kind === 'linux' && mimes.some((m) => appOpens(a.mimeTypes, m))) out.push({ id: a.id, label: a.name, run: (path) => lord.apps.launchWith(a.desktopId, path) });
  }
  return out;
}

// Edit: every change to the local copy (size or time, steady for one look) goes back up; stop() ends it
export function watchEdit(conn, rpath, local, perm, { onSaved, onError } = {}) {
  let last = null, seen = null, busy = false, live = true;
  const key = (st) => `${st.size}:${st.mtime}`;
  fs.statAsync(local).then((st) => { last = key(st); }, () => {});
  const id = setInterval(async () => {
    if (busy || !live || last == null) return;
    let st;
    try { st = await fs.statAsync(local); } catch (e) { return; }
    const k = key(st);
    if (k === last) { seen = null; return; }
    if (k !== seen) { seen = k; return; }                     // still being written: wait for a steady look
    busy = true;
    try {
      const r = await settled(await transfer('up', conn, { src: local, path: rpath, perm: perm & 0o777 }, baseName(rpath)));
      if (r.state === 'done') { last = k; seen = null; onSaved?.(); } else onError?.(r.error || 'failed');
    } catch (e) { onError?.(e.code || 'failed'); }
    busy = false;
  }, 1500);
  return () => { live = false; clearInterval(id); };
}

