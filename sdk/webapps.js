// webapps.js — web apps in the desktop (docs/WEBAPPS.md): the installed ones (lord.webapps.list), starting one
// with a profile, the probe of an address, and the permission questions lord-web asks (org.lordos.WebApps1.Ask,
// shown by <WebAppAskPrompt /> in hardware.jsx). Pure rules live in webappsmodel.js.
//
//   webApps (store) · refreshWebApps() · useWebApps() · webAppsAvailable()
//   launchWebApp(app, profile?)    the first profile when none is named; a running one is brought forward (by lord-server)
//   probeWebApp(url)               → promise of the probe's answer (lord-web --probe, at most 20 s)
//   installWebApp(entry) · webAppCall(fn, ...args)   (addProfile, renameProfile, removeProfile, setPermission,
//                                  setBackground, uninstall: each refreshes the list)
//   webAppOfWindow(w)              { entry, profile } for a window of a lord-web we started, else null
//   webAppMemory(app)              lord.webapps.memory: { kb, processes, profiles } while it runs, else null
//   webEngine()                    { installed, version, webkit }: the lordos-web package and the WebKit it links
//   webAsks (store) · answerWebAsk(ask, allow, remember)
//   webEditMenu(w)                 the Edit menu's items for a web app's window (lord.webapps.edit), else null
//   webMenu (store) · webMenuEvent(kind, data) · answerWebMenu(item)   a page's right click (org.lordos.WebApps1.Menu):
//                                  LordOS's own menu at the pointer, with items from lord-server's closed list
//   webBadges (store) · webBadgeEvent(data)    the unread counts lord-web set (SetBadge): { [app]: { [profile]: n } }
//   siteIcons (store) · wantSiteIcon(app) · refreshSiteIcon(app)   the site's own icon, fetched one at a time with the
//                                  probe (lord.webapps.fetchIcon) and kept in ~/.cache/lordos/web/icons/
//
// Only lord-server starts lord-web, with an app and a profile from the registry; nothing here names a URL to
// open or a program to run.
import { createStore, useStore } from './store.js';
import { useEffect } from 'react';
import { defaultProfile, parseWindowAppId, windowWebApp, askAnswer, upstreamVersion, badgeReducer, wantsSiteIcon, webEditItems, RUNTIME_PACKAGE, WEBKIT_PACKAGE } from './webappsmodel.js';
import { t } from './i18n.js';

const has = () => typeof lord !== 'undefined' && typeof lord.webapps === 'object' && lord.webapps !== null;

export const webApps = createStore([]);
export const webAsks = createStore([]);
export const webBadges = createStore({});
export const webMenu = createStore(null);
export const siteIcons = createStore({});

const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

export function refreshWebApps() {
  if (!has()) return;
  let list = [];
  try { list = lord.webapps.list() || []; } catch (e) { list = []; }
  if (!same(list, webApps.get())) webApps.set(list);
}

export function useWebApps() {
  useEffect(refreshWebApps, []);
  return useStore(webApps);
}

export function webAppsAvailable() {
  try { return has() && lord.webapps.available() === true; } catch (e) { return false; }
}

export const webAppEntry = (app) => webApps.get().find((e) => e.id === app) || null;

export function webRunning() {
  try { return has() ? lord.webapps.running() || [] : []; } catch (e) { return []; }
}

// Starts an app with a profile (the first one by default). Throws the server's error (with .code).
export function launchWebApp(app, profile) {
  if (!has()) return false;
  if (!webAppEntry(app)) refreshWebApps();
  const entry = webAppEntry(app);
  const p = profile || defaultProfile(entry)?.id;
  if (!entry || !p) return false;
  // 'running': lord-server has already asked that lord-web to show its window (an activation token over the bus),
  // which also brings it back from behind its tray icon; nothing else to do here
  lord.webapps.launch(app, p);
  // a catalog app installed before LordOS fetched sites' icons: its own icon, once, without waiting for it
  if (entry.source === 'catalog' && !entry.siteIcon) wantSiteIcon(app);
  return true;
}

