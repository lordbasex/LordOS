// studio.jsx — LordOS Studio's popover (docs/CAMERA-STUDIO-PLAN.md 9.1; phases 1 to 3): a live preview, the camera it
// uses, Portrait (the background blurred), Background (a picture or a short loop behind the person, with a real green
// screen if there is one, and the colours matched), Face Light, Auto Framing, Filters, Retouch, Mirror, Mic Mode
// (Standard, Noise Reduction, Full Range) and "Every app": the "LordOS Studio" camera that browsers, OBS and every
// other Linux app see once the Store's LordOS Studio Camera item (the lordos-studio-camera package, v4l2loopback) is
// installed. It opens from the capture item by the clock (sdk/capturestatus.jsx) in every theme, in the same panel and
// style: "Video effects" while an app uses the camera, Mic Mode alone (MicModePanel, no preview) while one uses the
// microphone. The LordOS Studio app (apps/studio, StudioWindow) shows the same controls in a window of their own, any
// time, and shares the preview with the popover (one open of the camera). The effects reach every app that chose the "LordOS Studio" camera (lord.media.cameras(), device
// "studio": the Camera app, isolated .lpk apps, go-link Desk; and /dev/video50 for Linux apps), all at once; lord-server
// runs them in the sandboxed lord-studio helper. Each row's "›" opens its detail page in the same popover.
// Plain logic: sdk/studiomodel.js. Settings: settings.studio, applied to lord-server at start and on every change.
// The background catalog and its downloads: sdk/studiobg.js.
import { useEffect, useState } from 'react';
import { View, Text, Image, MediaView } from '@lordos/runtime';
import { settings } from './settings.js';
import { createStore, useStore } from './store.js';
import { t, appName, direction } from './i18n.js';
import { Toggle, Slider, Segmented, ScrollView } from './components.jsx';
import { Layer } from './layers.js';
import { FilePicker } from './filepicker.jsx';
import { media } from './media.js';
import { markDeviceUse } from './privacy.js';
import { getApp, wm } from './windows.js';
import { castAppName } from './screencast.jsx';
import { lordosWallpapers } from './wallpapers.js';
import { sound, soundRaw, useSound } from './sound.js';
import { studioCatalog, bgDownloads, fetchBackground, removeBackground } from './studiobg.js';
import {
  normalizeStudio, realCameras, chosenCamera, cameraKey, serverConfig, portraitOn, backgroundOn, withPortrait, withBackground,
  withPicture, withMode, previewSize, usersOf, previewSource, perfLine, loopbackState, activeBackground, STUDIO_FILTERS,
  BG_CATEGORIES, bgItems, bgThumb, bgReady, bgCredit, bgAi, bgItemOf, micModeOf, micPlan, voiceAvailable, hardwareMics, MATCH_MAX,
} from './studiomodel.js';

const has = () => typeof lord !== 'undefined' && !!lord.studio;
const FILES = /\.(jpe?g|png|webp|mp4|webm|mkv|mov|m4v)$/i;
const cameras = () => { try { return media.cameras(); } catch (e) { return []; } };

// ---- settings → lord-server (the desktop's own process only: lord.studio exists nowhere else) ----
// Runs on every settings change: when LordOS Studio's own settings are as last applied it stops there, and the cameras
// are listed only when one was chosen. lord.media.cameras opens every /dev/video* (60–70 ms with two webcams), and
// calling it on each settings write added that pause to every switch in Settings (2026-11-23, docs/MAIN-THREAD.md).
let applied = null, appliedBg, appliedCfg = null;
function apply(s) {
  if (!has()) return;
  const cfg = normalizeStudio(s.studio);
  const ck = JSON.stringify(cfg);
  if (ck === appliedCfg) return;
  appliedCfg = ck;
  const server = serverConfig(cfg, cfg.camera ? cameras() : []);
  const key = JSON.stringify(server);
  const bg = activeBackground(cfg);
  if (key === applied && bg === appliedBg) return;
  try {
    if (key !== applied) lord.studio.set(server);
    if (bg !== appliedBg) lord.studio.setBackground(bg);
  } catch (e) { console.warn('studio:', e?.message || e); }
  applied = key;
  appliedBg = bg;
}
if (has()) {
  apply(settings.get());
  settings.subscribe(() => apply(settings.get()));
}
const update = (fn) => settings.set((s) => ({ ...s, studio: fn(normalizeStudio(s.studio)) }));
const set = (patch) => update((c) => ({ ...c, ...patch }));

