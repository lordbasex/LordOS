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
//
// Only lord-server starts lord-web, with an app and a profile from the registry; nothing here names a URL to
// open or a program to run.
import { createStore, useStore } from './store.js';
import { useEffect } from 'react';
import { defaultProfile, parseWindowAppId, windowWebApp, askAnswer, upstreamVersion, RUNTIME_PACKAGE, WEBKIT_PACKAGE } from './webappsmodel.js';

const has = () => typeof lord !== 'undefined' && typeof lord.webapps === 'object' && lord.webapps !== null;

export const webApps = createStore([]);
export const webAsks = createStore([]);

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
const CALLS = ['addProfile', 'renameProfile', 'removeProfile', 'setPermission', 'setBackground', 'uninstall'];
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

if (has()) {
  refreshWebApps();
  try { const pending = lord.webapps.asks(); if (Array.isArray(pending) && pending.length) webAsks.set(pending); } catch (e) { /* an older server */ }
}
