// dropupload.js — files dropped on a pane that runs ssh go up to the server (docs/TERMINAL.md, "In terminal mode"): to
// the remote shell's directory when it reports one (OSC 7 while ssh runs, meta.remoteCwd), else the remote home, over
// the tab's own ssh (lord.sftp, as Files mode). Nothing is typed into the terminal and it is never blocked; a name
// already on the server is never replaced: the copy takes a free name ("notes (2).txt"). ⌥ when dropping pastes the
// paths as before (Panes.jsx).
import { createStore, fs, term, userDirs, t as T } from '@lordos/sdk';
import { meta, toast } from './state.js';
import { request, transfer, freeName, joinPath, baseName, fromLocal, errCode, goodName } from './sftp.js';

// sid → { pid, conn, jobs: [job], hidden }: one sftp connection per ssh, kept while it lives
export const drops = createStore({});

const sshPid = (sid) => { const i = term.info(sid); return i && i.busy && i.process === 'ssh' ? i.fgPid : 0; };

// the pane runs a connected ssh: the drop is an upload (true), else the caller pastes the paths
export function dropUpload(wid, sid, paths) {
  if (!sshPid(sid) || !paths?.length) return false;
  run(wid, sid, paths);
  return true;
}

async function connOf(sid) {
  const pid = sshPid(sid), cur = drops.get()[sid];
  if (cur?.conn && cur.pid === pid) return cur.conn;
  if (cur?.conn) request('close', { conn: cur.conn }).catch(() => {});
  const r = await request('open', { sid });
  drops.set((d) => ({ ...d, [sid]: { pid, conn: r.conn, jobs: [], hidden: false } }));
  return r.conn;
}

// the ssh ended (forwards.js's watch): its connection goes with it
export function sshEnded(sid) {
  const cur = drops.get()[sid];
  if (!cur) return;
  if (cur.conn) request('close', { conn: cur.conn }).catch(() => {});
  drops.set((d) => { const n = { ...d }; delete n[sid]; return n; });
}

const addJob = (sid, job) => drops.set((d) => (d[sid] ? { ...d, [sid]: { ...d[sid], jobs: [...d[sid].jobs, job].slice(-50), hidden: false } } : d));
export const hideDrops = (sid) => drops.set((d) => (d[sid] ? { ...d, [sid]: { ...d[sid], hidden: true } } : d));
export const clearDrops = (sid, keep) => drops.set((d) => (d[sid] ? { ...d, [sid]: { ...d[sid], jobs: d[sid].jobs.filter(keep) } } : d));

async function run(wid, sid, paths) {
  let conn;
  try { conn = await connOf(sid); } catch (e) { toast(wid, T('terminal.drop.cant', { why: T(`terminal.files.err.${errCode(e)}`) })); return; }
  let dir = meta.get()[sid]?.remoteCwd || '';
  if (!dir) { try { dir = (await request('list', { conn, path: '.' })).path || '.'; } catch (e) { dir = '.'; } }
  let there = [];
  try { there = ((await request('list', { conn, path: dir })).entries || []).map((r) => r.name); } catch (e) { there = []; }
  const taken = new Set(there);
  drops.set((d) => (d[sid] ? { ...d, [sid]: { ...d[sid], dir } } : d));     // the notice shows where they go
  const renamed = [];
  // a folder goes up whole (a new one, so nothing inside it can clash), at most 20 levels deep
  let links = 0;
  const walk = async (src, rd, name, depth) => {
    const st = fromLocal(await fs.statAsync(src));
    if (st.link) { links++; return; }                       // never followed: it could lead out of what was dropped
    if (st.kind === 'd') {
      if (depth > 20) return;
      await request('mkdir', { conn, path: joinPath(rd, name) });
      for (const e of await fs.listAsync(src)) await walk(joinPath(src, e.name), joinPath(rd, name), e.name, depth + 1);
    } else if (st.kind === '-') {
      addJob(sid, await transfer('up', conn, { src, path: joinPath(rd, name), perm: st.perm & 0o777 }, name));
    }
  };
  for (const p of paths) {
    const name = baseName(p), free = freeName(name, taken);
    taken.add(free);
    if (free !== name) renamed.push(free);
    try { await walk(p, dir, free, 0); } catch (e) { toast(wid, `${name}: ${T(`terminal.files.err.${errCode(e)}`)}`); }
  }
  if (renamed.length) toast(wid, T('terminal.drop.renamed', { name: renamed[0], count: renamed.length }));
  if (links) toast(wid, T('terminal.drop.links', { count: links }));
}

// ❖ + click on a name in `ls` output (Panes.jsx, lsnames.js): it comes down to Downloads, resolved against the remote
// shell's directory (the remote home when it doesn't report one); a folder comes down whole, links are not followed,
// and a name already in Downloads gets a free one. False when the pane doesn't run ssh.
export function downloadName(wid, sid, name) {
  if (!sshPid(sid) || !name) return false;
  getOne(wid, sid, name);
  return true;
}

async function getOne(wid, sid, name) {
  let conn;
  try { conn = await connOf(sid); } catch (e) { toast(wid, T('terminal.drop.cant', { why: T(`terminal.files.err.${errCode(e)}`) })); return; }
  const base = meta.get()[sid]?.remoteCwd || '';
  const path = base ? joinPath(base, name) : name;
  let st;
  try { st = await request('stat', { conn, path }); } catch (e) {
    toast(wid, T(base ? 'terminal.ls.notFound' : 'terminal.ls.notFoundHome', { name }));
    return;
  }
  const dl = userDirs().downloads || fs.home();
  await fs.mkdirAsync(dl).catch(() => {});                  // there already, as a rule
  let here = [];
  try { here = (await fs.listAsync(dl)).map((e) => e.name); } catch (e) { here = []; }
  const local = freeName(name, new Set(here));
  drops.set((d) => (d[sid] ? { ...d, [sid]: { ...d[sid], dir: dl } } : d));
  const walk = async (rp, lp, kind, depth) => {
    if (kind === 'd') {
      if (depth > 20) return;
      await fs.mkdirAsync(lp);
      // names come from the server: lord-sftp already drops ".", "..", "/" and control characters; checked again here
      for (const e of (await request('list', { conn, path: rp })).entries || []) if (goodName(e.name)) await walk(joinPath(rp, e.name), joinPath(lp, e.name), e.kind, depth + 1);
    } else if (kind === '-') {
      addJob(sid, await transfer('down', conn, { path: rp, dest: lp }, baseName(lp)));
    }
  };
  try { await walk(st.path || path, joinPath(dl, local), st.kind, 0); } catch (e) { toast(wid, `${name}: ${T(`terminal.files.err.${errCode(e)}`)}`); }
  if (local !== name) toast(wid, T('terminal.ls.renamed', { name: local }));
}
