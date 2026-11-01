// "Lord Flow" theme (id default) — a menu bar and dock layout, with its own assets.
// The theme builds the desktop (wallpaper, bar, dock, app launcher, Control Center) and the
// window frames. Apps come from apps/ through the SDK.
import {
  startShell, View, Text, Image, screen, ThemeProvider, useSettings, useWindows, configureWindows, getApp, wm, addListener, t as tr,
  useLauncherApps, launchApp, useDockEntries, dockActivate, dockMenu, dockPrefs, DOCK_LAUNCHER,
  useTrash, openTrash, emptyTrash, trashMenu, ConfirmDialog,
  DesktopWidgets, registerGestureActions, screenDisplays, mainDisplay, settings, Layer, PowerDialog,
  findWallpaper, wallpaperSrc, useShortcuts, AppSwitcher, openSwitcher, cycleAppWindows, hideApp, hideOthers,
  toggleShowDesktop, appSettingsItem, flowDesktopKeys, flowGrabs, desktopSwipe, DesktopHud, OverviewBackdrop, OverviewChrome,
  openOverview, closeOverview, toggleOverview,
} from '@lordos/sdk';
import { apps } from '@lordos/apps';
import { useEffect, useRef, useState } from 'react';
import { makeTheme } from './tokens.js';
import { MenuBar } from './components/MenuBar.jsx';
import { Dock, LaunchpadIcon, dockThickness } from './components/Dock.jsx';
import { Window } from './components/Window.jsx';
import { Launchpad } from './components/Launchpad.jsx';
import { ControlCenter } from './components/ControlCenter.jsx';
import { Menu } from './components/Menu.jsx';

const MENU_H = 28;
// Files is always first and the Trash always last (sdk/dockmodel.js); the app launcher follows Files.
const DOCK_ORDER = ['files', DOCK_LAUNCHER, 'terminal', 'calculator', 'settings', 'about'];

// Area for windows: below the menu bar and without the Dock's side (unless it auto-hides),
// so maximized and new windows don't end up under the Dock.
// Several displays (docs/DISPLAYS.md): the menu bar and the Dock go on the main one; the menu bar
// also on the others with Settings → Displays → "Show menu bar on all displays".
function workAreaFor(p, d, menuAll) {
  const bar = d.main || menuAll ? MENU_H : 0;
  const a = { x: d.x, y: d.y + bar, w: d.w, h: d.h - bar };
  if (!d.main || p.dockAutohide) return a;
  const k = dockThickness(p.dockSize);
  if (p.dockPosition === 'left') { a.x += k; a.w -= k; }
  else if (p.dockPosition === 'right') a.w -= k;
  else a.h -= k;
  return a;
}
const areaFn = (d) => { const s = settings.get(); return workAreaFor(dockPrefs(s), d, !!s.menuBarAllDisplays); };

// chrome: borders (1 + 1) and title bar (40 + 1 separator) of Window.jsx's frame
configureWindows({ apps, workArea: areaFn, chrome: { w: 2, h: 43 } });

// Pinned by default (the user changes the Dock by dragging and with «Keep in Dock»: settings.dockPinned).
const DOCK_DEFAULT = [...DOCK_ORDER, ...apps.filter((a) => a.dock && !DOCK_ORDER.includes(a.id)).map((a) => a.id)];

// Minimize effect: a window that was just minimized stays drawn for MIN_MS, shrinking towards the
// Dock (transform: scale, painted from a cached raster by lord-server; layout doesn't change).
const MIN_MS = 260;
function useMinimizing(windows) {
  const prev = useRef(new Map());
  const [anim, setAnim] = useState([]);                 // [{ id, t0 }]
  useEffect(() => {
    const started = [];
    for (const w of windows) {
      if (w.minimized && prev.current.get(w.id) === false && !w.fullscreen && !w.pip) started.push({ id: w.id, t0: Date.now() });
      prev.current.set(w.id, !!w.minimized);
    }
    if (started.length) setAnim((a) => [...a.filter((x) => !started.some((y) => y.id === x.id)), ...started]);
  }, [windows]);
  const [, tick] = useState(0);
  useEffect(() => {
    if (!anim.length) return undefined;
    let raf = requestAnimationFrame(function step() {
      const now = Date.now();
      if (anim.some((x) => now - x.t0 >= MIN_MS)) setAnim((a) => a.filter((x) => now - x.t0 < MIN_MS));
      else { tick((n) => n + 1); raf = requestAnimationFrame(step); }
    });
    return () => cancelAnimationFrame(raf);
  }, [anim]);
  const now = Date.now();
  return anim.map((x) => ({ win: windows.find((w) => w.id === x.id), p: Math.min(1, (now - x.t0) / MIN_MS) }))
    .filter((x) => x.win && x.win.minimized);
}

