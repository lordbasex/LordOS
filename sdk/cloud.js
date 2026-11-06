// cloud.js — Cloud: the person's cloud storage accounts (lord.cloud, server/src/cloud, docs/CLOUD.md section 17).
//
//   const st = useCloud()              live State: { available, service, options, accounts, signin, recent }
//   cloud.available() / cloud.state()
//   cloud.providers() → Promise<[{ kind, label, description, group, verified, website?, fields, questions? }]> (asked fresh, 17.2)
//   cloud.beginSignIn(kind, name, { clientId, clientSecret }) → { id, url, expiresAt } | { error }
//   cloud.openSignInPage(id) / cloud.cancelSignIn(id)
//   cloud.add(kind, name, fields) → { id } | { question } | { error, field? }   cloud.answer(state, value) → Promise, the same (17.2)
//   cloud.remove(id, { discardPending }) → { ok } | { error: 'pending', files }
//   cloud.rename(id, name) / refreshAbout(id) / websiteUrl(id)
//   cloud.status(paths) → { [path]: { st, pct?, error? } }       cloud.keep(paths) / unkeep(paths)
//   cloud.free(paths) → { freed, skipped: [{ path, reason }] }
//   cloud.link(path, expire) → Promise<{ url } | { error }>       cloud.unlink(path) → Promise<{ ok } | { error }>
//   cloud.links(id) → [{ path, url, expires, made }]
//   cloud.emptyTrash(id) → Promise   cloud.measure(id) → Promise<{ bytes, count }>   cloud.delete(paths) → Promise (17.2)
//   cloud.pause(id|null) / resume(id|null) / setOptions({ … }) / restart() / showLog()
//
// lord-server's cloud thread is rclone's only client; JS never reaches it. The state is pushed (__lordCloud),
// at most twice a second, never polled. The two calls that talk to the provider (link, unlink) answer later.
import { createStore, useStore } from './store.js';
import { wm } from './windows.js';
import { defaultOpenerId } from './defaults.js';

const has = () => typeof lord !== 'undefined' && !!lord.cloud;
const EMPTY = {
  available: false,
  service: { running: false, version: '', uptimeS: 0, rssKb: 0, restarts: 0 },
  options: { bwUp: 0, bwDown: 0, pauseOnBattery: true, pauseOnMetered: true, autoFree: true, freeBelowGb: 10 },
  accounts: [], signin: null, recent: [],
};
const read = () => { try { return (has() && lord.cloud.state()) || EMPTY; } catch (e) { return EMPTY; } };

export const cloudState = createStore(read());
const waiting = new Map();      // req → resolve

globalThis.__lordCloud = (st) => { if (st && typeof st === 'object') cloudState.set(() => st); };
globalThis.__lordCloudReply = (req, res) => {
  const done = waiting.get(req);
  if (done) { waiting.delete(req); done(res); }
};

const later = (r) => (r && typeof r.then === 'function' ? r : r && r.req ? new Promise((resolve) => waiting.set(r.req, resolve)) : Promise.resolve(r || { error: 'cloud.failed' }));
// a call of 17.2 the running lord-server may not have yet
const maybe = (name, ...args) => (has() && typeof lord.cloud[name] === 'function' ? later(lord.cloud[name](...args)) : Promise.resolve({ error: 'cloud.unavailable' }));
const paths = (p) => (Array.isArray(p) ? p : [p]).map(String);

// lord-server opens the sign-in page itself; if the default browser isn't a Linux app it can launch, the session does
function openHere(url) {
  try {
    const def = defaultOpenerId('x-scheme-handler/http') || defaultOpenerId('x-scheme-handler/https');
    return !!def && lord.apps.launchUri(def.replace(/\.desktop$/, ''), url);
  } catch (e) { return false; }
}

