// Control Center: mode, transparency, accent, wallpaper and live CPU/RAM usage.
import {
  View, Text, Image, Toggle, Slider, useSettings, useCpuUsage, useSystemInfo, formatBytes, wm, session, addListener, t as tr,
  useSound, defaultSink, sound, NowPlayingTile, findWallpaper, wallpaperThumb,
} from '@lordos/sdk';
import { useEffect, useRef } from 'react';

// Four wallpapers fit in the tile: the current one and a few from the collection (all of them are
// in Settings → Wallpaper, which «Change Wallpaper…» opens).
const QUICK = ['infinite-galaxy', 'violet-horizon', 'eternal-dawn', 'event-horizon', 'aurora'];
function quickWallpapers(list, cur) {
  const current = findWallpaper(list, cur);
  const rest = QUICK.map((id) => list.find((w) => w.id === id)).filter((w) => w && w !== current);
  return [current, ...rest].filter(Boolean).slice(0, 4);
}

function Tile({ t, children, style }) {
  return <View style={[{ padding: 12, borderRadius: 12, backgroundColor: t.hover, border: `1px solid ${t.separator}` }, style]}>{children}</View>;
}

function Meter({ t, label, value, detail }) {
  return (
    <View style={{ gap: 6 }}>
      <View style={{ flexDirection: 'row', justifyContent: 'space-between' }}>
        <Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{label}</Text>
        <Text style={{ fontSize: 12, color: t.textDim }}>{detail}</Text>
      </View>
      <View style={{ height: 6, borderRadius: 3, backgroundColor: t.control }}>
        <View style={{ width: `${Math.round(value * 100)}%`, height: 6, borderRadius: 3, backgroundColor: t.accent }} />
      </View>
    </View>
  );
}

// Volume of the default output (PipeWire). Click on the speaker: mute.
function SoundTile({ t }) {
  const st = useSound();
  const out = defaultSink(st);
  if (!st.connected || !out) return null;
  const pct = Math.round(out.level * 100);
  return (
    <Tile t={t} style={{ gap: 8 }}>
      <View style={{ flexDirection: 'row', justifyContent: 'space-between' }}>
        <Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{tr('shell.cc.sound')}</Text>
        <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap', maxWidth: 200, overflow: 'hidden' }}>{out.description}</Text>
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, direction: 'ltr' }}>
        <View onClick={() => sound.setMute('sink', out.index, !out.mute)} style={{ width: 26, alignItems: 'center' }}>
          <Text style={{ fontSize: 15 }}>{out.mute || pct === 0 ? '🔇' : pct < 40 ? '🔈' : pct < 75 ? '🔉' : '🔊'}</Text>
        </View>
        <Slider value={Math.min(1, out.level)} width={196} onChange={(v) => sound.setVolume('sink', out.index, v)} />
        <Text style={{ fontSize: 11, color: t.textDim, width: 34, textAlign: 'end' }}>{out.mute ? '—' : `${pct} %`}</Text>
      </View>
    </Tile>
  );
}

// VPN interfaces (WireGuard, OpenVPN, IPsec, PPTP, OpenConnect, Fortinet).
const isVpnIf = (i) => ['wireguard', 'tun', 'xfrm', 'ppp'].includes(i.type) || /^(wg|ovpn|ips|ppp|oc|fvpn)-/.test(i.name);

// Network state (and active VPNs); a click opens Settings → Network.
function NetTile({ t, onClose }) {
  if (typeof lord.net === 'undefined') return null;
  const ifs = lord.net.interfaces();
  const main = ifs.find((i) => !isVpnIf(i) && i.type !== 'loopback' && i.addresses.some((a) => a.scope === 'global' && a.family === 4));
  const vpns = ifs.filter((i) => isVpnIf(i) && i.up);
  const ip = main?.addresses.find((a) => a.family === 4 && a.scope === 'global')?.address;
  return (
    <Tile t={t} style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
      <View onClick={() => { onClose(); wm.open('settings', { args: { section: 'network' } }); }} style={{ flex: 1, gap: 3 }}>
        <Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{tr('shell.cc.network')}</Text>
        <Text style={{ fontSize: 11, color: t.textDim }}>{main ? `${main.name} · ${ip}` : tr('shell.cc.offline')}</Text>
      </View>
      <View onClick={() => { onClose(); wm.open('settings', { args: { section: 'network', page: 'vpn' } }); }}
        style={{ paddingHorizontal: 10, paddingVertical: 5, borderRadius: 8, backgroundColor: vpns.length ? t.accent : t.control }}>
        <Text style={{ fontSize: 11, fontWeight: 600, color: vpns.length ? '#fff' : t.text }}>{vpns.length ? `VPN · ${vpns.length}` : 'VPN'}</Text>
      </View>
    </Tile>
  );
}

