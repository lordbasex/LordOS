// cloud.js — Cloud (lord.cloud, docs/CLOUD.md section 17): the person's cloud storage accounts.
//
//   const st = useCloud()             the last pushed State: { service, options, accounts, signin, recent }
//   cloud.providers() / beginSignIn(kind, name, { clientId, clientSecret }) / openSignInPage(id) / cancelSignIn(id)
//   cloud.add(kind, name, fields) / remove(id, { discardPending }) / rename(id, name) / refreshAbout(id) / websiteUrl(id)
//   cloud.status(paths) / keep(paths) / unkeep(paths) / free(paths)
//   cloud.link(path, expire) / unlink(path) / links(id)
//   cloud.pause(id|null) / resume(id|null) / setOptions({ … }) / restart() / showLog()
//
// A thin wrapper: every call goes to lord-server, which alone talks to rclone over its Unix socket; the state is
// pushed (__lordCloud, at most twice a second), never polled. Without lord.cloud (no lordos-rclone, an older
// server) available() is false and the rest answers empty. What reacts to state changes (notifications, the menu
// bar item) is in cloudstatus.jsx.
import { createStore, useStore } from './store.js';

const has = () => typeof lord !== 'undefined' && !!lord.cloud;
const call = (fn, d) => { if (!has()) return d; try { const r = fn(lord.cloud); return r === undefined ? d : r; } catch (e) { return d; } };
const read = () => call((c) => c.state(), null);

export const cloudState = createStore(read());

globalThis.__lordCloud = (st) => { cloudState.set(() => st); };

export const cloud = {
  available: () => has() && call((c) => !!c.available(), false),
  state: () => cloudState.get() || read(),
  providers: () => call((c) => c.providers(), []) || [],
  beginSignIn: (kind, name, opts = {}) => call((c) => c.beginSignIn(kind, name, opts), { error: 'cloud.unavailable' }),
  openSignInPage: (id) => call((c) => c.openSignInPage(id)),
  cancelSignIn: (id) => call((c) => c.cancelSignIn(id)),
  add: (kind, name, fields) => call((c) => c.add(kind, name, fields), { error: 'cloud.unavailable' }),
  remove: (id, opts = {}) => call((c) => c.remove(id, { discardPending: !!opts.discardPending }), { error: 'cloud.unavailable' }),
  rename: (id, name) => call((c) => c.rename(id, name), { error: 'cloud.unavailable' }),
  refreshAbout: (id) => call((c) => c.refreshAbout(id)),
  websiteUrl: (id) => call((c) => c.websiteUrl(id), null),
  status: (paths) => (paths && paths.length ? call((c) => c.status(paths), {}) || {} : {}),
  keep: (paths) => call((c) => c.keep(paths)),
  unkeep: (paths) => call((c) => c.unkeep(paths)),
  free: (paths) => call((c) => c.free(paths), { freed: 0, skipped: [] }),
  link: (path, expire) => call((c) => c.link(path, expire), { error: 'cloud.unavailable' }),
  unlink: (path) => call((c) => c.unlink(path), { error: 'cloud.unavailable' }),
  links: (id) => call((c) => c.links(id), []) || [],
  pause: (id = null) => call((c) => c.pause(id)),
  resume: (id = null) => call((c) => c.resume(id)),
  setOptions: (o) => call((c) => c.setOptions(o)),
  restart: () => call((c) => c.restart()),
  showLog: () => call((c) => c.showLog()),
  // Not in section 17 yet (docs/CLOUD.md, "Interface notes"): an account's own options (Settings → Cloud → the
  // account: only on unmetered networks, its upload limit). Without it those rows are disabled.
  accountOptionsSupported: () => has() && typeof lord.cloud.setAccountOptions === 'function',
  setAccountOptions: (id, o) => call((c) => (typeof c.setAccountOptions === 'function' ? c.setAccountOptions(id, o) : undefined)),
};

export const useCloud = () => useStore(cloudState);
