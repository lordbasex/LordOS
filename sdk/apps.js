// apps.js — every app that can be opened, for the app launcher, the start menu and the Store:
//   - the LordOS apps bundled with the theme (apps/),
//   - the LordOS apps installed as a package (.lpk, see docs/NATIVE-APPS.md),
//   - the installed Linux apps (.desktop files; they open as Wayland windows),
//   - the installed web apps (lord.webapps, docs/WEBAPPS.md; kind 'web', id "web:<app>"): each one opens
//     with its first profile, through lord.webapps.launch.
//
//   const list = useLauncherApps();      // [{ id, name, Icon, kind, comment, categories }]
//   launchApp(list[0].id);
//
// Linux apps are launched with lord.apps.launch(id): the server only accepts the id of an
// installed .desktop and runs its Exec line with no arguments and no shell (see server/src/apps.c).
// A terminal app (Terminal=true: htop, mc…) is not run by it: the server keeps its command and answers
// { terminal: token }, a one-shot token that opens it in a Terminal tab (lord.term.create({ app: token })).
import { createElement as h, useEffect } from 'react';
import { View, Text, Image } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { settings } from './settings.js';
import { locale, appName, t, direction } from './i18n.js';
import { errorText } from './errors.js';
import { notify } from './notify.js';
import { compareNames } from './collate.js';
import { iconSrc } from './iconsrc.js';
import { listApps, wm, windows, appRegistry, getApp as registryApp, openAllowed } from './windows.js';
import { appsOff } from './appsoff.js';
import { launchable } from './appsoffmodel.js';
import { webApps, refreshWebApps, launchWebApp, webAppOfWindow } from './webapps.js';
import { launcherItem, webAppOfKey, windowLabel, profileColor, profileHex, profileInitial, profileOf } from './webappsmodel.js';

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

// Icon from a PNG or an SVG drawn at the size shown (or the initial if there is none).
export function iconFromFile(path, name) {
  if (!path) return ({ size }) => h(LetterIcon, { size, name });
  return ({ size = 32 }) => h(Image, { src: iconSrc(path, size), style: { width: size, height: size, objectFit: 'contain' } });
}

// A terminal app's icon: its own, with the Terminal's icon small in the corner (it opens in a Terminal tab).
export function withTerminalMark(Icon) {
  return ({ size = 32 }) => {
    const Mark = registryApp('terminal')?.Icon;
    const m = Math.max(10, Math.round(size * 0.42));
    return h(View, { style: { width: size, height: size } },
      h(Icon, { size }),
      Mark ? h(View, { style: { position: 'absolute', right: -Math.round(m * 0.15), bottom: -Math.round(m * 0.15), width: m, height: m } }, h(Mark, { size: m })) : null);
  };
}

// A web app's icon: the site's own (cached by lord.webapps.iconResult) or its registry PNG or SVG (an SVG is drawn by
// lord-media), else its initial. One component per app and file, so a list that renders again keeps its pictures.
const webIcons = new Map();
export function webAppIcon(entry) {
  const path = entry && /\.(png|svg)$/.test(String(entry.iconPath || '')) ? entry.iconPath : '';
  const key = `${entry?.id}|${path}|${entry?.name}`;
  if (!webIcons.has(key)) webIcons.set(key, iconFromFile(path, entry?.name || '?'));
  return webIcons.get(key);
}
// A profile's icon: the app's (or Base) with the profile's color as a small dot holding its initial, in the bottom end
// corner; the app's own icon when the profile has no color.
const profileIcons = new Map();
export function webProfileIcon(entry, profileId, Base = null) {
  const color = profileHex(profileColor(entry, profileId));
  const Icon = Base || webAppIcon(entry);
  if (!color) return Icon;
  const letter = profileInitial(profileOf(entry, profileId));
  const key = `${entry.id}|${profileId}|${color}|${letter}|${entry.iconPath}|${Base ? 'b' : ''}`;
  if (!Base && profileIcons.has(key)) return profileIcons.get(key);
  const Marked = ({ size = 32 }) => {
    const d = Math.max(10, Math.round(size * 0.44));
    return h(View, { style: { width: size, height: size } },
      h(Icon, { size }),
      h(View, { style: { position: 'absolute', [direction() === 'rtl' ? 'left' : 'right']: -Math.round(d * 0.12), bottom: -Math.round(d * 0.12), width: d, height: d, borderRadius: d / 2,
        backgroundColor: color, border: `${Math.max(1, Math.round(d / 10))}px solid #ffffff`, alignItems: 'center', justifyContent: 'center' } },
      letter ? h(Text, { style: { fontSize: Math.round(d * 0.55), fontWeight: 700, color: '#ffffff' } }, letter) : null));
  };
  if (!Base) profileIcons.set(key, Marked);
  return Marked;
}
const webItems = (list) => list.map((e) => { const it = launcherItem(e); return it && { ...it, Icon: webAppIcon(e) }; }).filter(Boolean);

const linux = createStore([]);

