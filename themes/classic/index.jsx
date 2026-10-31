// "Classic" theme — the second full appearance of LordOS: a centered taskbar with a start menu,
// pinned and open apps, a tray with quick settings and a notification panel, window snapping
// (halves, thirds, quarters) and the SDK's Overview. The apps are the same as in Lord Flow: this theme
// only supplies the window frame and the shell, and restyles the SDK's shared controls through its
// tokens (tokens.js), so every app adapts on its own. See themes/README.md and docs/DESIGN.md.
//
//   tokens.js                   colors, materials and the shape tokens of the shared controls
//   glyphs.jsx                  drawn icons (window buttons, tray, quick settings)
//   components/Window.jsx       the frame: title at the start, buttons at the end, snap layouts
//   components/Taskbar.jsx      start, search, overview, apps, Trash, tray
//   components/StartMenu.jsx    search, pinned, recent, all apps, user and power
//   components/QuickSettings.jsx, NotificationCenter.jsx, Menu.jsx
import {
  startShell, settings, View, Image, Layer, screen, addListener, ThemeProvider, useSettings, useWindows, useStore, configureWindows,
  getApp, wm, t as tr, DesktopWidgets, registerGestureActions, dockPrefs, mainDisplay, screenDisplays, dockDrag, dockPoof,
  emptyTrash, ConfirmDialog, PowerDialog, findWallpaper, wallpaperSrc, snapPreview, notifyHistory, setBannerPlacement, Text, direction,
  useShortcuts, runShortcut, AppSwitcher, openSwitcher, toggleShowDesktop, spotlight, launchApp, dockActivate,
  classicDesktopKeys, desktopSwipe, DesktopHud, OverviewBackdrop, OverviewChrome, openOverview, closeOverview, toggleOverview, overview,
} from '@lordos/sdk';
import { apps } from '@lordos/apps';
import { useEffect, useRef, useState } from 'react';
import { makeTheme } from './tokens.js';
import { Window, TITLE_H } from './components/Window.jsx';
import { Taskbar, BAR_H, barSize, taskbarEntries } from './components/Taskbar.jsx';
import { StartMenu, startBox } from './components/StartMenu.jsx';
import { QuickSettings } from './components/QuickSettings.jsx';
import { NotificationCenter } from './components/NotificationCenter.jsx';
import { Menu } from './components/Menu.jsx';

// The taskbar's alignment (Settings → Desktop & Dock): 'center' (default) or 'start'.
const alignOf = (s) => (s.taskbarAlign === 'start' ? 'start' : 'center');

// Several displays (docs/DISPLAYS.md): the bar is on the main one; the others are all work area.
function workAreaFor(pos, d = mainDisplay()) {
  const a = { x: d.x, y: d.y, w: d.w, h: d.h };
  if (!d.main) return a;
  const b = barSize(pos);
  if (pos === 'top') { a.y += b; a.h -= b; } else if (pos === 'left') { a.x += b; a.w -= b; } else if (pos === 'right') a.w -= b; else a.h -= b;
  return a;
}
const areaFn = (d) => workAreaFor(dockPrefs(settings.get()).taskbarPosition, d);

// Is the point over the bar? (a click there doesn't close a panel: its button toggles it)
function onBar(pos, x, y) {
  const b = barSize(pos), m = mainDisplay();
  x -= m.x; y -= m.y;
  return pos === 'top' ? y < b : pos === 'left' ? x < b : pos === 'right' ? x >= m.w - b : y >= m.h - b;
}

// A panel at the tray's end of the bar: quick settings and notifications.
function trayPanel(pos, width, height, m = mainDisplay()) {
  const rtl = direction() === 'rtl' && (pos === 'top' || pos === 'bottom');
  const left = pos === 'left' ? m.x + barSize(pos) + 12 : pos === 'right' || !rtl ? m.x + m.w - (pos === 'right' ? barSize(pos) : 0) - 12 - width : m.x + 12;
  if (pos === 'bottom') return { pos: { left, bottom: screen.height - (m.y + m.h - BAR_H - 12) }, top: m.y + m.h - BAR_H - 12 - height, left, height };
  const top = pos === 'top' ? m.y + BAR_H + 12 : m.y + m.h - 12 - height;
  return { pos: { left, top }, top, left, height };
}

