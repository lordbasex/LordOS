// Celeste's bar, on the frame's thick side (the left, or the right in Arabic and Hebrew), one per display that
// gets a frame: the app launcher and Search, the desktops, the running apps (a click brings an app forward or back from
// minimized, a right click opens its menu), the camera/microphone/screen indicators and the apps' status items (as Lord
// Flow's menu bar and Workspaces' top bar show them), the quick status (Wi-Fi, Bluetooth, sound, battery), the
// notifications' bell, the clock and the session button. Every drawer opens with a click here; the hover edges are
// only a shortcut. The parts that change often subscribe on their own (the clock to one shared timer, the network to
// one 5 s poll), so the bar is not drawn again every second.
import {
  BatteryGlyph, BluetoothGlyph, PrivacyIndicators, ScrollView, SignalGlyph, Text, View, activeDesktop, defaultSink, desktopList,
  desktopName, dndActive, dockActivate, dockMenu, formatTime, sharedTicker, spotlight, statusItems, switchDesktop, t as tr,
  useBattery, useBluetooth, useDockEntries, useSound, useStore, wifiTile,
} from '@lordos/sdk';
import { memo, useState } from 'react';
import { BAR_W } from '../model.js';
import { Glyph } from './Glyph.jsx';
import { Menu } from '../../classic/components/Menu.jsx';

// one timer for every bar's clock (sdk/ticker.js), and one 5 s read of the network for every bar and the sidebar
const clockTicker = sharedTicker((d) => formatTime(d), 1000);
const readWifi = () => { try { return typeof lord.net !== 'undefined' ? wifiTile(lord.net.interfaces()) : null; } catch (e) { return null; } };
export const wifiTicker = sharedTicker(() => JSON.stringify(readWifi()), 5000);
export const useWifi = () => JSON.parse(useStore(wifiTicker));

function Btn({ cx, on, onClick, title, children, size = 36, style }) {
  const [hover, setHover] = useState(false);
  return (
    <View title={title} onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ width: size, height: size, borderRadius: size / 2, alignItems: 'center', justifyContent: 'center',
        backgroundColor: on ? cx.container : hover ? cx.surfaceHi : 'transparent', ...style }}>
      {children}
    </View>
  );
}

// hours over minutes; the clock's text from the region's format (formatTime), without AM/PM
const Clock = memo(function Clock({ cx, on, onClick }) {
  const text = useStore(clockTicker);
  const hm = (String(text).split(/\s+/).find((p) => p.includes(':')) || String(text)).split(':');
  return (
    <View title={tr('shell.celeste.bar.dashboard')} onClick={onClick}
      style={{ width: 40, paddingVertical: 8, borderRadius: 16, alignItems: 'center', backgroundColor: on ? cx.container : cx.surface }}>
      <Text style={{ fontSize: 15, fontWeight: 700, color: on ? cx.onContainer : cx.text }}>{hm[0]}</Text>
      <Text style={{ fontSize: 15, fontWeight: 700, color: cx.primary }}>{hm[1] || ''}</Text>
    </View>
  );
});

const Desktops = memo(function Desktops({ cx }) {
  const list = useStore(desktopList);
  const cur = useStore(activeDesktop);
  if (list.length < 2) return null;
  return (
    <View style={{ alignItems: 'center', gap: 6, paddingVertical: 8, paddingHorizontal: 4, borderRadius: 18, backgroundColor: cx.surface }}>
      {list.slice(0, 9).map((id, i) => {
        const on = id === cur;
        return (
          <View key={id} title={desktopName(id)} onClick={() => switchDesktop(id)}
            style={{ width: 28, height: on ? 34 : 28, borderRadius: 14, alignItems: 'center', justifyContent: 'center', backgroundColor: on ? cx.primary : cx.surfaceHi }}>
            <Text style={{ fontSize: 12, fontWeight: 700, color: on ? cx.onPrimary : cx.sub }}>{String(i + 1)}</Text>
          </View>
        );
      })}
    </View>
  );
});

