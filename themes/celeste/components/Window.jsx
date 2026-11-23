// Lord Celeste's window frame: a 38 px title bar with round lights (close, minimize, maximize, in the order and on the
// side Settings → Desktop & Dock → Title bars asks, useWindowButtons; its Size makes the bar, the lights, their glyphs and
// their hit areas larger, px, GitHub #151 and #219, and index.jsx's chrome follows it) and the title in the middle. Lord Celeste has no
// global menu bar, so an app that declares menus (useAppMenus, via useMenusWithSpeech) gets them in a strip under the
// title bar, with Window and Help, as Lord Classic and Lord Workspaces do; F10, or ⌥ pressed and released alone, opens
// the focused window's first menu (addMenuBarKeys). A right click on the title opens the app menu (About, Settings…,
// Hide, Quit: appMenuItems) and the Window menu (windowMenuItems). Moving and resizing are lord-server's
// (<WindowShell>, wm.beginMove); a maximized window is dragged out of its place by JS and snaps where it is dropped.
// A Linux app that draws its own bar (args.csd, decided on every render) gets only its content, the shadow and the
// window menu where it asks for one (w.windowMenu).
import {
  AppHost, Text, View, WindowShell, SeamCover, addListener, addMenuBarKeys, appGroup, appMenuItems, barIndex, getApp,
  snapZoneAt, solidOver, t as tr, useMenusWithSpeech, useWindowButtons, webHelpItems, windowAccent, windowMenuItems,
  windowTitle, wm,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { Menu } from '../../classic/components/Menu.jsx';

export const TITLE_H = 38;          // at the normal size: px(TITLE_H) is the bar now (useWindowButtons)
export const MENU_H = 26;
const DRAG_START = 6;
const LIGHT = 13;

const LIGHTS = {
  close: { color: '#f2777a', bars: [[8, 45], [8, -45]], act: (id) => wm.close(id), label: 'shell.window.close' },
  minimize: { color: '#f0c45a', bars: [[7, 0]], act: (id) => wm.minimize(id), label: 'shell.window.minimize' },
  maximize: { color: '#7bd88f', bars: [[7, 0], [7, 90]], act: (id) => wm.toggleMaximize(id), label: 'shell.window.maximize' },
};

// ×, − and + as bars centred on the light (text glyphs sit off-centre by their font metrics)
function LightGlyph({ l, color, px }) {
  const w = px(1.5), size = px(LIGHT);
  return l.bars.map(([n, deg], i) => {
    const len = px(n);
    return (
      <View key={i} style={{ position: 'absolute', left: (size - len) / 2, top: (size - w) / 2, width: len, height: w, borderRadius: w / 2,
        backgroundColor: color, pointerEvents: 'none', transform: deg ? `rotate(${deg}deg)` : undefined }} />
    );
  });
}

// px: the title bar's size (Settings → Appearance → Window options → Window buttons): the lights and their hit areas grow with it
function Lights({ win, t, focused, order, px }) {
  const [hover, setHover] = useState(false);
  const size = px(LIGHT);
  return (
    <View onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flexDirection: 'row', gap: px(8), paddingVertical: px(6), direction: 'ltr' }}>
      {order.map((key) => {
        const l = LIGHTS[key];
        return (
          <View key={key} title={tr(l.label)} onMouseDown={(e) => e.stopPropagation()} onClick={() => l.act(win.id)}
            style={{ width: size, height: size, borderRadius: size / 2, backgroundColor: focused || hover ? l.color : t.lightOff }}>
            {hover ? <LightGlyph l={l} px={px} color={solidOver('rgba(0,0,0,0.55)', l.color)} /> : null}
          </View>
        );
      })}
    </View>
  );
}

// The app's menus, then Window and Help; one open at a time, under its title; ← and → move between them.
function MenuStrip({ t, win, focused, menus, transparent }) {
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
  const bg = focused ? t.titleBg : t.titleBgBlur;
  return (
    <View onMouseDown={(e) => e.stopPropagation()}
      style={{ height: MENU_H, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 8, gap: 2, backgroundColor: bg, borderBottom: `1px solid ${t.separator}` }}>
      {transparent ? <SeamCover color={bg} below={2} above={0} /> : null}
      {all.map((m, i) => (
        <View key={i} ref={(inst) => { refs.current[i] = inst; }}
          onClick={() => setOpen((o) => (o?.i === i ? null : at(i)))}
          onMouseEnter={() => { if (open && open.i !== i) setOpen(at(i)); }}
          style={{ height: 22, paddingHorizontal: 8, borderRadius: 11, justifyContent: 'center', backgroundColor: open?.i === i ? t.hover : 'transparent' }}>
          <Text style={{ fontSize: 12, color: focused ? t.text : t.textDim, whiteSpace: 'nowrap' }}>{m.label}</Text>
        </View>
      ))}
      {open && all[open.i] ? <Menu key={open.i} t={t} x={open.x} y={open.y} items={all[open.i].items} onClose={() => setOpen(null)} width={250} keys={!!open.keys} onStep={step} /> : null}
    </View>
  );
}

// subscribes on its own: an app that sets its menus after every draw must not redraw the whole window
function AppMenuStrip({ t, win, focused, transparent }) {
  const menus = useMenusWithSpeech(win.id) || [];
  return menus.length ? <MenuStrip t={t} win={win} focused={focused} menus={menus} transparent={transparent} /> : null;
}

