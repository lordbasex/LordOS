// Window frame of Lord Workspaces: a 36 px title bar (the app's icon and title at the start, the buttons at the end, or
// first with Settings → Desktop & Dock → Title bars → Left) and, for an app with menus of its own, a strip with them
// under it, with Window and Help (there is no global menu bar: F10, or ⌥ pressed and released alone, opens the focused
// window's first menu, sdk/shortcuts.js addMenuBarKeys). The buttons are real targets (28 px, a glyph that says what
// they do, a fill on hover), never colour alone. Right click on the title: tile or float the window, move it to another
// activity, minimize, maximize, close.
// A Linux app that draws its own bar (args.csd, decided on every render) gets no frame: the content with the shadow, and
// the theme's window menu where it asks for one (w.windowMenu). Moving and resizing are lord-server's (<WindowShell>,
// wm.beginMove); a maximized window is dragged out of its place by JS. Memoized: a window redraws only when its own
// entry in the window list changes (index.jsx keeps the tokens object stable).
import {
  View, Text, addListener, wm, AppHost, windowTitle, windowAccent, WindowShell, windowIcon, useMenusWithSpeech, getApp, appGroup, windowMenuItems,
  solidOver, useWindowButtons, addMenuBarKeys, barIndex, webHelpItems, desktopList, desktopName, activeDesktop, t as tr,
} from '@lordos/sdk';
import { memo, useEffect, useRef, useState } from 'react';
import { MinimizeGlyph, MaximizeGlyph, RestoreGlyph, CloseGlyph, LockGlyph } from '../glyphs.jsx';
import { flyout, FLYOUT_DELAY } from './SnapTargets.jsx';
import { Menu } from '../../classic/components/Menu.jsx';
import { setFloating, isFloating } from '../tiler.js';
import { tween, EASE } from '../motion.js';
import { launchOrigin } from '../state.js';

export const TITLE_H = 36;
export const MENU_H = 26;
const BTN = 28;
// the shell started a moment ago: the windows coming back (session restore, a theme switch) don't zoom in
const started = Date.now();

function MenuStrip({ t, win, focused, menus }) {
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
    return { i, x, y: y + h + 2 };
  };
  const count = useRef(0);
  count.current = all.length;
  const live = useRef(focused);
  live.current = focused;
  const step = (dir) => setOpen((o) => (o ? { ...at(barIndex(count.current, o.i, dir)), keys: true } : o));
  useEffect(() => addMenuBarKeys({ can: () => live.current, run: () => setOpen((o) => (o ? null : { ...at(0), keys: true })) }), []);
  return (
    <View onMouseDown={(e) => e.stopPropagation()}
      style={{ height: MENU_H, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 8, gap: 2,
        backgroundColor: focused ? t.titleBg : t.titleBgBlur, borderBottom: `1px solid ${t.separator}` }}>
      {all.map((m, i) => (
        <View key={i} ref={(inst) => { refs.current[i] = inst; }}
          onClick={() => setOpen((o) => (o?.i === i ? null : at(i)))}
          onMouseEnter={() => { if (open && open.i !== i) setOpen(at(i)); }}
          style={{ height: 22, paddingHorizontal: 8, borderRadius: 6, justifyContent: 'center', backgroundColor: open?.i === i ? t.hover : 'transparent' }}>
          <Text style={{ fontSize: 12, color: focused ? t.text : t.textDim, whiteSpace: 'nowrap' }}>{m.label}</Text>
        </View>
      ))}
      {open && all[open.i] && <Menu key={open.i} t={t} x={open.x} y={open.y} items={all[open.i].items} onClose={() => setOpen(null)} width={250} keys={!!open.keys} onStep={step} />}
    </View>
  );
}

// subscribes on its own: an app that sets its menus after every draw must not redraw the window
function AppMenuStrip({ t, win, focused }) {
  const menus = useMenusWithSpeech(win.id) || [];
  return menus.length ? <MenuStrip t={t} win={win} focused={focused} menus={menus} /> : null;
}

function FrameButton({ t, focused, onClick, danger, children, label, onHover }) {
  const [hover, setHover] = useState(false);
  const ref = useRef(null);
  return (
    <View ref={ref} onMouseDown={(e) => e.stopPropagation()} onClick={onClick} title={onHover ? undefined : label}
      onMouseEnter={() => { setHover(true); onHover?.(true, ref.current); }} onMouseLeave={() => { setHover(false); onHover?.(false, ref.current); }}
      style={{ width: BTN, height: BTN, borderRadius: BTN / 2, alignItems: 'center', justifyContent: 'center', opacity: focused || hover ? 1 : 0.6,
        backgroundColor: hover ? (danger ? t.closeHover : t.hover) : 'transparent' }}>
      {children(hover && danger ? '#ffffff' : t.text)}
    </View>
  );
}

