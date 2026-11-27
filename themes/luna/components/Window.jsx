// Luna's window frame: a glossy blue title bar with rounded top corners, the app's icon and title in white, and
// the minimize / maximize / close buttons as rounded squares at its end (close in warm red); a thin blue edge around the
// rest of the window. Under the title bar, an app that has menus gets them in a light strip (File, Edit, View…, then
// Window and Help), as on a taskbar layout. Moving and resizing are lord-server's (<WindowShell>, wm.beginMove).
import { View, Text, wm, AppHost, windowTitle, WindowShell, windowIcon, useMenusWithSpeech, getApp, appGroup, windowMenuItems, moveToDesktopItems, alwaysOnTopItem,
  useWindowButtons, addMenuBarKeys, barIndex, webHelpItems, SeamCover, solidOver, t as tr, buttonSizeScale } from '@lordos/sdk';
import { memo, useEffect, useRef, useState } from 'react';
import { Menu } from '../../classic/components/Menu.jsx';
import { FrameGlyph } from '../glyphs.jsx';

export const TITLE_H = 30;
export const MENU_H = 24;
export const EDGE = 3;

function MenuStrip({ t, win, focused, menus, font }) {
  const [open, setOpen] = useState(null);
  const refs = useRef([]);
  const all = [
    ...menus,
    { label: tr('shell.menu.window'), items: windowMenuItems(win.id) },
    { label: tr('shell.menu.help'), items: [
      ...webHelpItems(win.id),
      ...(getApp('help') ? [{ label: tr('shell.help.open'), onClick: () => wm.open('help') }, '-'] : []),
      ...(getApp('appabout') && appGroup(win.id) ? [{ label: tr('shell.app.about', { app: appGroup(win.id).name }), onClick: () => wm.open('appabout', { new: true, args: { key: appGroup(win.id).key, win: win.id } }) }] : []),
      { label: tr('shell.menu.about'), onClick: () => wm.open('about') },
    ] },
  ];
  const at = (i) => {
    const n = refs.current[i];
    if (!n) return null;
    const [x, y, , h] = lord.getLayout(n.id);
    return { i, x, y: y + h };
  };
  const count = useRef(0);
  count.current = all.length;
  const live = useRef(focused);
  live.current = focused;
  const step = (dir) => setOpen((o) => (o ? { ...at(barIndex(count.current, o.i, dir)), keys: true } : o));
  useEffect(() => addMenuBarKeys({ can: () => live.current, run: () => setOpen((o) => (o ? null : { ...at(0), keys: true })) }), []);
  return (
    <View onMouseDown={(e) => e.stopPropagation()}
      style={{ height: MENU_H, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 2, backgroundColor: t.sidebarBg, borderBottom: `1px solid ${t.separator}` }}>
      {all.map((m, i) => (
        <View key={i} ref={(inst) => { refs.current[i] = inst; }}
          onClick={() => setOpen((o) => (o?.i === i ? null : at(i)))}
          onMouseEnter={() => { if (open && open.i !== i) setOpen(at(i)); }}
          style={{ height: MENU_H - 4, paddingHorizontal: 7, justifyContent: 'center', backgroundColor: open?.i === i ? t.accent : 'transparent' }}>
          <Text style={{ fontSize: 12, fontFamily: font, color: open?.i === i ? '#ffffff' : t.text, whiteSpace: 'nowrap' }}>{m.label}</Text>
        </View>
      ))}
      {open && all[open.i] && <Menu key={open.i} t={t} x={open.x} y={open.y} items={all[open.i].items} onClose={() => setOpen(null)} width={240} keys={!!open.keys} onStep={step} />}
    </View>
  );
}

function AppMenuStrip(props) {
  const menus = useMenusWithSpeech(props.win.id) || [];
  return menus.length ? <MenuStrip {...props} menus={menus} /> : null;
}

function FrameButton({ t, kind, focused, onClick, size }) {
  const [hover, setHover] = useState(false);
  const k = buttonSizeScale(size);
  const s = Math.round(21 * k);
  return (
    <View onMouseDown={(e) => e.stopPropagation()} onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width: s, height: s, borderRadius: 3, alignItems: 'center', justifyContent: 'center', border: '1px solid rgba(255,255,255,0.85)',
        background: kind === 'close' ? t.closeBg : t.frameButtonBg, opacity: focused ? (hover ? 0.88 : 1) : 0.72 }}>
      <FrameGlyph kind={kind} size={s} />
    </View>
  );
}

