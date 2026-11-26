// "Bluebell" — a tribute to the desktops of 2001, and the demo of style packs and app skins (part 4 of the
// community-themes plan; docs/THEMES-GUIDE.md "Style packs" and "App skins").
//
//   tokens.js              the colours, the shared controls' richer tokens (raised buttons, soft toolbars)
//   glyphs.jsx             our own icons in the era's style: folders, pages, round green arrows, the bluebell
//   components/Window.jsx  glossy blue title bars with rounded tops, a menu strip for the apps that have menus
//   components/Taskbar.jsx the green start button, quick launch, a button per window, the tray and the clock
//   skins/files.jsx        Files as that era's explorer: toolbar, address bar with Go, the task pane, icons or details
//
// Quick settings and menus are Lord Classic's own components with this theme's tokens; the start menu is its own. The style pack
// below gives the icons, white pointers with a black outline, a sound scheme of plain notes and an installed font
// (DejaVu Sans, of fonts-dejavu-core, which LordOS recommends): each part can be turned off in Settings → Appearance.
import {
  startShell, settings, View, Image, Layer, addListener, ThemeProvider, useTrackedSettings, useWindows, useStore, configureWindows, getApp, wm, t as tr,
  DesktopWidgets, PowerDialog, findWallpaper, wallpaperIdFor, wallpaperSrc, setBannerPlacement, useShortcuts, AppSwitcher, openSwitcher, switcherKeys,
  switcherGrabs, switcherHold, toggleShowDesktop, spotlight, launchApp, screen, screenDisplays, mainDisplay, hasBar, menuBarEveryDisplay, barDisplays,
  pointerDisplay, titleBarPx, registerStylePack, registerAppSkins, useThemeFont, LevelHud,
} from '@lordos/sdk';
import { apps } from '@lordos/apps';
import { useEffect, useRef, useState, Fragment } from 'react';
import { makeTheme, sky } from './tokens.js';
import { Window, TITLE_H, MENU_H, EDGE } from './components/Window.jsx';
import { Taskbar, BAR_H } from './components/Taskbar.jsx';
import { StartMenu } from './components/StartMenu.jsx';
import { QuickSettings } from '../classic/components/QuickSettings.jsx';
import { Menu } from '../classic/components/Menu.jsx';
import { fileIcons, toolbarIcons, appIcons } from './glyphs.jsx';
import { filesSkin } from './skins/files.jsx';
import { settingsSkin } from './skins/settings.jsx';
import { terminalSkin } from './skins/terminal.jsx';
import { editorSkin } from './skins/editor.jsx';
import { storeSkin } from './skins/store.jsx';

// ---- the style pack and the app skins (before startShell) ----
registerStylePack({
  icons: { files: fileIcons, toolbar: toolbarIcons, apps: appIcons },
  cursors: { fill: '#ffffff', outline: '#000000' },
  // our own short tunes, as plain notes [Hz, ms]: a rising chord to start, a two-note chime, a low knock, a soft rustle
  sounds: {
    startup: [[392, 160], [523.25, 160], [659.25, 160], [783.99, 420]],
    notify: [[880, 90], [1174.66, 220]],
    error: [[220, 110], [196, 220]],
    emptyTrash: [[1318.51, 50], [987.77, 50], [739.99, 60], [554.37, 120]],
  },
  fonts: { family: 'DejaVu Sans' },
});
registerAppSkins({ files: filesSkin, settings: settingsSkin, terminal: terminalSkin, editor: editorSkin, store: storeSkin });

const areaFn = (d) => {
  const a = { x: d.x, y: d.y, w: d.w, h: d.h };
  if (hasBar(d, menuBarEveryDisplay(settings.get()), screen.mirror)) a.h -= BAR_H;
  return a;
};
configureWindows({ apps, workArea: areaFn, chrome: () => ({ w: EDGE * 2, h: titleBarPx(TITLE_H) + EDGE, menu: MENU_H }), snap: true });

// The desktop when no picture was chosen: our own sky, with a few soft clouds made of overlapping puffs (no image).
const CLOUDS = [[0.1, 0.12, 0.2], [0.56, 0.07, 0.24], [0.76, 0.34, 0.16], [0.28, 0.42, 0.12]];
function Cloud({ x, y, w, color }) {
  const puffs = [[0, 0.35, 0.42], [0.22, 0.1, 0.5], [0.5, 0.2, 0.44], [0.7, 0.4, 0.3]];
  return (
    <View style={{ position: 'absolute', left: x, top: y, width: w, height: w * 0.5 }}>
      {puffs.map(([px, py, r]) => <View key={px} style={{ position: 'absolute', left: w * px, top: w * py * 0.5, width: w * r, height: w * r, borderRadius: w * r / 2, backgroundColor: color }} />)}
      <View style={{ position: 'absolute', left: w * 0.08, right: w * 0.04, top: w * 0.32, height: w * 0.18, borderRadius: w * 0.09, backgroundColor: color }} />
    </View>
  );
}
function Sky({ d, dark }) {
  const c = sky(dark);
  return (
    <View style={{ position: 'absolute', left: d.x, top: d.y, width: d.w, height: d.h, overflow: 'hidden', background: `linear-gradient(180deg, ${c.top}, ${c.bottom})` }}>
      {CLOUDS.map(([x, y, w]) => <Cloud key={x} x={d.w * x} y={d.h * y} w={d.w * w} color={c.cloud} />)}
    </View>
  );
}