// The activities a window can move to (and a new one), for its title bar's menu.
function moveItems(win) {
  const list = desktopList.get();
  return list.filter((d) => d !== win.desktop).map((d) => ({ label: desktopName(d), onClick: () => wm.moveToDesktop(win.id, d) }));
}

export const Window = memo(function Window({ win, t, focused, app, tight, leaving = false, locked = false }) {
  const drag = useRef(null);
  const geo = wm.frame(win);
  const [menu, setMenu] = useState(null);
  const { atStart, order: buttonOrder } = useWindowButtons(false);
  // on a locked display (the frame's padlock): only minimize, no move, resize or window menu (the SDK refuses them too)
  const order = locked ? [] : buttonOrder;
  const floating = isFloating(win.id);
  // zoom in from where it was launched (the spine, the search panel), on its cached raster; not the windows a session
  // or a theme switch brings back, and never with Reduce motion (motion.js)
  useEffect(() => {
    if (Date.now() - started < 2500 || win.minimized || win.desktop !== activeDesktop.get()) return;
    const node = wm.frameNode(win.id);
    if (!node) return;
    const recent = Date.now() - launchOrigin.t < 1500;
    if (recent) {
      try {
        lord.setStyle(node, 'transformOriginX', Math.max(0, Math.min(geo.w, launchOrigin.x - geo.x)));
        lord.setStyle(node, 'transformOriginY', Math.max(0, Math.min(geo.h, launchOrigin.y - geo.y)));
      } catch (e) { /* gone */ }
    }
    tween(node, { scaleX: [0.9, 1], scaleY: [0.9, 1], opacity: [0, 1] }, { ms: 200, ease: EASE.out,
      done: () => { try { lord.setStyle(node, 'transformOriginX', undefined); lord.setStyle(node, 'transformOriginY', undefined); } catch (e) { /* gone */ } } });
  }, []);
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      const d = drag.current;
      if (!d) return;
      if (!d.moving) {
        if (Math.abs(e.x - d.x0) + Math.abs(e.y - d.y0) < 6) return;
        d.moving = true;
        d.dx = Math.round(d.rel * win.w);
        d.dy = Math.min(TITLE_H / 2, d.y0 - geo.y);
      }
      wm.move(win.id, e.x - d.dx, e.y - d.dy);
    });
    const offUp = addListener('pointerup', () => { drag.current = null; });
    return () => { offMove(); offUp(); };
  }, [win.id, win.w, geo.y]);

  // resting on the maximize button opens the layout flyout under it (SnapTargets.jsx), as in other systems' snap layouts
  const flyTimer = useRef(0);
  const maxHover = (on, node) => {
    clearTimeout(flyTimer.current);
    if (!on || locked) return;
    flyTimer.current = setTimeout(() => {
      if (!node) return;
      try { const [x, y, w, h] = lord.getLayout(node.id); flyout.set({ id: win.id, x: x + w, y: y + h + 4 }); } catch (e) { /* gone */ }
    }, FLYOUT_DELAY);
  };
  useEffect(() => () => clearTimeout(flyTimer.current), []);
  const shadow = focused ? t.shadow : t.shadowSoft;
  const items = (extra) => [
    floating
      ? { label: tr('shell.ws.window.tile'), onClick: () => setFloating(win.id, false) }
      : { label: tr('shell.ws.window.float'), onClick: () => setFloating(win.id, true) },
    { label: tr('shell.ws.layout.move'), submenu: moveItems(win) },
    // the layout flyout from the window menu too: for Linux apps that draw their own maximize button (Chrome)
    ...(locked ? [] : [{ label: tr('shell.ws.snapfly.open'), onClick: () => { const m = menu || win.windowMenu; flyout.set({ id: win.id, x: m ? m.x : win.x + 40, y: m ? m.y : win.y + 40, at: 'point' }); } }]),
    '-',
    ...(extra || []),
    { label: tr('shell.classic.minimize'), onClick: () => wm.minimize(win.id) },
    win.maximized ? { label: tr('shell.classic.restore'), onClick: () => wm.toggleMaximize(win.id) } : { label: tr('shell.classic.maximize'), onClick: () => wm.toggleMaximize(win.id) },
    '-',
    { label: tr('shell.classic.close'), hint: 'Alt+F4', onClick: () => wm.close(win.id) },
  ];

  if (win.args?.csd || app.ownFrame) {
    const wmenu = win.windowMenu;
    return (
      <WindowShell win={win} leaving={leaving} resizable={!locked} onMouseDown={() => wm.focus(win.id)} style={{ ...(app.ownFrame ? { borderRadius: win.fullscreen ? 0 : 12, overflow: 'hidden' } : null), boxShadow: shadow }}>
        <AppHost id={win.id} focused={focused} />
        {app.ownFrame ? (
          <View style={{ position: 'absolute', left: 0, right: 0, top: 4, height: BTN, flexDirection: 'row', gap: 4, paddingHorizontal: 6, pointerEvents: 'box-none' }}>
            {atStart ? null : <View style={{ flex: 1, pointerEvents: 'none' }} />}
            {order.map((k) => button(k))}
            {atStart ? <View style={{ flex: 1, pointerEvents: 'none' }} /> : null}
          </View>
        ) : null}
        {wmenu && !locked && <Menu t={t} x={wmenu.x} y={wmenu.y} onClose={() => wm.closeWindowMenu(win.id)} items={items()} />}
      </WindowShell>
    );
  }

  function button(k) {
    if (k === 'close') return <FrameButton key={k} t={t} focused={focused} danger label={tr('shell.classic.close')} onClick={() => wm.close(win.id)}>{(c) => <CloseGlyph color={c} size={11} />}</FrameButton>;
    if (k === 'minimize') return <FrameButton key={k} t={t} focused={focused} label={tr('shell.classic.minimize')} onClick={() => wm.minimize(win.id)}>{(c) => <MinimizeGlyph color={c} />}</FrameButton>;
    return (
      <FrameButton key={k} t={t} focused={focused} label={tr(win.maximized ? 'shell.classic.restore' : 'shell.classic.maximize')} onClick={() => { flyout.set(null); wm.toggleMaximize(win.id); }} onHover={maxHover}>
        {(c) => (win.maximized ? <RestoreGlyph color={c} /> : <MaximizeGlyph color={c} />)}
      </FrameButton>
    );
  }

  const Icon = windowIcon(win);
  const buttons = locked
    ? (
      // locked: minimize stays (another app can take the slot), then the padlock where maximize and close were
      <View style={{ flexDirection: 'row', gap: 4 }}>
        {button('minimize')}
        <View style={{ width: BTN, height: BTN, alignItems: 'center', justifyContent: 'center', opacity: 0.55 }}><LockGlyph color={t.textDim} size={12} /></View>
      </View>
    )
    : <View style={{ flexDirection: 'row', gap: 4 }}>{order.map(button)}</View>;
  // a maximized window fills the group frame's work area, inside its margins: it keeps its rounded corners
  const flat = !!win.fullscreen;
  const border = focused ? t.windowBorder : t.separator;
  return (
    <WindowShell win={win} leaving={leaving} resizable={!locked} onMouseDown={() => wm.focus(win.id)} style={{
      borderRadius: flat ? 0 : 12, backgroundColor: app.transparent ? 'transparent' : t.windowBg, overflow: 'hidden',
      border: `1px solid ${app.transparent ? solidOver(border, t.titleBg) : border}`, boxShadow: shadow,
    }}>
      <View onMouseDown={(e) => {
        if (e.button || locked) return;
        if (!wm.beginMove(win.id)) drag.current = { x0: e.x, y0: e.y, rel: geo.w ? (e.x - geo.x) / geo.w : 0.5, moving: false };
      }}
        onDoubleClick={() => { if (!locked) wm.toggleMaximize(win.id); }} onContextMenu={(e) => { if (!locked) setMenu({ x: e.x, y: e.y }); }}
        style={{ height: TITLE_H, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 6, gap: 8, backgroundColor: focused ? t.titleBg : t.titleBgBlur }}>
        {atStart ? buttons : <View style={{ width: 4 }} />}
        {Icon ? <Icon size={16} /> : null}
        <Text style={{ flex: 1, fontSize: 12.5, fontWeight: focused ? 600 : 500, color: focused ? t.text : t.textDim, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{windowTitle(win)}</Text>
        {tight ? <View title={tr('shell.ws.layout.tight')} style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: '#E59A12' }} /> : null}
        {atStart ? <View style={{ width: 4 }} /> : buttons}
      </View>
      <AppMenuStrip t={t} win={win} focused={focused} />
      <View style={{ flex: 1 }}><AppHost id={win.id} focused={focused} /></View>
      {windowAccent(win) ? <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 3, backgroundColor: windowAccent(win), pointerEvents: 'none' }} /> : null}
      {menu && <Menu t={t} x={menu.x} y={menu.y} onClose={() => setMenu(null)} items={items()} />}
    </WindowShell>
  );
});
