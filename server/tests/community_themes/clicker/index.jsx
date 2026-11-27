// A test fixture (community_theme_headless.sh, "clicker"): a whole-screen View whose click mounts new nodes (an apps
// list with each app's Icon from useLauncherApps), a timer that mounts more, and a shortcut table in the SDK's own form
// (keys: [...]). The timer also opens a window, and each window's icon is drawn with windowIcon (BUG-H of the ws171 QA:
// okminimal died once a window was open). The session clicks it through lord-server-test's hooks; none of it may take
// the theme off. The person's own wallpaper (image:<id>) is found and drawn (BUG-H: findWallpaper read lord.fs from the
// theme's call, refused, and the theme died at once).
import { startShell, View, configureWindows, useLauncherApps, useShortcuts, useWindows, windowIcon, wm, Image,
  findWallpaper, wallpaperSrc, useSettings } from '@lordos/sdk';
import { apps } from '@lordos/apps';
import { useEffect, useState } from 'react';
import { BG } from './parts/colors.js';
import Row from './parts/Row.jsx';

configureWindows({ apps, workArea: (d) => ({ x: d.x, y: d.y, w: d.w, h: d.h }), chrome: { w: 0, h: 0 } });

function Opened({ n }) {
  const list = useLauncherApps();
  useEffect(() => { console.log(`CT clicker opened ${n} with ${list.length > 0 ? 'apps' : 'no apps'}`); }, []);
  return <View style={{ padding: 4 }}>{list.slice(0, 6).map((a) => <Row key={a.id} app={a} />)}</View>;
}

function Shell() {
  const [open, setOpen] = useState(0);
  const [late, setLate] = useState(false);
  const { windows: wins } = useWindows();
  const [s] = useSettings();
  const wall = findWallpaper([], s.wallpaper);
  useEffect(() => { console.log(`CT clicker wallpaper ${wall ? wall.category || wall.id : 'none'}`); }, [wall && wall.id]);
  useEffect(() => { const t = setTimeout(() => { setLate(true); wm.open('calculator'); }, 1500); return () => clearTimeout(t); }, []);
  useEffect(() => { if (wins.length) console.log(`CT clicker window icons ${wins.length}`); }, [wins.length]);
  useShortcuts({ table: [
    { id: 'ctKeys', keys: ['Alt+F4', 'Meta+Shift+K'], run: () => console.log('CT clicker shortcut ran') },
    { id: 'ctCombo', combo: 'Ctrl+Alt+J', run: () => {} },
    { id: 'ctBase', keys: ['Meta+Space'], run: () => console.log('CT clicker BASE SHADOWED') },
  ] });
  return (
    <View onClick={() => setOpen((x) => x + 1)} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: BG }}>
      {wall ? <Image src={wallpaperSrc(wall, 1024, 768)} style={{ position: 'absolute', left: 0, top: 0, width: 1024, height: 768 }} /> : null}
      <View style={{ flexDirection: 'row', gap: 4 }}>
        {wins.map((w) => { const Icon = windowIcon(w); return Icon ? <Icon key={w.id} size={18} /> : null; })}
      </View>
      {late ? <Opened n="timer" /> : null}
      {Array.from({ length: open }, (_, i) => <Opened key={i} n={`click${i + 1}`} />)}
    </View>
  );
}

startShell(<Shell />);