// The probe's answer: { ok, name, startUrl, scope, origin, themeColor, manifest, iconPath, iconKind } or
// { ok: false, error }. onStart(id) is told the probe's id (the sheet names it when installing).
export function probeWebApp(url, onStart) {
  if (!has()) return Promise.resolve({ ok: false, error: 'webapps.noRuntime' });
  let id;
  try { id = lord.webapps.probe(url); } catch (e) { return Promise.resolve({ ok: false, error: e.code || 'webapps.probeFailed' }); }
  onStart?.(id);
  return new Promise((resolve) => {
    const t0 = Date.now();
    const timer = setInterval(() => {
      let r = null;
      try { r = lord.webapps.probeResult(id); } catch (e) { r = { ok: false, error: 'webapps.probeFailed' }; }
      if (!r && Date.now() - t0 < 45000) return;   // two steps (page, icon) of at most 20 s each
      clearInterval(timer);
      resolve({ ...(r || { ok: false, error: 'webapps.probeTimeout' }), id });
    }, 250);
  });
}

export function installWebApp(entry) {
  const app = lord.webapps.install(entry);
  refreshWebApps();
  return app;
}

// One of the registry's calls by name, then the list again. Throws the server's error.
const CALLS = ['addProfile', 'renameProfile', 'removeProfile', 'setPermission', 'setBackground', 'uninstall', 'setTray', 'setNotifySound', 'setProfileColor'];
export function webAppCall(name, ...args) {
  if (!has() || !CALLS.includes(name)) throw new Error('unavailable');
  try { return lord.webapps[name](...args); } finally { refreshWebApps(); }
}

// lord-server says whose process a window is; asked once, when it maps (wayland.js puts it in args.web).
export function webOwnerOf(surface, appId) {
  if (!has() || !parseWindowAppId(appId)) return null;
  try { return lord.webapps.owner(surface) || null; } catch (e) { return null; }
}

export function webAppOfWindow(w) {
  const web = w?.args?.web;
  if (!web) return null;
  if (!webAppEntry(web.app)) refreshWebApps();
  return windowWebApp(w.args.appId, web, webApps.get());
}

// The memory a running app takes now (read-only; an older server without memory() answers null).
export function webAppMemory(app) {
  try { return has() && typeof lord.webapps.memory === 'function' ? lord.webapps.memory(app) : null; } catch (e) { return null; }
}

// The engine, from the package index (no program is run): lordos-web's version and the WebKit version it links.
export function webEngine() {
  const info = (name) => { try { return typeof lord !== 'undefined' && lord.pkg ? lord.pkg.info(name, {}) : null; } catch (e) { return null; } };
  const own = info(RUNTIME_PACKAGE), wk = info(WEBKIT_PACKAGE);
  return { installed: !!(own && own.installed), version: (own && own.installed) || '', webkit: upstreamVersion(wk && wk.installed) };
}

// ---- the site's own icon ----
//
// One probe at a time (each is a WebKit page load: about 220 MB for some seconds), queued; a failure is not tried
// again this session, except by refreshSiteIcon (Settings → Web apps → Refresh icon).
const tried = new Set();
const queue = [];
let busy = null;

function readIcons() {
  try { const m = has() && typeof lord.webapps.icons === 'function' ? lord.webapps.icons() : null; if (m && typeof m === 'object') siteIcons.set(m); } catch (e) { /* an older server */ }
}

function pump() {
  if (busy || !queue.length) return;
  const app = queue.shift();
  let id = 0;
  try { id = lord.webapps.fetchIcon(app); } catch (e) { id = 0; }
  if (!id) { pump(); return; }
  busy = app;
  const t0 = Date.now();
  const timer = setInterval(() => {
    let r = null;
    try { r = lord.webapps.iconResult(app); } catch (e) { r = { done: true, ok: false }; }
    if (!r && Date.now() - t0 < 45000) return;
    clearInterval(timer);
    busy = null;
    if (r && r.ok) { readIcons(); refreshWebApps(); }
    pump();
  }, 500);
}

