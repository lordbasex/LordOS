// dock.js — Dock and taskbar: pinned apps, windows grouped by app, the right-click menu, dragging
// to add, reorder and remove, and the Trash. Every theme uses it; each one draws it its own way.
//
//   const entries = useDockEntries(['files', 'terminal'], { launcher: { Icon } })   // the theme's defaults
//   // entry = { id, kind, name, Icon, windows: [window…], pinned, running, focused, locked, movable }
//   dockActivate(entry)        // click: opens the app or brings up its window
//   dockMenu(entry)            // items for the context menu: [{ label, onClick, checked, submenu, header } | '-']
//   dock.pin(id, index) · dock.move(id, index) · dock.unpin(id) · dock.reset()
//
// Pinned items are typed ids (sdk/dockmodel.js): "app:files", "lpk:<id>", "desktop:<desktop id>"
// and "shell:launcher". The methods also take a plain launcher id ("files", "desktop:firefox").
// The Trash is always last; Files, Settings and the launcher can't be removed but move like any
// other app (docs/DOCK.md, "Rules").
//
// An app can add its own items to the menu (the Terminal: New Window, New Tab…):
//   registerDockMenu('terminal', (entry) => [{ label: '…', onClick }])
// and quick actions at the top of it (Settings: one per section), from its module or its manifest
// ("dockActions": [{ id, label: 'i18n.key', args }]); choosing one opens the app with those args:
//   registerDockActions('settings', () => [{ id: 'network', label: 'settings.section.network', args: { section: 'network' } }])
import { createElement as h } from 'react';
import { useEffect } from 'react';
import { addListener } from '@lordos/runtime';
import { settings } from './settings.js';
import { createStore, useStore } from './store.js';
import { useWindows, wm, windowTitle, appIdOf, getApp, windows, setDisplayAssignment, isLiveSystem, activeDesktop, onActiveDesktop } from './windows.js';
import { pickWindow } from './desktopmodel.js';
import { desktopSuffix } from './desktops.js';
import { screen, onScreenChange, displayFor } from '@lordos/runtime';
import { useLauncherApps, launchApp, linuxAppFor, shortAppName, windowAppName, LetterIcon, windowIcon } from './apps.js';
import { t, locale } from './i18n.js';
import { restoredIds } from './sessionrestore.jsx';
import * as fs from './fs.js';
import {
  parseDockId, toTypedId, normalizePinned, ensureItems, moveItem, pinItem, unpinItem, isLocked, isMovable, cleanActions,
  LAUNCHER, LOCKED, FIRST, dockHideMode, dockHideSettings, dockCovered, DOCK_HIDE_MODES,
} from './dockmodel.js';

export { LAUNCHER as DOCK_LAUNCHER, dockHideSettings, DOCK_HIDE_MODES };

const menuProviders = new Map();
export function registerDockMenu(appId, fn) { menuProviders.set(appId, fn); return () => menuProviders.delete(appId); }
const actionProviders = new Map();
export function registerDockActions(appId, fn) { actionProviders.set(appId, fn); return () => actionProviders.delete(appId); }

// ---- ids ----

// What an old untyped id is now: a package or a LordOS app (null if unknown).
const classify = (id) => { const a = getApp(id); return !a || a.builtin ? null : a.package ? 'lpk' : 'app'; };

// A launcher app object or any id → its typed Dock id (null if it can't be pinned).
export function dockIdOf(x) {
  if (x && typeof x === 'object') {
    if (x.kind === 'linux') return x.desktopId ? `desktop:${x.desktopId}` : null;
    return toTypedId(x.package ? `lpk:${x.id}` : `app:${x.id}`);
  }
  return toTypedId(String(x ?? ''), classify);
}
// Typed id → the id launchApp knows ("files", "desktop:firefox"); null for the launcher.
const launcherKey = (id) => {
  const p = parseDockId(id);
  if (!p || p.kind === 'shell') return null;
  return p.kind === 'desktop' ? `desktop:${p.key}` : p.key;
};

