// "Lord Workspaces" theme (id workspaces) — a desktop organized by activities. Each activity is one of the SDK's
// desktops (at most 9) with a name, an icon, a colour, pinned apps and a tiling layout per display; its windows sit in a
// group frame per display. The Context Spine on the side opens Activities (the Overview), Apps, Files and
// the Control Center; Search opens with ❖Space, Ctrl+Space or ❖ alone. Every surface takes one of three effects styles:
// Opaque, Translucent or Crystal. See docs/WORKSPACES.md.
//
//   tokens.js                   colours, the three materials and the shape tokens of the shared controls
//   model/                      plain functions, tested with node: geometry (where things go on each display),
//                               layouts (the tiles), activities (the settings key)
//   state.js, tiler.js          the activities' data and the tiling at runtime
//   motion.js                   the transitions: cached transforms from one frame loop, none with Reduce motion
//   components/                 Spine, TopBar (activity strip, status cluster), GroupFrame, Window, Search,
//                               ControlCenter, ActivityEditor (onboarding and edit), Surface
// The menus and the notification panel are Lord Classic's own components, restyled by these tokens.
import {
  startShell, settings, View, Image, Layer, screen, addListener, ThemeProvider, useSettings, useWindows, useStore, configureWindows, getApp, wm, t as tr,
  DesktopWidgets, registerGestureActions, screenDisplays, mainDisplay, findWallpaper, wallpaperSrc, MotionWallpaper, snapPreview, notifyHistory,
  useShortcuts, AppSwitcher, openSwitcher, switcherKeys, switcherGrabs, switcherHold, cycleAppWindows, hideApp, hideOthers, appSettingsItem,
  flowDesktopKeys, flowGrabs, desktopSwipe, OverviewBackdrop, OverviewChrome, openOverview, closeOverview, toggleOverview, overview, LevelHud,
  PowerDialog, ConfirmDialog, session, launchApp, direction, pointerDisplay, registerSpotlightSource, desktopList, desktopName, activeDesktop,
  switchDesktop, windows as windowsStore, windowTitle, windowIcon, appName, removeDesktopById, renameDesktop, createStore, lordosWallpapers,
} from '@lordos/sdk';
import { apps } from '@lordos/apps';
import { Fragment, useEffect, useMemo, useRef, useState } from 'react';
import { makeTheme } from './tokens.js';
import { displayPlan, shownDisplays, onChrome } from './model/geometry.js';
import { wsPrefs, optPatch, effectiveSurface, needsOnboarding } from './model/activities.js';
import { minimizeStarts } from '../default/minimize.js';
import { activities, panel, deleting, startActivities, updateActivity, setUpActivity, activityOf, noteLaunch, newActivity } from './state.js';
import { startTiler, areasChanged, keyOfDisplay, setLayout, layoutOf, tiled } from './tiler.js';
import { tween, EASE, reduceMotion } from './motion.js';
import { Window, TITLE_H, MENU_H } from './components/Window.jsx';
import { Spine } from './components/Spine.jsx';
import { ActivityStrip, StatusCluster, STATUS_W } from './components/TopBar.jsx';
import { GroupFrame } from './components/GroupFrame.jsx';
import { Search } from './components/Search.jsx';
import { ControlCenter } from './components/ControlCenter.jsx';
import { ActivityEditor } from './components/ActivityEditor.jsx';
import { ActivityIcon } from './glyphs.jsx';
import { Menu } from '../classic/components/Menu.jsx';
import { NotificationCenter } from '../classic/components/NotificationCenter.jsx';

const rtlNow = () => direction() === 'rtl';
const planFor = (d, s) => { const p = wsPrefs(s); return displayPlan(d, { spine: p.spine, every: p.spineEvery, rtl: rtlNow(), primary: !!d.main }); };
// the work area of each display: the group frame's inside (model/geometry.js)
const areaFn = (d) => planFor(d, settings.get()).work;

// chrome: borders (1 + 1) and the title bar of Window.jsx's frame; snapping on for windows that float
configureWindows({ apps, workArea: areaFn, chrome: { w: 2, h: TITLE_H + 2, menu: MENU_H }, snap: true });

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
  files: (e) => { if (e) noteLaunch(e.x, e.y); launchApp('files'); },
  control: () => panel.set((p) => (p?.name === 'control' ? null : { name: 'control', display: pointerDisplay().id })),
  notif: () => panel.set((p) => (p?.name === 'notif' ? null : { name: 'notif', display: mainDisplay().id })),
  addWindow: (d, e) => { if (e) noteLaunch(e.x, e.y); panel.set({ name: 'search', mode: 'apps', display: d.id }); },
  edit: (id) => editing.set(id),
  closePanel: () => panel.set(null),
};

