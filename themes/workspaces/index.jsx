// "Lord Workspaces" theme (id workspaces) — a desktop organized by activities. Each activity is one of the SDK's
// desktops (at most 9) with a name, an icon, a colour, pinned apps and a tiling layout per display; its windows sit in a
// group frame per display. The Context Spine on the side opens Activities (the Overview), Apps, Files and
// the Control Center; Search opens with ❖Space, Ctrl+Space or ❖ alone. Every surface takes one of three effects styles:
// Opaque, Translucent or Crystal. See docs/WORKSPACES.md.
//
//   tokens.js                   colours, the three materials and the shape tokens of the shared controls
//   model/                      plain functions, tested with node: geometry (where things go on each display),
//                               layouts (the tiles), magnet (the free layout's halves), activities (the settings key)
//   state.js, tiler.js          the activities' data and the tiling at runtime
//   magnet.js                   the free layout's magnet: the other half filled or chosen, the shared edge
//   motion.js                   the transitions: cached transforms from one frame loop, none with Reduce motion
//   components/                 Spine, TopBar (activity strip, status cluster), GroupFrame, Window, Search,
//                               ControlCenter, ActivityEditor (onboarding and edit), SnapAssist, Surface
// The menus and the notification panel are Lord Classic's own components, restyled by these tokens.
import {
  startShell, settings, View, Image, Layer, screen, addListener, ThemeProvider, useSettings, useWindows, useStore, configureWindows, getApp, wm, t as tr,
  DesktopWidgets, registerGestureActions, screenDisplays, mainDisplay, displayFor, displayAt, startDeskWidgets, WidgetDropTarget, findWallpaper, wallpaperIdFor, wallpaperSrc, MotionWallpaper, SlideshowWallpapers, slideshowPrefs, snapPreview, notifyHistory,
  useShortcuts, AppSwitcher, openSwitcher, switcherKeys, switcherGrabs, switcherHold, cycleAppWindows, hideApp, hideOthers, appSettingsItem,
  flowDesktopKeys, flowGrabs, desktopSwipe, OverviewBackdrop, OverviewChrome, openOverview, closeOverview, toggleOverview, overview, LevelHud,
  PowerDialog, ConfirmDialog, session, launchApp, direction, pointerDisplay, registerSpotlightSource, desktopList, desktopName, activeDesktop,
  switchDesktop, windows as windowsStore, windowTitle, windowIcon, appName, removeDesktopById, renameDesktop, createStore, lordosWallpapers,
  VideoSetup, setDesktopDecor, setStatusPanelPlacer, WidgetPicker, widgetPicker, setPointerAccent,
} from '@lordos/sdk';
import { apps } from '@lordos/apps';
import { useFirstStartOpen } from '@lordos/sdk';   // (0.8.9) the activity onboarding waits for the first-start desktop chooser
import { Fragment, useEffect, useMemo, useRef, useState } from 'react';
import { makeTheme } from './tokens.js';
import { displayPlan, shownDisplays, onChrome } from './model/geometry.js';
import { wsPrefs, optPatch, effectiveSurface, needsOnboarding } from './model/activities.js';
import { minimizeStarts } from '../default/minimize.js';
import { startGroups, groupHidden, showGroup } from './state.js';
import { activities, panel, deleting, startActivities, updateActivity, setUpActivity, activityOf, noteLaunch, newActivity } from './state.js';
import { startTiler, areasChanged, keyOfDisplay, setLayout, layoutOf, tiled, windowLocked, aimLaunch } from './tiler.js';
import { startMagnet } from './magnet.js';
import { barFocus, setBarItems, enterBars, leaveBars, barKey } from './barfocus.js';
import { SnapAssist } from './components/SnapAssist.jsx';
import { GlassTarget, DropTarget, SnapFlyout, dropTarget, useDropTarget } from './components/SnapTargets.jsx';
import { tween, EASE, reduceMotion } from './motion.js';
import { Window, TITLE_H, MENU_H } from './components/Window.jsx';
import { Spine } from './components/Spine.jsx';
import { Surface } from './components/Surface.jsx';
import { ActivityStrip, ActivityPeek, StatusCluster, VolumePanel, STATUS_W } from './components/TopBar.jsx';
import { GroupFrame, trayNodes } from './components/GroupFrame.jsx';
import { Search } from './components/Search.jsx';
import { ControlCenter } from './components/ControlCenter.jsx';
import { ActivityEditor } from './components/ActivityEditor.jsx';
import { ShortcutsHelp, shortcutsHelp } from './components/ShortcutsHelp.jsx';
import { ActivityIcon } from './glyphs.jsx';
import { Menu } from '../classic/components/Menu.jsx';
import { NotificationCenter } from '../classic/components/NotificationCenter.jsx';