// the right click on the title: the app's menu and the Window menu
const titleItems = (win) => [...appMenuItems(win.id), '-', ...windowMenuItems(win.id)];

// fx: the minimize effect ({ p: 0..1, x, y }): the frame shrinks towards the bar
function fxStyle(fx, geo) {
  if (!fx) return null;
  const e = fx.p * fx.p;
  const dx = (fx.x - (geo.x + geo.w / 2)) * e, dy = (fx.y - (geo.y + geo.h / 2)) * e;
  return { transform: `translate(${dx}px, ${dy}px) scale(${1 - 0.9 * e})`, opacity: 1 - 0.7 * e, pointerEvents: 'none' };
}

export function Window({ win, app, t, focused, fx }) {
  const drag = useRef(null);
  const [menu, setMenu] = useState(null);
  const { atStart, order, px } = useWindowButtons(true);
  const bar = px(TITLE_H);
  const geo = wm.frame(win);
  const fxs = fxStyle(fx, geo);
  const radius = win.maximized || win.snapped ? 0 : 16;

  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      const d = drag.current;
      if (!d) return;
      if (!d.moving) {
        if (Math.abs(e.x - d.x0) + Math.abs(e.y - d.y0) < DRAG_START) return;
        d.moving = true;
        d.dx = Math.round(d.rel * win.w);
        d.dy = Math.min(bar / 2, d.y0 - geo.y);
      }
      wm.move(win.id, e.x - d.dx, e.y - d.dy);
    });
    const offUp = addListener('pointerup', (e) => {
      const d = drag.current;
      drag.current = null;
      if (d && d.moving) { const z = snapZoneAt(e.x, e.y); if (z) wm.snap(win.id, z); }
    });
    return () => { offMove(); offUp(); };
  }, [win.id, win.w, geo.y, bar]);

  if (win.args?.csd || app.ownFrame) {
    const wmenu = win.windowMenu;
    return (
      <WindowShell win={win} leaving={!!fx} onMouseDown={() => wm.focus(win.id)}
        style={{ ...(app.ownFrame ? { borderRadius: radius, overflow: 'hidden' } : null), boxShadow: focused ? t.shadow : t.shadowDim, ...fxs }}>
        <AppHost id={win.id} focused={focused} />
        {app.ownFrame && !atStart ? (
          <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: bar, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 14, pointerEvents: 'box-none' }}>
            <View style={{ flex: 1, pointerEvents: 'none' }} />
            <Lights win={win} t={t} focused={focused} order={order} px={px} />
          </View>
        ) : null}
        {wmenu ? <Menu t={t} x={wmenu.x} y={wmenu.y} onClose={() => wm.closeWindowMenu(win.id)} items={titleItems(win)} /> : null}
      </WindowShell>
    );
  }

  const lineColor = focused ? t.windowBorder : t.separator;
  return (
    <WindowShell win={win} leaving={!!fx} onMouseDown={() => wm.focus(win.id)} style={{
      borderRadius: radius, backgroundColor: app.transparent ? 'transparent' : t.windowBg, overflow: 'hidden',
      // a transparent app (the Terminal) has nothing behind its border: the border is drawn opaque
      border: `1px solid ${app.transparent ? solidOver(lineColor, t.titleBg) : lineColor}`,
      boxShadow: focused ? t.shadow : t.shadowDim,
      ...fxs,
    }}>
      <View onMouseDown={(e) => { if (e.button) return; if (!wm.beginMove(win.id)) drag.current = { x0: e.x, y0: e.y, rel: geo.w ? (e.x - geo.x) / geo.w : 0.5, moving: false }; }}
        onDoubleClick={() => wm.toggleMaximize(win.id)} onContextMenu={(e) => setMenu({ x: e.x, y: e.y })}
        style={{ height: bar, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 14, backgroundColor: focused ? t.titleBg : t.titleBgBlur }}>
        {app.transparent ? <SeamCover color={focused ? t.titleBg : t.titleBgBlur} below={2} /> : null}
        <Text style={{ position: 'absolute', left: 0, right: 0, top: 0, lineHeight: bar, textAlign: 'center', fontSize: 12, fontWeight: 600,
          color: focused ? t.text : t.textDim, pointerEvents: 'none', whiteSpace: 'nowrap', textOverflow: 'ellipsis', paddingHorizontal: px(84) }}>{windowTitle(win)}</Text>
        {atStart ? null : <View style={{ flex: 1 }} />}
        <Lights win={win} t={t} focused={focused} order={order} px={px} />
      </View>
      <View style={{ height: 1, backgroundColor: app.transparent ? solidOver(t.separator, t.titleBg) : t.separator }} />
      <AppMenuStrip t={t} win={win} focused={focused} transparent={!!app.transparent} />
      <View style={{ flex: 1 }}><AppHost id={win.id} focused={focused} /></View>
      {windowAccent(win) ? <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 3, backgroundColor: windowAccent(win), pointerEvents: 'none' }} /> : null}
      {menu ? <Menu t={t} x={menu.x} y={menu.y} onClose={() => setMenu(null)} items={titleItems(win)} /> : null}
    </WindowShell>
  );
}