function useStatus() {
  const [st, setSt] = useState(() => (has() ? lord.studio.status() : null));
  useEffect(() => {
    if (!has()) return undefined;
    const tm = setInterval(() => setSt(lord.studio.status()), 1000);
    return () => clearInterval(tm);
  }, []);
  return st;
}

// The preview: a native app's LordOS Studio source when one runs (no second open of the camera), else one of the
// desktop's own, shared by every Studio view on screen (the capture popover and the LordOS Studio app's window) and
// closed when the last of them goes: the frames of the loopback's writer while it runs (lord.studio.endPreview lets go
// of them; it isn't ours to close), else a source of its own. The capture indicator shows it like any camera.
const ownPreview = createStore({ id: null, err: null });
let previewRefs = 0, previewOff = null, previewHub = false;
function openPreview(status) {
  if (ownPreview.get().id || !has() || !status?.available) return;
  let id = null;
  try {
    const { width, height } = previewSize({ arch: typeof lord.sys?.arch === 'function' ? lord.sys.arch() : '', gpu: status.gpu !== false });
    id = lord.studio.preview(width, height);
  } catch (e) { ownPreview.set({ id: null, err: String(e?.code || e?.message || e) }); return; }
  if (!id) return;
  previewHub = id === status.hub || id === lord.studio.status()?.hub;
  previewOff = media.on(id, (type, d) => {
    if (type === 'error') ownPreview.set({ err: d?.message || 'busy' });
    else if (type === 'closed' || (type === 'state' && d.state === 'failed')) ownPreview.set((p) => ({ ...p, err: p.err || 'busy' }));
  });
  markDeviceUse('studio', 'camera', true, id);
  ownPreview.set({ id, err: null });
}
function closePreview() {
  const { id } = ownPreview.get();
  if (id) {
    if (previewOff) previewOff();
    if (previewHub) lord.studio.endPreview();
    else media.close(id);
    markDeviceUse('studio', 'camera', false);
  }
  previewOff = null;
  previewHub = false;
  ownPreview.set({ id: null, err: null });
}
function usePreview(status) {
  const shared = previewSource(status);
  const own = useStore(ownPreview);
  useEffect(() => {
    previewRefs++;
    return () => { if (--previewRefs === 0) closePreview(); };
  }, []);
  useEffect(() => { if (!shared) openPreview(status); }, [shared, status?.available]);
  const id = shared || own.id;
  useEffect(() => { if (id) media.view(id, { mirror: 1, fit: 'cover', background: 0xff000000 }); }, [id]);
  return { id, err: shared ? null : own.err };
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

// a labelled slider; the labels at its ends when given (wide ↔ tight)
function SliderRow({ th, label, value, min = 0, max = 100, step = 5, onChange, from, to }) {
  return (
    <View style={{ gap: 2 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
        <Text style={{ flexShrink: 0, minWidth: 70, fontSize: 12, color: th.textDim, textAlign: 'start' }}>{label}</Text>
        <Slider value={value} min={min} max={max} step={step} width={190} onChange={(v) => onChange(Math.round(v))} />
      </View>
      {from || to ? (
        <View style={{ flexDirection: 'row', paddingStart: 80 }}>
          <Text style={{ flex: 1, fontSize: 10, color: th.textDim, textAlign: 'start' }}>{from}</Text>
          <Text style={{ flex: 1, fontSize: 10, color: th.textDim, textAlign: 'end' }}>{to}</Text>
        </View>
      ) : null}
    </View>
  );
}

const Sep = ({ th }) => <View style={{ height: 1, backgroundColor: th.separator }} />;
const Hint = ({ th, children }) => <Text style={{ fontSize: 11, color: th.textDim, lineHeight: 1.35, textAlign: 'start' }}>{children}</Text>;
const Link = ({ th, label, onClick }) => (
  <View onClick={onClick} focusable accessibilityLabel={label}>
    <Text style={{ fontSize: 12, color: th.accent, textAlign: 'start' }}>{label}</Text>
  </View>
);

// big: the app's window, where the preview takes the room it has
function Preview({ th, status, pv, big }) {
  const { id, err } = pv;
  const perf = perfLine(status);
  const msg = !status?.available ? t('studio.unavailable') : err ? (/noCamera/.test(err) ? t('studio.noCamera') : t('studio.busy')) : !id ? t('studio.starting') : null;
  return (
    <View style={{ gap: 4, ...(big ? { flex: 1, minHeight: 0 } : null) }}>
      {/* the mouse's x means the same on screen whatever the language's direction (the eyedropper, later) */}
      <View style={{ alignSelf: 'stretch', ...(big ? { flex: 1, minHeight: 180 } : { height: 166 }), borderRadius: 8, overflow: 'hidden', backgroundColor: '#000',
        direction: 'ltr', alignItems: 'center', justifyContent: 'center' }}>
        {id && !err ? <MediaView media={id} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, objectFit: 'cover' }} /> : null}
        {msg ? <Text style={{ fontSize: 12, color: 'rgba(235,235,245,0.75)', textAlign: 'center', paddingHorizontal: 16 }}>{msg}</Text> : null}
      </View>
      {perf ? <Text style={{ fontSize: 10, color: th.textDim, textAlign: 'end' }}>{t(perf.gpu ? 'studio.perfGpu' : 'studio.perfCpu', { fps: perf.fps })}</Text> : null}
    </View>
  );
}

// LordOS Studio's camera for every app: on or off, or where to get it (the Store's "For this computer" card)
function EveryApp({ th, cfg, status }) {
  const st = loopbackState(status, cfg);
  if (st === 'missing') {
    return (
      <View style={{ gap: 4 }}>
        <Row th={th} label={t('studio.everyApp')} detail={t('studio.notInstalled')} />
        <Hint th={th}>{t('studio.everyAppHint')}</Hint>
        <Link th={th} label={t('studio.getCamera')} onClick={() => wm.open('store', { args: { page: 'suggested', nonce: Date.now() } })} />
      </View>
    );
  }
  return (
    <View style={{ gap: 4 }}>
      <Row th={th} label={t('studio.everyApp')} toggle={cfg.on} onToggle={(v) => set({ on: !!v })} />
      <Hint th={th}>{t(st === 'stopped' ? 'studio.stopped' : st === 'off' ? 'studio.everyAppOff' : 'studio.everyAppOn')}</Hint>
    </View>
  );
}

// Mic Mode: the default microphone through lord.sound (sdk/studiomodel.js micPlan); Noise Reduction is the
// lordos-voice.conf filter, there once PipeWire read that file (the next sign-in after the update)
function MicMode({ th, cfg }) {
  const snd = useSound();
  const [note, setNote] = useState('');
  const raw = soundRaw();
  const mode = micModeOf(snd, cfg);
  const can = voiceAvailable(raw);
  const mics = hardwareMics(raw);
  const choose = (m) => {
    const plan = micPlan(soundRaw(), m, cfg.micSource);
    if (!plan.ok) { setNote(m === 'voice' && mics.length ? t('studio.micUnavailable') : t('studio.noMic')); return; }
    setNote('');
    for (const [fn, ...args] of plan.calls) sound[fn](...args);
    set({ mic: m, micSource: plan.mic });
  };
  return (
    <View style={{ gap: 6 }}>
      <Text style={{ fontSize: 13, color: th.text, textAlign: 'start' }}>{t('studio.mic')}</Text>
      <Segmented small fill value={mode} onChange={choose}
        options={[['standard', t('studio.micStandard')], ['voice', t('studio.micVoice'), !can], ['wide', t('studio.micWide')]]} />
      <Hint th={th}>{note || (!can ? t('studio.micUnavailable') : t(`studio.micHint.${mode}`))}</Hint>
      {/* a mode is the default microphone: what reaches an app depends on the microphone it records from */}
      <Hint th={th}>{t('studio.micApps')}</Hint>
    </View>
  );
}

// Mic Mode's current name, for a row's detail
export function useMicModeName() {
  const s = useStore(settings);
  const snd = useSound();
  const mode = micModeOf(snd, normalizeStudio(s.studio));
  return t(mode === 'voice' ? 'studio.micVoice' : mode === 'wide' ? 'studio.micWide' : 'studio.micStandard');
}

const linuxId = (a) => /^(desktop|flatpak):/.test(a);
const userName = (a) => (a === 'studio' ? '' : linuxId(a) ? castAppName({ who: a }) : appName(getApp(a)) || a);

// the camera it takes the picture from and who uses it
function Source({ th, cfg, status, go }) {
  const cams = realCameras(cameras());
  const cam = chosenCamera(cfg.camera, cams);
  const users = usersOf(status, userName, t('shell.cast.unknownApp'));
  return (
    <>
      <Row th={th} label={t('studio.title')} detail={cam ? cam.name : t('studio.noCamera')} onOpen={cams.length > 1 ? () => go('camera') : null} />
      <Row th={th} label={t('studio.inUse')} detail={users.length ? users.join(' · ') : t('studio.nobody')} />
    </>
  );
}

// wide: the app's window, whose other column has the preview, the camera and Every app
function MainPage({ th, cfg, status, pv, go, onBack, onOpenApp, wide }) {
  return (
    <View style={{ gap: 10 }}>
      {wide ? <Header th={th} title={t('studio.effects')} /> : (
        <>
          <Header th={th} title={t('studio.title')} onBack={onBack} />
          <Preview th={th} status={status} pv={pv} />
          <Source th={th} cfg={cfg} status={status} go={go} />
          <Sep th={th} />
        </>
      )}
      <Row th={th} label={t('studio.portrait')} toggle={portraitOn(cfg)} onToggle={(v) => update((c) => withPortrait(c, v))} onOpen={() => go('portrait')} />
      <Row th={th} label={t('studio.background')} toggle={backgroundOn(cfg)} onToggle={(v) => update((c) => withBackground(c, v, lordosWallpapers))}
        onOpen={() => go('background')} />
      <Row th={th} label={t('studio.faceLight')} toggle={cfg.light} onToggle={(v) => set({ light: !!v })} onOpen={() => go('light')} />
      <Row th={th} label={t('studio.autoFraming')} toggle={cfg.stage} onToggle={(v) => set({ stage: !!v })} onOpen={() => go('stage')} />
      <Row th={th} label={t('studio.filters')} detail={t(`studio.filter.${cfg.filter}`)} onOpen={() => go('filters')} />
      <Row th={th} label={t('studio.retouch')} toggle={cfg.retouch} onToggle={(v) => set({ retouch: !!v })} onOpen={() => go('retouch')} />
      <Row th={th} label={t('studio.mirror')} toggle={cfg.mirror} onToggle={(v) => set({ mirror: !!v })} />
      <Sep th={th} />
      <MicMode th={th} cfg={cfg} />
      {wide ? null : (
        <>
          <Sep th={th} />
          <EveryApp th={th} cfg={cfg} status={status} />
          <Hint th={th}>{t('studio.howTo')}</Hint>
          {onOpenApp && getApp('studio') ? <Link th={th} label={t('studio.openApp')} onClick={onOpenApp} /> : null}
        </>
      )}
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
        <View key={c.device} onClick={() => set({ camera: cameraKey(c) })} focusable accessibilityLabel={c.name}
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
      <SliderRow th={th} label={t('studio.blurAmount')} value={cfg.blur} onChange={(v) => set({ blur: v })} />
      <Hint th={th}>{t('studio.portraitHint')}</Hint>
    </View>
  );
}

// one effect's page: its switch, its slider, a line about it (and a note when the face model is missing)
function EffectPage({ th, cfg, status, go, title, on, onToggle, slider, hint, faces }) {
  return (
    <View style={{ gap: 10 }}>
      <Header th={th} title={title} onBack={() => go('main')} />
      <Row th={th} label={title} toggle={on} onToggle={onToggle} />
      {slider}
      <Hint th={th}>{hint}</Hint>
      {faces && status && status.faces === false ? <Hint th={th}>{t('studio.faceless')}</Hint> : null}
    </View>
  );
}

const LightPage = ({ th, cfg, status, go }) => (
  <EffectPage th={th} cfg={cfg} status={status} go={go} title={t('studio.faceLight')} on={cfg.light} onToggle={(v) => set({ light: !!v })} faces
    slider={<SliderRow th={th} label={t('studio.intensity')} value={cfg.lightAmount} onChange={(v) => set({ lightAmount: v, light: true })} />}
    hint={t('studio.faceLightHint')} />
);
const StagePage = ({ th, cfg, status, go }) => (
  <EffectPage th={th} cfg={cfg} status={status} go={go} title={t('studio.autoFraming')} on={cfg.stage} onToggle={(v) => set({ stage: !!v })} faces
    slider={<SliderRow th={th} label={t('studio.framing')} value={cfg.stageAmount} onChange={(v) => set({ stageAmount: v, stage: true })}
      from={t('studio.wide')} to={t('studio.tight')} />}
    hint={t('studio.autoFramingHint')} />
);
const RetouchPage = ({ th, cfg, status, go }) => (
  <EffectPage th={th} cfg={cfg} status={status} go={go} title={t('studio.retouch')} on={cfg.retouch} onToggle={(v) => set({ retouch: !!v })} faces
    slider={<SliderRow th={th} label={t('studio.blurAmount')} value={cfg.retouchAmount} onChange={(v) => set({ retouchAmount: v, retouch: true })} />}
    hint={t('studio.retouchHint')} />
);

function FiltersPage({ th, cfg, go }) {
  return (
    <View style={{ gap: 10 }}>
      <Header th={th} title={t('studio.filters')} onBack={() => go('main')} />
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6 }}>
        {STUDIO_FILTERS.map((f) => {
          const on = cfg.filter === f;
          return (
            <View key={f} onClick={() => set({ filter: f })} focusable accessibilityLabel={t(`studio.filter.${f}`)}
              style={{ width: 92, height: 30, borderRadius: 6, alignItems: 'center', justifyContent: 'center', backgroundColor: on ? th.accent : th.control || 'rgba(127,127,127,0.18)' }}>
              <Text style={{ fontSize: 12, color: on ? '#fff' : th.text }}>{t(`studio.filter.${f}`)}</Text>
            </View>
          );
        })}
      </View>
      {cfg.filter !== 'none' ? <SliderRow th={th} label={t('studio.filterStrength')} value={cfg.filterStrength} onChange={(v) => set({ filterStrength: v })} /> : null}
      <Hint th={th}>{t('studio.filtersHint')}</Hint>
    </View>
  );
}