// chrome: borders (1 + 1) and the title bar of Window.jsx's frame
configureWindows({ apps, workArea: areaFn, chrome: { w: 2, h: TITLE_H + 2 }, snap: true });

// What is being dragged (from the bar or the start menu), and the remove cue.
function DragGhost({ t }) {
  const drag = useStore(dockDrag);
  if (!drag) return null;
  return (
    <View style={{ position: 'absolute', left: drag.x - 14, top: drag.y - 14, pointerEvents: 'none', opacity: drag.removing ? 0.5 : 0.9, alignItems: 'center' }}>
      {drag.Icon ? <drag.Icon size={28} /> : null}
      {drag.removing && (
        <View style={{ marginTop: 4, paddingHorizontal: 6, paddingVertical: 2, borderRadius: 4, backgroundColor: t.popupBg, border: `1px solid ${t.windowBorder}` }}>
          <Text style={{ fontSize: 11, color: t.text, whiteSpace: 'nowrap' }}>{tr('shell.dock.unpinCue')}</Text>
        </View>
      )}
    </View>
  );
}

// After an unpin: the button fades out where it was dropped (nothing with reduce motion).
function PoofFade() {
  const poof = useStore(dockPoof);
  const reduce = !!useStore(settings).reduceMotion;
  const [, tick] = useState(0);
  const p = poof ? Math.min(1, (Date.now() - poof.t0) / 300) : 1;
  useEffect(() => {
    if (!poof || p >= 1 || reduce) return undefined;
    const raf = requestAnimationFrame(() => tick((n) => n + 1));
    return () => cancelAnimationFrame(raf);
  });
  if (!poof || p >= 1 || reduce) return null;
  const k = Math.round(28 * (1 - 0.5 * p));
  return (
    <View style={{ position: 'absolute', left: poof.x - k / 2, top: poof.y - k / 2, pointerEvents: 'none', opacity: 1 - p }}>
      {poof.Icon ? <poof.Icon size={k} /> : null}
    </View>
  );
}