function enqueue(app, force) {
  if (!has() || typeof lord.webapps.fetchIcon !== 'function' || busy === app || queue.includes(app)) return;
  const cached = !!siteIcons.get()[app];
  if (!force && !wantsSiteIcon({ available: webAppsAvailable(), cached, tried: tried.has(app) })) return;
  if (force && !webAppsAvailable()) return;
  tried.add(app);
  queue.push(app);
  pump();
}
// The Store's cards and an app's launch: fetched once when lord-web is there and nothing is cached yet.
export const wantSiteIcon = (app) => enqueue(app, false);
// Settings → Web apps → Refresh icon: fetched again now.
export const refreshSiteIcon = (app) => enqueue(app, true);
export const siteIconBusy = (app) => busy === app || queue.includes(app);

// ---- unread counts ----

export function webBadgeEvent(data) {
  const next = badgeReducer(webBadges.get(), data);
  if (next !== webBadges.get()) webBadges.set(next);
}

// ---- permission questions ----

export function webAskEvent(kind, data) {
  if (!data || typeof data.id !== 'number') return;
  if (kind === 'webapps-ask') {
    if (!webAppEntry(data.app)) refreshWebApps();
    if (!webAsks.get().some((a) => a.id === data.id)) webAsks.set([...webAsks.get(), data]);
  } else webAsks.set(webAsks.get().filter((a) => a.id !== data.id));
}

export function answerWebAsk(ask, allow, remember) {
  webAsks.set(webAsks.get().filter((a) => a.id !== ask.id));
  try { lord.webapps.answer(ask.id, askAnswer(allow, remember)); } catch (e) { /* the app left meanwhile */ }
  if (remember) refreshWebApps();
}

// ---- editing ----

// The Edit menu of a window of a lord-web we started: each item asks lord-server to run that command in its page.
export function webEditMenu(w) {
  const web = w?.args?.web;
  if (!web || !has() || typeof lord.webapps.edit !== 'function') return null;
  return webEditItems((c) => t(`shell.edit.${c}`), (c) => { try { lord.webapps.edit(web.app, web.profile, c); } catch (e) { /* it ended */ } });
}

// A page's right click: shown at the pointer by <WebAppMenu /> (hardware.jsx); one at a time.
export function webMenuEvent(kind, data) {
  if (!data || typeof data.id !== 'number') return;
  const cur = webMenu.get();
  if (kind === 'webapps-menu') {
    if (cur && cur.id !== data.id) answerWebMenu('', cur);
    let at = { x: 0, y: 0 };
    try { const p = lord.wm.pointer(); at = { x: p.x, y: p.y }; } catch (e) { /* no pointer: the corner */ }
    webMenu.set({ ...data, ...at });
  } else if (cur && cur.id === data.id) webMenu.set(null);
}

// once per menu: a later answer for the same menu (its close after a pick) is ignored
export function answerWebMenu(item, menu = webMenu.get()) {
  if (!menu || webMenu.get()?.id !== menu.id) return;
  webMenu.set(null);
  try { lord.webapps.menuAnswer(menu.id, item || ''); } catch (e) { /* the app left meanwhile */ }
}

if (has()) {
  refreshWebApps();
  readIcons();
  try {
    const list = typeof lord.webapps.badges === 'function' ? lord.webapps.badges() : [];
    if (Array.isArray(list)) webBadges.set(list.reduce(badgeReducer, {}));
  } catch (e) { /* an older server */ }
  try { const pending = lord.webapps.asks(); if (Array.isArray(pending) && pending.length) webAsks.set(pending); } catch (e) { /* an older server */ }
}
