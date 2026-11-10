// forwards.js — port forwards over the tab's own ssh session (docs/TERMINAL.md, "Port forwards"). A profile lists
// "server host:port → 127.0.0.1:port" (the local port automatic or fixed); they open when an ssh started in one of its
// sessions connects, through that ssh's control socket (lord.sftp's forward: MUX_C_OPEN_FWD in server/src/term/sftpc.c,
// no program run), and end with the connection. The tab's badge lists them and adds more for this connection only.
import { createStore, term, clipboard, openUrl, notify, t as T } from '@lordos/sdk';
import { meta, patchMeta } from './state.js';
import { getProfile } from './profiles.js';
import { request } from './sftp.js';
import { sshEnded } from './dropupload.js';

// sid → { pid, items: [{ key, host, port, want, local, state: 'wait'|'opening'|'open'|'error', error, own }] }
// pid is the connected ssh's: a new one (the user ran ssh again) starts the list again.
export const forwards = createStore({});

const HOST = /^[A-Za-z0-9._:-]{1,253}$/;
// a forward as the profile keeps it, checked as lord-server will (sftpc.c do_forward): null when it can't be used
export function cleanForward(f) {
  const host = String(f?.host || '').trim() || 'localhost';
  const port = Number(f?.port), local = f?.local === '' || f?.local == null ? 0 : Number(f.local);
  if (!HOST.test(host) || !Number.isInteger(port) || port < 1 || port > 65535) return null;
  if (!Number.isInteger(local) || (local !== 0 && (local < 1024 || local > 65535))) return null;
  return { host, port, want: local };
}
// what the badge and the list show: "80 → 54321", or "db:5432 → 54322" for another machine
export const forwardLabel = (it) => `${it.host === 'localhost' || it.host === '127.0.0.1' ? '' : `${it.host}:`}${it.port} → ${it.local || it.want || '…'}`;
// an address worth opening in the browser: web ports, or any port when asked from the list
export const webPort = (port) => [80, 443, 3000, 4000, 5000, 5173, 8000, 8080, 8443, 8888, 9000].includes(Number(port));
export const localUrl = (it) => `${Number(it.port) === 443 || Number(it.port) === 8443 ? 'https' : 'http'}://127.0.0.1:${it.local}`;

let seq = 0;
const patch = (sid, fn) => forwards.set((all) => {
  const cur = all[sid];
  if (!cur) return all;
  return { ...all, [sid]: { ...cur, items: fn(cur.items) } };
});
const setItem = (sid, key, p) => patch(sid, (items) => items.map((x) => (x.key === key ? { ...x, ...p } : x)));

async function open(sid, it, tries = 0) {
  setItem(sid, it.key, { state: 'opening', error: '' });
  try {
    const r = await request('forward', { sid, host: it.host, port: it.port, ...(it.want ? { local: it.want } : {}) });
    if (!forwards.get()[sid]?.items.some((x) => x.key === it.key)) {      // closed (or the ssh ended) meanwhile
      request('forward', { sid, host: it.host, port: it.port, local: r.local, close: true }).catch(() => {});
      return null;
    }
    setItem(sid, it.key, { state: 'open', local: r.local });
    return { ...it, local: r.local };
  } catch (e) {
    // the control socket shows up once ssh has signed in: wait for it (a password or a code can take a while)
    if (e.code === 'nossh' && tries < 90) { setItem(sid, it.key, { state: 'wait' }); await new Promise((ok) => setTimeout(ok, 2000)); return forwards.get()[sid]?.items.some((x) => x.key === it.key) ? open(sid, it, tries + 1) : null; }
    setItem(sid, it.key, { state: 'error', error: e.code || 'failed' });
    return null;
  }
}

// a new connection: the profile's forwards, and one notice for those that opened
async function connected(sid, pid) {
  const list = (getProfile(meta.get()[sid]?.profile).forwards || []).filter((f) => f && f.enabled !== false).map(cleanForward).filter(Boolean);
  const items = list.map((f) => ({ ...f, key: ++seq, local: 0, state: 'wait', error: '', own: false }));
  forwards.set((all) => ({ ...all, [sid]: { pid, items } }));
  if (!items.length) return;
  const done = (await Promise.all(items.map((it) => open(sid, it)))).filter(Boolean);
  if (!done.length || forwards.get()[sid]?.pid !== pid) return;
  const first = done.find((x) => webPort(x.port)) || done[0];
  notify({ app: 'terminal', title: T('terminal.fwd.opened', { count: done.length }), body: done.map(forwardLabel).join(' · '),
    actions: [
      ...(webPort(first.port) ? [{ id: 'open', label: T('terminal.fwd.openBrowser'), primary: true, run: () => openUrl(localUrl(first)) }] : []),
      { id: 'copy', label: T('terminal.fwd.copy'), run: () => clipboard.set(`127.0.0.1:${first.local}`) },
    ] });
}

// every 2 s: which sessions run a connected ssh (the pane's foreground process, as the Files switch reads it)
function tick() {
  const all = forwards.get();
  for (const sid of Object.keys(meta.get())) {
    const info = term.info(Number(sid));
    const pid = info && info.busy && info.process === 'ssh' ? info.fgPid : 0;
    const cur = all[sid];
    if (pid && cur?.pid !== pid) { if (cur) sshEnded(Number(sid)); patchMeta(Number(sid), { remoteCwd: '' }); connected(Number(sid), pid); }
    else if (!pid && cur) {                                    // the master took its forwards (and the drop connection) with it
      forwards.set((x) => { const n = { ...x }; delete n[sid]; return n; });
      sshEnded(Number(sid));
      patchMeta(Number(sid), { remoteCwd: '' });
    }
  }
  for (const sid of Object.keys(all)) if (!meta.get()[sid]) forwards.set((x) => { const n = { ...x }; delete n[sid]; return n; });
}
let timer = null;
export function startForwards() { if (!timer) timer = setInterval(tick, 2000); }

// from the tab's list: one more for this connection only, or one closed
export function addForward(sid, f) {
  const c = cleanForward(f);
  if (!c || !forwards.get()[sid]) return false;
  const it = { ...c, key: ++seq, local: 0, state: 'wait', error: '', own: true };
  patch(sid, (items) => [...items, it]);
  open(sid, it);
  return true;
}
export function closeForward(sid, key) {
  const it = forwards.get()[sid]?.items.find((x) => x.key === key);
  if (!it) return;
  patch(sid, (items) => items.filter((x) => x.key !== key));
  if (it.state === 'open') request('forward', { sid, host: it.host, port: it.port, local: it.local, close: true }).catch(() => {});
}
// the forwards of the sessions of a tab (its panes), for the badge
export const forwardsOf = (all, sids) => sids.flatMap((sid) => (all[sid]?.items || []).map((it) => ({ ...it, sid })));