function GreenPage({ th, cfg, go }) {
  return (
    <View style={{ gap: 10 }}>
      <Header th={th} title={t('studio.greenScreen')} onBack={() => go('background')} />
      <Row th={th} label={t('studio.greenScreen')} toggle={cfg.green} onToggle={(v) => set({ green: !!v })} />
      <Segmented small fill value={cfg.greenKey} onChange={(k) => set({ greenKey: k, green: true })}
        options={[['green', t('studio.keyGreen')], ['blue', t('studio.keyBlue')]]} />
      <SliderRow th={th} label={t('studio.similarity')} value={cfg.similarity} onChange={(v) => set({ similarity: v })} />
      <SliderRow th={th} label={t('studio.smoothness')} value={cfg.smoothness} onChange={(v) => set({ smoothness: v })} />
      <SliderRow th={th} label={t('studio.spill')} value={cfg.spill} onChange={(v) => set({ spill: v })} />
      <Hint th={th}>{t('studio.greenHint')}</Hint>
    </View>
  );
}

// a tile of the catalog: its thumbnail, a mark while it downloads or when it needs to (↓)
function Tile({ th, src, label, on, ready, percent, onClick }) {
  return (
    <View onClick={onClick} focusable accessibilityLabel={label}
      style={{ width: 92, height: 52, borderRadius: 6, overflow: 'hidden', border: `2px solid ${on ? th.accent : 'transparent'}` }}>
      <Image src={src} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
      {percent != null ? <View style={{ position: 'absolute', left: 0, bottom: 0, height: 3, width: `${percent}%`, backgroundColor: th.accent }} /> : null}
      {!ready && percent == null ? (
        <View style={{ position: 'absolute', right: 3, bottom: 3, width: 14, height: 14, borderRadius: 7, backgroundColor: 'rgba(0,0,0,0.55)', alignItems: 'center', justifyContent: 'center' }}>
          <Text style={{ fontSize: 9, color: '#fff' }}>↓</Text>
        </View>
      ) : null}
    </View>
  );
}