// Lord Flow's own shortcuts (the common ones are in sdk/shortcuts.js).
const onFront = (fn) => () => { const id = wm.focused(); if (id) fn(id); };
const minimizeFront = onFront(wm.minimize);
const hideFront = onFront(hideApp);
const hideOtherApps = onFront(hideOthers);
const openForceQuit = () => wm.open('forcequit');
const toggleDockAutohide = () => settings.set({ dockAutohide: !dockPrefs(settings.get()).dockAutohide });
const frontSettings = () => { const id = wm.focused(); return id ? appSettingsItem(id) : null; };
const openFrontSettings = () => frontSettings()?.onClick();
const switchApps = (e) => openSwitcher(8, e.shift);
const cycleWindows = (e) => cycleAppWindows(e.shift);

const flowTable = (toggleLauncher) => [
  { id: 'launcher', keys: ['F4'], show: ['❖', 'F4'], run: toggleLauncher, label: 'shell.shortcuts.launcher', group: 'system' },
  { id: 'showDesktop', keys: ['Meta+F11'], run: toggleShowDesktop, label: 'shell.shortcuts.showDesktop', group: 'system' },
  { id: 'dockAutohide', keys: ['Meta+Alt+D'], run: toggleDockAutohide, label: 'shell.shortcuts.dockAutohide', group: 'system' },
  { id: 'switchApps', keys: ['Meta+Tab', 'Meta+Shift+Tab'], run: switchApps, label: 'shell.shortcuts.switchApps', group: 'windows' },
  { id: 'cycleWindows', keys: ['Meta+`', 'Meta+Shift+`'], run: cycleWindows, label: 'shell.shortcuts.cycleWindows', group: 'windows' },
  { id: 'minimize', keys: ['Meta+M'], run: minimizeFront, label: 'shell.shortcuts.minimize', group: 'windows' },
  { id: 'hide', keys: ['Meta+H'], run: hideFront, label: 'shell.shortcuts.hide', group: 'windows' },
  { id: 'hideOthers', keys: ['Meta+Alt+H'], run: hideOtherApps, label: 'shell.shortcuts.hideOthers', group: 'windows' },
  { id: 'forceQuit', keys: ['Meta+Alt+Escape'], run: openForceQuit, label: 'shell.shortcuts.forceQuit', group: 'apps' },
  { id: 'appSettings', keys: ['Meta+,'], run: openFrontSettings, when: frontSettings, label: 'shell.shortcuts.appSettings', group: 'apps' },
  ...flowDesktopKeys,
];

// The Dock's right-click menus open toward the inside of the screen, next to the icon.
function DockMenu({ pos, at, items, onClose }) {
  return pos === 'bottom'
    ? <Menu above side="right" x={at.x - 110} y={at.y} onClose={onClose} items={items} />
    : <Menu centered side={pos === 'left' ? 'right' : 'left'} x={at.x} y={at.y} onClose={onClose} items={items} />;
}

