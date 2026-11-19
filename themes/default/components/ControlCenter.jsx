// Control Center: mode, transparency, Do Not Disturb and Night Light, the volume and its output, the screen's brightness,
// the network, Wi-Fi and Bluetooth, accent, wallpaper and live CPU/RAM usage. The network, Wi-Fi, Bluetooth, Do Not
// Disturb, Night Light, brightness, CPU/RAM tiles and Log Out can be turned off in Settings → Desktop & Dock → Control
// Center (issues #58 and #114, sdk/menubarmodel.js).
// A click on a tile's name (and the line under it) opens its page in Settings, as the Wi-Fi tile always did: Dark
// Mode, Transparency and Accent open Appearance, Sound opens Sound and Wallpaper opens Wallpaper (issue #58).
// Switch tiles (issue #114): a click anywhere on the tile turns it on or off and the tile shows which (the accent when
// on, grey when off); its name and the › at its end open its page in Settings. Wi-Fi's (issue #192) is the accent when
// the radio is on too, but only its round icon turns it on or off; the rest of it opens Settings → Network → Wi-Fi.
// The panel scrolls when a short screen can't hold it all.
import {
  View, Text, Image, Slider, useSettings, settings, useCpuUsage, useSystemInfo, memUsedKB, formatBytes, wm, session, addListener, t as tr,
  useSound, defaultSink, sound, NowPlayingTile, findWallpaper, wallpaperThumb, wallpaperIdFor, wallpaperPatch, screenDisplays, menuBarPrefs, SoundOutputList, useSoundOutputs, useBluetooth, setBluetoothPower,
  panelLeft, inBar, ScrollView, screen, nightActive, setNightManual, notifyPrefs, dndActive, useBacklight, wifiTile, wifiTileView, admin, DisclosureGlyph,
  MoonGlyph, SunGlyph, ContrastGlyph, LayersGlyph, BluetoothGlyph, SignalGlyph,
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

// A switch tile's parts: the round icon (white on the accent when on, the control grey when off) and the › at the end
// that opens its page in Settings (mirrored in right to left).
const iconBg = (t, on) => (on ? '#ffffff' : t.control);
function TileIcon({ t, on, children }) {
  return (
    <View style={{ width: 28, height: 28, borderRadius: 14, alignItems: 'center', justifyContent: 'center', backgroundColor: iconBg(t, on) }}>
      {children(on ? t.accent : t.text, iconBg(t, on))}
    </View>
  );
}
function MoreButton({ color, onClick }) {
  return (
    <View onClick={(e) => { e?.stopPropagation?.(); onClick(); }} title={tr('shell.cc.openSettings')}
      style={{ width: 16, height: 28, alignItems: 'center', justifyContent: 'center' }}>
      <DisclosureGlyph open={false} color={color} size={9} width={12} height={12} />
    </View>
  );
}

// One line: the icon, the name over its state (On or Off unless detail says more) and the ›. A click on the tile turns
// it on or off; the name and the › open its page in Settings. glyph(color, bg): the icon.
function SwitchTile({ t, on, label, detail, glyph, onToggle, onSettings }) {
  const [hover, setHover] = useState(false);
  const fg = on ? '#ffffff' : t.text;
  const dim = on ? 'rgba(255,255,255,0.8)' : t.textDim;
  return (
    <View onClick={onToggle} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flex: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 9, paddingHorizontal: 9, borderRadius: 12,
        backgroundColor: on ? t.accent : t.hover, border: `1px solid ${on ? t.accent : t.separator}`, opacity: hover ? 0.9 : 1 }}>
      <TileIcon t={t} on={on}>{glyph}</TileIcon>
      <View style={{ flex: 1, minWidth: 0, gap: 1 }}>
        <View onClick={(e) => { e?.stopPropagation?.(); onSettings(); }}>
          <Text style={{ fontSize: 12, fontWeight: 600, color: fg, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{label}</Text>
        </View>
        <Text style={{ fontSize: 11, color: dim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{detail ?? tr(on ? 'shell.cc.on' : 'shell.cc.off')}</Text>
      </View>
      <MoreButton color={dim} onClick={onSettings} />
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

// The Wi-Fi radio (only when the computer has one; issue #192), w from wifiTile(): the accent when it is on and grey when
// it is off, like Bluetooth's tile. Its round icon turns it on or off with net-up / net-down, the administration
// helper's actions behind the switch on its page in Settings → Network (no other way in); with no helper, or a radio the
// system's own network service manages, the icon opens Settings like the rest. The icon lights the signal's bars in the
// accent, as Settings → Network → Wi-Fi does, and the line under the name is the network's name (or Not connected, or
// Off). Anywhere else opens Settings → Network → Wi-Fi. onChanged: read the interfaces again once the helper answers.
function WifiTile({ t, onClose, w, onChanged }) {
  const [want, setWant] = useState(null);           // the state asked for, while the helper works
  const [err, setErr] = useState(null);
  const [hover, setHover] = useState(false);
  const open = () => openSettings(onClose, 'network', 'wifi');
  const v = wifiTileView(w, want);
  const toggle = (e) => {
    e?.stopPropagation?.();
    if (!w.canSwitch || !admin.available()) { open(); return; }
    if (want != null) return;
    const to = !w.on;
    setWant(to);
    setErr(null);
    admin.run(to ? 'net-up' : 'net-down', w.name, undefined, { quiet: true })
      .then(() => { setWant(null); onChanged(); }, (er) => { setWant(null); setErr(er.message); });
  };
  const on = v.on;
  const fg = on ? '#ffffff' : t.text;
  const dim = on ? 'rgba(255,255,255,0.8)' : t.textDim;
  const detail = err || v.ssid || tr(v.key);
  return (
    <View onClick={open} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flex: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 9, paddingHorizontal: 9, borderRadius: 12,
        backgroundColor: on ? t.accent : t.hover, border: `1px solid ${on ? t.accent : t.separator}`, opacity: hover ? 0.9 : 1 }}>
      <View onClick={toggle} title={tr('shell.cc.wifiSwitch')}>
        <TileIcon t={t} on={on}>{(c) => <SignalGlyph color={c} size={14} level={v.bars} off={v.dim} />}</TileIcon>
      </View>
      <View style={{ flex: 1, minWidth: 0, gap: 1 }}>
        <Text style={{ fontSize: 12, fontWeight: 600, color: fg, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{tr('shell.cc.wifi')}</Text>
        <Text style={{ fontSize: 11, color: dim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{detail}</Text>
      </View>
      <MoreButton color={dim} onClick={open} />
    </View>
  );
}

// Bluetooth (only with an adapter): a click on the tile turns it on or off; the name opens Settings → Bluetooth.
function BluetoothTile({ t, onClose, bt }) {
  const adapter = bt.adapter;
  const dev = (bt.devices || []).find((d) => d.connected);
  return (
    <SwitchTile t={t} on={!!adapter.powered} label={tr('shell.cc.bluetooth')} detail={adapter.powered && dev ? dev.name : undefined}
      glyph={(c) => <BluetoothGlyph color={c} size={14} />} onToggle={() => setBluetoothPower(!adapter.powered)} onSettings={() => openSettings(onClose, 'bluetooth')} />
  );
}

// The screen's brightness (issue #114), only when LordOS can set it (a laptop's or tablet's own panel): the same
// action as the brightness keys. The name opens Settings → Displays.
function BrightnessTile({ t, onClose, bl }) {
  return (
    <Tile t={t} style={{ gap: 8 }}>
      <TileTitle t={t} onClick={() => openSettings(onClose, 'displays')}>{tr('prefs.display.brightness')}</TileTitle>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, direction: 'ltr' }}>
        <View style={{ width: 26, alignItems: 'center' }}><SunGlyph color={t.textDim} size={13} /></View>
        <Slider value={bl.fraction} width={196} onChange={bl.set} />
        <Text style={{ fontSize: 11, color: t.textDim, width: 34, textAlign: 'end' }}>{`${Math.round(bl.fraction * 100)} %`}</Text>
      </View>
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
const W = 340;       // (issue #114) 20 px wider than before, for a name next to each switch tile's icon
export function ControlCenter({ t, top, onClose, area = null, at = null }) {
  const left = panelLeft(at, W, area), ay = area ? area.y : 0;
  // several displays: the quick wallpapers are the picture of the display it opened on (sdk/wallpapers.js)
  const wd = area && screenDisplays().length > 1 ? screenDisplays().find((d) => d.id === area.id) || null : null;
  const maxH = Math.max(240, (area ? area.h : screen.height) - top - 16);
  const [s, set] = useSettings();
  const curWall = wallpaperIdFor(s, wd);
  const p = menuBarPrefs(s);
  let ifs = [];
  try { ifs = typeof lord.net !== 'undefined' ? lord.net.interfaces() : []; } catch (e) { ifs = []; }
  const wifi = p.ccWifi ? wifiTile(ifs) : null;
  const [, reread] = useState(0);                   // after the Wi-Fi tile turns the radio on or off
  const bt = useBluetooth();
  const btShown = p.ccBluetooth && !!bt?.adapter;
  const bl = useBacklight();
  const dnd = dndActive();
  const setDnd = (on) => settings.set((x) => ({ ...x, notifications: { ...notifyPrefs(x), dnd: !!on } }));
  const night = nightActive();
  const cpu = useCpuUsage(1500);
  const info = useSystemInfo(3000);
  const ref = useRef(null);
  const used = info.memTotalKB > 0 ? memUsedKB(info) / info.memTotalKB : 0;
  // a click on this display's bar is its items' own (the button toggles the panel); anywhere else closes it, another
  // display's bar too (its button then opens the panel there)
  useEffect(() => addListener('pointerdown', (e) => {
    if (!inside(e.target, ref) && !(area ? inBar(e.x, e.y, area, top) : e.y <= top)) onClose();
  }), [onClose, area, top]);

  return (
    <View ref={ref} style={{
      position: 'absolute', ...(left == null ? { right: 10 } : { left }), top: ay + top + 6, width: W, borderRadius: 16, overflow: 'hidden',
      backgroundColor: t.panelBg, backdropFilter: `blur(${t.blur}px)`, border: `1px solid ${t.windowBorder}`,
      boxShadow: '0 16px 40px rgba(0,0,0,0.4)',
    }}>
      <ScrollView style={{ maxHeight: maxH }} contentStyle={{ padding: 12, gap: 10 }}>
        <View style={{ flexDirection: 'row', gap: 10 }}>
          <SwitchTile t={t} on={s.mode === 'dark'} label={tr('shell.cc.dark')} glyph={(c) => <ContrastGlyph color={c} size={14} />}
            onToggle={() => set({ mode: s.mode === 'dark' ? 'light' : 'dark' })} onSettings={() => openSettings(onClose, 'appearance')} />
          <SwitchTile t={t} on={s.effects !== false} label={tr('shell.cc.effects')} glyph={(c) => <LayersGlyph color={c} size={14} />}
            onToggle={() => set({ effects: s.effects === false })} onSettings={() => openSettings(onClose, 'appearance')} />
        </View>
        {p.ccDnd || p.ccNight ? (
          <View style={{ flexDirection: 'row', gap: 10 }}>
            {p.ccDnd ? <SwitchTile t={t} on={dnd} label={tr('shell.classic.dnd')} glyph={(c, bg) => <MoonGlyph color={c} bg={bg} size={14} />}
              onToggle={() => setDnd(!dnd)} onSettings={() => openSettings(onClose, 'notifications')} /> : null}
            {p.ccNight ? <SwitchTile t={t} on={night} label={tr('shell.classic.nightLight')} glyph={(c) => <SunGlyph color={c} size={14} />}
              onToggle={() => setNightManual(!night)} onSettings={() => openSettings(onClose, 'displays')} /> : null}
          </View>
        ) : null}
        <SoundTile t={t} onClose={onClose} />
        {p.ccBrightness && bl ? <BrightnessTile t={t} onClose={onClose} bl={bl} /> : null}
        <NowPlayingTile />
        {p.ccNetwork && typeof lord.net !== 'undefined' ? <NetTile t={t} onClose={onClose} ifs={ifs} /> : null}
        {wifi || btShown ? (
          <View style={{ flexDirection: 'row', gap: 10 }}>
            {wifi ? <WifiTile t={t} onClose={onClose} w={wifi} onChanged={() => reread((n) => n + 1)} /> : null}
            {btShown ? <BluetoothTile t={t} onClose={onClose} bt={bt} /> : null}
          </View>
        ) : null}
        <Tile t={t} style={{ gap: 10 }}>
          <TileTitle t={t} onClick={() => openSettings(onClose, 'appearance')}>{tr('shell.cc.accent')}</TileTitle>
          <View style={{ flexDirection: 'row', gap: 10 }}>
            {Object.entries(t.accents).map(([name, c]) => (
              <View key={name} onClick={() => set({ accent: name })} style={{ width: 22, height: 22, borderRadius: 11,
                backgroundColor: c, border: (s.accent || t.accentName) === name ? `2px solid ${t.text}` : '0 solid transparent' }} />
            ))}
          </View>
        </Tile>
        <Tile t={t} style={{ gap: 8 }}>
          <TileTitle t={t} onClick={() => openSettings(onClose, 'wallpaper')}>{tr('shell.cc.wallpaper')}</TileTitle>
          <View style={{ flexDirection: 'row', gap: 8 }}>
            {quickWallpapers(t.wallpapers, curWall).map((w) => (
              <View key={w.id} onClick={() => set(wd ? wallpaperPatch(s, w.id, wd) : { wallpaper: w.id })} style={{ width: 62, height: 40, borderRadius: 6, overflow: 'hidden',
                border: curWall === w.id ? `2px solid ${t.accent}` : `1px solid ${t.separator}` }}>
                <Image src={wallpaperThumb(w)} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
              </View>
            ))}
          </View>
        </Tile>
        {p.ccSystem ? <Tile t={t} style={{ gap: 12 }}>
          <Meter t={t} label="CPU" value={cpu} detail={tr('shell.cc.cpuDetail', { pct: Math.round(cpu * 100), count: info.cores })} />
          <Meter t={t} label={tr('shell.cc.memory')} value={used} detail={tr('shell.cc.memDetail', { used: formatBytes(memUsedKB(info) * 1024), total: formatBytes(info.memTotalKB * 1024) })} />
        </Tile> : null}
        <View style={{ flexDirection: 'row', gap: 10 }}>
          <Tile t={t} style={{ flex: 1, alignItems: 'center' }}>
            <View onClick={() => { onClose(); wm.open('settings'); }}><Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{tr('shell.menu.settings')}</Text></View>
          </Tile>
          {p.ccLogout ? (
            <Tile t={t} style={{ flex: 1, alignItems: 'center' }}>
              <View onClick={() => session.quit()}><Text style={{ fontSize: 12, fontWeight: 600, color: '#ff5f57' }}>{tr('shell.menu.logout')}</Text></View>
            </Tile>
          ) : null}
        </View>
      </ScrollView>
    </View>
  );
}
