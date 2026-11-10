// Top menu bar: L menu, active app, Window menu, keyboard layout,
// Control Center, the notifications' bell and the clock (a click opens notifications and the calendar).
// The keyboard layout and the bell can be turned off in Settings → Desktop & Dock → Menu Bar (issue #58).
// <MenuBars> draws one full bar per display (0.8.8.6; Settings → Desktop & Dock → Menu Bar → "Show the menu bar on
// every display", on by default): the app's menus are lit on the focused window's display and dimmed on the others,
// where a click lights that bar and opens the menu there. sdk/menubarmodel.js decides which bar is lit.
// From the keyboard: F10, or ⌥ pressed and released alone, opens the lit bar's first menu (sdk/shortcuts.js
// addMenuBarKeys); ← and → then move between its menus (sdk/menunav.js barIndex), and the same key closes it.
import { View, Text, useStore, sharedTicker, barDisplays, activeBarId, pinBar, wm, getApp, session, useMenusWithSpeech, t as tr, appName, formatDate, formatTime, keyboard, useSettings, direction, screen, windowAppName, LordOSGlyph, spotlight, PrivacyIndicators, StatusItems, help, menuClock, appMenuItems, recentItems, storeUpdates, windowMenuItems, menuBarPrefs, addMenuBarKeys, barIndex } from '@lordos/sdk';
import { useState, useRef, useEffect } from 'react';
import { Menu } from './Menu.jsx';

// Date and time with the region's format (sdk/i18n.js),
// with the format chosen in Settings → Date & Time (sdk/hardware.jsx)
const format = (d) => menuClock(d);
// one timer for every bar's clock: N displays, one setInterval (sdk/ticker.js)
const clockTicker = sharedTicker(format, 1000);
// the [x, width] of a bar item (global), for the panel it opens to sit under it
const itemAt = (ref) => { try { const [x, , w] = lord.getLayout(ref.current.id); return [x, w]; } catch (e) { return null; } };

function BarItem({ t, children, onClick, active, anchorRef }) {
  const [hover, setHover] = useState(false);
  return (
    <View ref={anchorRef} onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ height: 22, paddingHorizontal: 9, borderRadius: 5, justifyContent: 'center', flexDirection: 'row', alignItems: 'center',
        backgroundColor: active || hover ? t.menuHover : 'transparent' }}>
      {children}
    </View>
  );
}

// dim: the app's menus on a display without the focused window
const Label = ({ t, children, bold, dim }) => (
  <Text style={{ fontSize: 13, fontWeight: bold ? 700 : 500, color: dim ? t.menuTextDim : t.menuText, whiteSpace: 'nowrap' }}>{children}</Text>
);

function ControlGlyph({ t }) {
  const bar = (top, knob) => (
    <View style={{ position: 'absolute', left: 0, top, width: 16, height: 6, borderRadius: 3, border: `1.5px solid ${t.menuText}` }}>
      <View style={{ position: 'absolute', left: knob, top: 0.5, width: 2, height: 2, borderRadius: 1, backgroundColor: t.menuText }} />
    </View>
  );
  return <View style={{ width: 16, height: 14 }}>{bar(0, 9)}{bar(8, 2)}</View>;
}

// A bell drawn with views (it doesn't depend on a font), with the accent's dot when there is something unread.
function BellGlyph({ color, dot }) {
  return (
    <View style={{ width: 14, height: 14, alignItems: 'center' }}>
      <View style={{ marginTop: 1, width: 10, height: 9, borderTopLeftRadius: 5, borderTopRightRadius: 5, border: `1.5px solid ${color}`, borderBottomWidth: 0 }} />
      <View style={{ width: 13, height: 1.5, borderRadius: 1, backgroundColor: color }} />
      <View style={{ marginTop: 1, width: 4, height: 2, borderBottomLeftRadius: 2, borderBottomRightRadius: 2, backgroundColor: color }} />
      {dot ? <View style={{ position: 'absolute', right: -2, top: -1, width: 6, height: 6, borderRadius: 3, backgroundColor: dot }} /> : null}
    </View>
  );
}

