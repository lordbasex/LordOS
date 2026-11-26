// Celeste (id celeste), a community theme: a frame around each display with rounded inner corners, a bar on its
// thick side and drawers that slide out of its edges (dashboard on top, app launcher at the bottom, session and
// sidebar at the end). Layout inspired by Caelestia (caelestia-dots/shell, GPL-3.0); no code or assets from it.
//
// Several displays: every display gets the frame and the bar unless Settings → Desktop & Dock → Menu Bar → "Show the
// menu bar on every display" is off or the displays are mirrored, as Flow's menu bar (sdk/menubarmodel.js
// hasBar); the wallpaper is each display's own (wallpaperIdFor, by connector). In Arabic and Hebrew the frame is
// mirrored. The theme's options (colour scheme, frame, hover edges, wallpaper effect) are in theme.json and shown by
// Settings → Appearance.
import {
  AppSwitcher, ConfirmDialog, DesktopHud, DesktopWidgets, Layer, LevelHud, MotionWallpaper, OverviewBackdrop, OverviewChrome,
  PowerDialog, ThemeProvider, View, WidgetDropTarget, WidgetPicker, addListener, appSettingsItem, closeOverview, configureWindows,
  cycleAppWindows, desktopSwipe, direction, displayAt, dndActive, findWallpaper, flowDesktopKeys, flowGrabs, getApp, hasBar, hideApp,
  hideOthers, keepWindowsInWorkAreas, mainDisplay, menuBarEveryDisplay, nightActive, notifyHistory, notifyPrefs, openOverview,
  openSwitcher, registerGestureActions, screen, screenDisplays, session, setNightManual, settings, snapPreview, startDeskWidgets,
  startShell, switcherGrabs, switcherHold, switcherKeys, t as tr, themeOptionPatch, toggleOverview, toggleShowDesktop, useSettings,
  useShortcuts, useStore, useWindows, wallpaperIdFor, wallpaperSrc, widgetPicker, wm,
  titleBarPx,
} from '@lordos/sdk';
import { apps } from '@lordos/apps';
import { Fragment, useEffect, useRef, useState } from 'react';
import { makeTheme } from './tokens.js';
import { minimizeStarts } from './minimize.js';
import { OPTIONS, THEME_ID, caeTokens, option } from './cae.js';
import { DWELL, drawerRect, frameGeom, hoverZone, outside, workArea } from './model.js';
import { Guard } from './components/Guard.jsx';
import { Frame } from './components/Frame.jsx';
import { Bar } from './components/Bar.jsx';
import { Backdrop } from './components/Backdrop.jsx';
import { Drawer } from './components/Drawer.jsx';
import { Dashboard, Launcher, SessionPanel, Sidebar, launcherHeight } from './components/Panels.jsx';
import { Window, TITLE_H, MENU_H } from './components/Window.jsx';
import { Menu } from '../classic/components/Menu.jsx';

const MIN_MS = 260;
const rtlNow = () => direction() === 'rtl';
const framed = (d, s) => hasBar(d, menuBarEveryDisplay(s), screen.mirror);
const areaFn = (d) => workArea(d, framed(d, settings.get()), rtlNow());

// chrome: borders (1 + 1), the title bar with its separator and the menu strip of a window with menus (a Linux app's
// Edit menu with Speech); snapping on (Settings → Desktop & Dock → Windows)
configureWindows({ apps, workArea: areaFn, chrome: () => ({ w: 2, h: titleBarPx(TITLE_H) + 3, menu: MENU_H }), snap: true });

