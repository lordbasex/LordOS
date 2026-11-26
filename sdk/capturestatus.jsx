// capturestatus.jsx — the capture indicator by the clock (2026-11-22): a green pill whose glyph says what is captured
// right now, system-wide — the screen, the camera, the microphone or two of them — and, clicked, which apps, each with
// its Stop (docs/PRIVACY.md, "What is captured now"). It replaced the red pill of each screen share and the camera and
// microphone dots. The logic is sdk/capturemodel.js; what it knows:
//   - the screen: the screen-sharing API (sdk/share.js), the ScreenCast and Screenshot portals (sdk/screencast.jsx);
//   - the camera and the microphone LordOS hands out itself: native apps (lord.media) and isolated .lpk apps (the
//     broker), brokerDevices in sdk/privacy.js;
//   - what Linux and Flatpak apps capture through PipeWire, as lord-server's PipeWire guard sees it (linuxCapture);
//   - the Linux apps reading LordOS Studio's camera device, which lord-server finds itself (studioUse): "Camera via
//     LordOS Studio (the real camera) · effects on".
//
// It is a security indicator: there is no setting to hide it, and every theme draws it where the privacy dots were
// (<PrivacyIndicators />, next to the status items; a Lord Classic taskbar on the side hides its status items but not
// this). A share's frames wait for it: lord-server sends none until an indicator is on screen (lord.share.shown,
// lord.portal.castShown), and the screen-sharing API's share ends if every indicator leaves. When a window is full screen
// the bars may be under it, so a small copy of the pill stays in the corner of each display (CaptureCorner).
import { useEffect, useRef, useState } from 'react';
import { View, Text, MediaView, screen } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { useTheme, popupBg } from './theme.js';
import { t, appName, direction } from './i18n.js';
import { Overlay } from './layers.js';
import { statusPanelAt, statusPanelRoom } from './statusitems.js';
import { ScrollView } from './components.jsx';
import { wm, windows, getApp } from './windows.js';
import { linuxAppFor, flatpakAppFor } from './apps.js';
import { share, shareHolder } from './share.js';
import { castRuns, castShown, castStop, shotTaken, castAppName } from './screencast.jsx';
import { perms, brokerDevices, linuxCapture, studioUse } from './privacy.js';
import { closeDevicesOf } from './media.js';
import { firewall } from './monitor.js';
import { componentOf } from './netmodel.js';
import { captureRows, captureGlyph, captureSummary, rowStops } from './capturemodel.js';
import { StudioPanel, MicModePanel, studioAvailable, useMicModeName, openStudioApp } from './studio.jsx';
import { studioWhat } from './studiomodel.js';

const GREEN = '#30d158';

// lord-server's ids of Linux apps, a running machine ("desktop:lordos-vm-<uuid>"), LordOS's components and LordOS itself
const isLinuxId = (id) => /^(desktop|flatpak):/.test(id || '') || id === 'lordos' || /^lordos:/.test(id || '');
const isMachine = (id) => /^desktop:lordos-vm-/.test(id || '');
const bare = (id) => (String(id || '').startsWith('lpk:') ? String(id).slice(4) : String(id || ''));
// "studio": LordOS Studio's own preview (sdk/studio.jsx), opened from this panel
const nameOf = (id) => (id === 'studio' ? t('studio.name') : isLinuxId(id) ? castAppName({ who: id }) : appName(getApp(bare(id))) || bare(id));
// Turn off is a firewall restriction of an app's own: never a machine's scope (Machines' own permission covers its
// microphone), a LordOS component or LordOS itself
const restrictable = (id) => /^(desktop|flatpak|lpk):/.test(id || '') && !isMachine(id) && firewall.enforceable(id);

// The rows, live
export function useCaptureRows() {
  const holder = useStore(shareHolder);
  const casts = useStore(castRuns);
  const shots = useStore(shotTaken);
  const devices = useStore(brokerDevices);
  const linux = useStore(linuxCapture);
  const studio = useStore(studioUse);
  return captureRows({ share: holder, casts, shots, devices, linux, studio }, { name: nameOf, enforceable: restrictable });
}