function BackgroundPage({ th, cfg, status, go }) {
  const [picking, setPicking] = useState(false);
  const [note, setNote] = useState('');
  const downloads = useStore(bgDownloads);
  const video = cfg.mode === 'video' || (cfg.mode !== 'picture' && !!cfg.video && !cfg.background);
  const [tab, setTab] = useState(video ? 'video' : 'picture');
  const kind = tab;
  const items = bgItems(studioCatalog, kind);
  const current = activeBackground(cfg);
  const choose = (it) => {
    setNote('');
    fetchBackground(it, { big: !!status?.gpu }).then((src) => { if (src) update((c) => withPicture(c, src)); }, (e) => setNote(t(`errors.${e?.code || 'studio.downloadFailed'}`)));
  };
  const walls = lordosWallpapers.filter((w) => w.category !== 'gradients' && !/cyberpunk/.test(w.category || ''));
  const pictured = cfg.mode === 'picture' || cfg.mode === 'video';
  return (
    <View style={{ gap: 10 }}>
      <Header th={th} title={t('studio.background')} onBack={() => go('main')} />
      <Segmented small fill value={cfg.mode} onChange={(m) => { update((c) => withMode(c, m)); if (m === 'picture' || m === 'video') setTab(m); }}
        options={[['none', t('studio.none')], ['blur', t('studio.blur')], ['picture', t('studio.picture'), !cfg.background], ['video', t('studio.video'), !cfg.video]]} />
      {pictured ? (
        <>
          <Row th={th} label={t('studio.greenScreen')} toggle={cfg.green} onToggle={(v) => set({ green: !!v })} onOpen={() => go('green')} />
          <SliderRow th={th} label={t('studio.match')} value={cfg.match} max={MATCH_MAX} onChange={(v) => set({ match: v })} />
        </>
      ) : null}
      <Segmented small fill value={tab} onChange={setTab} options={[['picture', t('studio.tabPictures')], ['video', t('studio.tabVideos')]]} />
      <ScrollView style={{ maxHeight: 230 }}>
        <View style={{ gap: 6 }}>
          {BG_CATEGORIES.map((cat) => {
            const list = items.filter((it) => it.category === cat);
            if (!list.length) return null;
            return (
              <View key={cat} style={{ gap: 4 }}>
                <Text style={{ fontSize: 11, fontWeight: 600, color: th.textDim, textAlign: 'start' }}>{t(`studio.cat.${cat}`)}</Text>
                <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6 }}>
                  {list.map((it, i) => (
                    <Tile key={it.id} th={th} src={bgThumb(it)} label={t('studio.tileLabel', { category: t(`studio.cat.${cat}`), n: i + 1 })} ready={bgReady(it, cfg.bgFiles)}
                      percent={downloads[it.id]} on={!!current && bgItemOf(studioCatalog, current)?.id === it.id} onClick={() => choose(it)} />
                  ))}
                </View>
              </View>
            );
          })}
          {kind === 'picture' ? (
            <View style={{ gap: 4 }}>
              <Text style={{ fontSize: 11, fontWeight: 600, color: th.textDim, textAlign: 'start' }}>{t('studio.pictures')}</Text>
              <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6 }}>
                {walls.map((w) => (
                  <Tile key={w.id} th={th} src={w.thumb} label={t(w.nameKey)} ready on={cfg.mode === 'picture' && cfg.background === w.src}
                    onClick={() => update((c) => withPicture(c, w.src))} />
                ))}
              </View>
            </View>
          ) : null}
        </View>
      </ScrollView>
      {note ? <Hint th={th}>{note}</Hint> : null}
      <Link th={th} label={t('studio.addOwn')} onClick={() => setPicking(true)} />
      <Link th={th} label={t('studio.credits')} onClick={() => go('credits')} />
      <Hint th={th}>{t('studio.backgroundHint')}</Hint>
      {picking ? (
        <Layer name="system">
          <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.35)' }}>
            <View style={{ position: 'relative', width: 880, maxWidth: '96%', height: 580, maxHeight: '94%' }}>
              <FilePicker modal title={t('studio.pickAny')} filter={FILES} onClose={() => setPicking(false)}
                onPick={(p) => { const path = Array.isArray(p) ? p[0] : p; setPicking(false); if (path) update((c) => withPicture(c, path)); }} />
            </View>
          </View>
        </Layer>
      ) : null}
    </View>
  );
}

