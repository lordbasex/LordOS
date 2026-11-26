// "Minimal" — an example LordOS theme (docs/THEMES-GUIDE.md). It is deliberately small: a bar at
// the bottom of the main display with an apps menu, one button per open window and a clock; a plain
// window frame; menus from the SDK's item lists. Everything an app needs from a theme is here
// (tokens, frames, both kinds of window, the work area, layers); the polish of the bundled themes
// (themes/default, themes/classic) is not.
//
//   tokens.js   the colors (the contract with the apps) and this theme's own keys
//   index.jsx   this file: work area, window frame, bar, menus, startShell
import {
  startShell, View, Text, Image, Layer, ScrollView, ThemeProvider, useSettings, useWindows, configureWindows, getApp, wm, AppHost,
  WindowShell, windowTitle, windowIcon, useAppMenus, windowMenuItems, useLauncherApps, launchApp, DesktopWidgets, PowerDialog,
  findWallpaper, wallpaperSrc, mainDisplay, screenDisplays, screen, useClock, formatTime, solidOver, session, t as tr,
} from '@lordos/sdk';
import { apps } from '@lordos/apps';
import { useState } from 'react';
import { makeTheme } from './tokens.js';

const BAR_H = 40;
const TITLE_H = 32;

// The free area for windows on each display: the bar takes the bottom of the main one; the other
// displays are all work area (docs/DISPLAYS.md). Maximized and new windows use it.
const workAreaFor = (d) => (d.main ? { x: d.x, y: d.y, w: d.w, h: d.h - BAR_H } : { x: d.x, y: d.y, w: d.w, h: d.h });

// chrome: what the frame adds around the content (1 px borders and the title bar), so Linux windows
// open at the size they ask for.
configureWindows({ apps, workArea: workAreaFor, chrome: { w: 2, h: TITLE_H + 2 } });

// A menu from the SDK's item lists ([{ label, onClick, hint, disabled, checked, submenu, header } | '-'],
// as useAppMenus, windowMenuItems and dockMenu return them). A submenu is shown inline under its
// label, to keep the example short. at: { x, y } where it opens, or { x, bottom } to open upwards.
function Menu({ t, at, items, onClose }) {
  const W = 240;
  const flat = [];
  const add = (list) => {
    for (const it of list || []) {
      if (it && it !== '-' && it.submenu) { flat.push({ label: it.label, header: true }); add(it.submenu); flat.push('-'); } else flat.push(it);
    }
  };
  add(items);
  // rows are 26 px and separators 9 px: a long list (every installed app) scrolls
  const h = Math.min(flat.reduce((n, it) => n + (it === '-' || it == null ? 9 : 26), 0), screen.height - 140);
  const left = Math.max(8, Math.min(at.x, screen.width - W - 8));
  const place = at.bottom != null ? { bottom: at.bottom } : { top: Math.max(8, Math.min(at.y, screen.height - h - 20)) };
  return (
    <Layer name="popover">
      {/* a click anywhere outside closes it */}
      <View onMouseDown={onClose} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }} />
      <View onMouseDown={(e) => e.stopPropagation()}
        style={{ position: 'absolute', left, ...place, width: W, padding: 5, borderRadius: t.menuRadius, backgroundColor: t.popupBg || t.windowBg,
          border: `1px solid ${t.windowBorder}`, boxShadow: t.shadow }}>
        <ScrollView style={{ height: h }}>
          {flat.map((it, i) => (it === '-' || it == null
            ? <View key={i} style={{ height: 1, marginVertical: 4, backgroundColor: t.separator }} />
            : <MenuItem key={i} t={t} item={it} onClose={onClose} />))}
        </ScrollView>
      </View>
    </Layer>
  );
}