// The running apps (the Dock model's entries with windows, sdk/dock.js): the focused one in the scheme's container
// colour, an app whose windows are all minimized faded with an open ring under it.
const Running = memo(function Running({ cx, t }) {
  const entries = useDockEntries([], { recents: 0 }).filter((e) => e.running);
  const [ctx, setCtx] = useState(null);
  if (!entries.length) return <View style={{ flex: 1 }} />;
  return (
    <View style={{ flex: 1, minHeight: 0, width: BAR_W, alignItems: 'center' }}>
      <ScrollView style={{ flex: 1, width: BAR_W }} contentStyle={{ alignItems: 'center', gap: 4, paddingVertical: 4 }}>
        {entries.map((e) => {
          const hidden = e.windows.every((w) => w.minimized);
          const Icon = e.Icon;
          return (
            <View key={e.id} title={e.name} onClick={() => dockActivate(e)} onContextMenu={(ev) => setCtx({ e, x: ev.x, y: ev.y })}
              style={{ width: 40, height: 40, borderRadius: 14, alignItems: 'center', justifyContent: 'center', backgroundColor: e.focused ? cx.container : 'transparent' }}>
              <View style={{ opacity: hidden ? 0.45 : 1 }}><Icon size={26} /></View>
              <View style={{ position: 'absolute', bottom: 1, left: hidden ? 17 : 18, width: hidden ? 6 : 4, height: hidden ? 6 : 4, borderRadius: 3,
                backgroundColor: hidden ? 'transparent' : e.attention ? cx.danger : cx.primary, border: hidden ? `1.5px solid ${cx.dim}` : undefined }} />
            </View>
          );
        })}
      </ScrollView>
      {ctx ? <Menu t={t} x={ctx.x} y={ctx.y} onClose={() => setCtx(null)} items={dockMenu(ctx.e)} /> : null}
    </View>
  );
});

// The apps' status items (sdk/statusitems.js), one under the other: the bar is narrow
const Status = memo(function Status({ cx }) {
  const list = useStore(statusItems);
  return (
    <View style={{ alignItems: 'center', gap: 2 }}>
      <PrivacyIndicators size={8} />
      {list.map(({ id, Component }) => <View key={id} style={{ height: 26, justifyContent: 'center' }}><Component color={cx.sub} height={26} /></View>)}
    </View>
  );
});

// Wi-Fi, Bluetooth, sound and battery at a glance; a click opens the sidebar
const Quick = memo(function Quick({ cx, on, onClick }) {
  const wifi = useWifi();
  const bt = useBluetooth();
  const bat = useBattery();
  const out = defaultSink(useSound());
  const muted = !out || out.mute || (out.level || 0) <= 0.001;
  const btOn = !!bt?.adapter?.powered;
  const pct = bat?.hasBattery && bat.percent >= 0 ? Math.round(bat.percent) : null;
  return (
    <View title={tr('shell.celeste.bar.sidebar')} onClick={onClick}
      style={{ alignItems: 'center', gap: 10, paddingVertical: 10, paddingHorizontal: 6, borderRadius: 18, backgroundColor: on ? cx.container : cx.surface }}>
      {wifi ? <SignalGlyph size={16} color={wifi.connected ? cx.primary : cx.dim} level={wifi.bars} off={!wifi.connected} /> : null}
      {bt?.adapter ? <BluetoothGlyph size={16} color={btOn ? cx.primary : cx.dim} /> : null}
      <Glyph name={muted ? 'volume-muted' : 'volume'} size={17} color={muted ? cx.dim : cx.primary} />
      {pct !== null ? (
        <View style={{ alignItems: 'center', gap: 2 }}>
          <BatteryGlyph percent={pct} charging={!!bat.charging} color={cx.text} width={20} />
          <Text style={{ fontSize: 10, fontWeight: 700, color: cx.text }}>{String(pct)}</Text>
        </View>
      ) : null}
    </View>
  );
});

// area: the bar's rectangle (model.js frameGeom().bar); display: its display; active: the drawer open on this display
export function Bar({ cx, t, area, display, active, onToggle, unread }) {
  return (
    <View style={{ position: 'absolute', left: area.x, top: area.y, width: area.w, height: area.h, alignItems: 'center', paddingVertical: 6, gap: 8 }}>
      <Btn cx={cx} on={active === 'launcher'} title={tr('shell.celeste.bar.launcher')} onClick={() => onToggle('launcher')}>
        <Glyph name="grid" size={20} color={cx.primary} />
      </Btn>
      <Btn cx={cx} title={tr('shell.celeste.bar.search')} onClick={() => spotlight.toggle(display)}>
        <Glyph name="search" size={18} color={cx.sub} />
      </Btn>
      <Desktops cx={cx} />
      <Running cx={cx} t={t} />
      <Status cx={cx} />
      <Quick cx={cx} on={active === 'sidebar'} onClick={() => onToggle('sidebar')} />
      <Btn cx={cx} on={active === 'sidebar'} size={34} title={tr('shell.celeste.side.notifications')} onClick={() => onToggle('sidebar')}>
        <Glyph name={dndActive() ? 'dnd' : 'bell'} size={17} color={unread ? cx.primary : cx.sub} />
        {unread ? <View style={{ position: 'absolute', right: 6, top: 6, width: 7, height: 7, borderRadius: 4, backgroundColor: cx.danger }} /> : null}
      </Btn>
      <Clock cx={cx} on={active === 'dash'} onClick={() => onToggle('dash')} />
      <Btn cx={cx} on={active === 'session'} title={tr('shell.celeste.bar.session')} onClick={() => onToggle('session')}>
        <Glyph name="power" size={18} color={cx.danger} />
      </Btn>
    </View>
  );
}