// the catalog's credits: every background's author, license and source (the twin of Settings → Wallpaper's credits)
function CreditsPage({ th, cfg, go }) {
  const [note, setNote] = useState('');
  const items = studioCatalog && Array.isArray(studioCatalog.items) ? studioCatalog.items : [];
  return (
    <View style={{ gap: 8 }}>
      <Header th={th} title={t('studio.credits')} onBack={() => go('background')} />
      <Hint th={th}>{t('studio.creditsHint')}</Hint>
      {note ? <Hint th={th}>{note}</Hint> : null}
      <ScrollView style={{ maxHeight: 330 }}>
        <View style={{ gap: 6 }}>
          {items.map((it) => (
            <View key={it.id} style={{ gap: 2 }}>
              <Text style={{ fontSize: 11, color: th.textDim, textAlign: 'start', direction: 'ltr' }}>{bgCredit(it)}</Text>
              {bgAi(it) ? <Text style={{ fontSize: 11, color: th.textDim, textAlign: 'start' }}>{t('studio.aiGenerated')}</Text> : null}
              {cfg.bgFiles[it.id] ? <Link th={th} label={t('studio.remove')} onClick={() => removeBackground(it).catch((e) => setNote(t(`errors.${e?.code || 'studio.removeFailed'}`)))} /> : null}
            </View>
          ))}
        </View>
      </ScrollView>
    </View>
  );
}