// ---- pinned (in ~/.config/lordos/settings.json → dockPinned) ----

let themeDefaults = [FIRST, LAUNCHER];
const dockOpened = new Map();         // running unpinned and pinned groups, in the order they opened
let shownFn = () => true;            // which pinned ids the Dock shows (installed), from the last render
const fromSettings = () => {
  const raw = settings.get().dockPinned;
  const base = raw == null ? themeDefaults : normalizePinned(raw, classify).list;
  return ensureItems(base, LOCKED, themeDefaults);
};
const save = (list) => settings.set({ dockPinned: list });

export const dock = {
  pinned: fromSettings,
  isPinned: (id) => fromSettings().includes(dockIdOf(id)),
  canRemove: (id) => { const d = dockIdOf(id); return !!d && !isLocked(d); },
  canMove: (id) => { const d = dockIdOf(id); return !!d && isMovable(d); },
  // index: position among the items the Dock shows (0: the first one)
  pin(id, index) {
    const d = dockIdOf(id);
    if (!d) return false;
    const cur = fromSettings(), next = pinItem(cur, d, index, shownFn);
    if (next !== cur) save(next);
    return true;
  },
  move(id, index) {
    const d = dockIdOf(id), cur = fromSettings();
    const next = d ? moveItem(cur, d, index, shownFn) : cur;
    if (next !== cur) save(next);
    return next !== cur;
  },
  unpin(id) {
    const d = dockIdOf(id), cur = fromSettings();
    const next = d ? unpinItem(cur, d) : cur;
    if (next !== cur) save(next);
    return next !== cur;
  },
  // the theme's defaults again (Files first; the locked items present)
  reset: () => settings.set({ dockPinned: null }),
};

// ---- Dock and taskbar preferences (Settings → Desktop & Dock) ----
// Stored in settings.json. Left and right are physical sides of the screen, even with a
// right-to-left language.
export const DOCK_DEFAULTS = {
  dockPosition: 'bottom',     // 'bottom' | 'left' | 'right' (Lord Flow theme)
  dockSize: 54,               // px of each icon (36–80)
  dockMagnify: true,          // magnification
  dockMagnifySize: 86,        // px of the largest icon when magnified (more than dockSize)
  dockAutohide: false,        // hides and comes back when the edge is touched
  dockIntellihide: false,     // with dockAutohide: hides only while a window covers it (dockmodel.js, #46)
  dockRecents: false,        // shows the most recently used apps that aren't pinned
  taskbarPosition: 'bottom',  // 'bottom' | 'top' | 'left' | 'right' (Classic theme)
};
export function dockPrefs(s = settings.get()) {
  const p = { ...DOCK_DEFAULTS };
  for (const k of Object.keys(DOCK_DEFAULTS)) if (s[k] !== undefined && s[k] !== null) p[k] = s[k];
  if (!['bottom', 'left', 'right'].includes(p.dockPosition)) p.dockPosition = 'bottom';
  if (!['bottom', 'top', 'left', 'right'].includes(p.taskbarPosition)) p.taskbarPosition = 'bottom';
  p.dockSize = Math.max(36, Math.min(80, Math.round(+p.dockSize) || 54));
  p.dockMagnifySize = Math.max(p.dockSize, Math.min(128, Math.round(+p.dockMagnifySize) || 86));
  p.dockAutohide = p.dockAutohide === true;
  p.dockIntellihide = p.dockIntellihide === true;
  p.dockHide = dockHideMode(p);   // 'never' | 'overlap' | 'always'
  return p;
}

// Does a window on the showing desktop cover rect ({ x0, y0, x1, y1 }: where the Dock is when shown)?
// Redraws the caller when windows open, close, move or change desktop. Off: always false, no work.
export function useDockCovered(rect, on = true) {
  const list = useStore(windows);
  const active = useStore(activeDesktop);
  return on && dockCovered(list, active, rect, { frameOf: wm.frame, displayOf: (w) => displayFor(w) });
}

