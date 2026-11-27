// appcontrol.js — what a theme offers for the app in front and for every app at once (0.8.5):
//
//   appGroup(winId)        { key, name, windows } of the app that owns a window (grouped as the Dock does)
//   appMenuItems(winId)    the app menu: About <app>, Settings…, (a web app's Browser identity),
//                          Hide <app>, Hide Others, Show All, Quit <app>
//   runningApps()          every app with a window, for Force Quit
//   forceQuit(key)         ends an app that stopped answering (see below)
//   recentItems(n)         the recent files as menu items (Recent Items in the crown menu)
//   storeUpdates()         how many package updates are waiting (the crown menu's Store item)
//
//   hideApp, hideOthers, toggleShowDesktop, appSettingsItem   what the keyboard shortcuts call too
//
// Flow shows the app menu under the app's name in its menu bar and the rest in the crown menu;
// Classic offers About in each window's Help menu. About <app> and Force Quit are system windows
// of their own (apps/appabout, apps/forcequit), the same in every theme.
import { windows, wm, getApp, appMenus, onActiveDesktop, activeDesktop } from './windows.js';
import { launcherIdOf, quitApp } from './dock.js';
import { linuxAppFor, shortAppName, windowAppName } from './apps.js';
import { openFileWithApps, openUrl } from './sysopen.js';
import { webAppOfWindow, webAppCall } from './webapps.js';
import { COLOR_SCHEMES, colorSchemeOf, USER_AGENTS, userAgentOf } from './webappsmodel.js';
import * as fs from './fs.js';
import { t, appName } from './i18n.js';
import { compareNames } from './collate.js';

// the theme's own panels; a Linux window also runs under a built-in app id, but it is an app
const isShellPanel = (w) => !!getApp(w.id)?.builtin && !String(w.id).startsWith('linux#');

function groupName(key, w) {
  const a = getApp(key);
  if (a) return appName(a);
  const lin = linuxAppFor(String(key).replace(/^desktop:/, ''));
  if (lin) return shortAppName(lin);
  return w ? windowAppName(w) : String(key);
}

export function appGroup(winId) {
  const w = wm.get(winId);
  if (!w) return null;
  const key = launcherIdOf(w);
  const wins = windows.get().filter((x) => launcherIdOf(x) === key && !isShellPanel(x));
  return { key, name: groupName(key, w), windows: wins };
}

// the app's own settings item: by convention the one with the ❖, shortcut (Passwords, Terminal)
function settingsItemOf(winId) {
  for (const g of appMenus.get()[winId] || []) {
    for (const it of g.items || []) {
      if (it && it !== '-' && it.hint === '❖,' && !it.disabled && typeof it.onClick === 'function') return it;
      for (const s of (it && it.submenu) || []) if (s && s !== '-' && s.hint === '❖,' && typeof s.onClick === 'function') return s;
    }
  }
  return null;
}

