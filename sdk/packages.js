// packages.js — native LordOS apps installed as a package (.lpk; see docs/NATIVE-APPS.md).
//
// A package brings its app already bundled (tools/lpk.mjs) without React or the SDK inside: it uses
// the theme's, which are shared here through globalThis.__lordos.require(). The app is only loaded
// when its first window opens.
//
//   packages.list()                 → [{ id, name, version, description, permissions, size… }]
//   await packages.inspect(path)    → { manifest, files, size }   (before installing, to confirm)
//   packages.install(path) / packages.remove(id)
//   const { instance } = await loadWasm('fractal', 'mandel.wasm', imports)   // from the app
import * as React from 'react';
import * as jsxRuntime from 'react/jsx-runtime';
import * as runtime from '@lordos/runtime';
import * as sdk from './index.js';
import { createStore } from './store.js';
import { locale } from './i18n.js';
import { setPackageApps, windows, wm, appIdOf } from './windows.js';
import { iconFromFile } from './apps.js';

const h = React.createElement;
const shared = {
  react: React, 'react/jsx-runtime': jsxRuntime, '@lordos/runtime': runtime, '@lordos/sdk': sdk,
};
globalThis.__lordos = {
  require(name) {
    if (!(name in shared)) throw new Error(`module not available to installed apps: ${name}`);
    return shared[name];
  },
};

const has = () => typeof lord.apps !== 'undefined' && typeof lord.apps.packages === 'function';
const loaded = new Map();               // id → { default, Icon } | { error }

function load(id) {
  if (!loaded.has(id)) {
    try {
      const mod = lord.apps.loadPackage(id);
      loaded.set(id, mod && typeof mod.default === 'function' ? mod : { error: 'app.js no exporta un componente' });
    } catch (e) { loaded.set(id, { error: String(e?.message || e) }); }
  }
  return loaded.get(id);
}

// With app mode (axia-server --app), an .lpk app is NEVER evaluated in the theme's context: if something
// opens its window in-process (wm.open, the Dock's «New window»…), the isolated process is launched and
// this empty window is closed. See docs/PRIVACY.md.
const isolated = () => typeof lord.apps !== 'undefined' && typeof lord.apps.launchPackage === 'function';

function Redirect({ id }) {
  const win = sdk.useAppWindow();
  React.useEffect(() => {
    // already running: a plain open focuses its window; «New window» (wm.open(id, { new: true }))
    // asks the running process for another window (axia-server forwards it over the broker)
    const running = windows.get().filter((w) => w.args?.pkg && w.args?.appId === id);
    const wantsNew = win && win.id && String(win.id).includes('#');
    if (running.length && !wantsNew) wm.focus(running[running.length - 1].id);
    else lord.apps.launchPackage(id);
    if (win && win.id) wm.close(win.id, true);
  }, []);
  return null;
}