// Recently used apps (for «Show recent apps»): each app that opens a window moves to the front.
let openIds = new Set();
windows.subscribe(() => {
  const now = new Set();
  for (const w of windows.get()) if (!(getApp(w.id)?.builtin && !String(w.id).startsWith('linux#'))) now.add(launcherIdOf(w));
  const fresh = [...now].filter((id) => !openIds.has(id) && !id.startsWith('linux:'));
  openIds = now;
  if (!fresh.length) return;
  const cur = settings.get().dockRecentApps || [];
  const next = [...fresh, ...cur.filter((x) => !fresh.includes(x))].slice(0, 8);
  if (next.join('\n') !== cur.join('\n')) settings.set({ dockRecentApps: next });
});

// Which launcher app a window belongs to: its LordOS app, or the Linux app from its .desktop.
export function launcherIdOf(w) {
  if (!String(w.id).startsWith('linux#')) return appIdOf(w.id);
  const appId = w.args?.appId || '';
  if (w.args?.pkg && appId && (getApp(appId)?.package || getApp(appId)?.isolated)) return appId;   // isolated app (.lpk or built-in): grouped under its own entry
  const a = linuxAppFor(appId);
  return a ? a.id : `linux:${appId || w.id}`;
}
// The Dock entry a window belongs to (a typed id, or "linux:…" for a window with no .desktop).
export function dockIdOfWindow(w) {
  const k = launcherIdOf(w);
  return k.startsWith('linux:') ? k : dockIdOf(k) || k;
}

// opts.recents: how many recent apps (neither pinned nor open) are appended at the end, with recent: true.
// opts.launcher: { Icon } draws the app launcher as a Dock item (without it, the theme has its own
// button and the launcher item is left out).
export function useDockEntries(defaults = [], opts = {}) {
  const typedDefaults = [FIRST, ...defaults.map((d) => dockIdOf(d)).filter(Boolean)].filter((x, i, a) => a.indexOf(x) === i);
  if (!typedDefaults.includes(LAUNCHER)) typedDefaults.splice(1, 0, LAUNCHER);
  themeDefaults = typedDefaults;
  const s = useStore(settings);
  const { windows: wins, focused } = useWindows();
  const apps = useLauncherApps();
  const byId = new Map(apps.map((a) => [a.id, a]));
  const appFor = (id) => {
    const p = parseDockId(id);
    if (!p) return null;
    if (p.kind === 'desktop') return byId.get(`desktop:${p.key}`) || null;
    const a = byId.get(p.key);
    if (!a || a.kind === 'linux') return null;
    return (p.kind === 'lpk') === !!a.package ? a : null;
  };
  const shown = (id) => (id === LAUNCHER ? !!opts.launcher : !!appFor(id));
  shownFn = shown;
  const pinned = fromSettings();
  // an older settings file (untyped ids, a locked item missing) is rewritten once
  useEffect(() => {
    const raw = s.dockPinned;
    if (raw != null && JSON.stringify(raw) !== JSON.stringify(pinned)) save(pinned);
  }, [JSON.stringify(s.dockPinned)]);
  const groups = new Map();
  for (const w of wins) {
    if (getApp(w.id)?.builtin && !String(w.id).startsWith('linux#')) continue;
    const k = dockIdOfWindow(w);
    if (!groups.has(k)) groups.set(k, []);
    groups.get(k).push(w);
  }
  const entry = (id, a, ws) => ({
    id, kind: parseDockId(id)?.kind || 'linux', launcherId: launcherKey(id),
    name: (a?.kind === 'linux' ? shortAppName(a) : a?.name) || (ws[0] ? windowAppName(ws[0]) : '') || id, Icon: a?.Icon || (ws[0] ? windowIcon(ws[0], getApp) : ({ size }) => h(LetterIcon, { size, name: id })),
    windows: ws, pinned: pinned.includes(id), running: ws.length > 0, focused: ws.some((w) => w.id === focused), app: a,
    locked: isLocked(id), movable: isMovable(id),
  });
  const out = [];
  for (const id of pinned) {
    if (id === LAUNCHER) {
      if (opts.launcher) out.push({ id, kind: 'shell', launcher: true, name: t('shell.menu.apps'), Icon: opts.launcher.Icon, windows: [],
        pinned: true, running: false, focused: false, locked: true, movable: true, onOpen: opts.launcher.onOpen });
      continue;
    }
    const a = appFor(id);
    if (!a) continue;                          // pinned but uninstalled: hidden, keeps its place
    out.push(entry(id, a, groups.get(id) || []));
  }
  // an unpinned app keeps its place in the order it opened: wins is the stacking order, which every raise changes
  for (const id of dockOpened.keys()) if (!groups.has(id)) dockOpened.delete(id);
  for (const id of groups.keys()) if (!dockOpened.has(id)) dockOpened.set(id, true);
  for (const id of dockOpened.keys()) if (!pinned.includes(id)) out.push(entry(id, appFor(id), groups.get(id)));
  if (opts.recents) {
    const have = new Set(out.map((e) => e.id));
    const rec = (s.dockRecentApps || []).map((x) => dockIdOf(x)).filter((x) => x && !have.has(x) && appFor(x)).slice(0, opts.recents);
    for (const id of rec) out.push({ ...entry(id, appFor(id), []), recent: true });
  }
  return out;
}

