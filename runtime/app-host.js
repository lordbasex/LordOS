// app-host.js — the «host» that runs inside the process of an isolated .lpk app
// (lord-server --app <id>, see server/src/app/appclient.c). It brings React, the runtime and the SDK
// (with the broker's `lord` object, whose fs/clipboard/perms are checked by lord-server), loads the
// package's app.js and draws it. The app uses `@lordos/sdk` as always.
//
// Theme, language and direction (RTL) come from lord-server: the state at startup
// (lord.broker.state()) and then live (__lordBrokerEvent), so the app looks like the rest of the
// desktop and changes with it (light/dark mode, accent, wallpaper, language).
//
// Several windows: the app's windows live in the SDK's window store of THIS process (wm.open with
// the app's own id, wm.close, wm.setTitle…). Each one is rendered as a child of the React root and
// becomes its own Wayland toplevel (lord.appWindows, one surface per window), so the theme frames
// it, the Dock groups it under the app and the compositor routes focus and input to it.
import * as React from 'react';
import * as jsxRuntime from 'react/jsx-runtime';
import * as runtime from '@lordos/runtime';
import * as sdk from '@lordos/sdk';

// Shared modules the app asks for with globalThis.__lordos.require (as in the theme).
const shared = { react: React, 'react/jsx-runtime': jsxRuntime, '@lordos/runtime': runtime, '@lordos/sdk': sdk };
globalThis.__lordos = globalThis.__lordos || {};
globalThis.__lordos.require = (name) => {
  if (!(name in shared)) throw new Error(`module not available: ${name}`);
  return shared[name];
};

const { createElement: h, Fragment } = React;
const { render } = runtime;
const { ThemeProvider, DEFAULT_TOKENS, PermissionPrompt, AppHost, createStore, useStore, locale, windows, wm } = sdk;

const tokens = createStore(DEFAULT_TOKENS);

function applyLocale(l) {
  if (!l || !l.lang) return;
  const cur = locale.get();
  const dir = l.dir === 'rtl' || l.dir === 'ltr' ? l.dir : undefined;
  if (cur.lang !== l.lang || (cur.region || '') !== (l.region || '') || cur.dir !== dir) locale.set({ lang: l.lang, region: l.region || '', dir });
}

function applyState(st) {
  if (!st) return;
  if (st.theme) tokens.set(st.theme);
  applyLocale(st.locale);
}

applyState(lord.broker.state());          // current session state, before the first frame

// ---- the app's windows ----
const APP = lord.appId;
const AW = lord.appWindows;              // undefined with an older lord-server: one window
const keys = new Map([[APP, 0]]);        // window id → surface key in C (0 = main window)
const idOfKey = (key) => { for (const [id, k] of keys) if (k === key) return id; return null; };

function readManifest() {
  try {
    const b = new Uint8Array(lord.apps.packageFile(APP, 'manifest.json'));
    let s = '';
    for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
    return JSON.parse(decodeURIComponent(escape(s)));
  } catch (e) { return {}; }
}
const manifest = readManifest();

let mod = null;
function AppMain() {
  if (!mod) mod = lord.loadApp();         // evaluates app.js once → { default, Icon }
  if (!mod || typeof mod.default !== 'function') return h(runtime.Text, { style: { color: '#fff', padding: 20 } }, 'The app does not export a component');
  return h(mod.default);
}
sdk.registerBuiltinApp({ id: APP, name: APP, builtin: true, size: Array.isArray(manifest.size) ? manifest.size : [640, 420], Component: AppMain });
wm.open(APP);                             // the main window (surface key 0, created in C)
// a packaged app's companion starts with it. In a home .lpk the same manifest is ignored: lord-server says no.
if (manifest.companion) setTimeout(() => { sdk.companion.start(manifest); }, 0);

// keep the C toplevels in step with the window store (open, close, title)
function syncWindows() {
  if (!AW) return;
  const list = windows.get();
  const ids = new Set(list.map((w) => w.id));
  for (const w of list) {
    if (!keys.has(w.id)) {
      const k = AW.open(w.title || '', w.w, w.h, !!w.ownSize);
      if (k >= 0) keys.set(w.id, k);
    }
    const k = keys.get(w.id);
    if (k != null) AW.setTitle(k, w.title || '');
  }
  for (const [id, k] of [...keys]) if (!ids.has(id)) { keys.delete(id); AW.close(k); }
}
windows.subscribe(syncWindows);

function attach(id, inst) {
  const k = keys.get(id);
  if (AW && inst && k != null) AW.attach(k, inst.id);
}

globalThis.__lordBrokerEvent = (kind, data) => {
  if (kind === 'theme' && data) tokens.set(data);
  else if (kind === 'locale') applyLocale(data);
  else if (kind === 'state') applyState(data);
  else if (kind === 'revoke' && data && data.perm) {
    // the user revoked a permission: turn the indicator off and tell the app
    try { sdk.markDeviceUse(lord.appId, data.perm, false); } catch (e) { /* */ }
    try { for (const fn of revokeListeners) fn(data.perm); } catch (e) { console.error('revoke listener:', e?.message || e); }
  }
  else if (kind === 'menu' && data && data.item) { try { sdk.appMenuClick(data.item); } catch (e) { /* */ } }
  else if (kind === 'appwindow' && data) {
    // from the compositor: close (the frame's button; close guards still apply) or focus
    const id = idOfKey(data.key);
    if (!id) return;
    if (data.type === 'close') wm.close(id);
    else if (data.type === 'focus') wm.focus(id);
  }
  else if (kind === 'window' && data && data.new) wm.open(APP, { new: true });   // Dock → «New window»
  else if (kind === 'open' && data && data.args && typeof data.args === 'object') {
    // the session opened this app with args (sdk/packages.js isolatedApp → lord.broker.openArgs):
    // a notification's "Open", the Dock's "New message", a link from another system app
    const args = data.args;
    if (data.new || Object.keys(args).length) wm.open(APP, data.new ? { new: true, args } : { args });
  }
  else if (kind === 'companion') { try { sdk.companionEvent(data); } catch (e) { console.error('companion:', e?.message || e); } }
  else if (kind === 'status') { try { sdk.statusItemEvent(data); } catch (e) { console.error('status item:', e?.message || e); } }
  else if (kind === 'quit') {
    // ❖Q or Dock → Quit: close every window; a close guard can still keep one open (unsaved
    // changes). The process ends when the last window closes (appclient.c).
    for (const w of [...windows.get()].reverse()) wm.close(w.id);
  }
};
const revokeListeners = new Set();
globalThis.__lordOnRevoke = (fn) => { revokeListeners.add(fn); return () => revokeListeners.delete(fn); };

function Root() {
  const tk = useStore(tokens);
  const { lang, region, dir } = useStore(locale);
  const list = useStore(windows);
  const focused = sdk.wm.focused();
  // a language change remounts the app, as in the theme (texts are built while rendering)
  return h(ThemeProvider, { tokens: tk },
    h(Fragment, { key: `${lang}|${region}|${dir || ''}` },
      list.map((w, i) => h(runtime.View, {
        key: w.id,
        ref: (inst) => attach(w.id, inst),
        // each window is laid out as its own root (appclient.c), so it states the UI direction itself
        style: { position: 'absolute', left: 0, top: 0, width: '100%', height: '100%', backgroundColor: tk.windowBg, direction: sdk.direction() },
      }, h(AppHost, { id: w.id, focused: w.id === focused }), i === 0 ? h(PermissionPrompt) : null))));
}

render(h(Root));
