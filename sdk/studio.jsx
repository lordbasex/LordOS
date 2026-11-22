// studio.jsx — LordOS Studio's popover (docs/CAMERA-STUDIO-PLAN.md 9.1; phase 1): a live preview, the camera it uses,
// Portrait (the background blurred) and Background (a picture behind the person), and Mirror. It opens from the camera
// capture item by the clock (sdk/capturestatus.jsx) in every theme, in the same panel and style. The effects reach
// every LordOS app that chose the "LordOS Studio" camera (lord.media.cameras(), device "studio": the Camera app,
// isolated .lpk apps, go-link Desk), all at once; lord-server runs them in the sandboxed lord-studio helper.
// Plain logic: sdk/studiomodel.js. Settings: settings.studio, applied to lord-server at start and on every change.
import { useEffect, useRef, useState } from 'react';
import { View, Text, Image, MediaView } from '@lordos/runtime';
import { settings } from './settings.js';
import { useStore } from './store.js';
import { t, appName, direction } from './i18n.js';
import { Toggle, Slider, Segmented, ScrollView } from './components.jsx';
import { Layer } from './layers.js';
import { FilePicker } from './filepicker.jsx';
import { media } from './media.js';
import { markDeviceUse } from './privacy.js';
import { getApp } from './windows.js';
import { lordosWallpapers } from './wallpapers.js';
import {
  normalizeStudio, realCameras, chosenCamera, cameraKey, serverConfig, portraitOn, backgroundOn, withPortrait, withBackground,
  withPicture, withMode, previewSize, usersOf, previewSource, perfLine,
} from './studiomodel.js';

const has = () => typeof lord !== 'undefined' && !!lord.studio;
const IMAGES = /\.(jpe?g|png|webp)$/i;
const cameras = () => { try { return media.cameras(); } catch (e) { return []; } };

// ---- settings → lord-server (the desktop's own process only: lord.studio exists nowhere else) ----
let applied = null;
function apply(s) {
  if (!has()) return;
  const cfg = normalizeStudio(s.studio);
  const key = JSON.stringify(cfg);
  if (key === applied) return;
  const before = applied ? JSON.parse(applied) : null;
  applied = key;
  try {
    lord.studio.set(serverConfig(cfg, cameras()));
    if (!before || before.background !== cfg.background) lord.studio.setBackground(cfg.background);
  } catch (e) { console.warn('studio:', e?.message || e); }
}
if (has()) {
  apply(settings.get());
  settings.subscribe(() => apply(settings.get()));
}
const update = (fn) => settings.set((s) => ({ ...s, studio: fn(normalizeStudio(s.studio)) }));

function useStatus() {
  const [st, setSt] = useState(() => (has() ? lord.studio.status() : null));
  useEffect(() => {
    if (!has()) return undefined;
    const tm = setInterval(() => setSt(lord.studio.status()), 1000);
    return () => clearInterval(tm);
  }, []);
  return st;
}

// The preview: a native app's LordOS Studio source when one runs (no second open of the camera), else the popover's own,
// closed when the popover closes. The capture indicator shows the popover's own like any camera.
function usePreview(status) {
  const shared = previewSource(status);
  const [own, setOwn] = useState(null);
  const [err, setErr] = useState(null);
  const ref = useRef({ id: null, off: null });
  useEffect(() => {
    if (shared || ref.current.id || !has() || !status?.available) return;
    let id = null;
    try {
      const { width, height } = previewSize({ arch: typeof lord.sys?.arch === 'function' ? lord.sys.arch() : '', gpu: status.gpu !== false });
      id = lord.studio.preview(width, height);
    } catch (e) { setErr(String(e?.code || e?.message || e)); return; }
    ref.current.id = id;
    ref.current.off = media.on(id, (type, d) => {
      if (type === 'error') setErr(d?.message || 'busy');
      else if (type === 'closed' || (type === 'state' && d.state === 'failed')) setErr((e) => e || 'busy');
    });
    markDeviceUse('studio', 'camera', true, id);
    setOwn(id);
  }, [shared, status?.available]);
  useEffect(() => () => {
    const r = ref.current;
    if (!r.id) return;
    if (r.off) r.off();
    media.close(r.id);
    markDeviceUse('studio', 'camera', false);
    r.id = null;
  }, []);
  const id = shared || own;
  useEffect(() => { if (id) media.view(id, { mirror: 1, fit: 'cover', background: 0xff000000 }); }, [id]);
  return { id, err: shared ? null : err };
}

function Chevron({ color }) {
  return <Text style={{ fontSize: 15, color, width: 12, textAlign: 'center' }}>{direction() === 'rtl' ? '‹' : '›'}</Text>;
}