// ---- the handshake: frames only once an indicator is on screen ----
let mounted = 0;
function useShownHandshake() {
  const holder = useStore(shareHolder);
  const casts = useStore(castRuns);
  const token = holder ? holder.token : 0;
  useEffect(() => {
    mounted++;
    return () => {
      mounted--;
      // a bar rebuilt (a display plugged in) mounts its new indicator right after: only when none came back
      setTimeout(() => {
        if (mounted > 0 || typeof lord === 'undefined' || !lord.share) return;
        const st = lord.share.status?.();
        if (st && st.token) lord.share.hidden();
      }, 0);
    };
  }, []);
  useEffect(() => { if (token && typeof lord !== 'undefined' && lord.share) lord.share.shown(token); }, [token]);
  const castIds = casts.map((r) => r.id).join(',');
  useEffect(() => { for (const r of casts) castShown(r.id); }, [castIds]);
}

// ---- glyphs (our own shapes, drawn with views: docs/DESIGN.md, "Icons") ----
function DisplayGlyph({ color, size }) {
  const s = size;
  return (
    <View style={{ width: s, height: s, alignItems: 'center', direction: 'ltr' }}>
      <View style={{ marginTop: s * 0.1, width: s, height: s * 0.62, borderRadius: s * 0.1, border: `${Math.max(1.2, s * 0.1)}px solid ${color}` }} />
      <View style={{ width: s * 0.12, height: s * 0.12, backgroundColor: color }} />
      <View style={{ width: s * 0.5, height: Math.max(1.2, s * 0.1), borderRadius: 1, backgroundColor: color }} />
    </View>
  );
}

function MicGlyph({ color, size }) {
  const s = size, line = Math.max(1.2, s * 0.1);
  return (
    <View style={{ width: s, height: s, alignItems: 'center', direction: 'ltr' }}>
      <View style={{ width: s * 0.38, height: s * 0.6, borderRadius: s, backgroundColor: color }} />
      <View style={{ position: 'absolute', top: s * 0.3, width: s * 0.66, height: s * 0.42, borderBottomLeftRadius: s, borderBottomRightRadius: s,
        borderLeft: `${line}px solid ${color}`, borderRight: `${line}px solid ${color}`, borderBottom: `${line}px solid ${color}` }} />
      <View style={{ position: 'absolute', top: s * 0.72, width: line, height: s * 0.16, backgroundColor: color }} />
      <View style={{ position: 'absolute', top: s * 0.88, width: s * 0.4, height: line, borderRadius: 1, backgroundColor: color }} />
    </View>
  );
}

function CameraGlyph({ color, size }) {
  const s = size;
  return (
    <View style={{ width: s, height: s, flexDirection: 'row', alignItems: 'center', direction: 'ltr' }}>
      <View style={{ width: s * 0.66, height: s * 0.52, borderRadius: s * 0.12, backgroundColor: color }} />
      {/* the lens hood: a square turned half out of the body */}
      <View style={{ width: s * 0.34, height: s * 0.52, overflow: 'hidden' }}>
        <View style={{ position: 'absolute', left: -s * 0.2, top: s * 0.05, width: s * 0.42, height: s * 0.42, transform: 'rotate(45deg)', backgroundColor: color }} />
      </View>
    </View>
  );
}

const MAIN = { screen: DisplayGlyph, camera: CameraGlyph, mic: MicGlyph };

// glyph: captureGlyph's answer; the second part is a badge at the bottom end of the first ('screen-mic', 'camera-mic',
// 'screen-camera'); bg: the color behind, cut around the badge
export function CaptureGlyph({ glyph, color = '#fff', bg = GREEN, size = 13 }) {
  if (!glyph) return null;
  const [main, badge] = glyph.split('-');
  const Main = MAIN[main], Badge = badge ? MAIN[badge] : null;
  const b = size * 0.62;
  return (
    <View style={{ width: size, height: size, direction: 'ltr' }}>
      <Main color={color} size={size} />
      {Badge ? (
        <View style={{ position: 'absolute', right: -b * 0.35, bottom: -b * 0.2, width: b + 2, height: b + 2, borderRadius: 3, backgroundColor: bg, alignItems: 'center', justifyContent: 'center' }}>
          <Badge color={color} size={b} />
        </View>
      ) : null}
    </View>
  );
}