const table = [
  { id: 'search', keys: ['Meta+Space'], run: () => openSearch(), label: 'shell.shortcuts.search', group: 'system' },
  { id: 'searchCtrl', keys: ['Ctrl+Space'], run: () => openSearch(), when: notInTerminal, label: 'shell.shortcuts.search', group: 'system' },
  { id: 'spine', keys: ['Meta+Alt+D'], run: toggleSpine, label: 'shell.ws.shortcuts.spine', group: 'system' },
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
  const offA = registerSpotlightSource('activities', (q, query, { best }) => desktopList.get().map((id, i) => {
    const name = desktopName(id);
    const n = windowsStore.get().filter((w) => w.desktop === id && !w.pip).length;
    const m = activityOf(id);
    return { key: `activity:${i}`, score: best(q, name), title: name, subtitle: tr('shell.ws.search.activitySub', { n, pos: i + 1 }),
      Icon: ({ size }) => <ActivityIcon icon={m.icon} color={m.color} bg="#ffffff" size={Math.round(size * 0.7)} />, run: () => switchDesktop(id) };
  }).filter((x) => x.score > 0));
  const offW = registerSpotlightSource('windows', (q, query, { best }) => windowsStore.get().filter((w) => !w.pip && !(getApp(w.id)?.builtin && !String(w.id).startsWith('linux#'))).map((w) => {
    const title = windowTitle(w);
    return { key: `window:${w.id}`, score: best(q, title, appName(getApp(w.id))), title, subtitle: desktopName(w.desktop), Icon: windowIcon(w), run: () => wm.focus(w.id) };
  }).filter((x) => x.score > 0));
  return () => { offA(); offW(); };
}

// Where a dragged floating window would snap.
function SnapPreview({ t }) {
  const p = useStore(snapPreview);
  if (!p) return null;
  const r = p.rect;
  return <View style={{ position: 'absolute', left: r.x + 6, top: r.y + 6, width: r.w - 12, height: r.h - 12, borderRadius: 14, pointerEvents: 'none',
    backgroundColor: t.mode === 'dark' ? 'rgba(124,92,255,0.16)' : 'rgba(91,63,224,0.14)', border: `2px solid ${t.accent}` }} />;
}