// The popover's content; onBack: back to the capture panel it opened from. The preview lives as long as the popover,
// whatever page shows, so the camera isn't opened again on the way back.
const PAGES = () => ({ camera: CameraPage, portrait: PortraitPage, background: BackgroundPage, green: GreenPage, credits: CreditsPage, light: LightPage,
  stage: StagePage, filters: FiltersPage, retouch: RetouchPage });

export function StudioPanel({ th, room, onBack, onOpenApp }) {
  const s = useStore(settings);
  const cfg = normalizeStudio(s.studio);
  const status = useStatus();
  const pv = usePreview(status);
  const [page, go] = useState('main');
  const P = PAGES()[page];
  return (
    <View style={{ width: 320, padding: 12, maxHeight: room }}>
      <ScrollView style={{ flexShrink: 1 }}>
        {P ? <P th={th} cfg={cfg} status={status} go={go} /> : <MainPage th={th} cfg={cfg} status={status} pv={pv} go={go} onBack={onBack} onOpenApp={onOpenApp} />}
      </ScrollView>
    </View>
  );
}

// Opens the LordOS Studio app (apps/studio), or brings its window forward
export const openStudioApp = () => wm.open('studio');

// Mic Mode alone, for the capture panel while only a microphone is in use: no preview, so the camera never turns on
// for it. The LordOS Studio app is a link away where Studio's video effects can work.
export function MicModePanel({ th, room, onBack, onOpenApp }) {
  const s = useStore(settings);
  const cfg = normalizeStudio(s.studio);
  return (
    <View style={{ width: 320, padding: 12, maxHeight: room }}>
      <ScrollView style={{ flexShrink: 1 }}>
        <View style={{ gap: 10 }}>
          <Header th={th} title={t('studio.name')} onBack={onBack} />
          <MicMode th={th} cfg={cfg} />
          {onOpenApp && studioAvailable() && getApp('studio') ? (
            <>
              <Sep th={th} />
              <Link th={th} label={t('studio.openApp')} onClick={onOpenApp} />
            </>
          ) : null}
        </View>
      </ScrollView>
    </View>
  );
}