function Pill({ glyph, scale = 1 }) {
  return (
    <View style={{ width: 30 * scale, height: 18 * scale, borderRadius: 9 * scale, backgroundColor: GREEN, alignItems: 'center', justifyContent: 'center',
      boxShadow: '0 0.5px 1.5px rgba(0,0,0,0.25)' }}>
      <CaptureGlyph glyph={glyph} size={12 * scale} />
    </View>
  );
}

// what an assistive technology reads: "Screen and microphone in use"
function summaryLabel(rows) {
  const s = captureSummary(rows);
  const key = s.screen && s.camera ? 'screenCamera' : s.screen && s.mic ? 'screenMic' : s.screen ? 'screen' : s.camera && s.mic ? 'cameraMic' : s.camera ? 'camera' : 'mic';
  return t(`shell.capture.inUse.${key}`);
}

// ---- the panel ----
function whatText(r) {
  const parts = [];
  if (r.screen) parts.push(t(`shell.capture.what.${r.screen}`));
  if (r.camera && r.via) for (const p of studioWhat(r.via)) parts.push(t(p.key, p.params));
  else if (r.camera) parts.push(t('shell.capture.what.camera'));
  if (r.mic) parts.push(t('shell.capture.what.mic'));
  if (r.control) parts.push(t('shell.capture.what.control'));
  return parts.join(' · ');
}

function iconFor(id) {
  if (!id) return null;
  if (isMachine(id)) return getApp('machines')?.Icon || null;
  if (id === 'lordos' || id.startsWith('lordos:')) { const c = componentOf(id); return c && c.icon ? getApp(c.icon)?.Icon || null : null; }
  if (id.startsWith('desktop:')) return linuxAppFor(id.slice(8))?.Icon || null;
  if (id.startsWith('flatpak:')) { const a = flatpakAppFor(id.slice(8)); return a && a.flatpakId === id.slice(8) ? a.Icon : null; }
  return getApp(bare(id))?.Icon || null;
}

function stopScreen(stops) {
  for (const s of stops) {
    if (s.kind === 'share') share.stop();
    else if (s.kind === 'cast') castStop(s.id);
  }
}

// Turn off: the app's own permission for LordOS's camera and microphone (an isolated app loses them at once, a native
// app's sources close), the firewall's restriction for a Linux app's (lord-server's PipeWire guard closes its streams
// within a second and keeps them closed)
function turnOff({ own = '', perms: list = [], app = '', flags = [] }) {
  for (const p of list) {
    perms.set(own, p, 'deny');
    closeDevicesOf(own, p);
  }
  if (flags.length && app) {
    const cur = (firewall.state().restricted || {})[app] || [];
    firewall.setRestricted(app, [...new Set([...cur, ...flags])]);
  }
}