export const cloud = {
  available: () => has() && !!lord.cloud.available(),
  state: () => cloudState.get(),
  // 17.2: a Promise, asked of rclone every time (an older server answered a list at once)
  providers: () => (has() ? later(lord.cloud.providers()).then((l) => (Array.isArray(l) ? l : l && l.error ? Promise.reject(l) : [])) : Promise.reject({ error: 'cloud.unavailable' })),
  beginSignIn: (kind, name, opts = {}) => (has() ? lord.cloud.beginSignIn(String(kind), String(name), {
    clientId: opts.clientId ? String(opts.clientId) : '', clientSecret: opts.clientSecret ? String(opts.clientSecret) : '',
  }) : { error: 'cloud.unavailable' }),
  openSignInPage(id) {
    if (!has()) return false;
    if (lord.cloud.openSignInPage(String(id))) return true;
    const s = cloudState.get().signin;
    return !!(s && s.id === id && s.url) && openHere(s.url);
  },
  cancelSignIn: (id) => has() && lord.cloud.cancelSignIn(String(id)),
  // { id } | { question: { state, name, help, type, examples, isPassword, required, default } } | { error } (17.2 item 3)
  add: (kind, name, fields = {}) => (has() ? later(lord.cloud.add(String(kind), String(name), fields)) : Promise.resolve({ error: 'cloud.unavailable' })),
  answer: (state, value) => maybe('answer', String(state), String(value ?? '')),
  emptyTrash: (id) => maybe('emptyTrash', String(id)),
  measure: (id) => maybe('measure', String(id)),
  cancelMeasure: (id) => maybe('cancelMeasure', String(id)),
  // removes them in the cloud through the mount (the provider's own trash where it has one): paths strictly under ~/Cloud/<account>
  delete: (p) => maybe('delete', paths(p)),
  remove: (id, { discardPending = false } = {}) => (has() ? lord.cloud.remove(String(id), { discardPending: !!discardPending }) : { error: 'cloud.unavailable' }),
  rename: (id, name) => (has() ? lord.cloud.rename(String(id), String(name)) : { error: 'cloud.unavailable' }),
  refreshAbout: (id) => has() && lord.cloud.refreshAbout(String(id)),
  websiteUrl: (id) => (has() ? lord.cloud.websiteUrl(String(id)) : null),
  status: (p) => (has() ? lord.cloud.status(paths(p)) || {} : {}),
  keep: (p) => has() && lord.cloud.keep(paths(p)),
  unkeep: (p) => has() && lord.cloud.unkeep(paths(p)),
  free: (p) => (has() ? lord.cloud.free(paths(p)) : { freed: 0, skipped: [] }),
  link: (path, expire = 'never') => (has() ? later(lord.cloud.link(String(path), String(expire))) : Promise.resolve({ error: 'cloud.unavailable' })),
  unlink: (path) => (has() ? later(lord.cloud.unlink(String(path))) : Promise.resolve({ error: 'cloud.unavailable' })),
  links: (id) => (has() ? lord.cloud.links(String(id)) || [] : []),
  pause: (id = null) => has() && lord.cloud.pause(id == null ? undefined : String(id)),
  resume: (id = null) => has() && lord.cloud.resume(id == null ? undefined : String(id)),
  setOptions: (o) => (has() ? lord.cloud.setOptions({ ...o }) : { error: 'cloud.unavailable' }),
  restart: () => has() && lord.cloud.restart(),
  showLog: () => wm.open('logs', { new: true, args: { file: has() ? lord.cloud.logName() : 'cloud.log' } }),
  // Not provided by lord-server in 0.8.8.5 (docs/CLOUD.md 17.1: one bandwidth limit for the whole daemon, metered
  // networks not known yet): an account's own options. Without it Settings disables those rows.
  accountOptionsSupported: () => has() && typeof lord.cloud.setAccountOptions === 'function',
  setAccountOptions: (id, o) => (has() && typeof lord.cloud.setAccountOptions === 'function' ? lord.cloud.setAccountOptions(String(id), o) : undefined),
};

export const useCloud = () => useStore(cloudState);
