// Lord Bluebell's taskbar, at the bottom of each display: a green start button with the bluebell mark, the pinned apps
// as small quick-launch icons, a button for every window (its icon and title; the one in front looks pressed), and the
// light-blue tray at the end with the status items and the clock. Pinning and the pinned apps' menus are the SDK's
// (sdk/dock.js), the same as in the other themes.
import { View, Text, useWindows, wm, windowTitle, windowIcon, formatTime, t as tr, sharedTicker, useStore, useDockEntries, dockActivate, dockMenu,
  StatusItems, PrivacyIndicators, mainDisplay } from '@lordos/sdk';
import { useState } from 'react';
import { Menu } from '../../classic/components/Menu.jsx';
import { StartMark } from '../glyphs.jsx';

export const BAR_H = 36;
const clock = sharedTicker(() => formatTime(new Date()), 15000);
export const QUICK = ['files', 'terminal', 'settings'];

function Clock({ font }) {
  const now = useStore(clock);
  return <Text style={{ fontSize: 12, fontFamily: font, color: '#ffffff', whiteSpace: 'nowrap' }}>{now}</Text>;
}

function TaskButton({ t, w, front, font, onMenu }) {
  const [hover, setHover] = useState(false);
  const Icon = windowIcon(w);
  return (
    <View onClick={() => (front && !w.minimized ? wm.minimize(w.id) : wm.focus(w.id))} onContextMenu={onMenu}
      onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flexShrink: 1, minWidth: 60, width: 170, height: BAR_H - 8, borderRadius: 3, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 7,
        background: front && !w.minimized ? t.taskBtnActive : t.taskBtn, border: `1px solid ${front && !w.minimized ? 'rgba(0,0,0,0.35)' : 'rgba(255,255,255,0.25)'}`, opacity: hover ? 0.9 : 1 }}>
      {Icon ? <Icon size={16} /> : null}
      <Text style={{ flex: 1, fontSize: 12, fontFamily: font, color: '#ffffff', whiteSpace: 'nowrap', textOverflow: 'ellipsis', overflow: 'hidden', textAlign: 'start' }}>{windowTitle(w)}</Text>
    </View>
  );
}

export function Taskbar({ t, area, start, onStart, onTray, font }) {
  const entries = useDockEntries(QUICK);
  const { windows, focused } = useWindows();
  const [menu, setMenu] = useState(null);
  const m = area || mainDisplay();
  const tasks = windows.filter((w) => !w.pip && !w.fullscreen);
  const quick = entries.filter((e) => e.kind === 'app' && e.pinned).slice(0, 5);
  return (
    <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: BAR_H, flexDirection: 'row', alignItems: 'center', background: t.barBg,
      borderTopWidth: 1, borderTopColor: 'rgba(255,255,255,0.35)' }}>
      {/* start: green, rounded on its trailing end, the bluebell and the word */}
      <View onClick={onStart} style={{ height: BAR_H, flexDirection: 'row', alignItems: 'center', gap: 6, paddingStart: 10, paddingEnd: 18,
        borderTopEndRadius: 14, borderBottomEndRadius: 14, background: t.startBg, opacity: start ? 0.85 : 1,
        boxShadow: '2px 0 4px rgba(0,0,0,0.35)' }}>
        <View style={{ width: 22, height: 22, borderRadius: 11, backgroundColor: 'rgba(255,255,255,0.92)', alignItems: 'center', justifyContent: 'center' }}><StartMark size={16} /></View>
        <Text style={{ fontSize: 16, fontWeight: 700, fontFamily: font, color: '#ffffff' }}>{tr('shell.bluebell.start')}</Text>
      </View>
      {/* quick launch */}
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 8, borderEndWidth: 1, borderEndColor: 'rgba(0,0,0,0.25)' }}>
        {quick.map((e) => (
          <View key={e.id} onClick={() => dockActivate(e)} onContextMenu={(ev) => setMenu({ x: ev.x, y: m.y + m.h - BAR_H - 4, items: dockMenu(e), above: true })}
            style={{ width: 24, height: 24, alignItems: 'center', justifyContent: 'center' }}>
            {e.Icon ? <e.Icon size={20} /> : null}
          </View>
        ))}
      </View>
      {/* the windows */}
      <View style={{ flex: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', gap: 3, paddingHorizontal: 6, overflow: 'hidden' }}>
        {tasks.map((w) => (
          <TaskButton key={w.id} t={t} w={w} font={font} front={w.id === focused}
            onMenu={(ev) => setMenu({ x: ev.x, y: m.y + m.h - BAR_H - 4, above: true, items: [
              { label: tr('shell.classic.restore'), disabled: !w.maximized && !w.minimized, onClick: () => (w.minimized ? wm.focus(w.id) : wm.toggleMaximize(w.id)) },
              { label: tr('shell.classic.minimize'), disabled: w.minimized, onClick: () => wm.minimize(w.id) },
              { label: tr('shell.classic.maximize'), disabled: w.maximized, onClick: () => wm.snap(w.id, 'max') }, '-',
              { label: tr('shell.classic.close'), hint: 'Alt+F4', onClick: () => wm.close(w.id) },
            ] })} />
        ))}
      </View>
      {/* the tray */}
      <View onClick={onTray} style={{ height: BAR_H, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 12, background: t.trayBg,
        borderStartWidth: 1, borderStartColor: 'rgba(0,0,0,0.35)' }}>
        <PrivacyIndicators />
        <StatusItems color="#ffffff" height={20} />
        <Clock font={font} />
      </View>
      {menu && <Menu t={t} x={menu.x} y={menu.y} above={menu.above} items={menu.items} onClose={() => setMenu(null)} />}
    </View>
  );
}