function Button({ th, label, onClick, danger }) {
  return (
    <View onClick={onClick} focusable accessibilityLabel={label} style={{ height: 22, paddingHorizontal: 9, borderRadius: 11, justifyContent: 'center',
      backgroundColor: danger ? 'rgba(255,69,58,0.16)' : th.control || 'rgba(127,127,127,0.2)' }}>
      <Text style={{ fontSize: 11, fontWeight: 600, color: danger ? '#ff453a' : th.text, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
}

function Row({ r, th }) {
  const Icon = iconFor(r.app);
  const glyph = captureGlyph([r]);
  const stops = rowStops(r);
  const base = r.name || t('shell.cast.unknownApp');
  // what of the app captures: "Machines — Windows 11 Pro"
  const name = r.details && r.details.length ? t('shell.capture.appDetail', { app: base, detail: r.details.join(', ') }) : base;
  const shotOnly = r.screen === 'shot' || r.screen === 'color';
  return (
    <View style={{ gap: 8 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
        <View style={{ width: 30, height: 30, borderRadius: 7, alignItems: 'center', justifyContent: 'center', backgroundColor: Icon ? 'transparent' : th.control || 'rgba(127,127,127,0.25)' }}>
          {Icon ? <Icon size={30} /> : <CaptureGlyph glyph={glyph} color={th.text} bg={th.control || '#3a3a40'} size={15} />}
        </View>
        <View style={{ flex: 1, minWidth: 0 }}>
          <Text style={{ fontSize: 13, fontWeight: 600, color: th.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{name}</Text>
          <Text style={{ fontSize: 11, color: th.textDim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{whatText(r)}</Text>
        </View>
      </View>
      {r.media ? (
        <View style={{ alignSelf: 'stretch', height: 150, borderRadius: 8, overflow: 'hidden', backgroundColor: '#000' }}>
          <MediaView media={r.media} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, objectFit: 'cover' }} />
        </View>
      ) : null}
      {(stops.screen.length && !shotOnly) || stops.off ? (
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', justifyContent: 'flex-end', gap: 6 }}>
          {stops.screen.length ? <Button th={th} danger label={t('shell.capture.stopSharing')} onClick={() => stopScreen(stops.screen)} /> : null}
          {stops.off ? <Button th={th} label={t('shell.capture.turnOff')} onClick={() => turnOff(stops.off)} /> : null}
        </View>
      ) : null}
    </View>
  );
}

// a LordOS Studio entry of the panel: its glyph, its name, what it is set to and a "›"
function StudioEntry({ th, glyph, label, detail, onClick }) {
  return (
    <View onClick={onClick} focusable accessibilityLabel={label} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, minHeight: 24 }}>
      <CaptureGlyph glyph={glyph} color={th.text} bg={th.control || '#3a3a40'} size={13} />
      <Text style={{ flexShrink: 0, fontSize: 13, color: th.text, textAlign: 'start' }}>{label}</Text>
      <Text style={{ flex: 1, minWidth: 0, fontSize: 12, color: th.textDim, textAlign: 'end', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{detail || ''}</Text>
      <Text style={{ fontSize: 15, color: th.textDim }}>{direction() === 'rtl' ? '‹' : '›'}</Text>
    </View>
  );
}

function MicEntry({ th, onClick }) {
  return <StudioEntry th={th} glyph="mic" label={t('studio.mic')} detail={useMicModeName()} onClick={onClick} />;
}

// Mic Mode is the default microphone (sdk/studiomodel.js micPlan): it needs LordOS's sound client and a microphone
const hasMic = () => typeof lord !== 'undefined' && !!lord.sound;

function Panel({ rows, th, room, onClose, onStudio, onMic }) {
  // a program LordOS can't name (the sound system's own processes it can)
  const unknown = rows.some((r) => !r.app && !r.sys && (r.camera || r.mic));
  // LordOS Studio (docs/CAMERA-STUDIO-PLAN.md 9.1), in this same panel: the camera's effects while an app uses the
  // camera, Mic Mode while one uses the microphone (both when both)
  const studio = rows.some((r) => r.camera) && studioAvailable();
  const mic = rows.some((r) => r.mic) && hasMic();
  return (
    <View style={{ width: 320, padding: 12, gap: 10, maxHeight: room }}>
      <Text style={{ fontSize: 11, fontWeight: 700, color: th.textDim, textAlign: 'start' }}>{t('shell.capture.title')}</Text>
      <ScrollView style={{ flexShrink: 1 }} contentStyle={{ gap: 12 }}>
        {rows.map((r) => <Row key={r.key} r={r} th={th} />)}
      </ScrollView>
      {unknown ? <Text style={{ fontSize: 11, color: th.textDim, lineHeight: 1.35, textAlign: 'start' }}>{t('shell.capture.unknownHint')}</Text> : null}
      {studio ? <StudioEntry th={th} glyph="camera" label={t('studio.open')} onClick={onStudio} /> : null}
      {mic ? <MicEntry th={th} onClick={onMic} /> : null}
      <View style={{ height: 1, backgroundColor: th.separator }} />
      <View onClick={() => { onClose(); wm.open('settings', { args: { section: 'privacy', nonce: Date.now() } }); }} focusable accessibilityLabel={t('shell.capture.settings')}>
        <Text style={{ fontSize: 12, color: th.accent, textAlign: 'start' }}>{t('shell.capture.settings')}</Text>
      </View>
    </View>
  );
}

// which bar's panel is open (with a bar on every display there is one item per bar): only the one clicked opens
const openAt = createStore(null);

// The item, in every theme's bar where the privacy dots were (the name stays: themes draw <PrivacyIndicators />)
export function PrivacyIndicators({ height = 22 }) {
  const th = useTheme();
  const rows = useCaptureRows();
  const glyph = captureGlyph(rows);
  const btn = useRef(null);
  const open = useStore(openAt) === btn;
  // the page of the popover: the panel (null), LordOS Studio's 'video' effects or its 'mic' mode
  const [studio, setStudio] = useState(null);
  useEffect(() => { if (!open) setStudio(null); }, [open]);
  const close = () => openAt.set(() => null);
  useEffect(() => { if (!glyph && openAt.get() === btn) openAt.set(() => null); }, [glyph]);
  if (!glyph) return null;
  return (
    <View ref={btn} onClick={() => openAt.set(() => (open ? null : btn))} focusable accessibilityLabel={summaryLabel(rows)} title={summaryLabel(rows)}
      style={{ height, paddingHorizontal: 4, alignItems: 'center', justifyContent: 'center' }}>
      <Handshake />
      <Pill glyph={glyph} />
      {open ? (
        <Overlay anchor={btn} onClose={() => openAt.set(() => null)}>
          <View style={{ ...statusPanelAt(btn), borderRadius: th.menuRadius || 10, backgroundColor: popupBg(th), border: `1px solid ${th.separator}`,
            boxShadow: '0 10px 30px rgba(0,0,0,0.35)' }}>
            {studio === 'video' ? <StudioPanel th={th} room={statusPanelRoom(btn)} onBack={() => setStudio(null)} onOpenApp={() => { close(); openStudioApp(); }} />
              : studio === 'mic' ? <MicModePanel th={th} room={statusPanelRoom(btn)} onBack={() => setStudio(null)} onOpenApp={() => { close(); openStudioApp(); }} />
                : <Panel rows={rows} th={th} room={statusPanelRoom(btn)} onClose={close} onStudio={() => setStudio('video')} onMic={() => setStudio('mic')} />}
          </View>
        </Overlay>
      ) : null}
    </View>
  );
}

function Handshake() { useShownHandshake(); return null; }

// Full screen: the bars may be under the window, so the pill stays in the top right corner of each display (no clicks:
// the window's own). Mounted in the notification layer, above full-screen windows. (0.8.9.6.2) With a community theme
// it is always there while something captures: LordOS draws it above every layer the theme draws in, whatever the
// theme shows or hides (sdk/themeapi.js, docs/THEMES-GUIDE.md "Community themes").
let cornerAlways = false;
export function captureCornerAlways() { cornerAlways = true; }
export function CaptureCorner() {
  const rows = useCaptureRows();
  const glyph = captureGlyph(rows);
  const full = useStore(windows, (l) => l.some((w) => w.fullscreen && !w.minimized));
  if (!glyph || !(full || cornerAlways)) return null;
  const list = screen.displays?.length ? screen.displays : [{ x: 0, y: 0, w: screen.width }];
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, pointerEvents: 'none' }} accessibilityLabel={summaryLabel(rows)}>
      <Handshake />
      {list.map((d, i) => (
        <View key={i} style={{ position: 'absolute', left: (d.x || 0) + (d.w || screen.width) - 34, top: (d.y || 0) + 4, opacity: 0.92 }}>
          <Pill glyph={glyph} scale={0.85} />
        </View>
      ))}
    </View>
  );
}