// Hide <app>: every window of the app goes away; Hide Others: every other app's on this desktop.
export function hideApp(winId) { appGroup(winId)?.windows.forEach((w) => wm.minimize(w.id)); }
export function hideOthers(winId) {
  const g = appGroup(winId);
  if (!g) return;
  const mine = new Set(g.windows.map((w) => w.id));
  windows.get().forEach((w) => { if (!mine.has(w.id) && !isShellPanel(w) && onActiveDesktop(w)) wm.minimize(w.id); });
}
// A web app's window (lord-web, docs/WEBAPPS.md): Settings… opens Settings → Web apps on that app, and Help offers
// the app's own site ("<app> on the web") in the browser.
function webSettingsItem(winId) {
  const web = webAppOfWindow(wm.get(winId));
  return web ? { onClick: () => wm.open('settings', { args: { section: 'webapps', app: web.entry.id, nonce: Date.now() } }) } : null;
}
export function webHelpItems(winId) {
  const web = webAppOfWindow(wm.get(winId));
  if (!web || !/^https?:\/\//.test(String(web.entry.origin || ''))) return [];
  return [{ label: t('shell.app.webSite', { app: web.entry.name }), onClick: () => openUrl(`${web.entry.origin}/`) }, '-'];
}

// A web app's Appearance (Automatic, Light, Dark): the light or dark preference its pages are told; open windows follow
// at once (lord.webapps.setColorScheme). The same choice as in Settings → Web apps; in the app menu since 0.8.9.6.2.
export function webAppearanceItem(winId) {
  const web = webAppOfWindow(wm.get(winId));
  if (!web) return null;
  const now = colorSchemeOf(web.entry);
  return {
    label: t('webapps.appearance.title'),
    submenu: COLOR_SCHEMES.map((v) => ({ label: t(`webapps.appearance.${v}`), checked: v === now,
      onClick: () => { try { webAppCall('setColorScheme', web.entry.id, v); } catch (e) { console.error('appearance:', e); } } })),
  };
}

// A web app window's Browser identity (Chrome on Linux, Edge on Linux, Firefox on Linux, LordOS (WebKit)): the browser that profile's pages
// are told it is; the window loads its page again at once (lord.webapps.setUserAgent). The same choice as in
// Settings → Web apps, for the window's own profile.
export function webUserAgentItem(winId) {
  const web = webAppOfWindow(wm.get(winId));
  if (!web) return null;
  const now = userAgentOf(web.entry, web.profile);
  return {
    label: t('webapps.identity.title'),
    submenu: USER_AGENTS.map((v) => ({ label: t(`webapps.identity.${v}`), checked: v === now,
      onClick: () => { try { webAppCall('setUserAgent', web.entry.id, web.profile, v); } catch (e) { console.error('identity:', e); } } })),
  };
}

// The app's own Settings… item, if it has one (❖,); a web app's is its page in Settings → Web apps.
export const appSettingsItem = (winId) => settingsItemOf(winId) || webSettingsItem(winId);

// Show desktop: everything visible on this desktop goes away, and the next time what it took away
// comes back (each desktop remembers its own).
const hiddenByDesktop = new Map();
export function toggleShowDesktop() {
  const desk = activeDesktop.get();
  const vis = windows.get().filter((w) => !w.minimized && !w.pip && !isShellPanel(w) && onActiveDesktop(w));
  if (vis.length) { hiddenByDesktop.set(desk, vis.map((w) => w.id)); vis.forEach((w) => wm.minimize(w.id)); return; }
  (hiddenByDesktop.get(desk) || []).forEach((id) => { if (wm.get(id)?.desktop === desk) wm.raise(id); });
  hiddenByDesktop.delete(desk);
}

export function appMenuItems(winId) {
  const g = appGroup(winId);
  if (!g) return [];
  const settingsItem = appSettingsItem(winId);
  const appearance = webAppearanceItem(winId);      // a web app's Light / Dark (0.8.9.6.2: lord-web on WPEPlatform)
  const identity = webUserAgentItem(winId);
  return [
    { label: t('shell.app.about', { app: g.name }), onClick: () => wm.open('appabout', { new: true, args: { key: g.key, win: winId } }) },
    '-',
    { label: t('shell.app.settings'), hint: '❖,', disabled: !settingsItem, onClick: () => settingsItem?.onClick() },
    ...(appearance ? [appearance] : []),
    ...(identity ? [identity] : []),
    '-',
    { label: t('shell.app.hide', { app: g.name }), hint: '❖H', onClick: () => hideApp(winId) },
    { label: t('shell.app.hideOthers'), hint: '❖⌥H', onClick: () => hideOthers(winId) },
    { label: t('shell.app.showAll'), onClick: () => windows.get().forEach((w) => { if (w.minimized) wm.raise(w.id); }) },
    '-',
    { label: t('shell.app.quit', { app: g.name }), hint: '❖Q', onClick: () => quitApp(g.windows) },
  ];
}

export function runningApps() {
  const groups = new Map();
  for (const w of windows.get()) {
    if (isShellPanel(w)) continue;
    if (w.app === 'forcequit') continue;
    const key = launcherIdOf(w);
    if (!groups.has(key)) groups.set(key, { key, name: groupName(key, w), windows: [] });
    groups.get(key).windows.push(w);
  }
  return [...groups.values()].sort((a, b) => compareNames(a.name, b.name));
}

// Force Quit, without asking the app: a Linux app's or an isolated app's process gets SIGKILL from
// lord-server (lord.wayland.kill: only its own client of this user, never lord-server; an X11 window
// is asked to close instead, since every X11 app shares Xwayland); a built-in app that runs inside
// the session closes without its close guard. Returns '' or an error code.
export function forceQuit(key) {
  const g = runningApps().find((x) => x.key === key);
  if (!g) return 'app.notFound';
  let err = '';
  const killed = new Set();
  for (const w of g.windows) {
    const sid = w.args?.surface;
    if (String(w.id).startsWith('linux#') && sid > 0) {
      if (w.args?.x11) { lord.wayland.close(sid); err = err || 'app.forceQuitX11'; continue; }
      if (killed.size && w.args?.pkg) continue;            // one process for all of an isolated app's windows
      const e = typeof lord.wayland.kill === 'function' ? lord.wayland.kill(sid) : 'app.forceQuitFailed';
      if (e) err = err || e; else killed.add(sid);
    } else wm.close(w.id, true);
  }
  return err;
}

export function recentItems(n = 10) {
  let list = [];
  try { list = fs.recentCached(n, 30) || []; } catch (e) { list = []; }
  if (!list.length) return [{ label: t('shell.recent.none'), disabled: true }];
  return list.slice(0, n).map((f) => ({ label: f.name || fs.basename(f.path), onClick: () => openFileWithApps(f.path, f.name) }));
}

export function storeUpdates() {
  try { return typeof lord !== 'undefined' && lord.pkg && typeof lord.pkg.upgrades === 'function' ? (lord.pkg.upgrades() || []).length : 0; } catch (e) { return 0; }
}
