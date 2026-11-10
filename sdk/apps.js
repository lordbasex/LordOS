// apps.js — every app that can be opened, for the app launcher, the start menu and the Store:
//   - the LordOS apps bundled with the theme (apps/),
//   - the LordOS apps installed as a package (.lpk, see docs/NATIVE-APPS.md),
//   - the installed Linux apps (.desktop files; they open as Wayland windows).
//
//   const list = useLauncherApps();      // [{ id, name, Icon, kind, comment, categories }]
//   launchApp(list[0].id);
//
// Linux apps are launched with lord.apps.launch(id): the server only accepts the id of an
// installed .desktop and runs its Exec line with no arguments and no shell (see server/src/apps.c).
import { createElement as h, useEffect } from 'react';
import { View, Text, Image } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { locale, appName } from './i18n.js';
import { errorText } from './errors.js';
import { notify } from './notify.js';
import { compareNames } from './collate.js';
import { listApps, wm, windows, appRegistry, getApp as registryApp } from './windows.js';

const DESKTOP = 'desktop:';
export const isLinuxApp = (id) => String(id).startsWith(DESKTOP);

// Fallback icon: the initial over a color derived from the name (apps without a PNG).
export function LetterIcon({ size: s = 32, name = '?' }) {
  let hsh = 0;
  for (const ch of String(name)) hsh = (hsh * 31 + ch.codePointAt(0)) >>> 0;
  const hue = hsh % 360;
  const hex = (l) => {             // hsl(hue, 55%, l) → #rrggbb (the engine doesn't understand hsl())
    const a = 0.55 * Math.min(l, 1 - l), f = (n) => { const k = (n + hue / 30) % 12; return l - a * Math.max(-1, Math.min(k - 3, 9 - k, 1)); };
    return '#' + [0, 8, 4].map((n) => Math.round(f(n) * 255).toString(16).padStart(2, '0')).join('');
  };
  const letter = (String(name).replace(/^.*\./, '')[0] || '?').toUpperCase();
  return h(View, { style: { width: s, height: s, borderRadius: s * 0.225, alignItems: 'center', justifyContent: 'center',
    background: `linear-gradient(180deg, ${hex(0.52)}, ${hex(0.36)})` } },
  h(Text, { style: { fontSize: s * 0.5, fontWeight: 700, color: '#fff' } }, letter));
}

// Icon from a PNG (or the initial if there is none).
export function iconFromFile(path, name) {
  if (!path) return ({ size }) => h(LetterIcon, { size, name });
  return ({ size = 32 }) => h(Image, { src: path, style: { width: size, height: size, objectFit: 'contain' } });
}

const linux = createStore([]);

function readLinux() {
  if (typeof lord.apps === 'undefined' || typeof lord.apps.linux !== 'function') return [];   // not in app mode
  return lord.apps.linux(locale.get().lang).map((a) => ({
    ...a, id: DESKTOP + a.id, desktopId: a.id, kind: 'linux', Icon: iconFromFile(a.icon, a.name),
  }));
}

// Re-reads the .desktop files (after installing or removing packages; it's cheap: the server
// only re-reads if the folders changed).
export function refreshApps() {
  const next = readLinux(), now = linux.get();
  if (next.length === now.length && next.every((a, i) => a.id === now[i].id && a.name === now[i].name && a.icon === now[i].icon)) return;
  linux.set(next);
}
// The installed Linux apps' ids, or null if the list can't be read or came back empty.
export function linuxIds() { refreshApps(); const l = linux.get(); return l.length ? new Set(l.map((a) => a.desktopId)) : null; }
locale.subscribe(refreshApps);

export function useLauncherApps() {
  const lin = useStore(linux);
  useEffect(refreshApps, []);    // an app installed outside the Store (a terminal, another app's installer) shows the next time a list opens
  useStore(locale);
  useStore(appRegistry);
  const own = listApps().map((a) => ({ ...a, kind: a.package ? 'package' : 'lordos', name: appName(a) }));
  return [...own, ...lin].sort((a, b) => compareNames(a.name, b.name));   // "go-link Desk" among the G's (#64)
}

// Main freedesktop standard categories (the same as AppStream's in the Store).
// A Linux app takes it from the Categories field of its .desktop; a LordOS one, from "category" in its
// manifest.
export const APP_CATEGORIES = ['AudioVideo', 'Development', 'Education', 'Game', 'Graphics', 'Network', 'Office', 'Science', 'Settings', 'System', 'Utility'];
const ALIASES = { Audio: 'AudioVideo', Video: 'AudioVideo', TerminalEmulator: 'System', FileManager: 'System', WebBrowser: 'Network', TextEditor: 'Utility' };
export function appCategory(a) {
  if (a.category && APP_CATEGORIES.includes(a.category)) return a.category;
  const list = String(a.categories || '').split(';').filter(Boolean);
  for (const c of list) if (APP_CATEGORIES.includes(c)) return c;
  for (const c of list) if (ALIASES[c]) return ALIASES[c];
  return 'Utility';
}

