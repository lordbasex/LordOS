// appcontrol.js — what a theme offers for the app in front and for every app at once (0.8.5):
//
//   appGroup(winId)        { key, name, windows } of the app that owns a window (grouped as the Dock does)
//   appMenuItems(winId)    the app menu: About <app>, Settings…, Hide <app>, Hide Others, Show All, Quit <app>
//   runningApps()          every app with a window, for Force Quit
//   forceQuit(key)         ends an app that stopped answering (see below)
//   recentItems(n)         the recent files as menu items (Recent Items in the crown menu)
//   storeUpdates()         how many package updates are waiting (the crown menu's Store item)
//
//   hideApp, hideOthers, toggleShowDesktop, appSettingsItem   what the keyboard shortcuts call too
//
// Lord Flow shows the app menu under the app's name in its menu bar and the rest in the crown menu;
// Lord Classic offers About in each window's Help menu. About <app> and Force Quit are system windows
// of their own (apps/appabout, apps/forcequit), the same in every theme.
import { windows, wm, getApp, appMenus } from './windows.js';
import { launcherIdOf, quitApp } from './dock.js';
import { linuxAppFor, shortAppName, windowAppName } from './apps.js';
import { openFileWithApps } from './sysopen.js';
import * as fs from './fs.js';
import { t, appName } from './i18n.js';

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

// Hide <app>: every window of the app goes away; Hide Others: every other app's.
export function hideApp(winId) { appGroup(winId)?.windows.forEach((w) => wm.minimize(w.id)); }
export function hideOthers(winId) {
  const g = appGroup(winId);
  if (!g) return;
  const mine = new Set(g.windows.map((w) => w.id));
  windows.get().forEach((w) => { if (!mine.has(w.id) && !isShellPanel(w)) wm.minimize(w.id); });
}
// The app's own Settings… item, if it has one (❖,).
export const appSettingsItem = settingsItemOf;

// Show desktop: everything visible goes away, and the next time what it took away comes back.
let hiddenByDesktop = [];
export function toggleShowDesktop() {
  const vis = windows.get().filter((w) => !w.minimized && !isShellPanel(w));
  if (vis.length) { hiddenByDesktop = vis.map((w) => w.id); vis.forEach((w) => wm.minimize(w.id)); return; }
  hiddenByDesktop.forEach((id) => wm.focus(id));
  hiddenByDesktop = [];
}

export function appMenuItems(winId) {
  const g = appGroup(winId);
  if (!g) return [];
  const settingsItem = settingsItemOf(winId);
  return [
    { label: t('shell.app.about', { app: g.name }), onClick: () => wm.open('appabout', { new: true, args: { key: g.key, win: winId } }) },
    '-',
    { label: t('shell.app.settings'), hint: '❖,', disabled: !settingsItem, onClick: () => settingsItem?.onClick() },
    '-',
    { label: t('shell.app.hide', { app: g.name }), hint: '❖H', onClick: () => hideApp(winId) },
    { label: t('shell.app.hideOthers'), hint: '❖⌥H', onClick: () => hideOthers(winId) },
    { label: t('shell.app.showAll'), onClick: () => windows.get().forEach((w) => { if (w.minimized) wm.focus(w.id); }) },
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
  return [...groups.values()].sort((a, b) => a.name.localeCompare(b.name));
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
  try { list = fs.recent(n, 30) || []; } catch (e) { list = []; }
  if (!list.length) return [{ label: t('shell.recent.none'), disabled: true }];
  return list.slice(0, n).map((f) => ({ label: f.name || fs.basename(f.path), onClick: () => openFileWithApps(f.path, f.name) }));
}

export function storeUpdates() {
  try { return typeof lord !== 'undefined' && lord.pkg && typeof lord.pkg.upgrades === 'function' ? (lord.pkg.upgrades() || []).length : 0; } catch (e) { return 0; }
}
