// admin.js — administration actions through the server's privileged helper
// (server/src/admin.c). Closed list: region, time zone, languages, packages, network and VPN (the
// `actions` table in admin.c). The non-network ones only work if the session user
// is an administrator (sudo group). Several can run at once.
//
//   await admin.run('locale-gen', 'pt_BR.UTF-8', (line) => showProgress(line))
//   const off = admin.onEvent((text) => ...)    // unsolicited notices: "net link eth0 up"...
//
// An ERR reply is a code line (server/src/errcode.h): the promise rejects with an Error whose
// message is already translated (t('errors.<code>')) and that carries .code and .params. OK and
// LOG texts arrive as sent; show a status code line with statusText() (sdk/errors.js).
import { createStore } from './store.js';
import { codeError } from './errors.js';

const pending = new Map();              // id → { action, arg, onLog, resolve, reject }
const eventListeners = new Set();
// The most recent visible action (to show progress): { action, arg, log: [] } or null.
export const adminBusy = createStore(null);

function refreshBusy() {
  const last = [...pending.values()].filter((p) => !p.quiet).pop();
  adminBusy.set(last ? { action: last.action, arg: last.arg, log: last.log } : null);
}

globalThis.__lordAdmin = (id, kind, text) => {
  if (id === 0) {
    for (const fn of eventListeners) { try { fn(text); } catch (e) { console.log(`admin.onEvent: ${e}`); } }
    return;
  }
  const p = pending.get(id);
  if (!p) return;
  // PROGRESS <n> (wallpaper-get, studio-bg-get): a line of progress like LOG, never the action's answer. Before 0.8.9.6.2
  // it ended the request as an error whose text was the first percent ("0"): the file still downloaded, but "Download
  // and use" never got to use it.
  if (kind === 'LOG' || kind === 'PROGRESS') {
    if (kind === 'PROGRESS') text = `PROGRESS ${text}`;
    p.onLog?.(text);
    p.log = [...p.log, text].slice(-200);
    if (!p.quiet) refreshBusy();
    return;
  }
  pending.delete(id);
  refreshBusy();
  if (kind === 'OK') p.resolve(text); else p.reject(codeError(text));
};

export const admin = {
  available: () => lord.admin.available(),
  // quiet: not shown as "in progress" (quick queries, such as the network state).
  run(action, arg = '', onLog, { quiet = false } = {}) {
    return new Promise((resolve, reject) => {
      const id = lord.admin.request(action, String(arg));
      if (!id) { reject(codeError('admin.unavailable')); return; }
      pending.set(id, { action, arg: quiet ? '' : arg, onLog, resolve, reject, quiet, log: [] });   // quiet: may carry a password
      if (!quiet) refreshBusy();
    });
  },
  onEvent(fn) { eventListeners.add(fn); return () => eventListeners.delete(fn); },
  busy: adminBusy,
};
