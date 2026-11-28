// Luna's start menu, in the two-column manner of 2001: a blue header with the person's picture and name, a white
// column of pinned and frequent apps with "All apps ▸" at its foot, a light-blue column of places (Documents, Pictures,
// Music, the computer, the network, Settings, Help, Search) and a blue footer with Log Out and Shut Down, which are
// LordOS's own session actions (session.quit and session.shutdown, the latter asking first). All drawn by us.
import { View, Text, ScrollView, useSettings, useLauncherApps, launchApp, appName, session, spotlight, wm, useSystemInfo, direction, t as tr } from '@lordos/sdk';
import { useState } from 'react';
import { startPins } from '../../classic/components/StartMenu.jsx';
import { Folder } from '../glyphs.jsx';

export const START_W = 420, START_H = 500;
const fontOf = (font) => (font ? { fontFamily: font } : null);

function Row({ t, font, icon, label, bold, onClick, light, size = 24 }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ minHeight: size + 8, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 8, backgroundColor: hover ? t.selectBg : 'transparent' }}>
      <View style={{ width: size, height: size, alignItems: 'center', justifyContent: 'center' }}>{icon}</View>
      <Text style={{ flex: 1, fontSize: 12, fontWeight: bold ? 700 : 400, color: hover ? '#ffffff' : light ? t.startPlaceText : t.text,
        whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start', ...fontOf(font) }}>{label}</Text>
    </View>
  );
}

// a small drawn mark for a place that isn't a folder: the computer, the network, help, search
function PlaceMark({ kind }) {
  if (kind === 'computer') {
    return (
      <View style={{ width: 22, height: 22, alignItems: 'center' }}>
        <View style={{ width: 20, height: 14, borderRadius: 2, background: 'linear-gradient(180deg, #8ec5ff, #2f63d8)', border: '1px solid #1d3f9a' }} />
        <View style={{ width: 10, height: 3, marginTop: 1, backgroundColor: '#7f8a9a' }} />
      </View>
    );
  }
  if (kind === 'network') {
    return (
      <View style={{ width: 22, height: 22 }}>
        <View style={{ position: 'absolute', left: 2, top: 2, width: 18, height: 18, borderRadius: 9, background: 'linear-gradient(180deg, #8fdc7a, #2a7fbf)', border: '1px solid #1d5f8a' }} />
        <View style={{ position: 'absolute', left: 3, top: 10, width: 16, height: 1, backgroundColor: 'rgba(255,255,255,0.8)' }} />
        <View style={{ position: 'absolute', left: 10, top: 3, width: 1, height: 16, backgroundColor: 'rgba(255,255,255,0.8)' }} />
      </View>
    );
  }
  const ch = kind === 'help' ? '?' : '⌕';
  const bg = kind === 'help' ? 'linear-gradient(180deg, #8ec5ff, #2f63d8)' : 'linear-gradient(180deg, #ffffff, #c9d8f5)';
  return (
    <View style={{ width: 20, height: 20, borderRadius: 10, alignItems: 'center', justifyContent: 'center', background: bg, border: '1px solid #1d3f9a' }}>
      <Text style={{ fontSize: 12, fontWeight: 700, color: kind === 'help' ? '#ffffff' : '#1d3f9a' }}>{ch}</Text>
    </View>
  );
}

function FootButton({ t, font, label, color, onClick }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)} style={{ flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 6, height: 30 }}>
      <View style={{ width: 22, height: 22, borderRadius: 4, background: color, border: '1px solid rgba(255,255,255,0.85)', alignItems: 'center', justifyContent: 'center', opacity: hover ? 0.85 : 1 }}>
        <View style={{ width: 10, height: 10, borderRadius: 5, borderWidth: 2, borderColor: '#ffffff' }} />
      </View>
      <Text style={{ fontSize: 12, color: '#ffffff', ...fontOf(font) }}>{label}</Text>
    </View>
  );
}