// The minimize effect: a window that was just minimized shrinks towards the bar for MIN_MS (none with reduce motion).
function useMinimizing(windows) {
  const prev = useRef(new Map());
  const [anim, setAnim] = useState([]);
  useEffect(() => {
    const started = minimizeStarts(windows, prev.current, { reduce: !!settings.get().reduceMotion });
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
  return anim.map((x) => ({ win: windows.find((w) => w.id === x.id), p: Math.min(1, (now - x.t0) / MIN_MS) })).filter((x) => x.win && x.win.minimized);
}

function SnapPreview({ cx }) {
  const p = useStore(snapPreview);
  if (!p) return null;
  const r = p.rect;
  return (
    <View style={{ position: 'absolute', left: r.x + 8, top: r.y + 8, width: r.w - 16, height: r.h - 16, borderRadius: 16, pointerEvents: 'none',
      backgroundColor: cx.scrim, border: `2px solid ${cx.primary}` }} />
  );
}

const onFront = (fn) => () => { const id = wm.focused(); if (id) fn(id); };
const frontSettings = () => { const id = wm.focused(); return id ? appSettingsItem(id) : null; };
const shortcutTable = (toggleLauncher) => [
  { id: 'launcher', keys: ['F4'], show: ['❖', 'F4'], run: toggleLauncher, label: 'shell.shortcuts.launcher', group: 'system' },
  { id: 'showDesktop', keys: ['Meta+F11'], run: toggleShowDesktop, label: 'shell.shortcuts.showDesktop', group: 'system' },
  { id: 'switchApps', keys: switcherKeys('meta'), run: (e) => openSwitcher(switcherHold(e), e.shift), label: 'shell.shortcuts.switchApps', group: 'windows' },
  { id: 'cycleWindows', keys: ['Meta+`', 'Meta+Shift+`'], run: (e) => cycleAppWindows(e.shift), label: 'shell.shortcuts.cycleWindows', group: 'windows' },
  { id: 'minimize', keys: ['Meta+M'], run: onFront(wm.minimize), label: 'shell.shortcuts.minimize', group: 'windows' },
  { id: 'hide', keys: ['Meta+H'], run: onFront(hideApp), label: 'shell.shortcuts.hide', group: 'windows' },
  { id: 'hideOthers', keys: ['Meta+Alt+H'], run: onFront(hideOthers), label: 'shell.shortcuts.hideOthers', group: 'windows' },
  { id: 'forceQuit', keys: ['Meta+Alt+Escape'], run: () => wm.open('forcequit'), label: 'shell.shortcuts.forceQuit', group: 'apps' },
  { id: 'appSettings', keys: ['Meta+,'], run: () => frontSettings()?.onClick(), when: frontSettings, label: 'shell.shortcuts.appSettings', group: 'apps' },
  ...flowDesktopKeys,
];

function App() {
  startDeskWidgets();
  const [s] = useSettings();
  const t = makeTheme(s);
  const cx = caeTokens(s);
  const reduce = !!s.reduceMotion;
  const rtl = rtlNow();
  const { windows, mounted, focused } = useWindows();
  const preview = useStore(snapPreview);
  const top = mounted[mounted.length - 1];
  const main = mainDisplay();
  const all = screenDisplays();
  const every = menuBarEveryDisplay(s);
  const frames = all.filter((d) => framed(d, s)).map((d) => ({ d, ...frameGeom(d, rtl) }));
  const mainFrame = frames.find((f) => f.d.id === main.id) || frames[0];
  const minimizing = useMinimizing(windows);
  const minFx = (id) => {
    const m = minimizing.find((x) => x.win.id === id);
    return m && mainFrame ? { p: m.p, x: mainFrame.bar.x + mainFrame.bar.w / 2, y: mainFrame.bar.y + mainFrame.bar.h / 2 } : undefined;
  };

  // the open drawer: { name: 'dash' | 'launcher' | 'session' | 'sidebar', display: id, hover: opened by a hover edge }
  const [open, setOpen] = useState(null);
  const [launcherOn, setLauncherOn] = useState(false);   // the launcher's layer stays while it slides back
  const [launcherRows, setLauncherRows] = useState(7);
  const [askLogout, setAskLogout] = useState(false);
  const [context, setContext] = useState(null);
  const close = () => setOpen(null);
  const toggle = (display) => (name) => setOpen((o) => (o && o.name === name && o.display === display ? null : { name, display, hover: false }));
  const openName = (name, display) => { if (name === 'launcher') setLauncherOn(true); setOpen({ name, display, hover: false }); };

  // the unread dot: a notification came since the sidebar was last out
  const history = useStore(notifyHistory);
  const seen = useRef(0);
  if (open?.name === 'sidebar' && history.length) seen.current = Math.max(seen.current, history[0].id);
  const unread = history.some((n) => n.id > seen.current);

  // the hover edges (theme option), a click outside a drawer and Escape close it
  const live = useRef({});
  const rectOf = (o) => { const f = o && frames.find((x) => x.d.id === o.display); return f ? drawerRect(o.name, f.inner, rtl, launcherHeight(launcherRows)) : null; };
  live.current = { ...live.current, open, frames, mainFrame, rtl, hover: option(s, 'hover'), rect: rectOf(open) };
  useEffect(() => {
    let down = false, timer = 0, pending = null;
    const cancel = () => { clearTimeout(timer); pending = null; };
    const offs = [
      addListener('pointerdown', (e) => {
        down = true;
        cancel();
        const L = live.current;
        if (!L.open || !L.rect) return;
        const onBar = L.frames.some((f) => !outside(e.x, e.y, f.bar));
        if (!onBar && outside(e.x, e.y, L.rect)) setOpen(null);
      }),
      addListener('pointerup', () => { down = false; }),
      addListener('pointermove', (e) => {
        live.current.pointer = { x: e.x, y: e.y };
        if (down) return;
        const L = live.current;
        if (L.open) {
          // a drawer a hover edge opened closes when the pointer goes well away from it
          if (L.open.hover && L.open.name !== 'launcher' && outside(e.x, e.y, L.rect, 40)) setOpen(null);
          return;
        }
        if (!L.hover) { cancel(); return; }
        const f = L.frames.find((x) => e.x >= x.d.x && e.x < x.d.x + x.d.w && e.y >= x.d.y && e.y < x.d.y + x.d.h);
        const z = f ? hoverZone(e.x, e.y, f.d, L.rtl) : null;
        const key = z ? `${z}@${f.d.id}` : null;
        if (key === pending) return;
        cancel();
        if (!z) return;
        pending = key;
        timer = setTimeout(() => {
          pending = null;
          if (live.current.open) return;
          if (z === 'launcher') setLauncherOn(true);
          setOpen({ name: z, display: f.d.id, hover: true });
        }, DWELL[z]);
      }),
      addListener('keydown', (e) => { if (e.key === 'Escape') { setOpen(null); setContext(null); } }),
    ];
    return () => { offs.forEach((off) => off()); clearTimeout(timer); };
  }, []);

  const toggleLauncher = () => {
    closeOverview();
    const L = live.current;
    if (L.open && L.open.name === 'launcher') { setOpen(null); return; }
    const p = L.pointer;
    const f = (p && L.frames.find((x) => p.x >= x.d.x && p.x < x.d.x + x.d.w && p.y >= x.d.y && p.y < x.d.y + x.d.h)) || L.mainFrame;
    if (f) openName('launcher', f.d.id);
  };
  useShortcuts({ grabs: switcherGrabs(flowGrabs()), table: shortcutTable(toggleLauncher), onSuperTap: toggleLauncher, appsPaste: true });
  useEffect(() => registerGestureActions({
    up: (g) => { if (g.fingers >= 4) { setOpen(null); openOverview(); } else if (mainFrame) openName('launcher', mainFrame.d.id); },
    down: () => { setOpen(null); closeOverview(); },
    left: desktopSwipe('left'),
    right: desktopSwipe('right'),
  }), [mainFrame?.d.id]);

  // the work area follows the frames (another display, mirror, the per-display setting, the reading direction)
  useEffect(() => { configureWindows({ apps, workArea: areaFn }); }, [every, screen.mirror, screen.width, screen.height, rtl, all.length]);
  const barsWere = useRef(every);
  useEffect(() => { if (barsWere.current !== every) { barsWere.current = every; keepWindowsInWorkAreas(); } }, [every]);

  const nextScheme = () => {
    const ids = OPTIONS.scheme.choices.map((c) => c[0]);
    const x = settings.get();
    settings.set(themeOptionPatch(x, THEME_ID, 'scheme', ids[(ids.indexOf(option(x, 'scheme')) + 1) % ids.length]));
  };
  const doAction = (id) => {
    if (id === 'wallpaper') wm.open('settings', { args: { section: 'wallpaper' } });
    else if (id === 'options') wm.open('settings', { args: { section: 'appearance' } });
    else if (id === 'lock') session.lock();
    else if (id === 'settings') wm.open('settings');
    else if (id === 'terminal') wm.open('terminal');
    else if (id === 'dnd') settings.set((x) => ({ ...x, notifications: { ...notifyPrefs(x), dnd: !dndActive() } }));
    else if (id === 'night') setNightManual(!nightActive());
    else if (id === 'scheme') nextScheme();
  };

  const effect = option(s, 'wpEffect');
  const drawer = (f, name, content, extra = {}) => (
    <Drawer key={`${name}:${f.d.id}`} color={cx.frame} rtl={rtl} reduce={reduce}
      rect={drawerRect(name, f.inner, rtl, name === 'launcher' ? launcherHeight(launcherRows) : undefined)}
      open={!!open && open.name === name && open.display === f.d.id} {...extra}>
      {content}
    </Drawer>
  );

  return (
    <ThemeProvider tokens={t}>
      <View style={{ width: '100%', height: '100%' }}>
        {all.map((d) => {
          const wp = findWallpaper(t.wallpapers, wallpaperIdFor(s, d));
          return (
            <Backdrop key={`${d.id}:${d.x}`} display={d} wid={wp ? wp.id : null} effect={effect} reduce={reduce} bg={cx.frameSolid}
              src={wallpaperSrc(wp, (d.w * (d.scale || 100)) / 100, (d.h * (d.scale || 100)) / 100)} onContextMenu={(e) => setContext({ x: e.x, y: e.y })} />
          );
        })}
        {all.map((d) => <MotionWallpaper key={`${d.id}:${d.x}`} display={d} />)}
        <DesktopWidgets layer="below" />
        <WidgetDropTarget />
        <OverviewBackdrop />
        {mounted.map((w) => (
          <Fragment key={w.id}>
            {preview && w === top ? <SnapPreview cx={cx} /> : null}
            <Window win={w} app={getApp(w.id)} t={t} focused={w.id === focused} fx={minFx(w.id)} />
          </Fragment>
        ))}
        {minimizing.filter((x) => !mounted.includes(x.win)).map(({ win }) => <Window key={win.id} win={win} app={getApp(win.id)} t={t} focused={false} fx={minFx(win.id)} />)}
        <DesktopWidgets layer="above" />
        <WidgetPicker />

        <Layer name="menubar">
          <Guard name="frame">
            {frames.map((f) => (
              <Fragment key={`${f.d.id}:${f.d.x}`}>
                <Frame color={cx.frame} area={f.d} rtl={rtl} />
                <Bar cx={cx} t={t} area={f.bar} display={f.d} active={open && open.display === f.d.id ? open.name : null} onToggle={(name) => { if (name === 'launcher') setLauncherOn(true); toggle(f.d.id)(name); }} unread={unread} />
                {drawer(f, 'dash', <Dashboard cx={cx} onClose={close} />)}
                {drawer(f, 'sidebar', <Sidebar cx={cx} width={drawerRect('sidebar', f.inner, rtl).w} onClose={close} />)}
                {drawer(f, 'session', <SessionPanel cx={cx} onClose={close} onLogout={() => setAskLogout(true)} />)}
              </Fragment>
            ))}
          </Guard>
        </Layer>
        {launcherOn ? (
          <Layer name="overlay">
            <Guard name="launcher">
              {frames.map((f) => drawer(f, 'launcher', <Launcher cx={cx} onClose={close} onAction={doAction} onRows={setLauncherRows} />,
                { onClosed: () => { const o = live.current.open; if (!o || o.name !== 'launcher') setLauncherOn(false); } }))}
            </Guard>
          </Layer>
        ) : null}

        {context ? (
          <Menu t={t} x={context.x} y={context.y} onClose={() => setContext(null)} items={[
            { label: tr('shell.celeste.bar.dashboard'), onClick: () => { const f = frames.find((x) => x.d.id === displayAt(context.x, context.y)?.id) || mainFrame; if (f) openName('dash', f.d.id); } },
            { label: tr('shell.celeste.bar.launcher'), onClick: () => { const f = frames.find((x) => x.d.id === displayAt(context.x, context.y)?.id) || mainFrame; if (f) openName('launcher', f.d.id); } },
            { label: tr('shell.desktop.wallpaper'), onClick: () => doAction('wallpaper') },
            { label: tr('shell.widgets.add'), onClick: () => widgetPicker.set({ display: displayAt(context.x, context.y) || main, x: context.x, y: context.y }) },
            { label: tr('shell.desktops.overview'), onClick: () => toggleOverview() },
            '-',
            { label: tr('shell.celeste.action.scheme'), onClick: nextScheme },
            { label: tr('shell.celeste.action.options'), onClick: () => doAction('options') },
            { label: tr('shell.menu.settings'), onClick: () => wm.open('settings') },
          ]} />
        ) : null}

        {askLogout ? (
          <Layer name="modal">
            <ConfirmDialog title={tr('shell.celeste.session.logoutTitle')} detail={tr('shell.celeste.session.logoutDetail')} okLabel={tr('shell.menu.logout')}
              cancelLabel={tr('shell.celeste.cancel')} cautious area={main}
              onCancel={() => setAskLogout(false)} onOk={() => { setAskLogout(false); session.quit(); }} />
          </Layer>
        ) : null}
        <Layer name="modal"><PowerDialog /></Layer>
        <AppSwitcher />
        <OverviewChrome />
        <DesktopHud />
        <LevelHud />
      </View>
    </ThemeProvider>
  );
}

startShell(<App />);