const rtlNow = () => direction() === 'rtl';
const planFor = (d, s) => { const p = wsPrefs(s); return displayPlan(d, { spine: p.spine, every: p.spineEvery, rtl: rtlNow(), primary: !!d.main }); };
// the work area of each display: the group frame's inside (model/geometry.js)
const areaFn = (d) => planFor(d, settings.get()).work;

// chrome: borders (1 + 1) and the title bar of Window.jsx's frame; snapping on for windows that float
configureWindows({ apps, workArea: areaFn, chrome: { w: 2, h: TITLE_H + 2, menu: MENU_H }, snap: true, locked: windowLocked, snapThirds: true });

// Search, the Control Center and the notifications open on the display under the pointer (or the main one).
const togglePanel = (name, extra = {}) => panel.set((p) => (p && p.name === name && (!extra.mode || p.mode === extra.mode) ? null : { name, display: pointerDisplay().id, ...extra }));
const openSearch = (mode = 'search') => { closeOverview(); togglePanel('search', { mode }); };
const notInTerminal = () => panel.get()?.name === 'search' || String(wm.focused() || '').split('#')[0] !== 'terminal';
const onFront = (fn) => () => { const id = wm.focused(); if (id) fn(id); };
const frontSettings = () => { const id = wm.focused(); return id ? appSettingsItem(id) : null; };
// the spine's own button (and ❖⌥D): the other of the two looks, kept as an explicit choice (Settings can set it back to
// Automatic)
const toggleSpine = () => {
  const s = settings.get();
  const now = planFor(mainDisplay(), s).mode;
  settings.set(optPatch(s, 'spine', now === 'collapsed' ? 'expanded' : 'collapsed'));
};
// The shell's handlers, stable across renders so the memoized pieces (spine, strip, frames) don't redraw for nothing.
const crownMenu = createStore(null);         // { x, y } the crown's menu
const deskMenu = createStore(null);          // { x, y } the wallpaper's right-click menu
const editing = createStore(null);           // the activity whose editor is open
const H = {
  crown: (e) => crownMenu.set({ x: e.x + 8, y: e.y + 8 }),
  activities: () => { panel.set(null); toggleOverview(); },
  apps: (e) => { if (e) noteLaunch(e.x, e.y); panel.set((p) => (p?.name === 'search' && p.mode === 'apps' ? null : { name: 'search', mode: 'apps', display: pointerDisplay().id })); },
  files: (e) => { if (e) noteLaunch(e.x, e.y); aimLaunch(e ? displayAt(e.x, e.y) || pointerDisplay() : pointerDisplay()); launchApp('files'); },
  control: () => panel.set((p) => (p?.name === 'control' ? null : { name: 'control', display: pointerDisplay().id })),
  notif: () => panel.set((p) => (p?.name === 'notif' ? null : { name: 'notif', display: mainDisplay().id })),
  volume: () => panel.set((p) => (p?.name === 'volume' ? null : { name: 'volume', display: mainDisplay().id })),
  addWindow: (d, e) => { if (e) noteLaunch(e.x, e.y); panel.set({ name: 'search', mode: 'apps', display: d.id }); },
  addWidget: (d) => widgetPicker.set({ display: d }),
  // the Store's Web apps page: "Add from an address" and the catalog (docs/WEBAPPS.md)
  addWebApp: () => wm.open('store', { args: { page: 'webapps' } }),
  edit: (id) => editing.set(id),
  activity: (id) => { panel.set(null); switchDesktop(id); },
  settings: () => { panel.set(null); wm.open('settings'); },
  help: () => { panel.set(null); shortcutsHelp.set((v) => !v); },
  closePanel: () => panel.set(null),
};