function Header({ th, title, onBack }) {
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, minHeight: 18 }}>
      {onBack ? (
        <View onClick={onBack} focusable accessibilityLabel={t('studio.back')} style={{ width: 18, height: 18, alignItems: 'center', justifyContent: 'center' }}>
          <Text style={{ fontSize: 15, color: th.accent }}>{direction() === 'rtl' ? '›' : '‹'}</Text>
        </View>
      ) : null}
      <Text style={{ flex: 1, fontSize: 11, fontWeight: 700, color: th.textDim, textAlign: 'start' }}>{title}</Text>
    </View>
  );
}

function Row({ th, label, detail, toggle, onToggle, onOpen }) {
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, minHeight: 26 }}>
      <View onClick={onOpen} focusable={!!onOpen} accessibilityLabel={label} style={{ flex: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        <Text style={{ flexShrink: 0, fontSize: 13, color: th.text, textAlign: 'start' }}>{label}</Text>
        {detail ? <Text style={{ flex: 1, minWidth: 0, fontSize: 12, color: th.textDim, textAlign: 'end', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{detail}</Text>
          : <View style={{ flex: 1 }} />}
      </View>
      {onToggle ? <Toggle small on={!!toggle} onChange={onToggle} /> : null}
      {onOpen ? <View onClick={onOpen}><Chevron color={th.textDim} /></View> : null}
    </View>
  );
}

const Sep = ({ th }) => <View style={{ height: 1, backgroundColor: th.separator }} />;
const Hint = ({ th, children }) => <Text style={{ fontSize: 11, color: th.textDim, lineHeight: 1.35, textAlign: 'start' }}>{children}</Text>;

function Preview({ th, status, pv }) {
  const { id, err } = pv;
  const perf = perfLine(status);
  const msg = !status?.available ? t('studio.unavailable') : err ? (/noCamera/.test(err) ? t('studio.noCamera') : t('studio.busy')) : !id ? t('studio.starting') : null;
  return (
    <View style={{ gap: 4 }}>
      {/* the mouse's x means the same on screen whatever the language's direction (the eyedropper, later) */}
      <View style={{ alignSelf: 'stretch', height: 166, borderRadius: 8, overflow: 'hidden', backgroundColor: '#000', direction: 'ltr', alignItems: 'center', justifyContent: 'center' }}>
        {id && !err ? <MediaView media={id} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, objectFit: 'cover' }} /> : null}
        {msg ? <Text style={{ fontSize: 12, color: 'rgba(235,235,245,0.75)', textAlign: 'center', paddingHorizontal: 16 }}>{msg}</Text> : null}
      </View>
      {perf ? <Text style={{ fontSize: 10, color: th.textDim, textAlign: 'end' }}>{t(perf.gpu ? 'studio.perfGpu' : 'studio.perfCpu', { fps: perf.fps })}</Text> : null}
    </View>
  );
}

function MainPage({ th, cfg, status, pv, go, onBack }) {
  const cams = realCameras(cameras());
  const cam = chosenCamera(cfg.camera, cams);
  const users = usersOf(status, (a) => (a === 'studio' ? '' : appName(getApp(a)) || a));
  return (
    <View style={{ gap: 10 }}>
      <Header th={th} title={t('studio.title')} onBack={onBack} />
      <Preview th={th} status={status} pv={pv} />
      <Row th={th} label={t('studio.title')} detail={cam ? cam.name : t('studio.noCamera')} onOpen={cams.length > 1 ? () => go('camera') : null} />
      <Row th={th} label={t('studio.inUse')} detail={users.length ? users.join(' · ') : t('studio.nobody')} />
      <Sep th={th} />
      <Row th={th} label={t('studio.portrait')} toggle={portraitOn(cfg)} onToggle={(v) => update((c) => withPortrait(c, v))} onOpen={() => go('portrait')} />
      <Row th={th} label={t('studio.background')} toggle={backgroundOn(cfg)} onToggle={(v) => update((c) => withBackground(c, v, lordosWallpapers))}
        onOpen={() => go('background')} />
      <Row th={th} label={t('studio.mirror')} toggle={cfg.mirror} onToggle={(v) => update((c) => ({ ...c, mirror: !!v }))} />
      <Sep th={th} />
      <Hint th={th}>{t('studio.howTo')}</Hint>
    </View>
  );
}