export function launchApp(id) {
  if (isLinuxApp(id)) {
    const ok = lord.apps.launch(String(id).slice(DESKTOP.length));
    // refused for a reason the user can act on (e.g. its restrictions need a newer kernel)
    const code = !ok && typeof lord.apps.lastError === 'function' ? lord.apps.lastError() : '';
    if (code) notify({ app: 'settings', title: errorText(code) });
    return ok;
  }
  // An installed .lpk app runs as its own isolated process (lord-server --app <id>), if the
  // server supports it; it reaches the desktop as a Wayland window with app_id = its id. Otherwise it
  // falls back to in-process mode (compatibility). See docs/PRIVACY.md.
  const app = registryApp(id);
  if (app && app.package && typeof lord.apps !== 'undefined' && typeof lord.apps.launchPackage === 'function')
    return lord.apps.launchPackage(id);
  // a built-in app that runs as its own sandboxed process (manifest "process": "isolated")
  if (app && app.isolated && typeof lord.apps !== 'undefined' && typeof lord.apps.launchPackage === 'function') {
    const running = windows.get().filter((w) => w.args?.pkg && w.args?.appId === id);
    if (running.length) { wm.focus(running[running.length - 1].id); return true; }
    if (globalThis.__lordTiming && lord.now) console.log(`timing: ${id} isolated launch at ${lord.now().toFixed(1)} ms (monotonic)`);
    if (lord.apps.launchPackage(id)) return true;
  }
  return wm.open(id) != null;
}

// Linux app of a Wayland window (by its app_id: the .desktop name or StartupWMClass).
export function linuxAppFor(appId) {
  if (!appId) return null;
  const want = String(appId).toLowerCase();
  if (!linux.get().length) refreshApps();
  return linux.get().find((a) => a.desktopId.toLowerCase() === want || a.wmClass.toLowerCase() === want ||
    a.desktopId.toLowerCase().endsWith(`.${want}`)) || null;
}

// Short label of a Linux app, for the shell (menu bar, Dock, notifications). If the (translated)
// Name contains, as whole words, the untranslated Name or the app's id, only those words:
// "Navegador web Chromium" and "Chromium Web Browser" (chromium.desktop) → "Chromium". Otherwise
// the translated Name as is, so a real translation ("Archivos") still shows.
export function shortAppName(app) {
  if (!app) return '';
  const name = String(app.name || app.nameC || '');
  const id = String(app.desktopId || '');
  // a reverse-DNS id ("org.gnome.TextEditor") is named by its last part; "python3.13" is not one
  const idName = /^[a-z]{2,}\.[^.]+\./i.test(id) ? id.slice(id.lastIndexOf('.') + 1) : id;
  for (const c of [app.nameC, idName, app.wmClass]) {
    const want = String(c || '').trim();
    if (want.length < 3 || !/\p{L}/u.test(want) || want.toLowerCase() === name.toLowerCase()) continue;
    const pat = want.replace(/[.*+?^${}()|[\]\\]/g, '\\$&').replace(/[-_\s]+/g, '[-_\\s]+');
    const m = new RegExp(`(?:^|[^\\p{L}\\p{N}])(${pat})(?=$|[^\\p{L}\\p{N}])`, 'iu').exec(name);
    if (m) return m[1];
  }
  return name;
}

// A Wayland app_id as a label when no .desktop matches: "org.example.Foo" → "Foo", "foot" → "Foot".
function prettyAppId(appId) {
  const last = String(appId || '').split('.').filter(Boolean).pop() || '';
  const s = last.replace(/[-_]+/g, ' ').trim();
  return s ? s[0].toUpperCase() + s.slice(1) : '';
}

// The app name of a window, never its title: a LordOS app's name; for a Linux window, the isolated
// app's package name, its .desktop's short name or its app_id.
export function windowAppName(w) {
  if (!w) return '';
  if (!String(w.id).startsWith('linux#')) return appName(registryApp(w.id));
  const appId = w.args?.appId || '';
  const pkg = appId ? registryApp(appId) : null;
  if (pkg && (pkg.package || pkg.isolated)) return pkg.isolated ? appName(pkg) : pkg.name;
  return shortAppName(linuxAppFor(appId)) || prettyAppId(appId) || appName(registryApp('linux'));
}

// Icon of a window: its app's; for a Linux window, its .desktop's if one is found.
export function windowIcon(w, getApp) {
  const appId = w?.args?.appId;
  if (String(w?.id).startsWith('linux#')) {
    // an isolated .lpk app (lord-server --app) arrives as a Wayland window with app_id = its id
    const pkg = appId ? registryApp(appId) : null;
    if (pkg && (pkg.package || pkg.isolated) && pkg.Icon) return pkg.Icon;
    const a = linuxAppFor(appId);
    return a ? a.Icon : ({ size }) => h(LetterIcon, { size, name: appId || w?.title || 'Linux' });
  }
  return getApp(w?.id)?.Icon || (({ size }) => h(LetterIcon, { size, name: w?.id }));
}

refreshApps();