// The minimize effect: the window shrinks into the spine on its cached raster (one render to start, one to end).
const MIN_MS = 220;
function useLeaving(list, spine) {
  const prev = useRef(new Map());
  const [leaving, setLeaving] = useState([]);
  useEffect(() => {
    const started = minimizeStarts(list, prev.current, { reduce: reduceMotion() });
    if (!started.length) return;
    setLeaving((l) => [...l.filter((x) => !started.some((y) => y.id === x.id)), ...started.map((x) => x.id)]);
    for (const { id } of started) {
      setTimeout(() => {
        const n = wm.frameNode(id), w = wm.get(id);
        if (!n || !w) return;
        const f = wm.frame(w);
        const to = spine ? { x: spine.x + spine.w / 2, y: spine.y + 110 } : { x: f.x, y: f.y };
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
  startTiler();
  const [s] = useSettings();
  const prefs = wsPrefs(s);
  const surface = effectiveSurface(prefs, s);
  const t = useMemo(() => makeTheme({ mode: s.mode, accent: s.accent, effects: s.effects, surface }, lordosWallpapers), [s.mode, s.accent, s.effects, surface]);
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
  const sig = shown.map((d) => `${d.id}:${d.x},${d.y},${d.w}x${d.h}:${d.main ? 1 : 0}`).join('|');
  const plans = useMemo(() => shown.map((d) => ({ d, key: keyOfDisplay(d), plan: planFor(d, s) })), [sig, prefs.spine, prefs.spineEvery, rtl]);
  const main = plans.find((p) => p.d.main) || plans[0];

  // the work areas follow the spine and the displays live; the tiles are laid out again
  useEffect(() => { configureWindows({ apps, workArea: areaFn }); areasChanged(); }, [sig, prefs.spine, prefs.spineEvery, rtl, screen.width, screen.height]);
  useEffect(registerSources, []);
  useShortcuts({ grabs: switcherGrabs(flowGrabs()), table, onSuperTap: () => openSearch('apps') });
  useEffect(() => addListener('keydown', (e) => { if (e.key === 'Escape') { panel.set(null); crownMenu.set(null); deskMenu.set(null); } }), []);
  useEffect(() => { if (inOverview) panel.set(null); }, [inOverview]);
  useEffect(() => registerGestureActions({
    up: (g) => { if (g.fingers >= 4) openOverview(); else openSearch('apps'); },
    down: () => { panel.set(null); closeOverview(); },
    left: desktopSwipe('left'), right: desktopSwipe('right'),
  }), []);

  const leaving = useLeaving(all, main?.plan.spine);
  const spineActive = useMemo(() => ({ overview: inOverview, apps: open?.name === 'search' && open.mode === 'apps', control: open?.name === 'control', menu: !!crown }),
    [inOverview, open, crown]);
  const stripRect = useMemo(() => (main?.plan.top ? { ...main.plan.top, w: main.plan.top.w - STATUS_W } : null), [main]);
  if (open?.name === 'notif' && history.length) seen.current = Math.max(seen.current, history[0].id);
  const unread = history.some((n) => n.id > seen.current);
  const isOnChrome = (x, y) => onChrome(plans.map((p) => p.plan), x, y);
  const panelDisplay = (open && displays.find((d) => d.id === open.display)) || mainDisplay();
  const panelPlan = plans.find((p) => p.d.id === panelDisplay.id) || main;
  const wallpaper = findWallpaper(t.wallpapers, s.wallpaper);
  const preview = useStore(snapPreview);
  const top = mounted[mounted.length - 1];
  const deskMeta = meta[desktop] || activityOf(desktop);
  const onDesk = all.filter((w) => w.desktop === desktop && !w.pip).length;
  // the header's Move button: a window of this activity in front and another activity to send it to
  const canMove = !!focused && list.length > 1;
  const onboarding = needsOnboarding(deskMeta, list.length, onDesk) && !inOverview;
  const controlAnchor = () => {
    const n = controlRef.current;
    if (n) { const [x, y, w, h] = lord.getLayout(n.id); return { x, y, w, h }; }
    const sp = panelPlan?.plan.spine || { x: panelDisplay.x, y: panelDisplay.y + panelDisplay.h - 60, w: 0, h: 48 };
    return { x: sp.x, y: sp.y + sp.h - 60, w: sp.w, h: 48 };
  };
  const leavingWins = leaving.map((id) => all.find((w) => w.id === id)).filter((w) => w && !mounted.includes(w));
  const tight = new Set(tiles.tight);
  const crownItems = [
    { label: tr('shell.menu.about'), onClick: () => wm.open('about') },
    { label: tr('shell.menu.settings'), onClick: () => wm.open('settings') },
    '-',
    ...(session.managed() ? [{ label: tr('shell.menu.lock'), onClick: () => session.lock() }, { label: tr('shell.menu.switchUser'), onClick: () => session.switchUser() }] : []),
    { label: tr('shell.menu.logout'), onClick: () => session.quit() },
    ...(session.canPower() ? ['-', { label: tr('shell.menu.restart'), onClick: () => session.restart() }, { label: tr('shell.menu.shutdown'), onClick: () => session.shutdown() }] : []),
  ];

  return (
    <ThemeProvider tokens={t}>
      <View style={{ width: '100%', height: '100%', backgroundColor: t.mode === 'dark' ? '#0F0D1C' : '#DCD6F2' }}>
        {displays.map((d) => (
          <Image key={d.id + ':' + d.x} src={wallpaperSrc(wallpaper, d.w * (d.scale || 100) / 100, d.h * (d.scale || 100) / 100)}
            onContextMenu={(e) => deskMenu.set({ x: e.x, y: e.y })}
            style={{ position: 'absolute', left: d.x, top: d.y, width: d.w, height: d.h, objectFit: 'cover' }} />
        ))}
        {displays.map((d) => <MotionWallpaper key={d.id + ':' + d.x} display={d} />)}
        <DesktopWidgets layer="below" />
        {plans.map(({ d, key, plan }) => (
          <GroupFrame key={d.id} t={t} plan={plan} d={d} dkey={key} desk={desktop} meta={deskMeta} multi={plans.length > 1} canMove={canMove}
            onAddWindow={H.addWindow} onEdit={H.edit} showEmpty={!onboarding} />
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
            {preview && w === top ? <SnapPreview t={t} /> : null}
            <Window win={w} t={t} focused={!inOverview && w.id === focused} app={getApp(w.id)} tight={tight.has(w.id)} leaving={leaving.includes(w.id)} />
          </Fragment>
        ))}
        {leavingWins.map((w) => <Window key={w.id} win={w} t={t} focused={false} app={getApp(w.id)} leaving />)}
        <DesktopWidgets layer="above" />
        <Layer name="dock">
          {plans.map(({ d, plan }) => (plan.spine ? (
            <Spine key={d.id} t={t} rect={plan.spine} mode={plan.mode} rtl={rtl} active={spineActive} controlRef={d.main ? controlRef : undefined}
              onCrown={H.crown} onActivities={H.activities} onApps={H.apps} onFiles={H.files} onControl={H.control} onToggle={toggleSpine} />
          ) : null))}
          {main?.plan.top ? (
            <>
              <ActivityStrip t={t} rect={stripRect} onEdit={H.edit} />
              <StatusCluster t={t} rect={main.plan.top} rtl={rtl} screenW={screen.width} unread={unread} onControl={H.control} onNotif={H.notif} />
            </>
          ) : null}
        </Layer>
        {open?.name === 'search' ? (
          <Layer name="overlay"><Search key={panelDisplay.id} t={t} display={panelDisplay} onClose={() => panel.set(null)} isOnChrome={isOnChrome} /></Layer>
        ) : null}
        {open?.name === 'control' ? (
          <Layer name="popover"><ControlCenter t={t} anchor={controlAnchor()} area={panelDisplay} rtl={rtl} onClose={() => panel.set(null)} isOnChrome={isOnChrome} /></Layer>
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
        <AppSwitcher />
        <OverviewChrome />
        <LevelHud />
      </View>
    </ThemeProvider>
  );
}

startShell(<App />);