const table = (ui) => [
  { id: 'start', keys: [], show: ['❖'], run: () => {}, label: 'shell.shortcuts.startMenu', group: 'system' },
  { id: 'searchRun', keys: ['Meta+R'], run: () => spotlight.open(), label: 'shell.shortcuts.search', group: 'system' },
  { id: 'showDesktop', keys: ['Meta+D'], run: toggleShowDesktop, label: 'shell.shortcuts.showDesktop', group: 'system' },
  { id: 'files', keys: ['Meta+E'], run: () => launchApp('files'), label: 'shell.shortcuts.files', group: 'system' },
  { id: 'settings', keys: ['Meta+I'], run: () => launchApp('settings'), label: 'shell.shortcuts.settings', group: 'system' },
  { id: 'switchApps', keys: switcherKeys('alt'), run: (e) => openSwitcher(switcherHold(e), e.shift), label: 'shell.shortcuts.switchApps', group: 'windows' },
  { id: 'closeWindow', keys: ['Alt+F4'], run: () => { const f = ui.current.focused; if (f) wm.close(f); }, label: 'shell.shortcuts.closeWindow', group: 'windows' },
];

function App() {
  const [s] = useTrackedSettings();
  const font = useThemeFont();
  const t = { ...makeTheme(s), fontFamily: font };
  const { mounted, focused } = useWindows();
  const [panel, setPanel] = useState(null);           // { name: 'start' | 'quick', display }
  const [desk, setDesk] = useState(null);
  const all = screenDisplays();
  const bars = barDisplays(all, menuBarEveryDisplay(s), screen.mirror);
  useEffect(() => { configureWindows({ apps, workArea: areaFn }); setBannerPlacement('bottom'); }, [screen.width, screen.height, screen.mirror]);
  const ui = useRef(null);
  const here = () => { const at = pointerDisplay(); return bars.find((b) => b.id === at.id) || bars[0] || mainDisplay(); };
  const toggle = (name, d) => setPanel((p) => (p && p.name === name && p.display === d.id ? null : { name, display: d.id }));
  ui.current = { focused };
  useShortcuts({ grabs: switcherGrabs('classic'), table: table(ui), onSuperTap: () => toggle('start', here()) });
  useEffect(() => addListener('keydown', (e) => { if (e.key === 'Escape') { setPanel(null); setDesk(null); } }), []);
  const pd = panel ? bars.find((d) => d.id === panel.display) : null;
  const isOnBar = (x, y) => bars.some((m) => x >= m.x && x < m.x + m.w && y >= m.y + m.h - BAR_H && y < m.y + m.h);
  const dark = s.mode === 'dark';
  return (
    <ThemeProvider tokens={t}>
      <View style={{ width: '100%', height: '100%', backgroundColor: dark ? '#0d1b33' : '#2c6fd6' }}>
        {all.map((d) => (s.wallpaper
          ? <Image key={d.id} src={wallpaperSrc(findWallpaper(t.wallpapers, wallpaperIdFor(s, d)), d.w * (d.scale || 100) / 100, d.h * (d.scale || 100) / 100)}
            onContextMenu={(e) => setDesk({ x: e.x, y: e.y })} style={{ position: 'absolute', left: d.x, top: d.y, width: d.w, height: d.h, objectFit: 'cover' }} />
          : <View key={d.id} onContextMenu={(e) => setDesk({ x: e.x, y: e.y })} style={{ position: 'absolute', left: d.x, top: d.y, width: d.w, height: d.h }}><Sky d={{ ...d, x: 0, y: 0 }} dark={dark} /></View>))}
        <DesktopWidgets layer="below" />
        {mounted.map((w) => (
          <Fragment key={w.id}>
            <Window win={w} t={t} font={font} focused={w.id === focused} app={getApp(w.id)} />
          </Fragment>
        ))}
        <DesktopWidgets layer="above" />
        <Layer name="dock">
          {bars.map((d) => (
            <View key={d.id} style={{ position: 'absolute', left: d.x, top: d.y, width: d.w, height: d.h, pointerEvents: 'box-none' }}>
              <Taskbar t={t} font={font} area={d} start={panel?.name === 'start' && pd?.id === d.id} onStart={() => toggle('start', d)} onTray={() => toggle('quick', d)} />
            </View>
          ))}
        </Layer>
        {panel?.name === 'start' && pd && <Layer name="overlay"><StartMenu key={pd.id} t={t} font={font} display={pd} barH={BAR_H} onClose={() => setPanel(null)} /></Layer>}
        {panel?.name === 'quick' && pd && (
          <Layer name="popover">
            <QuickSettings key={pd.id} t={t} at={{ pos: { left: pd.x + pd.w - 372, bottom: screen.height - (pd.y + pd.h - BAR_H - 8) }, top: pd.y + pd.h - BAR_H - 8 - 420, left: pd.x + pd.w - 372, height: 420 }}
              onClose={() => setPanel(null)} isOnBar={isOnBar} />
          </Layer>
        )}
        {desk && (
          <Menu t={t} x={desk.x} y={desk.y} onClose={() => setDesk(null)} items={[
            { label: tr('shell.classic.personalize'), onClick: () => wm.open('settings', { args: { section: 'appearance' } }) },
            { label: tr('shell.desktop.wallpaper'), onClick: () => wm.open('settings', { args: { section: 'wallpaper' } }) },
            { label: tr('shell.classic.displaySettings'), onClick: () => wm.open('settings', { args: { section: 'displays' } }) },
          ]} />
        )}
        <Layer name="modal"><PowerDialog /></Layer>
        <AppSwitcher />
        <LevelHud />
      </View>
    </ThemeProvider>
  );
}

startShell(<App />);