// Opens a Dock item through the existing safe paths only: wm.open / launchPackage(id) /
// lord.apps.launch(desktop id), all inside launchApp. No other way to run anything.
export function launchDockItem(id) {
  const p = parseDockId(id);
  if (!p || p.kind === 'shell') return false;
  if (p.kind === 'desktop') return launchApp(`desktop:${p.key}`);
  const a = getApp(p.key);
  if (!a || a.builtin || (p.kind === 'lpk') !== !!a.package) return false;
  return launchApp(p.key);
}

// Click on the icon: with no windows it opens the app; with windows it brings up the most recent one (if it
// was already in front and there are several, it moves to the next, like ❖`). A window on this desktop
// comes first; with none here it jumps to the desktop of the app's latest window.
export function dockActivate(e) {
  if (e.launcher) { e.onOpen?.(); return; }
  if (!e.windows.length) { if (!launchDockItem(e.id) && e.launcherId) launchApp(e.launcherId); return; }
  const top = wm.focused();
  const here = e.windows.filter(onActiveDesktop);
  const idx = here.findIndex((w) => w.id === top);
  const next = idx >= 0 && here.length > 1 ? here[(idx + here.length - 1) % here.length] : pickWindow(e.windows, activeDesktop.get());
  wm.focus(next.id);
}

// Quits an app: every window of its Dock entry closes, and each close guard still applies (it can
// keep a window with unsaved changes open). An isolated app (its own process) is asked through
// the broker, so its process closes its own windows and then ends.
export function quitApp(wins) {
  const iso = wins.find((w) => w.args?.pkg && w.args?.appId);
  if (iso && typeof lord !== 'undefined' && lord.broker && typeof lord.broker.quit === 'function' && lord.broker.quit(iso.args.appId)) return;
  for (const w of [...wins]) wm.close(w.id);
}

// ❖Q quits the app in front (sdk/shortcuts.js: only in the session, an isolated app never receives ❖Q).
export function quitFrontApp() {
  const top = wm.get(wm.focused());
  if (!top) return;
  const key = launcherIdOf(top);
  quitApp(windows.get().filter((w) => launcherIdOf(w) === key));
}

// ---- quick actions ----

// Can the session hand args and «new window» to an isolated built-in app's process?
const isolatedArgs = () => typeof lord !== 'undefined' && !!lord.broker && typeof lord.broker.openArgs === 'function';

