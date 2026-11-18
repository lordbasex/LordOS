// The Control Center, the theme's Control Atlas (a popover from the spine's button, unfolding from it): a live preview
// of the desktop drawn with the current choices, then a section each for Appearance (dark mode, Reduce motion, the
// effects style Opaque / Translucent / Crystal, the accent, the wallpaper), Window tiling (the layout of the activity in
// view on this display, the padlock), Activities (each one with its window count, New activity, Edit), Displays (Night
// Light, brightness, the displays connected, the spine on every display), Sound (volume, Do Not Disturb), Network
// (Wi-Fi, Bluetooth), Energy (the power mode) and Security (the firewall's mode, Lock Screen, Privacy). Every change
// shows at once in the preview or on the desktop itself. Nothing here is a second copy of a device's settings: the
// controls call the SDK (sound, Bluetooth, Night Light, notifications, power profiles, the firewall), and each section's
// title opens its page in Settings. The Wi-Fi and firewall states are read once when it opens, nothing polls.
import {
  View, Text, Image, Slider, Segmented, ScrollView, useSettings, settings, wm, addListener, useSound, defaultSink, sound, useBluetooth, setBluetoothPower,
  nightActive, setNightManual, notifyPrefs, dndActive, useBacklight, wifiBars, findWallpaper, wallpaperThumb, wallpaperIdFor, wallpaperPatch, slideshowStop, screenDisplays, usePowerProfiles, admin, t as tr,
  MoonGlyph, SunGlyph, ContrastGlyph, BluetoothGlyph, SignalGlyph, useStore, desktopList, desktopName, activeDesktop, switchDesktop, windows, getApp, firewall, session,
} from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { Surface, Crystal } from './Surface.jsx';
import { SpeakerGlyph, Chevron, LayoutMini, LAYOUT_CELLS, LockGlyph, ActivityIcon, PlusGlyph } from '../glyphs.jsx';
import { wsPrefs, optPatch, SURFACES, effectiveSurface, MAX_ACTIVITIES } from '../model/activities.js';
import { LAYOUTS } from '../model/layouts.js';
import { layoutOf, setLayout, keyOfDisplay, isLocked, setLocked } from '../tiler.js';
import { activities, newActivity } from '../state.js';
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

// A section's title; a click opens its page in Settings.
function Head({ t, label, onClick }) {
  return (
    <View onClick={onClick} style={{ flexDirection: 'row', alignItems: 'center', gap: 4, marginTop: 6, paddingHorizontal: 2 }}>
      <Text style={{ fontSize: 11, fontWeight: 700, color: t.textDim, letterSpacing: 0.3, textAlign: 'start' }}>{label}</Text>
      {onClick ? <Chevron color={t.textDim} size={3.5} dir="right" /> : null}
    </View>
  );
}

// Window tiling: the layout of the activity in view on this display, and its padlock.
function TilingTile({ t, display }) {
  const desk = useStore(activeDesktop);
  useStore(activities);
  const key = keyOfDisplay(display);
  const cur = layoutOf(desk, key).layout;
  const locked = isLocked(desk, key);
  return (
    <Tile t={t} style={{ gap: 8 }}>
      <View style={{ flexDirection: 'row', gap: 6, direction: 'ltr' }}>
        {LAYOUTS.map((l) => (
          <View key={l} onClick={() => setLayout(desk, key, l)} title={tr(`shell.ws.layout.${l}`)}
            style={{ flex: 1, height: 34, borderRadius: 8, alignItems: 'center', justifyContent: 'center', backgroundColor: l === cur ? t.accent : t.control }}>
            <LayoutMini cells={LAYOUT_CELLS[l]} w={30} h={20} color={l === cur ? t.onAccent : t.text} fill={l === cur ? t.onAccent : t.text} active />
          </View>
        ))}
      </View>
      <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>{tr(`shell.ws.layout.${cur}`)}</Text>
      {cur !== 'free' ? (
        <View onClick={() => setLocked(desk, key, !locked)} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 2 }}>
          <LockGlyph color={locked ? t.accent : t.textDim} size={12} open={!locked} />
          <Text style={{ fontSize: 12, color: t.text, textAlign: 'start' }}>{tr(locked ? 'shell.ws.lock.off' : 'shell.ws.lock.on')}</Text>
        </View>
      ) : null}
    </Tile>
  );
}