// The LordOS Studio app's window (apps/studio): the preview large on one side with the camera, who uses it and Every
// app; the effects and Mic Mode on the other, each row's detail page in place. The same settings and the same preview
// as the capture popover.
export function StudioWindow({ th }) {
  const s = useStore(settings);
  const cfg = normalizeStudio(s.studio);
  const status = useStatus();
  const pv = usePreview(status);
  const [page, go] = useState('main');
  const P = PAGES()[page];
  if (status && !status.available) {
    // lord-studio or its model is missing: Mic Mode still works (it is PipeWire's)
    return (
      <View style={{ flex: 1, padding: 24, gap: 16, alignItems: 'center', justifyContent: 'center' }}>
        <Text style={{ fontSize: 15, fontWeight: 600, color: th.text, textAlign: 'center' }}>{t('studio.unavailable')}</Text>
        <View style={{ width: 320 }}><MicMode th={th} cfg={cfg} /></View>
      </View>
    );
  }
  return (
    <View style={{ flex: 1, flexDirection: 'row', padding: 16, gap: 18, minHeight: 0 }}>
      <View style={{ flex: 1, minWidth: 0, gap: 10 }}>
        <Preview th={th} status={status} pv={pv} big />
        <Source th={th} cfg={cfg} status={status} go={go} />
        <Sep th={th} />
        <EveryApp th={th} cfg={cfg} status={status} />
        <Hint th={th}>{t('studio.howTo')}</Hint>
      </View>
      <View style={{ width: 320, minHeight: 0 }}>
        <ScrollView style={{ flex: 1 }}>
          {P ? <P th={th} cfg={cfg} status={status} go={go} /> : <MainPage th={th} cfg={cfg} status={status} pv={pv} go={go} wide />}
        </ScrollView>
      </View>
    </View>
  );
}

// Is LordOS Studio there at all (lord-studio and its model installed)?
export function studioAvailable() {
  try { return has() && !!lord.studio.status().available; } catch (e) { return false; }
}