function CameraPage({ th, cfg, go }) {
  const cams = realCameras(cameras());
  const cur = chosenCamera(cfg.camera, cams);
  return (
    <View style={{ gap: 8 }}>
      <Header th={th} title={t('studio.title')} onBack={() => go('main')} />
      {cams.map((c) => (
        <View key={c.device} onClick={() => update((x) => ({ ...x, camera: cameraKey(c) }))} focusable accessibilityLabel={c.name}
          style={{ flexDirection: 'row', alignItems: 'center', gap: 8, minHeight: 26 }}>
          <Text style={{ width: 14, fontSize: 13, color: th.accent }}>{cur && cur.device === c.device ? '✓' : ''}</Text>
          <Text style={{ flex: 1, minWidth: 0, fontSize: 13, color: th.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{c.name}</Text>
        </View>
      ))}
      <Hint th={th}>{t('studio.cameraHint')}</Hint>
    </View>
  );
}

function PortraitPage({ th, cfg, go }) {
  return (
    <View style={{ gap: 10 }}>
      <Header th={th} title={t('studio.portrait')} onBack={() => go('main')} />
      <Row th={th} label={t('studio.portrait')} toggle={portraitOn(cfg)} onToggle={(v) => update((c) => withPortrait(c, v))} />
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
        <Text style={{ flexShrink: 0, fontSize: 12, color: th.textDim, textAlign: 'start' }}>{t('studio.blurAmount')}</Text>
        <Slider value={cfg.blur} min={0} max={100} step={5} width={190} onChange={(v) => update((c) => ({ ...c, blur: Math.round(v) }))} />
      </View>
      <Hint th={th}>{t('studio.portraitHint')}</Hint>
    </View>
  );
}

function BackgroundPage({ th, cfg, go }) {
  const [picking, setPicking] = useState(false);
  const pics = lordosWallpapers.filter((w) => w.category !== 'gradients');
  return (
    <View style={{ gap: 10 }}>
      <Header th={th} title={t('studio.background')} onBack={() => go('main')} />
      <Segmented small fill value={cfg.mode} onChange={(m) => update((c) => withMode(c, m))}
        options={[['none', t('studio.none')], ['blur', t('studio.blur')], ['picture', t('studio.picture'), !cfg.background]]} />
      <Text style={{ fontSize: 11, fontWeight: 600, color: th.textDim, textAlign: 'start' }}>{t('studio.pictures')}</Text>
      <ScrollView style={{ maxHeight: 190 }} contentStyle={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6 }}>
        {pics.map((w) => {
          const on = cfg.mode === 'picture' && cfg.background === w.src;
          return (
            <View key={w.id} onClick={() => update((c) => withPicture(c, w.src))} focusable accessibilityLabel={t(w.nameKey)}
              style={{ width: 92, height: 52, borderRadius: 6, overflow: 'hidden', border: `2px solid ${on ? th.accent : 'transparent'}` }}>
              <Image src={w.thumb} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
            </View>
          );
        })}
      </ScrollView>
      <View onClick={() => setPicking(true)} focusable accessibilityLabel={t('studio.choose')}>
        <Text style={{ fontSize: 12, color: th.accent, textAlign: 'start' }}>{t('studio.choose')}</Text>
      </View>
      <Hint th={th}>{t('studio.backgroundHint')}</Hint>
      {picking ? (
        <Layer name="system">
          <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.35)' }}>
            <View style={{ position: 'relative', width: 880, maxWidth: '96%', height: 580, maxHeight: '94%' }}>
              <FilePicker modal title={t('studio.pick')} filter={IMAGES} onClose={() => setPicking(false)}
                onPick={(p) => { const path = Array.isArray(p) ? p[0] : p; setPicking(false); if (path) update((c) => withPicture(c, path)); }} />
            </View>
          </View>
        </Layer>
      ) : null}
    </View>
  );
}

// The popover's content; onBack: back to the capture panel it opened from. The preview lives as long as the popover,
// whatever page shows, so the camera isn't opened again on the way back.
export function StudioPanel({ th, room, onBack }) {
  const s = useStore(settings);
  const cfg = normalizeStudio(s.studio);
  const status = useStatus();
  const pv = usePreview(status);
  const [page, go] = useState('main');
  const P = { camera: CameraPage, portrait: PortraitPage, background: BackgroundPage }[page];
  return (
    <View style={{ width: 320, padding: 12, maxHeight: room }}>
      <ScrollView style={{ flexShrink: 1 }}>
        {P ? <P th={th} cfg={cfg} status={status} go={go} /> : <MainPage th={th} cfg={cfg} status={status} pv={pv} go={go} onBack={onBack} />}
      </ScrollView>
    </View>
  );
}

// Is LordOS Studio there at all (lord-studio and its model installed)?
export function studioAvailable() {
  try { return has() && !!lord.studio.status().available; } catch (e) { return false; }
}