const inside = (inst, ref) => { for (let n = inst; n; n = n.parent) if (n === ref.current) return true; return false; };

// area: the display of the menu bar it opened from (global coordinates)
export function ControlCenter({ t, top, onClose, area = null }) {
  const ax = area ? area.x + area.w : null, ay = area ? area.y : 0;
  const [s, set] = useSettings();
  const cpu = useCpuUsage(1500);
  const info = useSystemInfo(3000);
  const ref = useRef(null);
  const used = (info.memTotalKB - info.memAvailableKB) / info.memTotalKB;
  useEffect(() => addListener('pointerdown', (e) => { if (!inside(e.target, ref) && e.y > ay + top) onClose(); }), [onClose, ay]);

  return (
    <View ref={ref} style={{
      position: 'absolute', ...(ax == null ? { right: 10 } : { left: ax - 330 }), top: ay + top + 6, width: 320, padding: 12, gap: 10, borderRadius: 16,
      backgroundColor: t.panelBg, backdropFilter: `blur(${t.blur}px)`, border: `1px solid ${t.windowBorder}`,
      boxShadow: '0 16px 40px rgba(0,0,0,0.4)',
    }}>
      <View style={{ flexDirection: 'row', gap: 10 }}>
        <Tile t={t} style={{ flex: 1, gap: 8 }}>
          <Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{tr('shell.cc.dark')}</Text>
          <Toggle on={s.mode === 'dark'} onChange={(on) => set({ mode: on ? 'dark' : 'light' })} />
        </Tile>
        <Tile t={t} style={{ flex: 1, gap: 8 }}>
          <Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{tr('shell.cc.effects')}</Text>
          <Toggle on={s.effects} onChange={(effects) => set({ effects })} />
        </Tile>
      </View>
      <SoundTile t={t} />
      <NowPlayingTile />
      <NetTile t={t} onClose={onClose} />
      <Tile t={t} style={{ gap: 10 }}>
        <Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{tr('shell.cc.accent')}</Text>
        <View style={{ flexDirection: 'row', gap: 10 }}>
          {Object.entries(t.accents).map(([name, c]) => (
            <View key={name} onClick={() => set({ accent: name })} style={{ width: 22, height: 22, borderRadius: 11,
              backgroundColor: c, border: s.accent === name ? `2px solid ${t.text}` : '0 solid transparent' }} />
          ))}
        </View>
      </Tile>
      <Tile t={t} style={{ gap: 8 }}>
        <Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{tr('shell.cc.wallpaper')}</Text>
        <View style={{ flexDirection: 'row', gap: 8 }}>
          {quickWallpapers(t.wallpapers, s.wallpaper).map((w) => (
            <View key={w.id} onClick={() => set({ wallpaper: w.id })} style={{ width: 62, height: 40, borderRadius: 6, overflow: 'hidden',
              border: s.wallpaper === w.id ? `2px solid ${t.accent}` : `1px solid ${t.separator}` }}>
              <Image src={wallpaperThumb(w)} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
            </View>
          ))}
        </View>
      </Tile>
      <Tile t={t} style={{ gap: 12 }}>
        <Meter t={t} label="CPU" value={cpu} detail={tr('shell.cc.cpuDetail', { pct: Math.round(cpu * 100), count: info.cores })} />
        <Meter t={t} label={tr('shell.cc.memory')} value={used} detail={tr('shell.cc.memDetail', { used: formatBytes((info.memTotalKB - info.memAvailableKB) * 1024), total: formatBytes(info.memTotalKB * 1024) })} />
      </Tile>
      <View style={{ flexDirection: 'row', gap: 10 }}>
        <Tile t={t} style={{ flex: 1, alignItems: 'center' }}>
          <View onClick={() => { onClose(); wm.open('settings'); }}><Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{tr('shell.menu.settings')}</Text></View>
        </Tile>
        <Tile t={t} style={{ flex: 1, alignItems: 'center' }}>
          <View onClick={() => session.quit()}><Text style={{ fontSize: 12, fontWeight: 600, color: '#ff5f57' }}>{tr('shell.menu.logout')}</Text></View>
        </Tile>
      </View>
    </View>
  );
}