function MenuItem({ t, item, onClose }) {
  const [hover, setHover] = useState(false);
  const off = item.disabled || item.header;
  return (
    <View onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      onClick={() => { if (off) return; onClose(); item.onClick?.(); }}
      style={{ flexDirection: 'row', alignItems: 'center', height: 26, paddingHorizontal: 8, borderRadius: t.menuItemRadius,
        backgroundColor: hover && !off ? t.accent : 'transparent' }}>
      <Text style={{ width: 16, fontSize: 12, color: hover && !off ? '#ffffff' : t.text }}>{item.checked ? '✓' : ''}</Text>
      <Text style={{ flex: 1, fontSize: 13, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis',
        fontWeight: item.header ? 600 : 400, color: hover && !off ? '#ffffff' : off ? t.textDim : t.text }}>{item.label}</Text>
      {item.hint ? <Text style={{ fontSize: 12, color: hover && !off ? '#ffffff' : t.textDim }}>{item.hint}</Text> : null}
    </View>
  );
}

function FrameButton({ t, label, onClick }) {
  const [hover, setHover] = useState(false);
  return (
    <View onMouseDown={(e) => e.stopPropagation()} onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 22, paddingHorizontal: 8, borderRadius: t.controlRadiusSmall, justifyContent: 'center', backgroundColor: hover ? t.hover : 'transparent' }}>
      <Text style={{ fontSize: 12, color: t.text, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
}

// One window. Moving and resizing happen in C (WindowShell's edges, wm.beginMove): React gets no
// event per frame. A Linux window with its own decorations (args.csd) gets only the content.
function Window({ t, win, focused }) {
  const app = getApp(win.id);
  const menus = useAppMenus(win.id) || [];
  const [menu, setMenu] = useState(null);
  const border = focused ? t.windowBorder : t.separator;
  const items = [...menus.map((m) => ({ label: m.label, submenu: m.items })), { label: tr('shell.menu.window'), submenu: windowMenuItems(win.id) }];

  if (win.args?.csd) {
    // the app draws its own bar; a right click on it asks for the window menu (win.windowMenu)
    return (
      <WindowShell win={win} onMouseDown={() => wm.focus(win.id)} style={{ boxShadow: t.shadow }}>
        <AppHost id={win.id} focused={focused} />
        {win.windowMenu && <Menu t={t} at={win.windowMenu} items={windowMenuItems(win.id)} onClose={() => wm.closeWindowMenu(win.id)} />}
      </WindowShell>
    );
  }
  const Icon = windowIcon(win);
  return (
    <WindowShell win={win} onMouseDown={() => wm.focus(win.id)} style={{
      borderRadius: win.maximized ? 0 : 8, overflow: 'hidden', boxShadow: focused ? t.shadow : undefined,
      backgroundColor: app?.transparent ? 'transparent' : t.windowBg,
      // a transparent app (the Terminal) has nothing behind its border: draw it already mixed
      border: `1px solid ${app?.transparent ? solidOver(border, t.titleBg) : border}`,
    }}>
      <View onMouseDown={(e) => { if (!e.button) wm.beginMove(win.id); }} onDoubleClick={() => wm.toggleMaximize(win.id)}
        onContextMenu={(e) => setMenu({ x: e.x, y: e.y })}
        style={{ height: TITLE_H, flexDirection: 'row', alignItems: 'center', gap: 8, paddingStart: 10, paddingEnd: 4,
          backgroundColor: t.titleBg, borderBottom: `1px solid ${t.separator}` }}>
        {Icon ? <Icon size={16} /> : null}
        <Text style={{ flex: 1, fontSize: 12, fontWeight: 500, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis',
          color: focused ? t.text : t.textDim }}>{windowTitle(win)}</Text>
        <FrameButton t={t} label={tr('shell.classic.minimize')} onClick={() => wm.minimize(win.id)} />
        <FrameButton t={t} label={tr(win.maximized ? 'shell.classic.restore' : 'shell.classic.maximize')} onClick={() => wm.toggleMaximize(win.id)} />
        <FrameButton t={t} label={tr('shell.classic.close')} onClick={() => wm.close(win.id)} />
      </View>
      <View style={{ flex: 1 }}><AppHost id={win.id} focused={focused} /></View>
      {menu && <Menu t={t} at={menu} items={items} onClose={() => setMenu(null)} />}
    </WindowShell>
  );
}

// The bar: apps menu at the start, the windows, the clock at the end. The row flips by itself in a
// right-to-left language.
function Bar({ t, windows, focused, onApps }) {
  const m = mainDisplay();
  const clock = useClock((d) => formatTime(d), 1000);
  const own = windows.filter((w) => !(getApp(w.id)?.builtin && !String(w.id).startsWith('linux#')));
  return (
    <View style={{ position: 'absolute', left: m.x, top: m.y + m.h - BAR_H, width: m.w, height: BAR_H, flexDirection: 'row', alignItems: 'center',
      gap: 4, paddingHorizontal: 6, backgroundColor: t.barBg, backdropFilter: t.blur ? `blur(${t.blur}px)` : undefined,
      borderTop: `1px solid ${t.separator}` }}>
      <FrameButton t={t} label={tr('shell.menu.apps')} onClick={onApps} />
      <View style={{ flex: 1, flexDirection: 'row', gap: 4, overflow: 'hidden' }}>
        {own.map((w) => {
          const Icon = windowIcon(w);
          const front = w.id === focused;
          return (
            <View key={w.id} onClick={() => (front ? wm.minimize(w.id) : wm.focus(w.id))}
              style={{ maxWidth: 200, height: 30, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 8, borderRadius: t.controlRadius,
                backgroundColor: front ? t.hover : 'transparent', borderBottom: `2px solid ${front ? t.accent : 'transparent'}` }}>
              {Icon ? <Icon size={18} /> : null}
              <Text style={{ fontSize: 12, color: w.minimized ? t.textDim : t.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{windowTitle(w)}</Text>
            </View>
          );
        })}
      </View>
      <Text style={{ fontSize: 13, color: t.text, paddingHorizontal: 8 }}>{clock}</Text>
    </View>
  );
}

function App() {
  const [s] = useSettings();
  const t = makeTheme(s);
  const { windows, visible, focused } = useWindows();
  const launcher = useLauncherApps();
  const [appsMenu, setAppsMenu] = useState(null);
  const m = mainDisplay();
  const wallpaper = findWallpaper(t.wallpapers, s.wallpaper);
  const appItems = [
    ...launcher.map((a) => ({ label: a.name, onClick: () => launchApp(a.id) })),
    '-',
    { label: tr('shell.menu.settings'), onClick: () => wm.open('settings') },
    { label: tr('shell.menu.logout'), onClick: () => session.quit() },
  ];
  return (
    <ThemeProvider tokens={t}>
      <View style={{ width: '100%', height: '100%', backgroundColor: t.mode === 'dark' ? '#111114' : '#d5d9e0' }}>
        {/* a wallpaper per display, at that display's pixel size */}
        {screenDisplays().map((d) => (
          <Image key={d.id + ':' + d.x} src={wallpaperSrc(wallpaper, d.w * (d.scale || 100) / 100, d.h * (d.scale || 100) / 100)}
            style={{ position: 'absolute', left: d.x, top: d.y, width: d.w, height: d.h, objectFit: 'cover' }} />
        ))}
        <DesktopWidgets layer="below" />
        {visible.map((w) => <Window key={w.id} t={t} win={w} focused={w.id === focused} />)}
        <DesktopWidgets layer="above" />
        <Layer name="dock">
          <Bar t={t} windows={windows} focused={focused} onApps={() => setAppsMenu((v) => (v ? null : { x: m.x + 6, bottom: screen.height - (m.y + m.h - BAR_H) + 6 }))} />
        </Layer>
        {appsMenu && (
          <Menu t={t} at={appsMenu} onClose={() => setAppsMenu(null)} items={appItems} />
        )}
        {/* Restart… and Shut Down… confirmations (sdk/power.jsx) */}
        <Layer name="modal"><PowerDialog /></Layer>
      </View>
    </ThemeProvider>
  );
}

startShell(<App />);