// The quick actions of a Dock item: a LordOS app's (manifest "dockActions" + registerDockActions),
// or a Linux app's desktop actions (Actions= in its .desktop, run by lord.apps.launchAction).
export function dockQuickActions(e) {
  const p = parseDockId(e.id);
  if (!p) return [];
  if (p.kind === 'desktop') {
    if (typeof lord === 'undefined' || typeof lord.apps?.actions !== 'function') return [];
    let list = [];
    try { list = lord.apps.actions(p.key, locale.get().lang) || []; } catch (err) { list = []; }
    return list.filter((a) => a && a.id && a.name).map((a) => ({ label: a.name, onClick: () => lord.apps.launchAction(p.key, a.id) }));
  }
  if (p.kind !== 'app') return [];                          // .lpk: launchPackage takes no arguments
  const app = getApp(p.key);
  // an isolated built-in app (its own process) gets the action's args through the session
  // (sdk/packages.js isolatedApp → lord.broker.openArgs)
  if (!app || (app.isolated && !isolatedArgs())) return [];
  let fromModule = [];
  try { fromModule = actionProviders.get(p.key)?.() || []; } catch (err) { fromModule = []; }
  return cleanActions([...(Array.isArray(app.dockActions) ? app.dockActions : []), ...fromModule]).map((a) => ({
    label: t(a.label),
    onClick: () => wm.open(p.key, { args: { ...a.args, action: a.id }, new: a.new }),
  }));
}

// ---- the menu ----

export function dockMenu(e, opts = {}) {
  if (e.launcher) return [{ label: t('shell.dock.open'), onClick: () => e.onOpen?.() }];
  const top = wm.focused();
  const items = [];
  const quick = dockQuickActions(e);
  if (quick.length) items.push(...quick, '-');
  if (e.windows.length) {
    for (const w of [...e.windows].reverse()) {
      items.push({ label: `${windowTitle(w) || e.name}${desktopSuffix(w)}`, checked: w.id === top, hint: w.minimized ? '–' : '', onClick: () => wm.focus(w.id) });
    }
    items.push('-');
  }
  const p = parseDockId(e.id);
  const appKey = p && p.kind === 'app' ? p.key : null;
  const custom = appKey && menuProviders.get(appKey) ? menuProviders.get(appKey)(e) : null;
  if (custom && custom.length) items.push(...custom, '-');
  else if (e.windows.length && p && !(appKey && getApp(appKey)?.singleWindow)) {
    const iso = appKey && getApp(appKey)?.isolated;
    items.push({ label: t('shell.dock.newWindow'), onClick: () => (appKey && (!iso || isolatedArgs()) ? wm.open(appKey, { new: true }) : launchDockItem(e.id)) }, '-');
  }
  if (p) {
    const options = [];
    if (!dock.isPinned(e.id)) options.push({ label: opts.keepLabel || t('shell.dock.keep'), onClick: () => dock.pin(e.id) });
    else if (!isLocked(e.id)) options.push({ label: opts.removeLabel || t('shell.dock.remove'), onClick: () => dock.unpin(e.id) });
    // where the app lives, when Files can open it (a .lpk's folder; a .desktop of the user's own)
    if (p.kind === 'lpk' && getApp(p.key)?.dir) options.push({ label: t('shell.dock.showInFiles'), onClick: () => wm.open('files', { args: { path: getApp(p.key).dir } }) });
    if (p.kind === 'desktop' && e.app?.user) options.push({ label: t('shell.dock.showInFiles'), onClick: () => wm.open('files', { args: { path: '~/.local/share/applications' } }) });
    // Open at Login (0.8.5): started through the same safe path as a Dock click, once per session
    if (p.kind !== 'shell') {
      const on = loginApps().includes(e.id);
      options.push({ label: t('shell.dock.openAtLogin'), checked: on, onClick: () => settings.set({ loginApps: on ? loginApps().filter((x) => x !== e.id) : [...loginApps(), e.id] }) });
    }
    // Assign to a display (0.8.5): its new windows open there; by the display's connector (two monitors
    // of the same model share a name), kept across boots
    if (screen.displays.length > 1 && e.launcherId) {
      const cur = assignedDisplay((settings.get().appDisplays || {})[e.launcherId]);
      const set = (name) => { const m = { ...(settings.get().appDisplays || {}) }; if (name) m[e.launcherId] = name; else delete m[e.launcherId]; settings.set({ appDisplays: m }); };
      options.push('-', { label: t('shell.dock.assignTo'), disabled: true });
      screen.displays.forEach((d, i) => options.push({ label: t('shell.dock.assignDisplay', { n: i + 1, name: d.name || '' }).trim(), checked: cur === d, onClick: () => set(displayKey(d)) }));
      options.push({ label: t('shell.dock.assignNone'), checked: !cur, onClick: () => set('') });
    }
    if (options.length) items.push({ label: t('shell.dock.options'), submenu: options }, '-');
  }
  if (e.windows.length) {
    items.push(
      { label: t('shell.dock.showAll'), onClick: () => { for (const w of e.windows) wm.focus(w.id); } },
      { label: t('shell.dock.hide'), onClick: () => { for (const w of e.windows) wm.minimize(w.id); } },
      { label: t('shell.dock.quit'), onClick: () => quitApp(e.windows) },
    );
  } else items.push({ label: t('shell.dock.open'), onClick: () => dockActivate(e) });
  while (items[items.length - 1] === '-') items.pop();
  return items;
}