const DIGITS = { 2: 1, 3: 2, 4: 3, 5: 4, 6: 5, 7: 6, 8: 7, 9: 8, 10: 9 };     // evdev codes of the digits row
function stepActivity(dir) {
  const list = desktopList.get(), i = list.indexOf(activeDesktop.get());
  if (list.length > 1) switchDesktop(list[(i + dir + list.length) % list.length]);
}
const table = [
  { id: 'search', keys: ['Meta+Space'], run: () => openSearch(), label: 'shell.shortcuts.search', group: 'system' },
  { id: 'searchCtrl', keys: ['Ctrl+Space'], run: () => openSearch(), when: notInTerminal, label: 'shell.shortcuts.search', group: 'system' },
  { id: 'spine', keys: ['Meta+Alt+D'], run: toggleSpine, label: 'shell.ws.shortcuts.spine', group: 'system' },
  // ❖1…❖9: the activity at that position; ❖Tab and ❖⇧Tab, the next and the previous one, while the app switcher is on ⌥Tab
  // only (Settings → Keyboard): the proposal's ❖Tab for activities, by choice
  { id: 'activityN', keys: ['1', '2', '3', '4', '5', '6', '7', '8', '9'].map((n) => `Meta+${n}`), show: ['❖1…9'],
    run: (e) => { const id = desktopList.get()[DIGITS[e.code] - 1]; if (id != null) switchDesktop(id); },
    label: 'shell.ws.shortcuts.activityN', group: 'desktops' },
  { id: 'activityTab', keys: ['Meta+Tab', 'Meta+Shift+Tab'], run: (e) => stepActivity(e.shift ? -1 : 1), when: () => !switcherKeys('meta').includes('Meta+Tab'),
    listed: () => !switcherKeys('meta').includes('Meta+Tab'), label: 'shell.ws.shortcuts.activityTab', group: 'desktops' },
  { id: 'forceQuit', keys: ['Meta+Alt+Escape'], run: () => { aimLaunch(pointerDisplay()); wm.open('forcequit'); }, when: () => !!getApp('forcequit'), label: 'shell.shortcuts.forceQuit', group: 'apps' },
  { id: 'help', keys: ['Meta+/'], show: ['❖/'], run: () => H.help(), label: 'shell.ws.shortcuts.help', group: 'system' },
  { id: 'bars', keys: ['Ctrl+F2'], run: () => { panel.set(null); enterBars(`pill:${activeDesktop.get()}`); }, label: 'shell.ws.shortcuts.bars', group: 'system' },
  { id: 'switchApps', keys: switcherKeys('meta'), run: (e) => openSwitcher(switcherHold(e), e.shift), label: 'shell.shortcuts.switchApps', group: 'windows' },
  { id: 'cycleWindows', keys: ['Meta+`', 'Meta+Shift+`'], run: (e) => cycleAppWindows(e.shift), label: 'shell.shortcuts.cycleWindows', group: 'windows' },
  { id: 'minimize', keys: ['Meta+M'], run: onFront(wm.minimize), label: 'shell.shortcuts.minimize', group: 'windows' },
  { id: 'hide', keys: ['Meta+H'], run: onFront(hideApp), label: 'shell.shortcuts.hide', group: 'windows' },
  { id: 'hideOthers', keys: ['Meta+Alt+H'], run: onFront(hideOthers), label: 'shell.shortcuts.hideOthers', group: 'windows' },
  { id: 'appSettings', keys: ['Meta+,'], run: () => frontSettings()?.onClick(), when: frontSettings, label: 'shell.shortcuts.appSettings', group: 'apps' },
  ...flowDesktopKeys,
];

