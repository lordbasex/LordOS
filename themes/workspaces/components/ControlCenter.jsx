// The Control Center (a popover from the spine's button, unfolding from it): a live preview of the desktop drawn with
// the current choices, then the look (dark mode, the effects style Opaque / Translucent / Crystal, Reduce motion, the
// accent, the wallpaper) and the quick settings Lord Flow's Control Center has: Do Not Disturb, Night Light, Bluetooth,
// Wi-Fi (its state; a click opens Settings → Network → Wi-Fi), the screen's brightness, the volume and the power mode.
// Everything comes from the SDK's stores (sound, Bluetooth, Night Light, notifications, power profiles); the Wi-Fi
// state is read once when it opens, nothing polls.
import {
  View, Text, Image, Slider, Segmented, ScrollView, useSettings, settings, wm, addListener, useSound, defaultSink, sound, useBluetooth, setBluetoothPower,
  nightActive, setNightManual, notifyPrefs, dndActive, useBacklight, wifiBars, findWallpaper, wallpaperThumb, wallpaperIdFor, wallpaperPatch, screenDisplays, usePowerProfiles, admin, t as tr,
  MoonGlyph, SunGlyph, ContrastGlyph, BluetoothGlyph, SignalGlyph,
} from '@lordos/sdk';
import { useEffect, useRef } from 'react';
import { Surface, Crystal } from './Surface.jsx';
import { SpeakerGlyph, Chevron } from '../glyphs.jsx';
import { wsPrefs, optPatch, SURFACES, effectiveSurface } from '../model/activities.js';
import { tween, EASE } from '../motion.js';

export const CC_W = 360;
const QUICK = ['lumo-celestial', 'crystal-crown', 'royal-flow', 'event-horizon'];
const openSettings = (onClose, section, page) => { onClose(); wm.open('settings', { args: { section, ...(page ? { page } : null) } }); };

function Tile({ t, children, style }) {
  return <View style={{ padding: 10, borderRadius: 14, backgroundColor: t.hover, border: `1px solid ${t.separator}`, ...style }}>{children}</View>;
}