// ---- the Trash (always the last Dock item) ----
// Full or empty, from ~/.local/share/Trash/files: a stat every 2 s while some Dock shows it (its
// mtime changes when something goes in or out), and the whole list (disks' trashes too) only when
// it changed or every 30 s. Never a busy loop.
export const trashState = createStore({ full: false });
let trashSubs = 0, trashTimer = 0, trashMtime = null, trashTicks = 0;
function trashCheck(force) {
  let m = 0;
  try { m = fs.stat(`${fs.home()}/.local/share/Trash/files`).mtime || 0; } catch (err) { m = 0; }
  if (!force && m === trashMtime && trashTicks++ % 15) return;
  trashMtime = m;
  let full = false;
  try { full = fs.trashList().length > 0; } catch (err) { full = false; }
  if (full !== trashState.get().full) trashState.set({ full });
}
export function useTrash() {
  useEffect(() => {
    if (!trashSubs++) { trashCheck(true); trashTimer = setInterval(() => trashCheck(false), 2000); }
    return () => { if (!--trashSubs) clearInterval(trashTimer); };
  }, []);
  return useStore(trashState);
}
export const refreshTrash = () => trashCheck(true);
export const openTrash = () => wm.open('files', { args: { place: 'trash' } });
export function emptyTrash() { try { fs.emptyTrash(); } finally { trashCheck(true); } }
// Right-click on the Trash. onEmpty: the theme asks first (an in-app confirmation), then calls emptyTrash().
export function trashMenu({ onEmpty }) {
  return [
    { label: t('shell.dock.open'), onClick: openTrash },
    '-',
    { label: t('shell.trash.empty'), disabled: !trashState.get().full, onClick: onEmpty },
  ];
}

// ---- dragging (shared by the Dock and the taskbar) ----
// The theme starts a press with beginDockDrag on mouse down (an icon of the Dock, the app launcher
// or the start menu); past DRAG_START px it becomes a drag. The bar that accepts drops registers
// a target: hit(x, y) → { index } (a position among the shown pinned items) | { trash: true } | null,
// and near(x, y) → is the pointer still close to the bar. A Dock item taken more than about 60 px
// away and held there for REMOVE_HOLD ms shows the remove cue; releasing it then unpins it (a
// locked item springs back). dockDrag holds what is happening, for the theme to draw.
const DRAG_START = 6, REMOVE_HOLD = 350;
export const dockDrag = createStore(null);   // { id, Icon, x, y, source, over, out, removing, removable }
export const dockPoof = createStore(null);   // { x, y, Icon, t0 }: the removal animation, drawn by the theme
let target = null, press = null, holdTimer = 0, endedAt = 0;
export function registerDockTarget(tg) { target = tg; return () => { if (target === tg) target = null; }; }
// true just after a drag ended (the click that follows a release must not open the app)
export const dockDragJustEnded = () => Date.now() - endedAt < 250;

