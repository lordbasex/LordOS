// Control Center: mode, transparency, the volume and its output, the network, Wi-Fi and Bluetooth, accent,
// wallpaper and live CPU/RAM usage. The network, Wi-Fi, Bluetooth and CPU/RAM tiles can be turned off in Settings →
// Desktop & Dock → Control Center (issue #58, sdk/menubarmodel.js).
// A click on a tile's name (and the line under it) opens its page in Settings, as the Wi-Fi tile always did: Dark
// Mode, Transparency and Accent open Appearance, Sound opens Sound and Wallpaper opens Wallpaper (issue #58).
import {
  View, Text, Image, Toggle, Slider, useSettings, useCpuUsage, useSystemInfo, formatBytes, wm, session, addListener, t as tr,
  useSound, defaultSink, sound, NowPlayingTile, findWallpaper, wallpaperThumb, menuBarPrefs, SoundOutputList, useSoundOutputs, useBluetooth, setBluetoothPower,
  panelLeft, inBar,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';

// Four wallpapers fit in the tile: the current one and a few from the collection (all of them are
// in Settings → Wallpaper, which «Change Wallpaper…» opens).
const QUICK = ['lumo-celestial', 'crystal-crown', 'royal-flow', 'event-horizon', 'aurora'];
function quickWallpapers(list, cur) {
  const current = findWallpaper(list, cur);
  const rest = QUICK.map((id) => list.find((w) => w.id === id)).filter((w) => w && w !== current);
  return [current, ...rest].filter(Boolean).slice(0, 4);
}

// Settings on one page, the Control Center closed first
const openSettings = (onClose, section, page) => { onClose(); wm.open('settings', { args: { section, ...(page ? { page } : null) } }); };

// A tile's name, a click away from its page in Settings
function TileTitle({ t, onClick, children }) {
  return <View onClick={onClick}><Text style={{ fontSize: 12, fontWeight: 600, color: t.text, textAlign: 'start' }}>{children}</Text></View>;
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

// A small chevron drawn with two borders (it doesn't depend on a font): down when closed, up when open.
function Chevron({ color, open }) {
  return (
    <View style={{ width: 12, height: 12, alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: 5, height: 5, marginTop: open ? 3 : -3, borderRightWidth: 1.5, borderBottomWidth: 1.5, borderColor: color, transform: `rotate(${open ? -135 : 45}deg)` }} />
    </View>
  );
}

// Volume of the default output (PipeWire). Click on the speaker: mute. With more than one output, the button at the
// end lists them; choosing one makes it the default, and its own volume comes back with it.
function SoundTile({ t, onClose }) {
  const st = useSound();
  const out = defaultSink(st);
  const outputs = useSoundOutputs();
  const [pick, setPick] = useState(false);
  if (!st.connected || !out) return null;
  const pct = Math.round(out.level * 100);
  const many = outputs.length > 1;
  return (
    <Tile t={t} style={{ gap: 8 }}>
      <View onClick={() => openSettings(onClose, 'sound')} style={{ flexDirection: 'row', justifyContent: 'space-between' }}>
        <Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{tr('shell.cc.sound')}</Text>
        <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap', maxWidth: 200, overflow: 'hidden' }}>{out.description}</Text>
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, direction: 'ltr' }}>
        <View onClick={() => sound.setMute('sink', out.index, !out.mute)} style={{ width: 26, alignItems: 'center' }}>
          <Text style={{ fontSize: 15 }}>{out.mute || pct === 0 ? '🔇' : pct < 40 ? '🔈' : pct < 75 ? '🔉' : '🔊'}</Text>
        </View>
        <Slider value={Math.min(1, out.level)} width={many ? 168 : 196} onChange={(v) => sound.setVolume('sink', out.index, v)} />
        <Text style={{ fontSize: 11, color: t.textDim, width: 34, textAlign: 'end' }}>{out.mute ? '—' : `${pct} %`}</Text>
        {many ? (
          <View onClick={() => setPick((v) => !v)} title={tr('shell.cc.output')}
            style={{ width: 20, height: 20, borderRadius: 10, alignItems: 'center', justifyContent: 'center', backgroundColor: pick ? t.accent : t.control }}>
            <Chevron color={pick ? '#fff' : t.text} open={pick} />
          </View>
        ) : null}
      </View>
      {many && pick ? <SoundOutputList onPick={() => setPick(false)} /> : null}
    </Tile>
  );
}

// The Wi-Fi radio (only when the computer has one): connected or not; a click opens Settings → Network → Wi-Fi.
function WifiTile({ t, onClose, radios }) {
  const on = radios.find((i) => i.addresses.some((a) => a.scope === 'global' && a.family === 4));
  const ip = on?.addresses.find((a) => a.family === 4 && a.scope === 'global')?.address;
  return (
    <Tile t={t} style={{ flex: 1, gap: 3 }}>
      <View onClick={() => openSettings(onClose, 'network', 'wifi')} style={{ gap: 3 }}>
        <Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{tr('shell.cc.wifi')}</Text>
        <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{on ? `${on.name} · ${ip}` : tr('shell.cc.notConnected')}</Text>
      </View>
    </Tile>
  );
}

// Bluetooth (only with an adapter): the switch turns it on or off; the name opens Settings → Bluetooth.
function BluetoothTile({ t, onClose, bt }) {
  const adapter = bt.adapter;
  const dev = (bt.devices || []).find((d) => d.connected);
  return (
    <Tile t={t} style={{ flex: 1, gap: 6 }}>
      <View onClick={() => openSettings(onClose, 'bluetooth')} style={{ gap: 3 }}>
        <Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{tr('shell.cc.bluetooth')}</Text>
        <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>
          {!adapter.powered ? tr('shell.cc.off') : dev ? dev.name : tr('shell.cc.on')}
        </Text>
      </View>
      <Toggle on={!!adapter.powered} onChange={(on) => setBluetoothPower(on)} />
    </Tile>
  );
}

