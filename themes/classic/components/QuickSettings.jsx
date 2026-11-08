// Quick settings, from the tray: toggles for the network, Bluetooth, Do Not Disturb, Night Light,
// dark mode and transparency, the volume and its output (the chevron by the slider lists the outputs when there is
// more than one, issue #58), what is playing, the battery and a way into Settings.
// Everything comes from the SDK's stores (sound, Bluetooth, battery, Night Light, notifications);
// nothing here polls except the network glyph (every 5 s, Taskbar.jsx useNet).
import {
  View, Text, Slider, useSettings, settings, useSound, defaultSink, sound, useBluetooth, useBattery, BatteryGlyph, NowPlayingTile,
  nightActive, setNightManual, notifyPrefs, dndActive, wm, addListener, t as tr, SoundOutputList, useSoundOutputs,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { NetworkGlyph, BluetoothGlyph, MoonGlyph, SunGlyph, ContrastGlyph, LayersGlyph, SpeakerGlyph, GearGlyph, Chevron } from '../glyphs.jsx';
import { useNet } from './Taskbar.jsx';

const W = 360;
const TILE = Math.floor((W - 2 - 32 - 16) / 3);   // three per row (border, padding and gaps out)

function Toggle({ t, on, label, detail, onClick, onMore, children }) {
  const [hover, setHover] = useState(false);
  const fg = on ? '#ffffff' : t.text;
  return (
    <View style={{ width: TILE, gap: 6, alignItems: 'center' }}>
      <View onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
        style={{ alignSelf: 'stretch', height: 48, borderRadius: 6, flexDirection: 'row', overflow: 'hidden',
          backgroundColor: on ? t.accent : t.tile, border: `1px solid ${on ? t.accent : t.tileBorder}`, opacity: hover ? 0.9 : 1, direction: 'ltr' }}>
        <View onClick={onClick} style={{ flex: 1, alignItems: 'center', justifyContent: 'center' }}>{children(fg, on ? t.accent : (t.mode === 'dark' ? '#303035' : '#fbfbfc'))}</View>
        {onMore ? (
          <View onClick={onMore} style={{ width: 24, alignItems: 'center', justifyContent: 'center', borderLeftWidth: 1, borderColor: on ? 'rgba(255,255,255,0.3)' : t.tileBorder }}>
            <Chevron color={fg} size={5} dir="right" />
          </View>
        ) : null}
      </View>
      <Text style={{ fontSize: 11, color: t.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis', maxWidth: TILE, textAlign: 'center' }}>{label}</Text>
      {detail ? <Text style={{ fontSize: 10, color: t.textDim, marginTop: -4, whiteSpace: 'nowrap', textOverflow: 'ellipsis', maxWidth: TILE }}>{detail}</Text> : null}
    </View>
  );
}

// at: { pos: { left, top | bottom }, height } — next to the tray, computed by the theme for the taskbar's side
export function QuickSettings({ t, at, onClose, isOnBar }) {
  const ref = useRef(null);
  const [s, set] = useSettings();
  const st = useSound();
  const out = defaultSink(st);
  const outputs = useSoundOutputs();
  const many = outputs.length > 1;
  const [pick, setPick] = useState(false);
  const bt = useBluetooth();
  const bat = useBattery();
  const net = useNet();
  const adapter = bt?.adapter;
  useEffect(() => addListener('pointerdown', (e) => {
    for (let n = e.target; n; n = n.parent) if (n === ref.current) return;
    if (!isOnBar(e.x, e.y)) onClose();
  }), []);
  useEffect(() => addListener('keydown', (e) => { if (e.key === 'Escape') onClose(); }), []);
  const open = (section, page) => { onClose(); wm.open('settings', { args: { section, ...(page ? { page } : null) } }); };
  const dnd = dndActive();
  const setDnd = (on) => settings.set((x) => ({ ...x, notifications: { ...notifyPrefs(x), dnd: !!on } }));
  const night = nightActive();
  const pct = out ? Math.round(out.level * 100) : 0;
  const H = at.height;
  return (
    <View ref={ref} style={{ position: 'absolute', ...at.pos, width: W, borderRadius: 10, overflow: 'hidden',
      backgroundColor: t.panelBg, backdropFilter: t.blur ? `blur(${t.blur}px)` : undefined, border: `1px solid ${t.windowBorder}`, boxShadow: t.shadow }}>
      <View style={{ padding: 16, gap: 14, maxHeight: H }}>
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8 }}>
          <Toggle t={t} on={net.online} label={tr('shell.cc.network')} detail={net.online ? (net.vpn ? `${net.name} · VPN` : net.name) : tr('shell.cc.offline')}
            onClick={() => open('network')} onMore={() => open('network', net.vpn ? 'vpn' : undefined)}>
            {(c) => <NetworkGlyph color={c} size={16} off={!net.online} />}
          </Toggle>
          {adapter ? (
            <Toggle t={t} on={!!adapter.powered} label={tr('shell.classic.bluetooth')} onClick={() => lord.bt.power(!adapter.powered)} onMore={() => open('bluetooth')}>
              {(c) => <BluetoothGlyph color={c} size={16} />}
            </Toggle>
          ) : null}
          <Toggle t={t} on={dnd} label={tr('shell.classic.dnd')} onClick={() => setDnd(!dnd)}>
            {(c, bg) => <MoonGlyph color={c} bg={bg} size={16} />}
          </Toggle>
          <Toggle t={t} on={night} label={tr('shell.classic.nightLight')} onClick={() => setNightManual(!night)} onMore={() => open('displays')}>
            {(c) => <SunGlyph color={c} size={16} />}
          </Toggle>
          <Toggle t={t} on={s.mode === 'dark'} label={tr('shell.cc.dark')} onClick={() => set({ mode: s.mode === 'dark' ? 'light' : 'dark' })}>
            {(c) => <ContrastGlyph color={c} size={16} />}
          </Toggle>
          <Toggle t={t} on={s.effects !== false} label={tr('shell.cc.effects')} onClick={() => set({ effects: s.effects === false })}>
            {(c) => <LayersGlyph color={c} size={16} />}
          </Toggle>
        </View>
        {out && st.connected ? (
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, direction: 'ltr' }}>
            <View onClick={() => sound.setMute('sink', out.index, !out.mute)} style={{ width: 28, height: 28, borderRadius: 4, alignItems: 'center', justifyContent: 'center' }}>
              <SpeakerGlyph color={t.text} size={18} level={out.mute || pct === 0 ? 0 : pct < 50 ? 1 : 2} muted={out.mute} />
            </View>
            <Slider value={Math.min(1, out.level)} width={W - 32 - 28 - 10 - 44 - 10 - (many ? 38 : 0)} onChange={(v) => sound.setVolume('sink', out.index, v)} />
            <Text style={{ width: 44, fontSize: 12, color: t.textDim, textAlign: 'right' }}>{out.mute ? '—' : `${pct} %`}</Text>
            {many ? (
              <View onClick={() => setPick((v) => !v)} title={tr('shell.cc.output')}
                style={{ width: 28, height: 28, borderRadius: 4, alignItems: 'center', justifyContent: 'center', backgroundColor: pick ? t.hover : 'transparent' }}>
                <Chevron color={t.text} size={5} dir={pick ? 'up' : 'down'} />
              </View>
            ) : null}
          </View>
        ) : null}
        {out && st.connected && many && pick ? <SoundOutputList onPick={() => setPick(false)} /> : null}
        <NowPlayingTile style={{ borderRadius: 6 }} />
      </View>
      <View style={{ height: 48, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 16, gap: 10, backgroundColor: t.panelFoot,
        borderTopWidth: 1, borderColor: t.separator }}>
        {bat && bat.hasBattery ? (
          <View onClick={() => open('power')} style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
            <BatteryGlyph percent={bat.percent} charging={bat.charging} color={t.text} width={22} />
            <Text style={{ fontSize: 12, color: t.text }}>{bat.percent >= 0 ? `${bat.percent} %` : ''}</Text>
          </View>
        ) : null}
        <View style={{ flex: 1 }} />
        <View onClick={() => open('general')} style={{ width: 32, height: 32, borderRadius: 4, alignItems: 'center', justifyContent: 'center' }}>
          <GearGlyph color={t.text} bg={t.mode === 'dark' ? '#222226' : '#f0f0f2'} size={16} />
        </View>
      </View>
    </View>
  );
}