const ownWin = (w) => !w.pip && !(getApp(w.id)?.builtin && !String(w.id).startsWith('linux#'));
// Activities: each one (its mark, name and windows; a click goes there), New activity and Edit.
function ActivitiesTile({ t, onClose, onEdit }) {
  const list = useStore(desktopList);
  const desk = useStore(activeDesktop);
  const meta = useStore(activities);
  const counts = JSON.parse(useStore(windows, (all) => { const c = {}; for (const w of all) if (ownWin(w)) c[w.desktop] = (c[w.desktop] || 0) + 1; return JSON.stringify(c); }));
  return (
    <Tile t={t} style={{ gap: 2, padding: 6 }}>
      {list.map((id) => {
        const m = meta[id] || { icon: 'spark', color: t.accent };
        return (
          <View key={id} onClick={() => switchDesktop(id)} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, height: 30, paddingHorizontal: 6, borderRadius: 8,
            backgroundColor: id === desk ? t.itemHover : 'transparent' }}>
            <View style={{ width: 18, height: 18, borderRadius: 5, backgroundColor: m.color, alignItems: 'center', justifyContent: 'center' }}>
              <ActivityIcon icon={m.icon} color="#ffffff" bg={m.color} size={10} />
            </View>
            <Text style={{ flex: 1, fontSize: 12, fontWeight: id === desk ? 700 : 500, color: t.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{desktopName(id)}</Text>
            <Text style={{ fontSize: 11, color: t.textDim }}>{counts[id] || 0}</Text>
          </View>
        );
      })}
      <View style={{ flexDirection: 'row', gap: 6, marginTop: 4 }}>
        <View onClick={() => { if (list.length < MAX_ACTIVITIES) newActivity(); }} style={{ flex: 1, height: 28, borderRadius: 8, flexDirection: 'row', gap: 6, alignItems: 'center', justifyContent: 'center',
          backgroundColor: t.control, opacity: list.length < MAX_ACTIVITIES ? 1 : 0.45 }}>
          <PlusGlyph color={t.text} size={10} />
          <Text style={{ fontSize: 11.5, fontWeight: 600, color: t.text }}>{tr('shell.ws.activity.new')}</Text>
        </View>
        <View onClick={() => { onClose(); onEdit(desk); }} style={{ flex: 1, height: 28, borderRadius: 8, alignItems: 'center', justifyContent: 'center', backgroundColor: t.control }}>
          <Text style={{ fontSize: 11.5, fontWeight: 600, color: t.text }}>{tr('shell.ws.activity.editTitle')}</Text>
        </View>
      </View>
    </Tile>
  );
}

// Displays: Night Light and brightness, then the displays connected and whether the spine shows on each.
function DisplaysTile({ t, s, set, onClose, bl, night }) {
  const list = screenDisplays();
  return (
    <>
      <View style={{ flexDirection: 'row', gap: 8 }}>
        <Switch t={t} on={night} label={tr('shell.classic.nightLight')} glyph={(c) => <SunGlyph color={c} size={14} />}
          onToggle={() => setNightManual(!night)} onMore={() => openSettings(onClose, 'displays')} />
      </View>
      <Tile t={t} style={{ gap: 8 }}>
        {bl ? (
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, direction: 'ltr' }}>
            <View style={{ width: 24, alignItems: 'center' }}><SunGlyph color={t.textDim} size={13} /></View>
            <Slider value={bl.fraction} width={250} onChange={bl.set} />
            <Text style={{ fontSize: 11, color: t.textDim, width: 38, textAlign: 'end' }}>{`${Math.round(bl.fraction * 100)} %`}</Text>
          </View>
        ) : null}
        {list.map((d) => (
          <View key={d.id} style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
            <View style={{ width: 18, height: 12, borderRadius: 2, border: `1.5px solid ${d.main ? t.accent : t.textDim}` }} />
            <Text style={{ flex: 1, fontSize: 12, color: t.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis', textAlign: 'start' }}>{[d.name, d.connector].filter(Boolean).join(' · ') || String(d.id)}</Text>
            <Text style={{ fontSize: 11, color: t.textDim, direction: 'ltr' }}>{`${d.w}×${d.h}`}</Text>
          </View>
        ))}
        {list.length > 1 ? (
          <View onClick={() => set(optPatch(settings.get(), 'spineEvery', !wsPrefs(s).spineEvery))} style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
            <View style={{ width: 16, height: 16, borderRadius: 4, alignItems: 'center', justifyContent: 'center', backgroundColor: wsPrefs(s).spineEvery ? t.accent : t.control }}>
              {wsPrefs(s).spineEvery ? <Text style={{ fontSize: 11, fontWeight: 700, color: t.onAccent }}>✓</Text> : null}
            </View>
            <Text style={{ flex: 1, fontSize: 12, color: t.text, textAlign: 'start' }}>{tr('shell.ws.opt.spineEvery')}</Text>
          </View>
        ) : null}
      </Tile>
    </>
  );
}