// Two sections of their own in Search (and in the SDK's panel while this theme runs): Activities and Open windows.
function registerSources() {
  const offA = registerSpotlightSource('activities', (q, query, { best }) => {
    const out = [];
    desktopList.get().forEach((id, i) => {
      const name = desktopName(id);
      const n = windowsStore.get().filter((w) => w.desktop === id && !w.pip).length;
      const m = activityOf(id);
      out.push({ key: `activity:${i}`, score: best(q, name), title: name, subtitle: tr('shell.ws.search.activitySub', { n, pos: i + 1 }),
        Icon: ({ size }) => <ActivityIcon icon={m.icon} color={m.color} bg="#ffffff" size={Math.round(size * 0.7)} />, run: () => showGroup(id, null) });
      // its groups: more activities on the same desktop
      (m.groups || []).forEach((g, k) => {
        const gname = g.name || tr('shell.ws.group.name', { n: k + 1 });
        out.push({ key: `activity:${i}:${g.id}`, score: best(q, gname, name), title: gname, subtitle: tr('shell.ws.group.inActivity', { activity: name }),
          Icon: ({ size }) => <ActivityIcon icon={g.icon} color={g.color} bg="#ffffff" size={Math.round(size * 0.7)} />, run: () => showGroup(id, g.id) });
      });
    });
    return out.filter((x) => x.score > 0);
  });
  const offW = registerSpotlightSource('windows', (q, query, { best }) => windowsStore.get().filter((w) => !w.pip && !(getApp(w.id)?.builtin && !String(w.id).startsWith('linux#'))).map((w) => {
    const title = windowTitle(w);
    return { key: `window:${w.id}`, score: best(q, title, appName(getApp(w.id))), title, subtitle: desktopName(w.desktop), Icon: windowIcon(w), run: () => wm.focus(w.id) };
  }).filter((x) => x.score > 0));
  return () => { offA(); offW(); };
}

// Where a dragged window would snap (a free display; on a tiled one the drop target shows its slot instead), in glass.
function SnapPreview({ t }) {
  const p = useStore(snapPreview);
  const target = useStore(dropTarget);
  if (!p || target) return null;
  return <GlassTarget t={t} rect={p.rect} />;
}

// The minimize effect: the window shrinks into its display's tray of minimized windows (the frame header,
// GroupFrame.jsx), or the spine before the tray has a place, on its cached raster (one render to start, one to end).
const MIN_MS = 220;
function useLeaving(list, spine) {
  const prev = useRef(new Map());
  const [leaving, setLeaving] = useState([]);
  useEffect(() => {
    // a group switch's windows go without the effect: they aren't minimized by the person (state.js, groups)
    const hid = groupHidden.get();
    const started = minimizeStarts(list, prev.current, { reduce: reduceMotion() }).filter((x) => !hid.has(x.id));
    if (!started.length) return;
    setLeaving((l) => [...l.filter((x) => !started.some((y) => y.id === x.id)), ...started.map((x) => x.id)]);
    for (const { id } of started) {
      setTimeout(() => {
        const n = wm.frameNode(id), w = wm.get(id);
        if (!n || !w) return;
        const f = wm.frame(w);
        let to = spine ? { x: spine.x + spine.w / 2, y: spine.y + 110 } : { x: f.x, y: f.y };
        const tray = trayNodes.get(displayFor(f)?.id);
        if (tray) try { const [tx, ty, tw, th] = lord.getLayout(tray); if (tw > 0 && tx > -10000) to = { x: tx + tw / 2, y: ty + th / 2 }; } catch (e) { /* gone */ }
        try { lord.setStyle(n, 'transformOriginX', to.x - f.x); lord.setStyle(n, 'transformOriginY', to.y - f.y); } catch (e) { /* gone */ }
        tween(n, { scaleX: [1, 0.12], scaleY: [1, 0.12], opacity: [1, 0] }, { ms: MIN_MS, ease: EASE.inOut,
          done: () => {
            try { lord.setStyle(n, 'transformOriginX', undefined); lord.setStyle(n, 'transformOriginY', undefined); } catch (e) { /* gone */ }
            setLeaving((l) => l.filter((x) => x !== id));
          } });
      }, 0);
    }
  }, [list]);
  return leaving;
}