function readLinux() {
  if (typeof lord.apps === 'undefined' || typeof lord.apps.linux !== 'function') return [];   // not in app mode
  return lord.apps.linux(locale.get().lang).map((a) => {
    const Icon = iconFromFile(a.icon, a.name);
    return { ...a, id: DESKTOP + a.id, desktopId: a.id, kind: 'linux', terminal: !!a.terminal, Icon: a.terminal ? withTerminalMark(Icon) : Icon };
  });
}

// Re-reads the .desktop files (after installing or removing packages; it's cheap: the server
// only re-reads if the folders changed).
export function refreshApps() {
  const next = readLinux(), now = linux.get();
  if (next.length === now.length && next.every((a, i) => a.id === now[i].id && a.name === now[i].name && a.icon === now[i].icon &&
    a.terminal === now[i].terminal)) return;
  linux.set(next);
}
// The installed Linux apps' ids, or null if the list can't be read or came back empty.
export function linuxIds() { refreshApps(); const l = linux.get(); return l.length ? new Set(l.map((a) => a.desktopId)) : null; }
locale.subscribe(refreshApps);

// Deactivated built-in apps (Store → Installed, sdk/appsoffmodel.js) are left out; { all: true } keeps
// them, marked off: true (the Store's Installed page, which turns them back on).
export function useLauncherApps({ all = false } = {}) {
  const lin = useStore(linux);
  useStore(settings, (s) => s.appsOff);
  useEffect(refreshApps, []);    // an app installed outside the Store (a terminal, another app's installer) shows the next time a list opens
  useStore(locale);
  useStore(appRegistry);
  const web = useStore(webApps);
  useEffect(refreshWebApps, []);
  const own = listApps().map((a) => ({ ...a, kind: a.package ? 'package' : 'lordos', name: appName(a) }));
  const every = [...own, ...lin, ...webItems(web)].sort((a, b) => compareNames(a.name, b.name));   // "go-link Desk" among the G's (#64)
  const off = appsOff();
  if (all) return off.length ? every.map((a) => (a.kind === 'lordos' && off.includes(a.id) ? { ...a, off: true } : a)) : every;
  return launchable(every, off);
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
  const web = webAppOfKey(id);
  if (web) {
    // a web app: its first profile (the Dock's menu offers the others)
    try { return launchWebApp(web); } catch (e) { notify({ app: 'settings', title: errorText(e) }); return false; }
  }
  if (isLinuxApp(id)) {
    const desktopId = String(id).slice(DESKTOP.length);
    const ok = lord.apps.launch(desktopId);
    // a terminal app: a new Terminal window whose tab runs it (the token is all the Terminal gets), named after it
    if (ok && typeof ok === 'object' && typeof ok.terminal === 'string') {
      const app = linux.get().find((a) => a.desktopId === desktopId);
      return wm.open('terminal', { new: true, args: { app: ok.terminal, title: app ? app.name : desktopId } }) != null;
    }
    // refused for a reason the user can act on (e.g. its restrictions need a newer kernel)
    const code = !ok && typeof lord.apps.lastError === 'function' ? lord.apps.lastError() : '';
    if (code) notify({ app: 'settings', title: errorText(code) });
    return ok === true;
  }
  // An installed .lpk app runs as its own isolated process (lord-server --app <id>), if the
  // server supports it; it reaches the desktop as a Wayland window with app_id = its id. Otherwise it
  // falls back to in-process mode (compatibility). See docs/PRIVACY.md.
  // a deactivated built-in app doesn't start: a notice offers to activate it (appsoffactions.js)
  if (!openAllowed(id)) return false;
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
  const web = webAppOfWindow(w);
  if (web) {
    const l = windowLabel(web.entry, web.profile);
    return l.profile ? t('webapps.windowName', { app: l.name, profile: l.profile }) : l.name;
  }
  const appId = w.args?.appId || '';
  const pkg = appId ? registryApp(appId) : null;
  if (pkg && (pkg.package || pkg.isolated)) return pkg.isolated ? appName(pkg) : pkg.name;
  return shortAppName(linuxAppFor(appId)) || prettyAppId(appId) || appName(registryApp('linux'));
}

// Icon of a window: its app's; for a Linux window, its .desktop's if one is found.
export function windowIcon(w, getApp) {
  const appId = w?.args?.appId;
  if (String(w?.id).startsWith('linux#')) {
    const web = webAppOfWindow(w);
    if (web) return webProfileIcon(web.entry, web.profile);
    // an isolated .lpk app (lord-server --app) arrives as a Wayland window with app_id = its id
    const pkg = appId ? registryApp(appId) : null;
    if (pkg && (pkg.package || pkg.isolated) && pkg.Icon) return pkg.Icon;
    const a = linuxAppFor(appId);
    return a ? a.Icon : ({ size }) => h(LetterIcon, { size, name: appId || w?.title || 'Linux' });
  }
  return getApp(w?.id)?.Icon || (({ size }) => h(LetterIcon, { size, name: w?.id }));
}

refreshApps();