function WindowFrame({ win, t, focused, app, font }) {
  const geo = wm.frame(win);
  const [menu, setMenu] = useState(null);
  const flat = win.maximized || !!win.snapped;
  const { atStart, order, size, px } = useWindowButtons(false);
  const bar = px(TITLE_H);
  const edge = focused ? t.frameEdge : t.frameEdgeBlur;
  const shadow = focused ? t.shadow : '0 4px 12px rgba(0,0,0,0.25)';
  const items = [
    { label: tr('shell.classic.restore'), disabled: !win.maximized && !win.snapped, onClick: () => wm.snap(win.id, 'restore') },
    { label: tr('shell.classic.minimize'), onClick: () => wm.minimize(win.id) },
    { label: tr('shell.classic.maximize'), disabled: win.maximized, onClick: () => wm.snap(win.id, 'max') },
    alwaysOnTopItem(win.id),
    { label: tr('shell.desktops.moveTo'), submenu: moveToDesktopItems(win.id) },
    '-',
    { label: tr('shell.classic.close'), hint: 'Alt+F4', onClick: () => wm.close(win.id) },
  ];

  // a Linux app with its own frame: only its content, with the shadow
  if (win.args?.csd || app.ownFrame) {
    const wmenu = win.windowMenu;
    return (
      <WindowShell win={win} onMouseDown={() => wm.focus(win.id)} style={{ boxShadow: shadow }}>
        <AppHost id={win.id} focused={focused} />
        {wmenu && <Menu t={t} x={wmenu.x} y={wmenu.y} onClose={() => wm.closeWindowMenu(win.id)} items={items} />}
      </WindowShell>
    );
  }

  const Icon = windowIcon(win);
  const button = (k) => (k === 'close' ? <FrameButton key={k} t={t} kind="close" size={size} focused={focused} onClick={() => wm.close(win.id)} />
    : k === 'minimize' ? <FrameButton key={k} t={t} kind="minimize" size={size} focused={focused} onClick={() => wm.minimize(win.id)} />
      : <FrameButton key={k} t={t} kind={win.maximized || win.snapped ? 'restore' : 'maximize'} size={size} focused={focused} onClick={() => wm.toggleMaximize(win.id)} />);
  const buttons = <View style={{ flexDirection: 'row', gap: 2 }}>{order.map(button)}</View>;
  const round = flat ? 0 : 8;
  const border = app.transparent ? solidOver(edge, '#000000') : edge;
  return (
    <WindowShell win={win} onMouseDown={() => wm.focus(win.id)} style={{
      borderTopLeftRadius: round, borderTopRightRadius: round, overflow: 'hidden', boxShadow: shadow,
      backgroundColor: app.transparent ? 'transparent' : t.windowBg,
      borderLeftWidth: flat ? 0 : EDGE, borderRightWidth: flat ? 0 : EDGE, borderBottomWidth: flat ? 0 : EDGE, borderTopWidth: 0, borderColor: border,
    }}>
      <View onMouseDown={(e) => { if (!e.button) wm.beginMove(win.id); }} onDoubleClick={() => wm.toggleMaximize(win.id)} onContextMenu={(e) => setMenu({ x: e.x, y: e.y })}
        style={{ height: bar, flexDirection: 'row', alignItems: 'center', gap: 6, paddingStart: 6, paddingEnd: 5, background: focused ? t.titleBg : t.titleBgBlur }}>
        {app.transparent ? <SeamCover color={focused ? '#0858d6' : '#7c9ee0'} /> : null}
        {/* the gloss: a lighter band along the top of the bar */}
        <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: Math.round(bar * 0.3), backgroundColor: 'rgba(255,255,255,0.16)', pointerEvents: 'none' }} />
        {atStart ? buttons : null}
        {Icon ? <Icon size={16} /> : null}
        <Text style={{ flex: 1, fontSize: 13, fontWeight: 700, fontFamily: font, color: focused ? t.titleText : t.titleTextBlur, whiteSpace: 'nowrap', textOverflow: 'ellipsis',
          textAlign: 'start' }}>{windowTitle(win)}</Text>
        {atStart ? null : buttons}
      </View>
      <AppMenuStrip t={t} win={win} focused={focused} font={font} />
      <View style={{ flex: 1 }}><AppHost id={win.id} focused={focused} /></View>
      {menu && <Menu t={t} x={menu.x} y={menu.y} onClose={() => setMenu(null)} items={items} />}
    </WindowShell>
  );
}

// memo: a window draws again for its own changes (win, focused, the tokens, the minimize effect, a maximized one's work
// area: frameKey), never because another window's title or the desktop around it changed (0.8.9.6.2 QA, BUG-9)
export const Window = memo(WindowFrame);