// Where a dragged window would snap: a translucent accent rectangle under the window.
function SnapPreview({ t }) {
  const p = useStore(snapPreview);
  if (!p) return null;
  const r = p.rect;
  return (
    <View style={{ position: 'absolute', left: r.x + 6, top: r.y + 6, width: r.w - 12, height: r.h - 12, borderRadius: 8, pointerEvents: 'none',
      backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.10)' : 'rgba(255,255,255,0.35)', border: `2px solid ${t.accent}` }} />
  );
}

// Lord Classic's own shortcuts (the common ones are in sdk/shortcuts.js). `ui` is a ref to what the
// shell's state offers at that moment.
const switchApps = (e) => openSwitcher(2, e.shift);
const openFiles = () => launchApp('files');
const openSettings = () => launchApp('settings');
const openMonitor = () => launchApp('monitor');
const openSearch = () => spotlight.open();
const captureArea = () => runShortcut('capture.area');
function openTaskbarItem(e) {
  const entry = taskbarEntries()[e.code - 2];
  if (entry) dockActivate(entry);
}
const SNAP_DIRS = { 103: 'up', 105: 'left', 106: 'right', 108: 'down' };     // physical sides, also in right-to-left languages

const classicTable = (ui) => {
  const snapFront = (e) => { const f = ui.current.focused; if (f) wm.snapKey(f, SNAP_DIRS[e.code]); };
  const closeFront = () => { const f = ui.current.focused; if (f) wm.close(f); };
  return [
    { id: 'start', keys: [], show: ['❖'], run: () => {}, label: 'shell.shortcuts.startMenu', group: 'system' },
    { id: 'searchRun', keys: ['Meta+R'], run: openSearch, label: 'shell.shortcuts.search', group: 'system' },
    { id: 'quick', keys: ['Meta+A'], run: () => ui.current.toggle('quick'), label: 'shell.shortcuts.quickSettings', group: 'system' },
    { id: 'notif', keys: ['Meta+N'], run: () => ui.current.toggle('notif'), label: 'shell.shortcuts.notifications', group: 'system' },
    { id: 'showDesktop', keys: ['Meta+D'], run: toggleShowDesktop, label: 'shell.shortcuts.showDesktop', group: 'system' },
    { id: 'files', keys: ['Meta+E'], run: openFiles, label: 'shell.shortcuts.files', group: 'system' },
    { id: 'settings', keys: ['Meta+I'], run: openSettings, label: 'shell.shortcuts.settings', group: 'system' },
    { id: 'monitor', keys: ['Ctrl+Shift+Escape'], run: openMonitor, label: 'shell.shortcuts.monitor', group: 'system' },
    { id: 'captureArea', keys: ['Meta+Shift+S'], run: captureArea, label: 'shell.shortcuts.captureArea', group: 'system' },
    { id: 'switchApps', keys: ['Alt+Tab', 'Alt+Shift+Tab'], run: switchApps, label: 'shell.shortcuts.switchApps', group: 'windows' },
    { id: 'snap', keys: ['Meta+ArrowLeft', 'Meta+ArrowRight', 'Meta+ArrowUp', 'Meta+ArrowDown'], show: ['❖←→↑↓'], run: snapFront, repeat: true, label: 'shell.shortcuts.snap', group: 'windows' },
    { id: 'closeWindow', keys: ['Alt+F4'], run: closeFront, label: 'shell.shortcuts.closeWindow', group: 'windows' },
    { id: 'taskbar', keys: ['Meta+1', 'Meta+2', 'Meta+3', 'Meta+4', 'Meta+5', 'Meta+6', 'Meta+7', 'Meta+8', 'Meta+9'], show: ['❖1…9'], run: openTaskbarItem, label: 'shell.shortcuts.taskbarItem', group: 'apps' },
    ...classicDesktopKeys,
  ];
};

function App() {
  const [s] = useSettings();
  const t = makeTheme(s);
  const { mounted, focused } = useWindows();
  const taskView = !!useStore(overview);
  const [panel, setPanel] = useState(null);           // 'start' | 'quick' | 'notif' | null
  const [desk, setDesk] = useState(null);             // the desktop's right-click menu
  const [askEmpty, setAskEmpty] = useState(false);
  const history = useStore(notifyHistory);
  const seen = useRef(0);
  const pos = dockPrefs(s).taskbarPosition;
  const align = alignOf(s);
  const m = mainDisplay();
  const all = screenDisplays();
  // the work area and the banners follow the bar live
  useEffect(() => {
    configureWindows({ apps, workArea: areaFn });
    setBannerPlacement(pos === 'top' ? 'top' : 'bottom');
  }, [pos, screen.width, screen.height]);
  const toggle = (name) => setPanel((p) => (p === name ? null : name));
  const close = () => setPanel(null);
  if (panel === 'notif' && history.length) seen.current = Math.max(seen.current, history[0].id);
  const unread = history.some((n) => n.id > seen.current);

  // ❖ alone opens the start menu (only if released without being used in a shortcut); Escape closes
  // what is open. The rest of the keys are in classicTable.
  const ui = useRef(null);
  ui.current = { toggle, focused };
  useShortcuts({ grabs: 'classic', table: classicTable(ui), onSuperTap: () => { closeOverview(); toggle('start'); } });
  useEffect(() => addListener('keydown', (e) => { if (e.key === 'Escape') { setPanel(null); setDesk(null); } }), []);
  // the overview takes the place of the open panels
  useEffect(() => { if (taskView) setPanel(null); }, [taskView]);
  // trackpad: a 3-finger swipe up opens the start menu (4 fingers: the overview), down closes them;
  // left/right switch desktops, or windows with only one desktop (docs/TRACKPAD.md)
  useEffect(() => registerGestureActions({
    up: (g) => { if (g.fingers >= 4) openOverview(); else setPanel('start'); },
    down: () => { setPanel(null); closeOverview(); },
    left: desktopSwipe('left'), right: desktopSwipe('right'),
  }), []);

  const wallpaper = findWallpaper(t.wallpapers, s.wallpaper);
  const isOnBar = (x, y) => onBar(pos, x, y);
  const preview = useStore(snapPreview);
  const top = mounted[mounted.length - 1];
  const qsAt = trayPanel(pos, 360, m.h - BAR_H - 24, m);
  const ncH = Math.min(760, m.h - (pos === 'left' || pos === 'right' ? 0 : BAR_H) - 24);
  const ncAt = trayPanel(pos, 380, ncH, m);
  return (
    <ThemeProvider tokens={t}>
      <View style={{ width: '100%', height: '100%', backgroundColor: t.mode === 'dark' ? '#101216' : '#c9d3e0' }}>
        {all.map((d) => (
          <Image key={d.id + ':' + d.x} src={wallpaperSrc(wallpaper, d.w * (d.scale || 100) / 100, d.h * (d.scale || 100) / 100)}
            onContextMenu={(e) => setDesk({ x: e.x, y: e.y })}
            style={{ position: 'absolute', left: d.x, top: d.y, width: d.w, height: d.h, objectFit: 'cover' }} />
        ))}
        <DesktopWidgets layer="below" />
        <OverviewBackdrop />
        {mounted.map((w) => [
          preview && w === top ? <SnapPreview key="snap" t={t} /> : null,
          <Window key={w.id} win={w} t={t} focused={!taskView && w.id === focused} app={getApp(w.id)} />,
        ])}
        <DesktopWidgets layer="above" />
        {/* the bar on the main display and the panels above it: each in its layer (sdk/layers.js,
            docs/LAYERS.md), so the order and the clicks are the layers' */}
        <Layer name="dock">
          <View style={{ position: 'absolute', left: m.x, top: m.y, width: m.w, height: m.h, pointerEvents: 'box-none' }}>
            <Taskbar t={t} pos={pos} align={align} start={panel === 'start'} onStart={() => { closeOverview(); toggle('start'); }}
              onSearch={() => { closeOverview(); toggle('start'); }} taskView={taskView} onTaskView={() => toggleOverview()}
              quick={panel === 'quick'} onQuick={() => toggle('quick')} notif={panel === 'notif'} onNotif={() => toggle('notif')} unread={unread}
              onEmptyTrash={() => setAskEmpty(true)} />
          </View>
        </Layer>
        {panel === 'start' && <Layer name="popover"><StartMenu t={t} box={startBox(pos, align, m)} onClose={close} isOnBar={isOnBar} /></Layer>}
        {panel === 'quick' && <Layer name="popover"><QuickSettings t={t} at={qsAt} onClose={close} isOnBar={isOnBar} /></Layer>}
        {panel === 'notif' && <Layer name="popover"><NotificationCenter t={t} at={ncAt} onClose={close} isOnBar={isOnBar} /></Layer>}
        {desk && (
          <Menu t={t} x={desk.x} y={desk.y} onClose={() => setDesk(null)} items={[
            { label: tr('shell.desktops.overview'), onClick: () => openOverview() },
            '-',
            { label: tr('shell.classic.personalize'), onClick: () => wm.open('settings', { args: { section: 'appearance' } }) },
            { label: tr('shell.desktop.wallpaper'), onClick: () => wm.open('settings', { args: { section: 'wallpaper' } }) },
            { label: tr('shell.classic.displaySettings'), onClick: () => wm.open('settings', { args: { section: 'displays' } }) },
          ]} />
        )}
        <Layer name="drag">
          <DragGhost t={t} />
          <PoofFade />
        </Layer>
        {/* Restart…, Shut Down… (sdk/power.jsx) */}
        <Layer name="modal"><PowerDialog /></Layer>
        <AppSwitcher />
        <OverviewChrome />
        <DesktopHud />
        {askEmpty && (
          <Layer name="modal">
            <ConfirmDialog title={tr('shell.trash.confirmTitle')} detail={tr('shell.trash.confirmDetail')} okLabel={tr('shell.trash.empty')}
              cancelLabel={tr('shell.trash.cancel')} destructive area={m}
              onCancel={() => setAskEmpty(false)} onOk={() => { setAskEmpty(false); emptyTrash(); }} />
          </Layer>
        )}
      </View>
    </ThemeProvider>
  );
}

startShell(<App />);
