// Window frame of the Classic theme: the app's icon and title at the start of a 34 px title bar and
// the minimize / maximize / close buttons at the end (on the left in right-to-left languages, the
// engine mirrors the row). Rounded corners unless maximized or snapped. Hovering over maximize
// offers the snap layouts (halves, thirds, quarters; sdk/windows.js "Snapping"). Moving and
// resizing are done in C (<WindowShell>, wm.beginMove; docs/WINDOWS.md); a maximized window is
// dragged out of its place by JS, and snaps where it is dropped.
// Lord Classic has no global menu bar: an app that declares menus (File, Edit, View, Go…; the same
// ones Lord Flow shows on top, useAppMenus) gets them in a strip under its title bar, with Window and
// Help, as desktop apps do on a taskbar layout. Apps without menus of their own get no strip.
import { View, Text, Layer, addListener, wm, AppHost, windowTitle, WindowShell, windowIcon, snapZoneAt, screen, useAppMenus, getApp, appGroup, windowMenuItems, moveToDesktopItems, solidOver, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { MinimizeGlyph, MaximizeGlyph, RestoreGlyph, CloseGlyph, LayoutGlyph } from '../glyphs.jsx';
import { Menu } from './Menu.jsx';

export const TITLE_H = 34;
export const MENU_H = 26;
const BTN_W = 44;

// The window's menu strip: the app's menus, then Window and Help. One menu open at a time, under its
// item; hovering another item while one is open moves to it, as menu bars do.
function MenuStrip({ t, win, focused, menus }) {
  const [open, setOpen] = useState(null);           // { i, x, y }
  const refs = useRef([]);
  const all = [
    ...menus,
    { label: tr('shell.menu.window'), items: windowMenuItems(win.id) },
    { label: tr('shell.menu.help'), items: [
      ...(getApp('help') ? [{ label: tr('shell.help.open'), onClick: () => wm.open('help') }, '-'] : []),
      ...(getApp('appabout') && appGroup(win.id) ? [{ label: tr('shell.app.about', { app: appGroup(win.id).name }), onClick: () => wm.open('appabout', { new: true, args: { key: appGroup(win.id).key, win: win.id } }) }] : []),
      { label: tr('shell.menu.about'), onClick: () => wm.open('about') },
    ] },
  ];
  const at = (i) => {
    const n = refs.current[i];
    if (!n) return null;
    const [x, y, , h] = lord.getLayout(n.id);
    return { i, x, y: y + h + 2 };
  };
  return (
    <View onMouseDown={(e) => e.stopPropagation()}
      style={{ height: MENU_H, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 6, gap: 2,
        backgroundColor: focused ? t.titleBg : t.titleBgBlur, borderBottom: `1px solid ${t.separator}` }}>
      {all.map((m, i) => (
        <View key={i} ref={(inst) => { refs.current[i] = inst; }}
          onClick={() => setOpen((o) => (o?.i === i ? null : at(i)))}
          onMouseEnter={() => { if (open && open.i !== i) setOpen(at(i)); }}
          style={{ height: 22, paddingHorizontal: 8, borderRadius: 4, justifyContent: 'center',
            backgroundColor: open?.i === i ? t.hover : 'transparent' }}>
          <Text style={{ fontSize: 12, color: focused ? t.text : t.textDim, whiteSpace: 'nowrap' }}>{m.label}</Text>
        </View>
      ))}
      {open && all[open.i] && <Menu t={t} x={open.x} y={open.y} items={all[open.i].items} onClose={() => setOpen(null)} width={250} />}
    </View>
  );
}

// The snap layouts (cells in 0..1 of the work area) and the zone each cell snaps to.
const LAYOUTS = [
  { cells: [[0, 0, 0.5, 1], [0.5, 0, 0.5, 1]], zones: ['left', 'right'] },
  { cells: [[0, 0, 2 / 3, 1], [2 / 3, 0, 1 / 3, 1]], zones: ['left-two-thirds', 'right-third'] },
  { cells: [[0, 0, 1 / 3, 1], [1 / 3, 0, 1 / 3, 1], [2 / 3, 0, 1 / 3, 1]], zones: ['left-third', 'center-third', 'right-third'] },
  { cells: [[0, 0, 0.5, 0.5], [0.5, 0, 0.5, 0.5], [0, 0.5, 0.5, 0.5], [0.5, 0.5, 0.5, 0.5]], zones: ['top-left', 'top-right', 'bottom-left', 'bottom-right'] },
];

const LAYOUTS_W = 4 * 72 + 3 * 8 + 24;

// at: { x, y } under the maximize button (the flyout ends there), or { left, y } where it starts
// (opened from the window menu of an app that draws its own bar).
function SnapLayouts({ t, win, at, onEnter, onLeave, onDone }) {
  const [hover, setHover] = useState(null);         // [layout, cell]
  const W = LAYOUTS_W;
  const left = at.left != null ? Math.min(at.left, screen.width - W - 6) : at.x - W + BTN_W * 1.5;
  return (
    <Layer name="popover">
      <View onMouseEnter={onEnter} onMouseLeave={onLeave}
        style={{ position: 'absolute', left: Math.max(6, left), top: at.y, width: W, padding: 12, gap: 8, borderRadius: t.menuRadius + 2,
          backgroundColor: t.panelBg, backdropFilter: t.blur ? `blur(${t.blur}px)` : undefined, border: `1px solid ${t.windowBorder}`, boxShadow: t.shadow }}>
        <Text style={{ fontSize: 11, fontWeight: 600, color: t.textDim, textAlign: 'start' }}>{tr('shell.classic.snapLayouts')}</Text>
        <View style={{ flexDirection: 'row', gap: 8, direction: 'ltr' }}>
          {LAYOUTS.map((l, li) => (
            <View key={li} style={{ padding: 6, borderRadius: 6, backgroundColor: t.hover }}>
              <LayoutGlyph cells={l.cells} w={60} h={40} color={t.textDim} accent={t.accent}
                hoverIndex={hover && hover[0] === li ? hover[1] : -1}
                onHover={(i) => setHover(i < 0 ? null : [li, i])}
                onPick={(i) => { onDone(); wm.focus(win.id); wm.snap(win.id, l.zones[i]); }} />
            </View>
          ))}
        </View>
      </View>
    </Layer>
  );
}

function FrameButton({ t, focused, onClick, danger, children, anchor, onEnter, onLeave }) {
  const [hover, setHover] = useState(false);
  return (
    <View ref={anchor} onMouseDown={(e) => e.stopPropagation()} onClick={onClick}
      onMouseEnter={() => { setHover(true); onEnter?.(); }} onMouseLeave={() => { setHover(false); onLeave?.(); }}
      style={{ width: BTN_W, height: TITLE_H, alignItems: 'center', justifyContent: 'center', opacity: focused || hover ? 1 : 0.55,
        backgroundColor: hover ? (danger ? t.closeHover : t.hover) : 'transparent' }}>
      {children(hover && danger ? '#ffffff' : t.text)}
    </View>
  );
}

export function Window({ win, t, focused, app }) {
  const drag = useRef(null);
  const geo = wm.frame(win);
  const maxRef = useRef(null);
  const [layouts, setLayouts] = useState(null);       // where the snap layouts open
  const [menu, setMenu] = useState(null);
  const hoverTimer = useRef(0), leaveTimer = useRef(0);
  const flat = win.maximized || !!win.snapped;
  const appMenus = useAppMenus(win.id) || [];
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      const d = drag.current;
      if (!d) return;
      if (!d.moving) {
        if (Math.abs(e.x - d.x0) + Math.abs(e.y - d.y0) < 6) return;
        d.moving = true;
        // out of the maximized place: back to its own size, under the pointer where it was grabbed
        d.dx = Math.round(d.rel * win.w);
        d.dy = Math.min(TITLE_H / 2, d.y0 - geo.y);
      }
      wm.move(win.id, e.x - d.dx, e.y - d.dy);
    });
    const offUp = addListener('pointerup', (e) => {
      const d = drag.current;
      drag.current = null;
      if (d && d.moving) { const z = snapZoneAt(e.x, e.y); if (z) wm.snap(win.id, z); }
    });
    return () => { offMove(); offUp(); clearTimeout(hoverTimer.current); clearTimeout(leaveTimer.current); };
  }, [win.id, win.w, geo.y]);

  const shadow = focused ? t.shadow : (t.mode === 'dark' ? '0 8px 22px rgba(0,0,0,0.4)' : '0 8px 22px rgba(0,0,0,0.16)');

  // closes the snap layouts a moment after the pointer leaves (or `ms` after opening them from a menu)
  const leaveMax = (ms = 300) => { clearTimeout(hoverTimer.current); clearTimeout(leaveTimer.current); leaveTimer.current = setTimeout(() => setLayouts(null), ms); };
  // The title bar's menu; for an app that draws its own bar it also offers the snap layouts (its
  // maximize button is the app's, so there is no hover flyout).
  const windowMenuItems = (extra) => [
    { label: tr('shell.classic.restore'), disabled: !win.maximized && !win.snapped, onClick: () => wm.snap(win.id, 'restore') },
    { label: tr('shell.classic.minimize'), onClick: () => wm.minimize(win.id) },
    { label: tr('shell.classic.maximize'), disabled: win.maximized, onClick: () => wm.snap(win.id, 'max') },
    { label: tr('shell.desktops.moveTo'), submenu: moveToDesktopItems(win.id) },
    '-',
    ...(extra || []),
    { label: tr('shell.classic.snapLeft'), onClick: () => wm.snap(win.id, 'left') },
    { label: tr('shell.classic.snapRight'), onClick: () => wm.snap(win.id, 'right') },
    '-',
    { label: tr('shell.classic.close'), hint: 'Alt+F4', onClick: () => wm.close(win.id) },
  ];

  // Linux app with its own frame (GTK, Chromium and Firefox with their own bar): only the content,
  // with the theme's shadow. A right click on its bar asks for the window menu (w.windowMenu).
  if (win.args?.csd || app.ownFrame) {
    const wmenu = win.windowMenu;
    return (
      <WindowShell win={win} onMouseDown={() => wm.focus(win.id)} style={{ ...(app.ownFrame ? { borderRadius: win.maximized ? 0 : 12, overflow: 'hidden' } : null), boxShadow: shadow }}>
        <AppHost id={win.id} focused={focused} />
        {wmenu && (
          <Menu t={t} x={wmenu.x} y={wmenu.y} onClose={() => wm.closeWindowMenu(win.id)} items={windowMenuItems([
            { label: tr('shell.classic.snapLayouts'), onClick: () => { setLayouts({ left: wmenu.x, y: wmenu.y }); leaveMax(3000); } },
          ])} />
        )}
        {layouts && <SnapLayouts t={t} win={win} at={layouts} onEnter={() => clearTimeout(leaveTimer.current)} onLeave={() => leaveMax()} onDone={() => setLayouts(null)} />}
      </WindowShell>
    );
  }

  const Icon = windowIcon(win);
  const openLayouts = () => {
    if (!maxRef.current) return;
    const [x, y, w, h] = lord.getLayout(maxRef.current.id);
    setLayouts({ x: x + w / 2 - BTN_W / 2, y: y + h + 4 });
  };
  const enterMax = () => { clearTimeout(leaveTimer.current); clearTimeout(hoverTimer.current); hoverTimer.current = setTimeout(openLayouts, 550); };
  const titleMenu = (e) => setMenu({ x: e.x, y: e.y });
  return (
    <WindowShell win={win} onMouseDown={() => wm.focus(win.id)} style={{
      borderRadius: flat ? 0 : 8, backgroundColor: app.transparent ? 'transparent' : t.windowBg, overflow: 'hidden',
      // a transparent app (the Terminal) has nothing behind its border: the border is drawn opaque
      border: `1px solid ${app.transparent ? solidOver(focused ? t.windowBorder : t.separator, t.titleBg) : (focused ? t.windowBorder : t.separator)}`, boxShadow: shadow,
    }}>
      <View onMouseDown={(e) => {
        if (e.button) return;
        if (!wm.beginMove(win.id)) drag.current = { x0: e.x, y0: e.y, rel: geo.w ? (e.x - geo.x) / geo.w : 0.5, moving: false };
      }}
        onDoubleClick={() => wm.toggleMaximize(win.id)} onContextMenu={titleMenu}
        style={{ height: TITLE_H, flexDirection: 'row', alignItems: 'center', paddingStart: 12, gap: 8,
          backgroundColor: focused ? t.titleBg : t.titleBgBlur }}>
        {Icon ? <Icon size={16} /> : null}
        <Text style={{ flex: 1, fontSize: 12, color: focused ? t.text : t.textDim, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{windowTitle(win)}</Text>
        <FrameButton t={t} focused={focused} onClick={() => wm.minimize(win.id)}>{(c) => <MinimizeGlyph color={c} />}</FrameButton>
        <FrameButton t={t} focused={focused} anchor={maxRef} onClick={() => { setLayouts(null); wm.toggleMaximize(win.id); }} onEnter={enterMax} onLeave={leaveMax}>
          {(c) => (win.maximized || win.snapped ? <RestoreGlyph color={c} /> : <MaximizeGlyph color={c} />)}
        </FrameButton>
        <FrameButton t={t} focused={focused} danger onClick={() => wm.close(win.id)}>{(c) => <CloseGlyph color={c} />}</FrameButton>
      </View>
      {appMenus.length ? <MenuStrip t={t} win={win} focused={focused} menus={appMenus} /> : null}
      <View style={{ flex: 1 }}><AppHost id={win.id} focused={focused} /></View>
      {layouts && <SnapLayouts t={t} win={win} at={layouts} onEnter={() => clearTimeout(leaveTimer.current)} onLeave={() => leaveMax()} onDone={() => setLayouts(null)} />}
      {menu && (
        <Menu t={t} x={menu.x} y={menu.y} onClose={() => setMenu(null)} items={windowMenuItems()} />
      )}
    </WindowShell>
  );
}