function App() {
  startActivities();
  startGroups();
  startTiler();
  startMagnet();
  startDeskWidgets();
  useDropTarget();
  const [s] = useSettings();
  const prefs = wsPrefs(s);
  const surface = effectiveSurface(prefs, s);
  const t = useMemo(() => makeTheme({ mode: s.mode, accent: s.accent, effects: s.effects, surface, contrast: prefs.contrast }, lordosWallpapers),
    [s.mode, s.accent, s.effects, surface, prefs.contrast]);
  // the theme's own pointer: the arrow in the accent colour (cursor.c), changed with the accent
  useEffect(() => { setPointerAccent(t.accent); }, [t.accent]);
  const { windows: all, mounted, focused, desktop } = useWindows();
  const meta = useStore(activities);
  const list = useStore(desktopList);
  const tiles = useStore(tiled);
  const open = useStore(panel);
  const del = useStore(deleting);
  const inOverview = !!useStore(overview);
  const history = useStore(notifyHistory);
  const editId = useStore(editing);
  const crown = useStore(crownMenu);
  const desk = useStore(deskMenu);
  const seen = useRef(0);
  const controlRef = useRef(null);
  const rtl = rtlNow();
  const displays = screenDisplays();
  const shown = shownDisplays(displays, screen.mirror);
  const sig = shown.map((d) => `${d.id}:${d.connector || ''}:${d.x},${d.y},${d.w}x${d.h}:${d.main ? 1 : 0}`).join('|');
  // a display's name in its frame header: its model, or its connector too when two displays share the model
  const label = (d) => (d.connector && shown.filter((x) => x.name === d.name).length > 1 ? `${d.name || ''} · ${d.connector}`.replace(/^ · /, '') : d.name || String(d.id));
  const plans = useMemo(() => shown.map((d) => ({ d, key: keyOfDisplay(d), label: label(d), plan: planFor(d, s) })), [sig, prefs.spine, prefs.spineEvery, rtl]);
  const main = plans.find((p) => p.d.main) || plans[0];

  // the work areas follow the spine and the displays live; the tiles are laid out again
  useEffect(() => { configureWindows({ apps, workArea: areaFn }); areasChanged(); }, [sig, prefs.spine, prefs.spineEvery, rtl, screen.width, screen.height]);
  useEffect(registerSources, []);
  // the status items' panels (Cloud, Cleaner…) open where the notifications do: under the band, at its far edge
  const bandRect = main?.plan.top;
  useEffect(() => setStatusPanelPlacer((box) => {
    if (!bandRect) return null;
    const top = bandRect.y + bandRect.h + 8 - box[1];
    return rtl ? { position: 'absolute', top, left: bandRect.x - box[0] } : { position: 'absolute', top, right: box[0] + box[2] - (bandRect.x + bandRect.w) };
  }), [bandRect?.x, bandRect?.y, bandRect?.w, bandRect?.h, rtl]);
  // the Overview's strip shows each desktop as its activity: the icon on a tile of its colour
  useEffect(() => setDesktopDecor((id) => {
    const m = activityOf(id);
    return { color: m.color, Icon: ({ size }) => <ActivityIcon icon={m.icon} color="#ffffff" bg={m.color} size={size} /> };
  }), []);
  // Lord Flow's key table plus ⌃F2 (server/src/wl/keygrab.c "workspaces")
  useShortcuts({ grabs: switcherGrabs(flowGrabs().replace('flow', 'workspaces')), table, onSuperTap: () => openSearch('apps'), appsPaste: true });
  // the bars' keyboard (barfocus.js): the spine from the crown down, then the strip's pills and New
  useEffect(() => {
    setBarItems(() => {
      const desk = activeDesktop.get(), sp = main?.plan.spine;
      const at = sp ? { x: sp.x + sp.w, y: sp.y + 8 } : { x: 40, y: 40 };
      return [
        ...(sp ? [
          { id: 'crown', run: () => H.crown(at) },
          { id: 'activities', run: H.activities }, { id: 'apps', run: () => H.apps() }, { id: 'files', run: () => H.files() },
          { id: 'help', run: H.help }, { id: 'settings', run: H.settings }, { id: 'control', run: H.control },
        ] : []),
        ...(main?.plan.top ? [...desktopList.get().map((id) => ({ id: `pill:${id}`, run: () => switchDesktop(id) })),
          ...(desktopList.get().length < 9 ? [{ id: 'new', run: () => newActivity() }] : [])] : []),
      ];
    });
  }, [main]);
  const barsOn = useStore(barFocus) != null;
  useEffect(() => {
    if (!barsOn) return undefined;
    const offKey = addListener('keydown', (e) => barKey(e));
    const offDown = addListener('pointerdown', () => leaveBars());     // a press of the pointer gives the keyboard back
    return () => { offKey(); offDown(); };
  }, [barsOn]);
  useEffect(() => addListener('keydown', (e) => { if (e.key === 'Escape') { panel.set(null); crownMenu.set(null); deskMenu.set(null); shortcutsHelp.set(false); } }), []);
  useEffect(() => { if (inOverview) panel.set(null); }, [inOverview]);
  useEffect(() => registerGestureActions({
    up: (g) => { if (g.fingers >= 4) openOverview(); else openSearch('apps'); },
    down: () => { panel.set(null); closeOverview(); },
    left: desktopSwipe('left'), right: desktopSwipe('right'),
  }), []);

  const leaving = useLeaving(all, main?.plan.spine);
  const helpOpen = useStore(shortcutsHelp);
  const spineActive = useMemo(() => ({ overview: inOverview, apps: open?.name === 'search' && open.mode === 'apps', control: open?.name === 'control', notif: open?.name === 'notif', help: helpOpen, menu: !!crown }),
    [inOverview, open, crown, helpOpen]);
  const stripRect = useMemo(() => (main?.plan.top ? { ...main.plan.top, w: main.plan.top.w - STATUS_W } : null), [main]);
  if (open?.name === 'notif' && history.length) seen.current = Math.max(seen.current, history[0].id);
  const unread = history.some((n) => n.id > seen.current);
  const isOnChrome = (x, y) => onChrome(plans.map((p) => p.plan), x, y);
  const panelDisplay = (open && displays.find((d) => d.id === open.display)) || mainDisplay();
  const panelPlan = plans.find((p) => p.d.id === panelDisplay.id) || main;
  const top = mounted[mounted.length - 1];
  const deskMeta = meta[desktop] || activityOf(desktop);
  const onDesk = all.filter((w) => w.desktop === desktop && !w.pip).length;
  // the header's Move button: a window of this activity in front and another activity to send it to
  const canMove = !!focused && list.length > 1;
  const chooserOpen = useFirstStartOpen();
  const onboarding = needsOnboarding(deskMeta, list.length, onDesk) && !inOverview && !chooserOpen;
  const controlAnchor = () => {
    const n = controlRef.current;
    if (n) { const [x, y, w, h] = lord.getLayout(n.id); return { x, y, w, h }; }
    const sp = panelPlan?.plan.spine || { x: panelDisplay.x, y: panelDisplay.y + panelDisplay.h - 60, w: 0, h: 48 };
    return { x: sp.x, y: sp.y + sp.h - 60, w: sp.w, h: 48 };
  };
  const leavingWins = leaving.map((id) => all.find((w) => w.id === id)).filter((w) => w && !mounted.includes(w));
  const tight = new Set(tiles.tight);
  // what the crown's menu opens goes to the display that menu is on (the spine's), not to wherever the last window was
  const openHere = (id) => { aimLaunch((crown && displayAt(crown.x, crown.y)) || mainDisplay()); wm.open(id); };
  const crownItems = [
    { label: tr('shell.menu.about'), onClick: () => openHere('about') },
    { label: tr('shell.menu.settings'), onClick: () => openHere('settings') },
    { label: tr('shell.ws.help.menu'), onClick: () => H.help() },
    '-',
    // as Lord Flow's menu: an app that doesn't answer is ended from here (❖⌥Esc)
    ...(getApp('forcequit') ? [{ label: tr('shell.menu.forceQuit'), hint: '❖⌥Esc', onClick: () => openHere('forcequit') }, '-'] : []),
    ...(session.managed() ? [{ label: tr('shell.menu.lock'), onClick: () => session.lock() }, { label: tr('shell.menu.switchUser'), onClick: () => session.switchUser() }] : []),
    { label: tr('shell.menu.logout'), onClick: () => session.quit() },
    ...(session.canPower() ? ['-', { label: tr('shell.menu.restart'), onClick: () => session.restart() }, { label: tr('shell.menu.shutdown'), onClick: () => session.shutdown() }] : []),
  ];

  return (
    <ThemeProvider tokens={t}>
      <View style={{ width: '100%', height: '100%', backgroundColor: t.mode === 'dark' ? '#0F0D1C' : '#DCD6F2' }}>
        {/* the still pictures, or the slideshow's (Settings → Wallpaper; sdk/slideshow.jsx) */}
        {slideshowPrefs(s).on ? <SlideshowWallpapers list={t.wallpapers} displays={displays} onContextMenu={(e) => deskMenu.set({ x: e.x, y: e.y })} /> : displays.map((d) => (
          <Image key={d.id + ':' + d.x} src={wallpaperSrc(findWallpaper(t.wallpapers, wallpaperIdFor(s, d, t.defaultWallpaper), t.defaultWallpaper), d.w * (d.scale || 100) / 100, d.h * (d.scale || 100) / 100)}
            onContextMenu={(e) => deskMenu.set({ x: e.x, y: e.y })}
            style={{ position: 'absolute', left: d.x, top: d.y, width: d.w, height: d.h, objectFit: 'cover' }} />
        ))}
        {displays.map((d) => <MotionWallpaper key={d.id + ':' + d.x} display={d} />)}
        <DesktopWidgets layer="below" />
        <WidgetDropTarget />
        {plans.map(({ d, key, label: dname, plan }) => (
          <GroupFrame key={d.id} t={t} plan={plan} d={d} dkey={key} dname={dname} desk={desktop} meta={deskMeta} multi={plans.length > 1} canMove={canMove}
            onAddWindow={H.addWindow} onAddWidget={H.addWidget} onAddWebApp={H.addWebApp} onEdit={H.edit} showEmpty={!onboarding} />
        ))}
        {onboarding && main ? (
          <View style={{ position: 'absolute', left: main.plan.work.x, top: main.plan.work.y, width: main.plan.work.w, height: main.plan.work.h, alignItems: 'center', justifyContent: 'center' }}>
            <ActivityEditor t={t} id={desktop} meta={deskMeta} layout={layoutOf(desktop, main.key).layout} onboarding
              onSkip={() => updateActivity(desktop, (m) => ({ ...m, setup: true }))}
              onDone={(v) => { setLayout(desktop, main.key, v.layout); setUpActivity(desktop, v); }} />
          </View>
        ) : null}
        <OverviewBackdrop />
        {mounted.map((w) => (
          <Fragment key={w.id}>
            {w === top ? <><SnapPreview t={t} /><DropTarget t={t} /></> : null}
            <Window win={w} t={t} focused={!inOverview && w.id === focused} app={getApp(w.id)} tight={tight.has(w.id)} leaving={leaving.includes(w.id)} locked={windowLocked(w)} />
          </Fragment>
        ))}
        {leavingWins.map((w) => <Window key={w.id} win={w} t={t} focused={false} app={getApp(w.id)} leaving />)}
        <SnapAssist t={t} />
        <SnapFlyout t={t} />
        <WidgetPicker t={t} Panel={Surface} />
        <VideoSetup />
        <DesktopWidgets layer="above" />
        <Layer name="dock">
          {plans.map(({ d, plan }) => (plan.spine ? (
            <Spine key={d.id} t={t} rect={plan.spine} mode={plan.mode} rtl={rtl} active={spineActive} controlRef={d.main ? controlRef : undefined}
              onCrown={H.crown} onActivities={H.activities} onApps={H.apps} onFiles={H.files} onControl={H.control} onToggle={toggleSpine}
              onSettings={H.settings} onHelp={H.help} />
          ) : null))}
          {main?.plan.top ? (
            <>
              <ActivityStrip t={t} rect={stripRect} onEdit={H.edit} />
              <ActivityPeek t={t} />
              <StatusCluster t={t} rect={main.plan.top} rtl={rtl} screenW={screen.width} unread={unread} onControl={H.control} onNotif={H.notif} onVolume={H.volume} />
            </>
          ) : null}
        </Layer>
        {open?.name === 'search' ? (
          <Layer name="overlay"><Search key={panelDisplay.id} t={t} display={panelDisplay} onClose={() => panel.set(null)} isOnChrome={isOnChrome} /></Layer>
        ) : null}
        {open?.name === 'control' ? (
          <Layer name="popover"><ControlCenter t={t} anchor={controlAnchor()} area={panelDisplay} rtl={rtl} onClose={() => panel.set(null)} isOnChrome={isOnChrome} onEdit={H.edit} /></Layer>
        ) : null}
        {open?.name === 'volume' && main?.plan.top ? (
          <Layer name="popover">
            <VolumePanel t={t} isOnBar={isOnChrome} onClose={() => panel.set(null)}
              at={{ left: rtl ? main.plan.top.x : main.plan.top.x + main.plan.top.w - 320, top: main.plan.top.y + main.plan.top.h + 8 }} />
          </Layer>
        ) : null}
        {open?.name === 'notif' && main?.plan.top ? (
          <Layer name="popover">
            <NotificationCenter t={t} isOnBar={isOnChrome} onClose={() => panel.set(null)}
              at={{ left: rtl ? main.plan.top.x : main.plan.top.x + main.plan.top.w - 380, top: main.plan.top.y + main.plan.top.h + 8, height: main.d.h - main.plan.top.h - 44 }} />
          </Layer>
        ) : null}
        {crown && <Menu t={t} x={crown.x} y={crown.y} onClose={() => crownMenu.set(null)} items={crownItems} />}
        {desk && (
          <Menu t={t} x={desk.x} y={desk.y} onClose={() => deskMenu.set(null)} items={[
            { label: tr('shell.ws.layout.add'), onClick: () => H.addWindow(displayAt(desk.x, desk.y) || mainDisplay(), desk) },
            { label: tr('shell.widgets.add'), onClick: () => widgetPicker.set({ display: displayAt(desk.x, desk.y) || mainDisplay(), x: desk.x, y: desk.y }) },
            { label: tr('shell.ws.add.webapp'), onClick: () => H.addWebApp() },
            '-',
            { label: tr('shell.ws.activity.new'), disabled: list.length >= 9, onClick: () => newActivity() },
            { label: tr('shell.desktops.overview'), onClick: () => openOverview() },
            '-',
            { label: tr('shell.desktop.wallpaper'), onClick: () => wm.open('settings', { args: { section: 'wallpaper' } }) },
            { label: tr('shell.classic.personalize'), onClick: () => wm.open('settings', { args: { section: 'appearance' } }) },
          ]} />
        )}
        {editId != null && list.includes(editId) ? (
          <Layer name="modal">
            <View style={{ position: 'absolute', left: 0, top: 0, width: '100%', height: '100%', backgroundColor: 'rgba(8,6,20,0.42)', alignItems: 'center', justifyContent: 'center' }}>
              <ActivityEditor key={editId} t={t} id={editId} meta={meta[editId] || activityOf(editId)} layout={layoutOf(editId, main.key).layout}
                onCancel={() => editing.set(null)}
                onDone={(v) => {
                  renameDesktop(editId, v.name);
                  updateActivity(editId, (m) => ({ ...m, icon: v.icon, color: v.color, pins: v.pins, setup: true }));
                  setLayout(editId, main.key, v.layout);
                  editing.set(null);
                }} />
            </View>
          </Layer>
        ) : null}
        {del != null && list.includes(del) ? (
          <Layer name="modal">
            <ConfirmDialog title={tr('shell.ws.activity.deleteTitle', { name: desktopName(del) })} detail={tr('shell.ws.activity.deleteDetail')}
              okLabel={tr('shell.ws.activity.deleteOk')} cancelLabel={tr('shell.ws.onboard.cancel')} destructive area={mainDisplay()}
              onCancel={() => deleting.set(null)} onOk={() => { const id = del; deleting.set(null); removeDesktopById(id); }} />
          </Layer>
        ) : null}
        <Layer name="modal"><PowerDialog /></Layer>
        <ShortcutsHelp t={t} />
        {barsOn ? <Layer name="overlay"><View style={{ position: 'absolute', left: 0, top: 0, width: 1, height: 1 }} /></Layer> : null}
        <AppSwitcher />
        <OverviewChrome />
        <LevelHud />
      </View>
    </ThemeProvider>
  );
}

startShell(<App />);