function Switch({ t, on, label, detail, glyph, onToggle, onMore }) {
  const fg = on ? t.onAccent : t.text;
  return (
    <View onClick={onToggle} style={{ flex: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', gap: 8, padding: 9, borderRadius: 14,
      backgroundColor: on ? t.accent : t.hover, border: `1px solid ${on ? t.accent : t.separator}` }}>
      <View style={{ width: 28, height: 28, borderRadius: 14, alignItems: 'center', justifyContent: 'center', backgroundColor: on ? '#ffffff' : t.control }}>
        {glyph(on ? t.accent : t.text, on ? '#ffffff' : t.control)}
      </View>
      <View style={{ flex: 1, minWidth: 0 }}>
        <Text style={{ fontSize: 12, fontWeight: 600, color: fg, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{label}</Text>
        <Text style={{ fontSize: 11, color: on ? 'rgba(255,255,255,0.82)' : t.textDim, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{detail ?? tr(on ? 'shell.cc.on' : 'shell.cc.off')}</Text>
      </View>
      {onMore ? (
        <View onClick={(e) => { e?.stopPropagation?.(); onMore(); }} title={tr('shell.cc.openSettings')} style={{ width: 16, height: 28, alignItems: 'center', justifyContent: 'center' }}>
          <Chevron color={on ? '#ffffff' : t.textDim} size={4.5} dir="right" />
        </View>
      ) : null}
    </View>
  );
}

// A small picture of this desktop with the choices as they are: the spine, the strip, the group frame and two
// windows, each surface in the chosen style (the preview's own surfaces don't blur: it is a drawing).
function Preview({ t, accent }) {
  const w = 336, h = 128;
  const pane = { backgroundColor: t.mat.bg === 'transparent' ? t.solidBg : t.mat.bg, border: `1px solid ${t.mat.border}`, overflow: 'hidden' };
  return (
    <View style={{ width: w, height: h, borderRadius: 12, overflow: 'hidden', direction: 'ltr',
      background: t.mode === 'dark' ? 'linear-gradient(160deg, #3A2A78, #120F24)' : 'linear-gradient(160deg, #CFC4FF, #F4F1FF)' }}>
      <View style={{ position: 'absolute', left: 6, top: 6, bottom: 6, width: 22, borderRadius: 8, ...pane }}>
        <Crystal t={t} radius={8} />
        <View style={{ alignItems: 'center', gap: 6, marginTop: 7 }}>
          {[0, 1, 2].map((i) => <View key={i} style={{ width: 10, height: 10, borderRadius: 3, backgroundColor: i === 0 ? accent : t.textDim, opacity: i === 0 ? 1 : 0.5 }} />)}
        </View>
      </View>
      <View style={{ position: 'absolute', left: 34, top: 6, width: 120, height: 16, borderRadius: 8, flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 4, ...pane }}>
        <Crystal t={t} radius={8} />
        <View style={{ width: 30, height: 9, borderRadius: 5, backgroundColor: accent }} />
        <View style={{ width: 26, height: 6, borderRadius: 3, backgroundColor: t.textDim, opacity: 0.5 }} />
        <View style={{ width: 22, height: 6, borderRadius: 3, backgroundColor: t.textDim, opacity: 0.5 }} />
      </View>
      <View style={{ position: 'absolute', left: 34, top: 28, right: 6, bottom: 6, borderRadius: 9, backgroundColor: t.frameBg, border: `1px solid ${accent}` }}>
        <View style={{ position: 'absolute', left: 6, top: 14, width: 176, bottom: 6, borderRadius: 5, backgroundColor: t.windowBg, border: `1px solid ${t.windowBorder}` }}>
          <View style={{ height: 9, backgroundColor: t.titleBg }} />
        </View>
        <View style={{ position: 'absolute', left: 188, top: 14, right: 6, bottom: 6, borderRadius: 5, backgroundColor: t.windowBg, border: `1px solid ${t.windowBorder}` }}>
          <View style={{ height: 9, backgroundColor: t.titleBg }} />
        </View>
      </View>
    </View>
  );
}

function SoundRow({ t }) {
  const st = useSound();
  const out = defaultSink(st);
  if (!st.connected || !out) return null;
  const pct = Math.round(out.level * 100);
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, direction: 'ltr' }}>
      <View onClick={() => sound.setMute('sink', out.index, !out.mute)} style={{ width: 24, alignItems: 'center' }}>
        <SpeakerGlyph color={t.text} size={15} level={pct === 0 ? 0 : pct < 50 ? 1 : 2} muted={out.mute} />
      </View>
      <Slider value={Math.min(1, out.level)} width={250} onChange={(v) => sound.setVolume('sink', out.index, v)} />
      <Text style={{ fontSize: 11, color: t.textDim, width: 38, textAlign: 'end' }}>{out.mute ? '—' : `${pct} %`}</Text>
    </View>
  );
}

function PowerRow({ t, onClose }) {
  const p = usePowerProfiles();
  const list = (p?.profiles || []).filter((x) => ['power-saver', 'balanced', 'performance'].includes(x));
  if (list.length < 2) return null;
  const set = (v) => { if (!(p.daemon && lord.power.setProfile(v))) admin.run('power-profile', v).catch(() => {}); };
  return (
    <Tile t={t} style={{ gap: 8 }}>
      <View onClick={() => openSettings(onClose, 'power')}><Text style={{ fontSize: 12, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('prefs.power.mode')}</Text></View>
      <Segmented small value={p.active} onChange={set} options={['power-saver', 'balanced', 'performance'].filter((x) => list.includes(x)).map((x) => [x, tr(`prefs.power.profile.${x}`)])} />
    </Tile>
  );
}

// anchor: { x, y, bottom } the spine button's box (the panel unfolds from it); area: its display
export function ControlCenter({ t, anchor, area, rtl, onClose, isOnChrome }) {
  const [s, set] = useSettings();
  const ref = useRef(null);
  const prefs = wsPrefs(s);
  const surface = effectiveSurface(prefs, s);
  const bt = useBluetooth();
  const bl = useBacklight();
  const st = useSound();
  const soundOn = !!(st.connected && defaultSink(st));
  const dnd = dndActive();
  const night = nightActive();
  let radios = [];
  try { radios = typeof lord.net !== 'undefined' ? lord.net.interfaces().filter((i) => i.type === 'wifi') : []; } catch (e) { radios = []; }
  const wifi = radios.find((i) => i.addresses.some((a) => a.scope === 'global' && a.family === 4));
  useEffect(() => {
    const n = ref.current?.id;
    if (n) tween(n, { scaleX: [0.94, 1], scaleY: [0.86, 1], opacity: [0, 1] }, { ms: 220, ease: EASE.back });
  }, []);
  useEffect(() => addListener('pointerdown', (e) => {
    for (let n = e.target; n; n = n.parent) if (n === ref.current) return;
    if (!isOnChrome(e.x, e.y)) onClose();
  }), []);
  const height = Math.min(area.h - 24, 720);
  const left = rtl ? anchor.x - CC_W - 10 : anchor.x + anchor.w + 10;
  const top = Math.max(area.y + 12, Math.min(anchor.y + anchor.h - height, area.y + area.h - 12 - height));
  // several displays: the quick wallpapers are the picture of the display it opened on (sdk/wallpapers.js)
  const wd = screenDisplays().length > 1 ? screenDisplays().find((d) => d.id === area.id) || null : null;
  const curWall = wallpaperIdFor(s, wd);
  const quick = [findWallpaper(t.wallpapers, curWall), ...QUICK.map((id) => t.wallpapers.find((w) => w.id === id))].filter((w, i, a) => w && a.indexOf(w) === i).slice(0, 4);
  return (
    <Surface ref={ref} t={t} radius={20} style={{ position: 'absolute', left, top, width: CC_W, height, transformOrigin: rtl ? 'right bottom' : 'left bottom',
      backgroundColor: t.panelBg, ...(t.panelBlur ? { backdropFilter: `blur(${t.panelBlur}px)` } : null) }}>
      <ScrollView style={{ flex: 1 }} contentStyle={{ padding: 12, gap: 10 }}>
        <Preview t={t} accent={t.accent} />
        <View style={{ flexDirection: 'row', gap: 8 }}>
          <Switch t={t} on={s.mode === 'dark'} label={tr('shell.cc.dark')} glyph={(c) => <ContrastGlyph color={c} size={14} />}
            onToggle={() => set({ mode: s.mode === 'dark' ? 'light' : 'dark' })} onMore={() => openSettings(onClose, 'appearance')} />
          <Switch t={t} on={!!s.reduceMotion} label={tr('shell.ws.cc.reduceMotion')} glyph={(c) => <Chevron color={c} size={5} dir="right" />}
            onToggle={() => set({ reduceMotion: !s.reduceMotion })} />
        </View>
        <Tile t={t} style={{ gap: 8 }}>
          <Text style={{ fontSize: 12, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('shell.ws.surface.title')}</Text>
          <Segmented small value={surface} options={SURFACES.map((x) => [x, tr(`shell.ws.surface.${x}`)])}
            onChange={(v) => set({ ...optPatch(settings.get(), 'surface', v), ...(v !== 'opaque' && s.effects === false ? { effects: true } : null) })} />
          <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>{tr(`shell.ws.surface.${surface}Hint`)}</Text>
        </Tile>
        <Tile t={t} style={{ gap: 8 }}>
          <View onClick={() => openSettings(onClose, 'appearance')}><Text style={{ fontSize: 12, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('shell.cc.accent')}</Text></View>
          <View style={{ flexDirection: 'row', gap: 10, direction: 'ltr' }}>
            {Object.entries(t.accents).map(([name, c]) => (
              <View key={name} onClick={() => set({ accent: name })} style={{ width: 22, height: 22, borderRadius: 11, backgroundColor: c,
                border: s.accent === name ? `2px solid ${t.text}` : '0 solid transparent' }} />
            ))}
          </View>
        </Tile>
        <View style={{ flexDirection: 'row', gap: 8 }}>
          <Switch t={t} on={dnd} label={tr('shell.classic.dnd')} glyph={(c, bg) => <MoonGlyph color={c} bg={bg} size={14} />}
            onToggle={() => settings.set((x) => ({ ...x, notifications: { ...notifyPrefs(x), dnd: !dnd } }))} onMore={() => openSettings(onClose, 'notifications')} />
          <Switch t={t} on={night} label={tr('shell.classic.nightLight')} glyph={(c) => <SunGlyph color={c} size={14} />}
            onToggle={() => setNightManual(!night)} onMore={() => openSettings(onClose, 'displays')} />
        </View>
        {radios.length || bt?.adapter ? (
          <View style={{ flexDirection: 'row', gap: 8 }}>
            {radios.length ? <Switch t={t} on={false} label={tr('shell.cc.wifi')} detail={wifi ? wifi.name : tr('shell.cc.notConnected')}
              glyph={(c) => <SignalGlyph color={c} size={14} level={wifi ? wifiBars(wifi.signal) : 0} off={!wifi} />}
              onToggle={() => openSettings(onClose, 'network', 'wifi')} onMore={() => openSettings(onClose, 'network', 'wifi')} /> : null}
            {bt?.adapter ? <Switch t={t} on={!!bt.adapter.powered} label={tr('shell.cc.bluetooth')} glyph={(c) => <BluetoothGlyph color={c} size={14} />}
              onToggle={() => setBluetoothPower(!bt.adapter.powered)} onMore={() => openSettings(onClose, 'bluetooth')} /> : null}
          </View>
        ) : null}
        {soundOn || bl ? <Tile t={t} style={{ gap: 10 }}>
          {soundOn ? <View onClick={() => openSettings(onClose, 'sound')}><Text style={{ fontSize: 12, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('shell.cc.sound')}</Text></View> : null}
          <SoundRow t={t} />
          {bl ? (
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, direction: 'ltr' }}>
              <View style={{ width: 24, alignItems: 'center' }}><SunGlyph color={t.textDim} size={13} /></View>
              <Slider value={bl.fraction} width={250} onChange={bl.set} />
              <Text style={{ fontSize: 11, color: t.textDim, width: 38, textAlign: 'end' }}>{`${Math.round(bl.fraction * 100)} %`}</Text>
            </View>
          ) : null}
        </Tile> : null}
        <PowerRow t={t} onClose={onClose} />
        <Tile t={t} style={{ gap: 8 }}>
          <View onClick={() => openSettings(onClose, 'wallpaper')}><Text style={{ fontSize: 12, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('shell.cc.wallpaper')}</Text></View>
          <View style={{ flexDirection: 'row', gap: 8 }}>
            {quick.map((w) => (
              <View key={w.id} onClick={() => set(wd ? wallpaperPatch(s, w.id, wd) : { wallpaper: w.id })} style={{ width: 72, height: 44, borderRadius: 8, overflow: 'hidden',
                border: curWall === w.id ? `2px solid ${t.accent}` : `1px solid ${t.separator}` }}>
                <Image src={wallpaperThumb(w)} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
              </View>
            ))}
          </View>
        </Tile>
        <View onClick={() => openSettings(onClose, 'appearance')} style={{ height: 34, borderRadius: 12, alignItems: 'center', justifyContent: 'center', backgroundColor: t.hover, border: `1px solid ${t.separator}` }}>
          <Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{tr('shell.menu.settings')}</Text>
        </View>
      </ScrollView>
    </Surface>
  );
}