// area: the display it is on (global coordinates; the main one, or each one with "menu bar on every display")
// clockPanel / controlCenter: that panel is open from this bar; unread: a notification came since it was last opened
// onClock(at) / onControlCenter(at): at is the clicked item's [x, width], for the panel to open under it
// active: the app's menus are lit here (false: dimmed, and a click first calls onActivate to light this bar)
export function MenuBar({ t, height, focused, onLaunchpad, onControlCenter, controlCenter, onClock, clockPanel, unread, area = { x: 0, y: 0, w: screen.width, h: screen.height }, active = true, onActivate }) {
  const clock = useStore(clockTicker);
  const [s] = useSettings();         // the active keyboard layout lives in the settings
  const bar = menuBarPrefs(s);
  const [open, setOpen] = useState(null);
  const refs = { logo: useRef(null), app: useRef(null), window: useRef(null), help: useRef(null), cc: useRef(null), clock: useRef(null), bell: useRef(null) };
  const appRefs = useRef({});
  const app = focused ? getApp(focused) : null;
  const extra = useMenusWithSpeech(focused) || [];
  // Menu position: below the item that opened it (from its right edge in Arabic/Hebrew).
  const anchorX = (ref) => {
    if (!ref?.current) return direction() === 'rtl' ? area.x + area.w - 6 : area.x + 6;
    const [x, , w] = lord.getLayout(ref.current.id);
    return direction() === 'rtl' ? x + w : x;
  };
  const toggle = (name, ref) => {
    if (!active && onActivate) onActivate();
    setOpen((o) => (o?.name === name ? null : { name, x: anchorX(ref) }));
  };
  const dim = !active;
  const close = () => setOpen(null);
  // the menus in the bar's order, for ← and → (keys: opened from the keyboard, its first row highlighted)
  const order = ['logo', 'app', ...extra.map((m, i) => `app${i}`), 'window', 'help'];
  const refOf = (name) => refs[name] || { current: appRefs.current[name] };
  const step = (dir) => setOpen((o) => {
    if (!o) return o;
    const name = order[barIndex(order.length, order.indexOf(o.name), dir)];
    return { name, x: anchorX(refOf(name)), keys: true };
  });
  const live = useRef(active);
  live.current = active;
  useEffect(() => addMenuBarKeys({
    can: () => live.current,
    run: () => setOpen((o) => (o ? null : { name: 'logo', x: anchorX(refs.logo), keys: true })),
  }), []);

  const menus = {
    logo: { items: [
      { label: tr('shell.menu.about'), onClick: () => wm.open('about') },
      '-',
      { label: tr('shell.menu.settings'), onClick: () => wm.open('settings') },
      ...(getApp('store') ? [(() => { const n = storeUpdates(); return { label: tr('shell.menu.store'), hint: n ? tr('shell.menu.storeUpdates', { count: n }) : undefined,
        onClick: () => wm.open('store', n ? { args: { page: 'updates' } } : undefined) }; })()] : []),
      { label: tr('shell.menu.apps'), hint: 'F4', onClick: onLaunchpad },
      '-',
      { label: tr('shell.menu.recent'), submenu: recentItems(10) },
      '-',
      ...(getApp('forcequit') ? [{ label: tr('shell.menu.forceQuit'), hint: '❖⌥Esc', onClick: () => wm.open('forcequit') }, '-'] : []),
      ...(session.managed() ? [
        { label: tr('shell.menu.lock'), hint: '❖L', onClick: () => session.lock() },
        { label: tr('shell.menu.switchUser'), onClick: () => session.switchUser() },
      ] : []),
      { label: tr('shell.menu.logout'), onClick: () => session.quit() },
      ...(session.canPower() ? [
        '-',
        { label: tr('shell.menu.restart'), onClick: () => session.restart() },
        { label: tr('shell.menu.shutdown'), onClick: () => session.shutdown() },
      ] : []),
    ] },
    // the app menu under the app's name (About, Settings…, Hide, Quit; sdk/appcontrol.js)
    app: { items: app ? appMenuItems(focused) : [{ label: tr('shell.menu.about'), onClick: () => wm.open('about') }] },
    // Minimize, Zoom, Fill, Center, Move & Resize, Move to <display>, the open windows (sdk/windowmenu.js)
    window: { items: windowMenuItems(app ? focused : null) },
    help: { items: [
      ...(getApp('help') ? [{ label: tr('shell.help.open'), onClick: () => wm.open('help') }, { label: tr('shell.help.tour'), onClick: () => help.startTour() }, '-'] : []),
      { label: tr('shell.menu.about'), onClick: () => wm.open('about') },
      { label: tr('shell.help.theme'), disabled: true },
    ] },
  };
  extra.forEach((m, i) => { menus[`app${i}`] = { items: m.items }; });

  const cur = open && menus[open.name];
  const anchor = open && refOf(open.name);
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, width: 0, height: 0 }}>
      <View style={{
        position: 'absolute', left: area.x, top: area.y, width: area.w, height, flexDirection: 'row', alignItems: 'center',
        paddingHorizontal: 8, backgroundColor: t.menuBg, backdropFilter: `blur(${t.blur}px)`,
      }}>
        <BarItem t={t} active={open?.name === 'logo'} anchorRef={refs.logo} onClick={() => toggle('logo', refs.logo)}>
          <LordOSGlyph size={16} color={t.menuText} />
        </BarItem>
        <BarItem t={t} active={open?.name === 'app'} anchorRef={refs.app} onClick={() => toggle('app', refs.app)}><Label t={t} bold dim={dim}>{app ? (app.builtin ? windowAppName(wm.get(focused)) : appName(app)) : 'LordOS'}</Label></BarItem>
        {extra.map((m, i) => {
          const key = `app${i}`;
          const ref = { get current() { return appRefs.current[key]; } };
          return (
            <BarItem key={key} t={t} active={open?.name === key} anchorRef={(inst) => { appRefs.current[key] = inst; }}
              onClick={() => toggle(key, ref)}><Label t={t} dim={dim}>{m.label}</Label></BarItem>
          );
        })}
        <BarItem t={t} active={open?.name === 'window'} anchorRef={refs.window} onClick={() => toggle('window', refs.window)}><Label t={t} dim={dim}>{tr('shell.menu.window')}</Label></BarItem>
        <BarItem t={t} active={open?.name === 'help'} anchorRef={refs.help} onClick={() => toggle('help', refs.help)}><Label t={t} dim={dim}>{tr('shell.menu.help')}</Label></BarItem>
        <View style={{ flex: 1 }} />
        {bar.keyboard ? (
          <BarItem t={t} onClick={() => keyboard.next()}>
            <Label t={t}>{keyboard.label(keyboard.active())}</Label>
          </BarItem>
        ) : null}
        <StatusItems color={t.menuText} />
        <PrivacyIndicators />
        <BarItem t={t} onClick={() => spotlight.toggle(area)}><Label t={t}>⌕</Label></BarItem>
        <BarItem t={t} active={controlCenter} anchorRef={refs.cc} onClick={() => onControlCenter(itemAt(refs.cc))}><ControlGlyph t={t} /></BarItem>
        {bar.bell ? <BarItem t={t} active={clockPanel} anchorRef={refs.bell} onClick={() => onClock(itemAt(refs.bell))}><BellGlyph color={t.menuText} dot={unread ? t.accent : null} /></BarItem> : null}
        <BarItem t={t} active={clockPanel} anchorRef={refs.clock} onClick={() => onClock(itemAt(refs.clock))}><Label t={t}>{clock}</Label></BarItem>
      </View>
      {cur && <Menu key={open.name} x={open.x} y={area.y + height + 2} items={cur.items} onClose={close} anchor={anchor} width={260} keys={!!open.keys} onStep={step} />}
    </View>
  );
}