export function StartMenu({ t, font, display, barH, onClose }) {
  const [s] = useSettings();
  const apps = useLauncherApps();
  const info = useSystemInfo(60000);
  const [all, setAll] = useState(false);
  const byId = new Map(apps.map((a) => [a.id, a]));
  const pins = startPins(s, apps).map((id) => byId.get(id)).filter(Boolean).slice(0, 9);
  const go = (fn) => () => { onClose(); fn(); };
  const openApp = (a) => go(() => launchApp(a.id));
  const name = info.fullName || info.user || '';
  const initials = name.split(/\s+/).filter(Boolean).slice(0, 2).map((w) => w[0].toUpperCase()).join('') || '?';
  const places = [
    ['documents', <Folder size={22} />, tr('files.place.documents'), () => wm.open('files', { new: true, args: { path: '~/Documents' } })],
    ['pictures', <Folder size={22} />, tr('files.place.pictures'), () => wm.open('files', { new: true, args: { path: '~/Pictures' } })],
    ['music', <Folder size={22} />, tr('files.place.music'), () => wm.open('files', { new: true, args: { path: '~/Music' } })],
    '-',
    ['computer', <PlaceMark kind="computer" />, tr('files.place.computer'), () => wm.open('files', { new: true, args: { path: '/' } })],
    ['network', <PlaceMark kind="network" />, tr('files.place.network'), () => wm.open('files', { new: true, args: { place: 'network' } })],
    '-',
    ['settings', byId.get('settings')?.Icon ? (() => { const I = byId.get('settings').Icon; return <I size={22} />; })() : null, appName(byId.get('settings')) || 'Settings', () => launchApp('settings')],
    ['help', <PlaceMark kind="help" />, appName(byId.get('help')) || tr('shell.menu.help'), () => launchApp('help')],
    ['search', <PlaceMark kind="search" />, tr('shell.classic.search'), () => spotlight.open()],
  ];
  const rtl = direction() === 'rtl';
  const left = rtl ? display.x + display.w - START_W : display.x;
  const top = display.y + display.h - barH - START_H;
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }} onMouseDown={onClose}>
      <View onMouseDown={(e) => e.stopPropagation()} style={{ position: 'absolute', left, top, width: START_W, height: START_H, borderTopLeftRadius: 8, borderTopRightRadius: 8,
        overflow: 'hidden', backgroundColor: t.startPanel, border: `1px solid ${t.frameEdge}`, boxShadow: t.shadow }}>
        {/* the header: the person's picture (a framed tile with their initials) and name */}
        <View style={{ height: 62, flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 10, background: t.titleBg }}>
          <View style={{ width: 44, height: 44, borderRadius: 4, border: '2px solid #ffffff', alignItems: 'center', justifyContent: 'center', background: 'linear-gradient(180deg, #ffd36b, #e88d1c)' }}>
            <Text style={{ fontSize: 17, fontWeight: 700, color: '#ffffff' }}>{initials}</Text>
          </View>
          <Text style={{ flex: 1, fontSize: 15, fontWeight: 700, color: '#ffffff', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start', ...fontOf(font) }}>{name}</Text>
        </View>
        <View style={{ height: 2, backgroundColor: '#f0a040' }} />
        <View style={{ flex: 1, flexDirection: 'row' }}>
          {/* the apps: pinned, then all of them */}
          <View style={{ flex: 1, backgroundColor: t.startLeft }}>
            <ScrollView style={{ flex: 1 }}>
              <View style={{ paddingVertical: 6 }}>
                {(all ? apps : pins).map((a) => (
                  <Row key={a.id} t={t} font={font} size={all ? 18 : 28} bold={!all && pins[0] === a} label={a.name || appName(a)} onClick={openApp(a)}
                    icon={a.Icon ? <a.Icon size={all ? 18 : 28} /> : null} />
                ))}
              </View>
            </ScrollView>
            <View style={{ height: 1, marginHorizontal: 10, backgroundColor: t.separator }} />
            <View onClick={() => setAll(!all)} style={{ height: 34, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 8 }}>
              <Text style={{ fontSize: 12, fontWeight: 700, color: t.text, ...fontOf(font) }}>{all ? tr('shell.classic.back') : tr('shell.classic.allApps')}</Text>
              <View style={{ width: 16, height: 16, borderRadius: 3, alignItems: 'center', justifyContent: 'center', background: 'linear-gradient(180deg, #8fdc7a, #2f8a25)' }}>
                <Text style={{ fontSize: 9, color: '#ffffff' }}>{(all ? !rtl : rtl) ? '◀' : '▶'}</Text>
              </View>
            </View>
          </View>
          {/* the places */}
          <View style={{ width: 186, paddingVertical: 6, backgroundColor: t.startRight, borderStartWidth: 1, borderStartColor: '#95bdee' }}>
            {places.map((p, i) => (p === '-'
              ? <View key={i} style={{ height: 1, marginVertical: 4, marginHorizontal: 8, backgroundColor: '#95bdee' }} />
              : <Row key={p[0]} t={t} font={font} light bold={i < 3} icon={p[1]} label={p[2]} onClick={go(p[3])} />))}
          </View>
        </View>
        {/* the footer: LordOS's own session actions */}
        <View style={{ height: 44, flexDirection: 'row', alignItems: 'center', justifyContent: 'flex-end', gap: 6, paddingHorizontal: 10, background: t.titleBg }}>
          <FootButton t={t} font={font} label={tr('shell.menu.logout')} color="linear-gradient(180deg, #ffcf63, #e39a12)" onClick={go(() => session.quit())} />
          <FootButton t={t} font={font} label={tr('shell.menu.shutdown')} color={t.closeBg} onClick={go(() => session.shutdown())} />
        </View>
      </View>
    </View>
  );
}