// VPN interfaces (WireGuard, OpenVPN, IPsec, PPTP, OpenConnect, Fortinet).
const isVpnIf = (i) => ['wireguard', 'tun', 'xfrm', 'ppp'].includes(i.type) || /^(wg|ovpn|ips|ppp|oc|fvpn)-/.test(i.name);

// Network state (and active VPNs); a click opens Settings → Network.
function NetTile({ t, onClose, ifs }) {
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

// area: the display of the menu bar it opened from (global coordinates); at: the [x, width] of the bar item that
// opened it, so it sits under that item, inside that display (sdk/menubarmodel.js panelLeft)
export function ControlCenter({ t, top, onClose, area = null, at = null }) {
  const left = panelLeft(at, 320, area), ay = area ? area.y : 0;
  const [s, set] = useSettings();
  const p = menuBarPrefs(s);
  let ifs = [];
  try { ifs = typeof lord.net !== 'undefined' ? lord.net.interfaces() : []; } catch (e) { ifs = []; }
  const radios = p.ccWifi ? ifs.filter((i) => i.type === 'wifi') : [];
  const bt = useBluetooth();
  const btShown = p.ccBluetooth && !!bt?.adapter;
  const cpu = useCpuUsage(1500);
  const info = useSystemInfo(3000);
  const ref = useRef(null);
  const used = (info.memTotalKB - info.memAvailableKB) / info.memTotalKB;
  // a click on this display's bar is its items' own (the button toggles the panel); anywhere else closes it, another
  // display's bar too (its button then opens the panel there)
  useEffect(() => addListener('pointerdown', (e) => {
    if (!inside(e.target, ref) && !(area ? inBar(e.x, e.y, area, top) : e.y <= top)) onClose();
  }), [onClose, area, top]);

  return (
    <View ref={ref} style={{
      position: 'absolute', ...(left == null ? { right: 10 } : { left }), top: ay + top + 6, width: 320, padding: 12, gap: 10, borderRadius: 16,
      backgroundColor: t.panelBg, backdropFilter: `blur(${t.blur}px)`, border: `1px solid ${t.windowBorder}`,
      boxShadow: '0 16px 40px rgba(0,0,0,0.4)',
    }}>
      <View style={{ flexDirection: 'row', gap: 10 }}>
        <Tile t={t} style={{ flex: 1, gap: 8 }}>
          <TileTitle t={t} onClick={() => openSettings(onClose, 'appearance')}>{tr('shell.cc.dark')}</TileTitle>
          <Toggle on={s.mode === 'dark'} onChange={(on) => set({ mode: on ? 'dark' : 'light' })} />
        </Tile>
        <Tile t={t} style={{ flex: 1, gap: 8 }}>
          <TileTitle t={t} onClick={() => openSettings(onClose, 'appearance')}>{tr('shell.cc.effects')}</TileTitle>
          <Toggle on={s.effects} onChange={(effects) => set({ effects })} />
        </Tile>
      </View>
      <SoundTile t={t} onClose={onClose} />
      <NowPlayingTile />
      {p.ccNetwork && typeof lord.net !== 'undefined' ? <NetTile t={t} onClose={onClose} ifs={ifs} /> : null}
      {radios.length || btShown ? (
        <View style={{ flexDirection: 'row', gap: 10 }}>
          {radios.length ? <WifiTile t={t} onClose={onClose} radios={radios} /> : null}
          {btShown ? <BluetoothTile t={t} onClose={onClose} bt={bt} /> : null}
        </View>
      ) : null}
      <Tile t={t} style={{ gap: 10 }}>
        <TileTitle t={t} onClick={() => openSettings(onClose, 'appearance')}>{tr('shell.cc.accent')}</TileTitle>
        <View style={{ flexDirection: 'row', gap: 10 }}>
          {Object.entries(t.accents).map(([name, c]) => (
            <View key={name} onClick={() => set({ accent: name })} style={{ width: 22, height: 22, borderRadius: 11,
              backgroundColor: c, border: s.accent === name ? `2px solid ${t.text}` : '0 solid transparent' }} />
          ))}
        </View>
      </Tile>
      <Tile t={t} style={{ gap: 8 }}>
        <TileTitle t={t} onClick={() => openSettings(onClose, 'wallpaper')}>{tr('shell.cc.wallpaper')}</TileTitle>
        <View style={{ flexDirection: 'row', gap: 8 }}>
          {quickWallpapers(t.wallpapers, s.wallpaper).map((w) => (
            <View key={w.id} onClick={() => set({ wallpaper: w.id })} style={{ width: 62, height: 40, borderRadius: 6, overflow: 'hidden',
              border: s.wallpaper === w.id ? `2px solid ${t.accent}` : `1px solid ${t.separator}` }}>
              <Image src={wallpaperThumb(w)} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
            </View>
          ))}
        </View>
      </Tile>
      {p.ccSystem ? <Tile t={t} style={{ gap: 12 }}>
        <Meter t={t} label="CPU" value={cpu} detail={tr('shell.cc.cpuDetail', { pct: Math.round(cpu * 100), count: info.cores })} />
        <Meter t={t} label={tr('shell.cc.memory')} value={used} detail={tr('shell.cc.memDetail', { used: formatBytes((info.memTotalKB - info.memAvailableKB) * 1024), total: formatBytes(info.memTotalKB * 1024) })} />
      </Tile> : null}
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