// One bar per display (only the main display's when the setting is off or the displays are mirrored). displays:
// screenDisplays(); every: menuBarEveryDisplay(settings); mirror: screen.mirror. control / clock: the display id
// whose Control Center / clock panel is open. onControlCenter(display, at) and onClock(display, at) open them there.
// Adding or removing a monitor re-renders the shell (startShell's screen revision): its bar comes and goes with it.
export function MenuBars({ t, height, displays, every, mirror, focused, focusRect, control, clock, unread, onClock, onControlCenter, onLaunchpad }) {
  const [pin, setPin] = useState(null);       // the dimmed bar the user clicked (sdk/menubarmodel.js pinBar)
  const bars = barDisplays(displays, every, mirror);
  const focus = focused ? { id: focused, rect: focusRect || null } : null;
  const lit = activeBarId(displays, bars, focus, pin);
  return bars.map((d) => (
    <MenuBar key={d.id + ':' + d.x} t={t} height={height} focused={focused} area={d} active={d.id === lit}
      onActivate={() => setPin(pinBar(displays, focus, d.id))}
      controlCenter={control === d.id} clockPanel={clock === d.id} unread={unread}
      onClock={(at) => onClock(d, at)} onControlCenter={(at) => onControlCenter(d, at)} onLaunchpad={onLaunchpad} />
  ));
}