// A built-in app that runs as its own sandboxed process (manifest "process": "isolated", shipped as
// <assets>/sysapps/<id>, see tools/build-theme.mjs): opening its window in the theme starts the
// process instead and closes this placeholder. Without app mode (or without the built system
// package) the app runs in-process as before.
export function isolatedApp(id, Fallback) {
  return function IsolatedApp(props) {
    const win = sdk.useAppWindow();
    const [inproc, setInproc] = React.useState(false);
    React.useEffect(() => {
      const running = windows.get().filter((w) => w.args?.pkg && w.args?.appId === id);
      // what the window was opened with (a notification's "Open", the Dock's "New message", another
      // app's link) goes to the app process: now if it runs, or when it starts (lord.broker.openArgs)
      // a single-window app (manifest "singleWindow": Passwords holds one vault view) only gets focused
      const wantsNew = !!(win && win.id && String(win.id).includes('#')) && !sdk.getApp(id)?.singleWindow;
      const { nonce, ...args } = win?.args || {};
      const hasArgs = Object.keys(args).length > 0;
      const forward = () => { if ((hasArgs || wantsNew) && lord.broker && typeof lord.broker.openArgs === 'function') lord.broker.openArgs(id, JSON.stringify(args), wantsNew); };
      if (globalThis.__lordTiming && lord.now) {
        const t0 = sdk.openedAt?.get(win?.id);
        console.log(`timing: ${id} isolated launch at ${lord.now().toFixed(1)} ms (monotonic)${t0 != null ? `, ${(lord.now() - t0).toFixed(1)} ms after wm.open at ${t0.toFixed(1)}` : ''}`);
      }
      if (isolated() && running.length) {
        forward();
        if (!wantsNew) wm.focus(running[running.length - 1].id);
        if (win && win.id) wm.close(win.id, true);
      } else if (isolated() && lord.apps.launchPackage(id)) { forward(); if (win && win.id) wm.close(win.id, true); }
      else setInproc(true);
    }, []);
    React.useEffect(() => {               // developer timing of the in-process fallback
      const t0 = inproc && globalThis.__lordTiming ? sdk.openedAt?.get(win?.id) : null;
      if (t0 != null) { sdk.openedAt.delete(win.id); console.log(`timing: ${id} in-process window committed ${(lord.now() - t0).toFixed(1)} ms after wm.open`); }
    }, [inproc]);
    return inproc && Fallback ? h(Fallback, props) : null;
  };
}

function lazyComponent(id) {
  return function PackageApp(props) {
    if (isolated()) return h(Redirect, { id });
    const mod = load(id);
    if (mod.error) {
      return h(runtime.View, { style: { flex: 1, padding: 20 } },
        h(runtime.Text, { style: { fontSize: 13, color: '#ff453a' } }, sdk.t('shell.packages.loadError', { error: mod.error })));
    }
    return h(mod.default, props);
  };
}

const localized = (v, lang) => (v && typeof v === 'object' ? v[lang] || v[lang.split('-')[0]] || v.en || Object.values(v)[0] : v) || '';

export const packageList = createStore([]);

function read() {
  if (!has()) return [];
  const lang = locale.get().lang;
  const out = [];
  for (const p of lord.apps.packages()) {
    let m;
    try { m = JSON.parse(p.manifest); } catch (e) { continue; }
    if (m.id !== p.id) continue;
    const name = localized(m.name, lang) || p.id;
    out.push({
      id: p.id, name, version: String(m.version || ''), description: localized(m.description, lang),
      permissions: Array.isArray(m.permissions) ? m.permissions.map(String) : [],
      size: Array.isArray(m.size) ? m.size : [640, 420], minSize: Array.isArray(m.minSize) ? m.minSize : undefined,
      transparent: !!m.transparent, dock: false, package: true, dir: p.dir, category: typeof m.category === 'string' ? m.category : undefined,
      Icon: iconFromFile(m.icon ? `${p.dir}/${m.icon}` : '', name), Component: lazyComponent(p.id),
    });
  }
  return out;
}

export function refreshPackages() {
  const list = read();
  packageList.set(list);
  setPackageApps(list);
}

export const packages = {
  list: () => packageList.get(),
  inspect: (path) => lord.apps.inspectPackage(path),
  install(path) {
    const r = lord.apps.installPackage(path);
    loaded.delete(r.manifest.id);       // a new version is reloaded when opened
    for (const w of windows.get()) if (appIdOf(w.id) === r.manifest.id) wm.close(w.id, true);
    refreshPackages();
    return r;
  },
  remove(id) {
    for (const w of windows.get()) if (appIdOf(w.id) === id) wm.close(w.id, true);
    const ok = lord.apps.removePackage(id);
    loaded.delete(id);
    refreshPackages();
    return ok;
  },
};

// File of an installed app (ArrayBuffer) and its already instantiated WebAssembly module.
export const packageFile = (appId, path) => lord.apps.packageFile(appId, path);
export function loadWasm(appId, path, imports = {}) {
  if (typeof WebAssembly === 'undefined') return Promise.reject(new Error('WebAssembly is not available'));
  return WebAssembly.instantiate(packageFile(appId, path), imports);
}

locale.subscribe(refreshPackages);
