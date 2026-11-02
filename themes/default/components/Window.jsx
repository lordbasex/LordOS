// Window frame: draggable title bar, traffic lights, shadow. The content is the app
// (the SDK's AppHost); the theme only decides how the frame looks. Moving and resizing (invisible
// borders on all 4 sides and 4 corners, <WindowShell>) is done by axia-server in C
// (docs/WINDOWS.md); the JS drag below is only kept for a maximized window.
import { View, Text, addListener, wm, AppHost, windowTitle, WindowShell, moveToDesktopItems, t as tr, solidOver } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { Menu } from './Menu.jsx';

const TITLE_H = 40;
const LIGHTS = [
  { key: 'close', color: '#ff5f57', glyph: '×', act: (id) => wm.close(id) },
  { key: 'min', color: '#febc2e', glyph: '−', act: (id) => wm.minimize(id) },
  { key: 'max', color: '#28c840', glyph: '+', act: (id) => wm.toggleMaximize(id) },
];

// fx: the minimize effect ({ p: 0..1, x, y: where the Dock is}): the frame shrinks towards it.
function fxStyle(fx, geo) {
  if (!fx) return null;
  const e = fx.p * fx.p;                                // ease-in
  const dx = (fx.x - (geo.x + geo.w / 2)) * e, dy = (fx.y - (geo.y + geo.h / 2)) * e;
  return { transform: `translate(${dx}px, ${dy}px) scale(${1 - 0.9 * e})`, opacity: 1 - 0.7 * e, pointerEvents: 'none' };
}

export function Window({ win, app, t, focused, fx }) {
  const drag = useRef(null);
  const [hover, setHover] = useState(false);
  const geo = wm.frame(win);
  const fxs = fxStyle(fx, geo);

  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      if (drag.current) wm.move(win.id, e.x - drag.current.dx, e.y - drag.current.dy);
    });
    const offUp = addListener('pointerup', () => { drag.current = null; });
    return () => { offMove(); offUp(); };
  }, [win.id]);

  // Linux app that draws its own frame (GTK, Chromium and Firefox with their own bar): only the
  // content, with the theme's shadow. A right click on its bar asks for the window menu
  // (w.windowMenu): the same items as the menu bar's Window menu.
  if (win.args?.csd || app.ownFrame) {
    const wmenu = win.windowMenu;
    return (
      <WindowShell win={win} leaving={!!fx} onMouseDown={() => wm.focus(win.id)}
        style={{ ...(app.ownFrame ? { borderRadius: win.maximized ? 0 : 12, overflow: 'hidden' } : null), boxShadow: focused ? '0 24px 56px rgba(0,0,0,0.5)' : '0 10px 28px rgba(0,0,0,0.3)', ...fxs }}>
        <AppHost id={win.id} focused={focused} />
        {wmenu && (
          <Menu x={wmenu.x} y={wmenu.y} onClose={() => wm.closeWindowMenu(win.id)} items={[
            { label: tr('shell.window.minimize'), onClick: () => wm.minimize(win.id) },
            { label: tr('shell.window.maximize'), checked: !!win.maximized, onClick: () => wm.toggleMaximize(win.id) },
            { label: tr('shell.desktops.moveTo'), submenu: moveToDesktopItems(win.id) },
            '-',
            { label: tr('shell.window.close'), onClick: () => wm.close(win.id) },
          ]} />
        )}
      </WindowShell>
    );
  }

  return (
    <WindowShell win={win} leaving={!!fx} onMouseDown={() => wm.focus(win.id)} style={{
      borderRadius: win.maximized ? 0 : 12, backgroundColor: app.transparent ? 'transparent' : t.windowBg, overflow: 'hidden',
      // a transparent app (the Terminal) has nothing behind its border: the border is drawn opaque
      border: `1px solid ${app.transparent ? solidOver(t.windowBorder, t.titleBg) : t.windowBorder}`,
      boxShadow: focused ? '0 24px 56px rgba(0,0,0,0.55)' : '0 10px 28px rgba(0,0,0,0.32)',
      ...fxs,
    }}>
      <View onMouseDown={(e) => { if (!wm.beginMove(win.id)) drag.current = { dx: e.x - geo.x, dy: e.y - geo.y }; }}
        onDoubleClick={() => wm.toggleMaximize(win.id)}
        style={{ height: TITLE_H, flexDirection: 'row', alignItems: 'center', paddingLeft: 14, backgroundColor: t.titleBg }}>
        <Text style={{ position: 'absolute', left: 0, right: 0, top: 0, lineHeight: TITLE_H, textAlign: 'center',
          fontSize: 13, fontWeight: 600, color: focused ? t.text : t.textDim, pointerEvents: 'none', whiteSpace: 'nowrap',
          paddingHorizontal: 80 }}>{windowTitle(win)}</Text>
        <View onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
          style={{ flexDirection: 'row', gap: 8, paddingVertical: 6 }}>
          {LIGHTS.map((l) => (
            <View key={l.key} onMouseDown={(e) => e.stopPropagation()} onClick={() => l.act(win.id)}
              style={{ width: 13, height: 13, borderRadius: 7, alignItems: 'center', justifyContent: 'center',
                backgroundColor: focused || hover ? l.color : t.lightOff }}>
              {hover && <Text style={{ fontSize: 11, fontWeight: 700, lineHeight: 13, color: 'rgba(0,0,0,0.6)' }}>{l.glyph}</Text>}
            </View>
          ))}
        </View>
      </View>
      <View style={{ height: 1, backgroundColor: app.transparent ? solidOver(t.separator, t.titleBg) : t.separator }} />
      <View style={{ flex: 1 }}><AppHost id={win.id} focused={focused} /></View>
    </WindowShell>
  );
}