// Security: the firewall's mode (read when the panel opens), Lock Screen.
function SecurityTile({ t }) {
  const [mode, setMode] = useState(() => (firewall.available() ? firewall.state().mode || 'silent' : null));
  const managed = session.managed();
  if (!mode && !managed) return null;
  return (
    <Tile t={t} style={{ gap: 8 }}>
      {mode ? (
        <>
          <Text style={{ fontSize: 12, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('monitor.status.mode')}</Text>
          <Segmented small value={mode === 'ask' ? 'silent' : mode} onChange={(m) => { if (firewall.setMode(m) !== false) setMode(m); }}
            options={[['silent', tr('firewall.mode.silent')], ['deny', tr('firewall.mode.deny')], ['off', tr('firewall.mode.off')]]} />
          <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>
            {tr(mode === 'deny' ? 'firewall.mode.denyHint' : mode === 'off' ? 'firewall.mode.offHint' : 'firewall.mode.silentHint')}</Text>
        </>
      ) : null}
      {managed ? (
        <View onClick={() => session.lock()} style={{ height: 28, borderRadius: 8, flexDirection: 'row', gap: 6, alignItems: 'center', justifyContent: 'center', backgroundColor: t.control }}>
          <LockGlyph color={t.text} size={12} />
          <Text style={{ fontSize: 11.5, fontWeight: 600, color: t.text }}>{tr('shell.menu.lock')}</Text>
        </View>
      ) : null}
    </Tile>
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
export function ControlCenter({ t, anchor, area, rtl, onClose, isOnChrome, onEdit = () => {} }) {
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
  const curWall = wallpaperIdFor(s, wd, t.defaultWallpaper);
  const quick = [findWallpaper(t.wallpapers, curWall, t.defaultWallpaper), ...QUICK.map((id) => t.wallpapers.find((w) => w.id === id))].filter((w, i, a) => w && a.indexOf(w) === i).slice(0, 4);
  return (
    <Surface ref={ref} t={t} radius={20} style={{ position: 'absolute', left, top, width: CC_W, height, transformOrigin: rtl ? 'right bottom' : 'left bottom',
      backgroundColor: t.panelBg, ...(t.panelBlur ? { backdropFilter: `blur(${t.panelBlur}px)` } : null) }}>
      <ScrollView style={{ flex: 1 }} contentStyle={{ padding: 12, gap: 10 }}>
        <Preview t={t} accent={t.accent} />
        <Head t={t} label={tr('settings.section.appearance')} onClick={() => openSettings(onClose, 'appearance')} />
        <View style={{ flexDirection: 'row', gap: 8 }}>
          <Switch t={t} on={s.mode === 'dark'} label={tr('shell.cc.dark')} glyph={(c) => <ContrastGlyph color={c} size={14} />}
            onToggle={() => set({ mode: s.mode === 'dark' ? 'light' : 'dark' })} onMore={() => openSettings(onClose, 'appearance')} />
          <Switch t={t} on={!!s.reduceMotion} label={tr('shell.ws.cc.reduceMotion')} glyph={(c) => <Chevron color={c} size={5} dir="right" />}
            onToggle={() => set({ reduceMotion: !s.reduceMotion })} />
        </View>
        <View style={{ flexDirection: 'row', gap: 8 }}>
          <Switch t={t} on={prefs.contrast} label={tr('shell.ws.opt.contrast')} glyph={(c) => <ContrastGlyph color={c} size={14} />}
            onToggle={() => set(optPatch(settings.get(), 'contrast', !prefs.contrast))} />
        </View>
        {prefs.contrast ? null : <Tile t={t} style={{ gap: 8 }}>
          <Text style={{ fontSize: 12, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('shell.ws.surface.title')}</Text>
          <Segmented small value={surface} options={SURFACES.map((x) => [x, tr(`shell.ws.surface.${x}`)])}
            onChange={(v) => set({ ...optPatch(settings.get(), 'surface', v), ...(v !== 'opaque' && s.effects === false ? { effects: true } : null) })} />
          <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>{tr(`shell.ws.surface.${surface}Hint`)}</Text>
        </Tile>}
        <Tile t={t} style={{ gap: 8 }}>
          <View onClick={() => openSettings(onClose, 'appearance')}><Text style={{ fontSize: 12, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('shell.cc.accent')}</Text></View>
          <View style={{ flexDirection: 'row', gap: 10, direction: 'ltr' }}>
            {Object.entries(t.accents).map(([name, c]) => (
              <View key={name} onClick={() => set({ accent: name })} style={{ width: 22, height: 22, borderRadius: 11, backgroundColor: c,
                border: (s.accent || t.accentName) === name ? `2px solid ${t.text}` : '0 solid transparent' }} />
            ))}
          </View>
        </Tile>
        <Tile t={t} style={{ gap: 8 }}>
          <View onClick={() => openSettings(onClose, 'wallpaper')}><Text style={{ fontSize: 12, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('shell.cc.wallpaper')}</Text></View>
          <View style={{ flexDirection: 'row', gap: 8 }}>
            {quick.map((w) => (
              <View key={w.id} onClick={() => set({ ...(wd ? wallpaperPatch(s, w.id, wd) : { wallpaper: w.id }), ...slideshowStop(s) })} style={{ width: 72, height: 44, borderRadius: 8, overflow: 'hidden',
                border: curWall === w.id ? `2px solid ${t.accent}` : `1px solid ${t.separator}` }}>
                <Image src={wallpaperThumb(w)} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
              </View>
            ))}
          </View>
        </Tile>
        <Head t={t} label={tr('shell.ws.atlas.tiling')} onClick={() => openSettings(onClose, 'dock')} />
        <TilingTile t={t} display={area} />
        <Head t={t} label={tr('shell.ws.spine.activities')} onClick={() => openSettings(onClose, 'dock')} />
        <ActivitiesTile t={t} onClose={onClose} onEdit={onEdit} />
        <Head t={t} label={tr('settings.section.displays')} onClick={() => openSettings(onClose, 'displays')} />
        <DisplaysTile t={t} s={s} set={set} onClose={onClose} bl={bl} night={night} />
        <Head t={t} label={tr('settings.section.sound')} onClick={() => openSettings(onClose, 'sound')} />
        <View style={{ flexDirection: 'row', gap: 8 }}>
          <Switch t={t} on={dnd} label={tr('shell.classic.dnd')} glyph={(c, bg) => <MoonGlyph color={c} bg={bg} size={14} />}
            onToggle={() => settings.set((x) => ({ ...x, notifications: { ...notifyPrefs(x), dnd: !dnd } }))} onMore={() => openSettings(onClose, 'notifications')} />
        </View>
        {soundOn ? <Tile t={t} style={{ gap: 10 }}><SoundRow t={t} /></Tile> : null}
        {radios.length || bt?.adapter ? <Head t={t} label={tr('settings.section.network')} onClick={() => openSettings(onClose, 'network')} /> : null}
        {radios.length || bt?.adapter ? (
          <View style={{ flexDirection: 'row', gap: 8 }}>
            {radios.length ? <Switch t={t} on={false} label={tr('shell.cc.wifi')} detail={wifi ? wifi.name : tr('shell.cc.notConnected')}
              glyph={(c) => <SignalGlyph color={c} size={14} level={wifi ? wifiBars(wifi.signal) : 0} off={!wifi} />}
              onToggle={() => openSettings(onClose, 'network', 'wifi')} onMore={() => openSettings(onClose, 'network', 'wifi')} /> : null}
            {bt?.adapter ? <Switch t={t} on={!!bt.adapter.powered} label={tr('shell.cc.bluetooth')} glyph={(c) => <BluetoothGlyph color={c} size={14} />}
              onToggle={() => setBluetoothPower(!bt.adapter.powered)} onMore={() => openSettings(onClose, 'bluetooth')} /> : null}
          </View>
        ) : null}
        <PowerRow t={t} onClose={onClose} />
        {firewall.available() || session.managed() ? <Head t={t} label={tr('shell.ws.atlas.security')} onClick={() => openSettings(onClose, 'privacy')} /> : null}
        <SecurityTile t={t} />
        <View onClick={() => openSettings(onClose, 'appearance')} style={{ height: 34, borderRadius: 12, alignItems: 'center', justifyContent: 'center', backgroundColor: t.hover, border: `1px solid ${t.separator}` }}>
          <Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{tr('shell.menu.settings')}</Text>
        </View>
      </ScrollView>
    </Surface>
  );
}