export function beginDockDrag({ id, Icon, x, y, source = 'dock', onStart }) {
  const d = dockIdOf(id);
  if (!d) return;
  press = { id: d, Icon, x, y, source, onStart, started: false, outSince: 0 };
}
function dragState(x, y) {
  const over = target ? target.hit(x, y) : null;
  const removable = press.source === 'dock' && dock.isPinned(press.id) && !isLocked(press.id);
  const out = press.source === 'dock' && !over && !(target && target.near(x, y));
  if (out && !press.outSince) {
    press.outSince = Date.now();
    clearTimeout(holdTimer);
    holdTimer = setTimeout(() => { if (press?.started) dockDrag.set(dragState(press.lx, press.ly)); }, REMOVE_HOLD + 10);
  } else if (!out) press.outSince = 0;
  const removing = removable && out && Date.now() - press.outSince >= REMOVE_HOLD;
  return { id: press.id, Icon: press.Icon, x, y, source: press.source, over, out, removing, removable, movable: isMovable(press.id) };
}
addListener('pointermove', (e) => {
  if (!press) return;
  press.lx = e.x; press.ly = e.y;
  if (!press.started) {
    if (Math.hypot(e.x - press.x, e.y - press.y) < DRAG_START) return;
    press.started = true;
    press.onStart?.();
  }
  dockDrag.set(dragState(e.x, e.y));
});
addListener('pointerup', (e) => {
  const p = press;
  press = null;
  clearTimeout(holdTimer);
  if (!p || !p.started) return;
  endedAt = Date.now();
  const over = target ? target.hit(e.x, e.y) : null;
  const st = dockDrag.get();
  dockDrag.set(null);
  if (over && over.trash) {
    if (p.source === 'dock' && dock.unpin(p.id)) dockPoof.set({ x: e.x, y: e.y, Icon: p.Icon, t0: Date.now() });
    return;
  }
  if (over && over.index != null) {
    if (p.source === 'dock' && dock.isPinned(p.id)) dock.move(p.id, over.index); else dock.pin(p.id, over.index);
    return;
  }
  if (p.source === 'dock' && st?.removing && dock.unpin(p.id)) dockPoof.set({ x: e.x, y: e.y, Icon: p.Icon, t0: Date.now() });
});
// Escape cancels a drag.
addListener('keydown', (e) => {
  if (e.key !== 'Escape' || !press) return;
  press = null;
  clearTimeout(holdTimer);
  if (dockDrag.get()) { endedAt = Date.now(); dockDrag.set(null); }
});

// ---- Open at Login and display assignments (0.8.5) ----
const loginApps = () => (Array.isArray(settings.get().loginApps) ? settings.get().loginApps : []);
// An assignment is the display's connector ("HDMI-A-1"); one saved before by name still works while
// that name belongs to a single display.
const displayKey = (d) => d.connector || d.name;
function assignedDisplay(v) {
  if (!v) return null;
  const byConn = screen.displays.find((x) => x.connector && x.connector === v);
  if (byConn) return byConn;
  const byName = screen.displays.filter((x) => x.name === v);
  return byName.length === 1 ? byName[0] : null;
}
setDisplayAssignment((key) => {
  const d = assignedDisplay((settings.get().appDisplays || {})[key]);
  return d ? d.id : null;
});
// once per session, a few seconds after it starts (never in the live system's first run, never in an
// isolated app's process), each through launchDockItem: the same safe paths as a Dock click
if (typeof lord !== 'undefined' && typeof lord.appId !== 'string' && typeof lord.greeter === 'undefined' && !isLiveSystem()) {
  setTimeout(() => { for (const id of loginApps()) { if (restoredIds.has(id)) continue; try { launchDockItem(id); } catch (e) { /* uninstalled */ } } }, 3000);
}