function App() {
  const [s] = useSettings();
  const t = makeTheme(s);
  const { windows, mounted, focused } = useWindows();
  const minimizing = useMinimizing(windows);
  const minFx = (id) => { const m = minimizing.find((x) => x.win.id === id); return m ? { p: m.p, x: main.x + main.w / 2, y: main.y + main.h - 36 } : undefined; };
  const [launchpad, setLaunchpad] = useState(false);
  const [control, setControl] = useState(null);         // the display whose menu bar opened it
  const [context, setContext] = useState(null);
  const [dockCtx, setDockCtx] = useState(null);
  const [trashCtx, setTrashCtx] = useState(null);         // where the Trash's menu opens
  const [askEmpty, setAskEmpty] = useState(false);
  const trash = useTrash();
  const launcherApps = useLauncherApps();
  const dp = dockPrefs(s);
  // the work area follows the Dock live (position, size, auto-hide, another screen)
  useEffect(() => { configureWindows({ apps, workArea: areaFn }); },
    [dp.dockPosition, dp.dockSize, dp.dockAutohide, s.menuBarAllDisplays, screen.width, screen.height]);
  const main = mainDisplay();
  const all = screenDisplays();
  const bars = s.menuBarAllDisplays && !screen.mirror ? all : [main];

  // F4 opens the app launcher; ❖ too, but only if released without being used in a shortcut.
  const toggleLauncher = () => { closeOverview(); setLaunchpad((v) => !v); };
  useShortcuts({ grabs: flowGrabs(), table: flowTable(toggleLauncher), onSuperTap: toggleLauncher, appsPaste: true });
  useEffect(() => addListener('keydown', (e) => { if (e.key === 'Escape') { setControl(null); setContext(null); } }), []);

  // Trackpad: a 3-finger swipe up opens the app launcher (4 fingers: the overview) and down closes
  // them; left/right switch desktops, or windows with only one desktop. See docs/TRACKPAD.md.
  useEffect(() => registerGestureActions({
    up: (g) => { if (g.fingers >= 4) { setLaunchpad(false); openOverview(); } else setLaunchpad(true); },
    down: () => { setLaunchpad(false); closeOverview(); },
    left: desktopSwipe('left'), right: desktopSwipe('right'),
  }), []);

  const wallpaper = findWallpaper(t.wallpapers, s.wallpaper);
  // Pinned and open apps; windows of the same app (Linux too) share a single icon. The app launcher
  // is a Dock item too (it moves, but can't be removed).
  const entries = useDockEntries(DOCK_DEFAULT, { recents: dp.dockRecents ? 3 : 0, launcher: { Icon: LaunchpadIcon, onOpen: () => setLaunchpad(true) } });
  const byId = new Map(entries.map((e) => [e.id, e]));

  return (
    <ThemeProvider tokens={t}>
      <View style={{ width: '100%', height: '100%' }}>
        {all.map((d) => (
          <Image key={d.id + ':' + d.x} src={wallpaperSrc(wallpaper, d.w * (d.scale || 100) / 100, d.h * (d.scale || 100) / 100)} onContextMenu={(e) => setContext({ x: e.x, y: e.y })}
            style={{ position: 'absolute', left: d.x, top: d.y, width: d.w, height: d.h, objectFit: 'cover' }} />
        ))}
        <DesktopWidgets layer="below" />
        <OverviewBackdrop />
        {mounted.map((w) => <Window key={w.id} win={w} app={getApp(w.id)} t={t} focused={w.id === focused} fx={minFx(w.id)} />)}
        {minimizing.filter((x) => !mounted.includes(x.win)).map(({ win }) => <Window key={win.id} win={win} app={getApp(win.id)} t={t} focused={false} fx={minFx(win.id)} />)}
        <DesktopWidgets layer="above" />
        {/* Everything above the windows goes in its layer (sdk/layers.js, docs/LAYERS.md): the
            order is the layers', not this tree's, and clicks follow it. Menus go to «popover»
            by themselves (Menu.jsx). */}
        <Layer name="menubar">
          {bars.map((d) => (
            <MenuBar key={d.id + ':' + d.x} t={t} height={MENU_H} focused={focused} controlCenter={!!control} area={d}
              onLaunchpad={() => setLaunchpad(true)} onControlCenter={() => setControl((v) => (v ? null : d))} />
          ))}
        </Layer>
        <Layer name="dock">
        <Dock t={t} items={entries} running={entries.filter((e) => e.running).map((e) => e.id)}
          onOpen={(id) => byId.has(id) && dockActivate(byId.get(id))}
          onContext={(id, at) => byId.has(id) && setDockCtx({ entry: byId.get(id), at })}
          onTrash={openTrash} onTrashContext={(at) => setTrashCtx(at)} trashFull={trash.full}
          position={dp.dockPosition} size={dp.dockSize} magnify={dp.dockMagnify} maxSize={dp.dockMagnifySize}
          autohide={dp.dockAutohide} top={MENU_H} keepOpen={!!dockCtx || !!trashCtx} area={main} />
        </Layer>
        {dockCtx && <DockMenu pos={dp.dockPosition} at={dockCtx.at} onClose={() => setDockCtx(null)} items={dockMenu(byId.get(dockCtx.entry.id) || dockCtx.entry)} />}
        {trashCtx && <DockMenu pos={dp.dockPosition} at={trashCtx} onClose={() => setTrashCtx(null)} items={trashMenu({ onEmpty: () => setAskEmpty(true) })} />}
        {askEmpty && (
          <Layer name="modal">
            <ConfirmDialog title={tr('shell.trash.confirmTitle')} detail={tr('shell.trash.confirmDetail')} okLabel={tr('shell.trash.empty')}
              cancelLabel={tr('shell.trash.cancel')} destructive area={main}
              onCancel={() => setAskEmpty(false)} onOk={() => { setAskEmpty(false); emptyTrash(); }} />
          </Layer>
        )}
        {/* Restart… and Shut Down… (sdk/power.jsx): inside the ThemeProvider, so it takes the theme's colors */}
        <Layer name="modal"><PowerDialog /></Layer>
        <AppSwitcher />
        <OverviewChrome />
        <DesktopHud />
        {control && <Layer name="popover"><ControlCenter t={t} top={MENU_H} area={control} onClose={() => setControl(null)} /></Layer>}
        {context && (
          <Menu x={context.x} y={context.y} onClose={() => setContext(null)} items={[
            { label: tr('shell.desktop.wallpaper'), onClick: () => setControl(main) },
            { label: tr('shell.desktop.openLaunchpad'), onClick: () => setLaunchpad(true) },
            { label: tr('shell.desktops.overview'), onClick: () => toggleOverview() },
            '-',
            { label: tr('shell.menu.settings'), onClick: () => wm.open('settings') },
            { label: tr('shell.menu.about'), onClick: () => wm.open('about') },
          ]} />
        )}
        {launchpad && <Layer name="overlay"><Launchpad t={t} apps={launcherApps} onOpen={launchApp} onClose={() => setLaunchpad(false)} area={main} /></Layer>}
      </View>
    </ThemeProvider>
  );
}

startShell(<App />);
