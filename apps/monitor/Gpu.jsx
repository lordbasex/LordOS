// Monitor → GPU's sampler and shared pieces (docs/MONITOR.md, "GPU"): useGpu, the fan by hand (administrators, amdgpu,
// lord-fand), the APIs installed for a card, and the Overview's GPU and AI lines. The tab itself is GpuView.jsx.
// lord.monitor.gpu() is read once per second only while the GPU tab or the Overview is on screen and the window isn't
// minimized; the model is gpumodel.js.
import {
  View, Text, Slider, Toggle, useTheme, useLocale, useStore, createStore, useAppWindow, useWindowHidden, monitor, formatBytes,
  formatNumber, typeScale, fanControl, scanVoices, voiceConf, VOICE_LANGS, appName, wm,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { Card, Big, Stat, StatRow, useCardStyle, isolatedAppOf } from './index.jsx';
import { Glyph } from './icons.jsx';
import { createGpuState, gpuStep, pushHistory, mainCard, topApp, aiRows } from './gpumodel.js';

const POLL_MS = 1000;

// ---- the sampler: one for the window, shared by the GPU tab and the Overview ----

export const gpuView = createStore(null);
export const gpuHistory = createStore({});
let users = 0, timer = null, state = createGpuState();

function poll() {
  timer = null;
  const raw = monitor.gpu();
  if (raw) {
    const v = gpuStep(state, raw);
    if (v) {
      v.aiRows = aiRows(state, v, raw, sentinelaState());
      gpuView.set(() => v);
      gpuHistory.set((h) => pushHistory(h, v));
    }
  }
  if (users > 0) timer = setTimeout(poll, raw ? POLL_MS : 250);
}

// Sentinela's engine (lord.sentinela.state(): its pid and model), read at most every 5 s; only in the desktop's own
// context (null elsewhere, and while it is off)
let sstate = null, sAt = 0;
function sentinelaState() {
  const now = Date.now();
  if (now - sAt < 5000) return sstate;
  sAt = now;
  try { const s = globalThis.lord?.sentinela?.state?.(); sstate = s && s.state !== 'off' && Number.isInteger(s.pid) ? s : null; } catch { sstate = null; }
  return sstate;
}

// The latest view, sampled while a caller is mounted, active and its window isn't minimized.
export function useGpu(active = true) {
  const { id } = useAppWindow() || {};
  const hidden = useWindowHidden(id);
  const on = active && !hidden;
  useEffect(() => {
    if (!on) return undefined;
    if (users++ === 0) { monitor.wantGpu(true); timer = setTimeout(poll, 50); }
    return () => {
      if (--users > 0) return;
      monitor.wantGpu(false);
      clearTimeout(timer);
      timer = null;
      state = createGpuState();
      gpuView.set(() => null);
      gpuHistory.set(() => ({}));
    };
  }, [on]);
  return useStore(gpuView);
}

// ---- pieces ----

function Hint({ children, color }) {
  const t = useTheme();
  return <Text style={{ fontSize: 11, color: color || t.textDim, lineHeight: 1.4 }}>{children}</Text>;
}

const pct = (tr, v) => (v == null ? '—' : tr('monitor.pct', { v: formatNumber(Math.round(v * 100), 0) }));
const mhz = (tr, v) => (v == null ? '—' : tr('monitor.gpu.mhz', { v: formatNumber(v, 0) }));

export function gpuLabel(tr, c) {
  return c.name || tr(`monitor.gpu.driver.${['v3d', 'vc4', 'virtio_gpu', 'vmwgfx', 'simpledrm', 'i915', 'xe', 'amdgpu', 'nouveau', 'nvidia', 'radeon'].includes(c.driver) ? c.driver : 'other'}`, { driver: c.driver });
}

export function appLabel(tr, item) {
  if (item.app === 'lordos' || item.self) return tr('monitor.lordos');
  const iso = isolatedAppOf(item);
  return iso ? appName(iso) : item.appName || item.name;
}

// amdgpu's sensors are "edge", "junction" and "mem"; another driver's label is shown as it comes
const sensor = (tr, label) => (['edge', 'junction', 'mem'].includes(label) ? tr(`monitor.gpu.sensor.${label}`) : label || tr('cooling.temperature'));

const apiText = (tr, a) => (a.detail ? tr('monitor.gpu.apiWith', { api: tr(`monitor.gpu.api.${a.id}`), detail: a.detail }) : tr(`monitor.gpu.api.${a.id}`));

function catText(tr, cats) {
  return Object.entries(cats).filter(([, v]) => v >= 0.005).sort((a, b) => b[1] - a[1]).slice(0, 3)
    .map(([k, v]) => tr('monitor.gpu.engineShare', { engine: tr(`monitor.gpu.engine.${k}`), v: formatNumber(Math.round(v * 100), 0) })).join(' · ');
}

// The fan by hand (lord-fand's GPUFAN): administrators only, amdgpu only; everyone else sees its state.
export function FanControl({ card, gf, canChange }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [err, setErr] = useState('');
  const [val, setVal] = useState(gf.mode === 'manual' ? gf.set : Math.max(gf.floor, card.fan?.pct ?? 50));
  const pending = useRef(null);
  useEffect(() => () => clearTimeout(pending.current), []);
  const send = (v) => {
    const r = fanControl.setGpuFan(gf.id, v);
    setErr(r ? tr(`errors.${r}`) : '');
  };
  const onSlide = (x) => {
    const v = Math.round(x);
    setVal(v);
    clearTimeout(pending.current);
    pending.current = setTimeout(() => send(v), 350);
  };
  const manual = gf.mode === 'manual';
  const reverted = !manual && gf.reverted && gf.reverted !== 'stop' ? tr(`monitor.gpu.fan.reverted.${gf.reverted}`, { c: gf.backstop }) : '';
  return (
    <Card icon="fan" title={tr('monitor.gpu.fan.title')}
      right={canChange ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
          <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.gpu.fan.auto')}</Text>
          <Toggle small on={!manual} onChange={(on) => send(on ? 'auto' : val)} />
        </View>
      ) : <Text style={{ fontSize: 12, color: t.textDim }}>{tr(manual ? 'monitor.gpu.fan.manualNow' : 'monitor.gpu.fan.autoNow')}</Text>}>
      {canChange && manual && (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12 }}>
          <Slider value={val} min={gf.floor} max={100} width={260} onChange={onSlide} />
          <Text style={{ fontSize: 13, fontWeight: 600, color: t.text }}>{tr('cooling.fanPct', { v: val })}</Text>
        </View>
      )}
      <Hint color={err ? '#ff453a' : null}>{err || reverted || tr(canChange ? 'monitor.gpu.fan.hint' : 'monitor.gpu.fan.adminOnly', { floor: gf.floor, c: gf.backstop })}</Hint>
    </Card>
  );
}

function VoiceSummary() {
  const { t: tr } = useLocale();
  const s = useMemo(() => {
    const scan = scanVoices();
    const conf = voiceConf();
    const langs = VOICE_LANGS.filter((L) => scan[L.id]?.standard || scan[L.id]?.hd).map((L) => {
      const q = [scan[L.id].hd ? tr('settings.ai.quality.hd') : '', scan[L.id].standard ? tr('settings.ai.quality.standard') : ''].filter(Boolean).join(', ');
      return tr('monitor.gpu.ai.model', { lang: tr(`settings.ai.lang.${L.id}`), quality: q });
    });
    return {
      models: langs.join(' · '),
      quality: tr(`settings.ai.quality.${['auto', 'standard', 'hd'].includes(conf.quality) ? conf.quality : 'auto'}`),
      backend: tr(`settings.ai.backend.${['auto', 'cpu', 'gpu'].includes(conf.backend) ? conf.backend : 'auto'}`),
    };
  }, [tr]);
  return (
    <>
      <Hint>{s.models ? tr('monitor.gpu.ai.installed', { models: s.models }) : tr('monitor.gpu.ai.noModels')}</Hint>
      <Hint>{tr('monitor.gpu.ai.settings', { quality: s.quality, backend: s.backend })}</Hint>
    </>
  );
}

export function AiCard({ view, style, compact }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const ai = view?.ai || [];
  const openSettings = () => wm.open('settings', { args: { section: 'ai', nonce: Date.now() } });
  return (
    <Card icon="ai" title={tr('monitor.gpu.ai.title')} style={style}
      right={compact ? null : <View onClick={openSettings}><Text style={{ fontSize: 11, color: t.accent, whiteSpace: 'nowrap' }}>{tr('monitor.gpu.ai.open')}</Text></View>}>
      {!ai.length && <Text style={{ fontSize: 12, color: t.text }}>{tr('monitor.gpu.ai.none')}</Text>}
      {ai.map((e) => (
        <View key={e.id} style={{ gap: 2 }}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
            <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: e.state === 'paused' ? '#ff9f0a' : '#30d158' }} />
            <Text style={{ flex: 1, fontSize: 12, fontWeight: 600, color: t.text }}>{tr(`monitor.gpu.ai.${e.id}`)}</Text>
            <Text style={{ fontSize: 12, color: t.textDim, whiteSpace: 'nowrap' }}>{tr(`monitor.gpu.ai.state.${e.state}`)}</Text>
          </View>
          <Hint>
            {[
              e.backend === 'gpu' ? tr('monitor.gpu.ai.onGpu') : e.backend === 'cpu' ? tr(e.id === 'ocr' ? 'monitor.gpu.ai.ocrCpu' : 'monitor.gpu.ai.onCpu') : tr('monitor.gpu.ai.backendUnknown'),
              e.quality ? tr(`settings.ai.quality.${e.quality}`) : '',
              e.lang && VOICE_LANGS.some((L) => L.id === e.lang) ? tr(`settings.ai.lang.${e.lang}`) : '',
              compact || e.backend !== 'gpu' ? '' : e.mem != null ? tr('monitor.gpu.ai.mem', { mem: formatBytes(e.mem) }) : e.hidden ? tr('monitor.gpu.ai.memHidden') : '',
            ].filter(Boolean).join(' · ')}
          </Hint>
        </View>
      ))}
      {!ai.length && !compact && <VoiceSummary />}
    </Card>
  );
}

export function ApisCard({ card }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const tone = { available: '#30d158', browser: '#32ade6', missing: '#ff9f0a', unsupported: t.textDim, none: t.textDim };
  if (!card.apis.length) return null;
  return (
    <Card icon="cpu" title={tr('monitor.gpu.apis')}>
      {card.apis.map((a) => (
        <View key={a.id} style={{ flexDirection: 'row', alignItems: 'center', gap: 10, minHeight: 20 }}>
          <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: tone[a.state] }} />
          <Text style={{ width: 150, fontSize: 12, color: t.text, whiteSpace: 'nowrap' }}>{tr(`monitor.gpu.api.${a.id}`)}</Text>
          <Text style={{ flex: 1, minWidth: 0, fontSize: 12, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
            {a.detail && a.state === 'available' ? tr('monitor.gpu.apiState.availableWith', { detail: a.detail }) : a.detail && a.state === 'missing' && a.id === 'rocm' ? tr('monitor.gpu.apiState.kfdOnly') : tr(`monitor.gpu.apiState.${a.state}`)}
          </Text>
        </View>
      ))}
      <Hint>{tr('monitor.gpu.apisHint')}</Hint>
    </Card>
  );
}

// ---- the Overview's GPU card ----

export function OverviewGpu({ onOpen, style }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const view = useGpu(true);
  const card = useCardStyle();
  const { main, more } = mainCard(view);
  const app = topApp(view, main);
  if (!view || !main) return null;
  return (
    <View onClick={onOpen} style={{ ...card.style, flex: 1, ...style }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        <Glyph name="gpu" color={t.accent} />
        <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.textDim, whiteSpace: 'nowrap' }}>{tr('monitor.gpu.title')}</Text>
        <View style={{ flex: 1 }} />
        <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
          {gpuLabel(tr, main)}{more ? ` ${tr('monitor.gpu.more', { n: more })}` : ''}
        </Text>
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'flex-end', gap: 18, flexWrap: 'wrap' }}>
        <Big value={pct(tr, main.busy)} small={tr('monitor.gpu.load')} />
        {main.vram && <Big value={formatBytes(main.vram.used || 0)} small={tr('monitor.ofTotal', { total: formatBytes(main.vram.total) })} />}
        {main.temp != null && <Big value={tr('cooling.temp', { v: formatNumber(Math.round(main.temp), 0) })} />}
        {main.fan?.rpm != null && <Big value={tr('cooling.rpm', { v: formatNumber(main.fan.rpm, 0) })} />}
      </View>
      <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
        {app ? tr('monitor.gpu.topApp', { app: appLabel(tr, app.item), v: formatNumber(Math.round(app.total * 100), 0) }) : tr('monitor.gpu.noApps')}
      </Text>
    </View>
  );
}

// The Overview's line for LordOS's AI and the virtual machines (both come with the GPU sample).
export function OverviewAiVms({ style }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const view = useStore(gpuView);
  if (!view) return null;
  const voice = view.ai[0];
  const ai = voice ? tr('monitor.gpu.ai.active', {
    engine: tr(`monitor.gpu.ai.${voice.id}`),
    where: voice.backend === 'gpu' ? tr('monitor.gpu.ai.onGpu') : voice.backend === 'cpu' ? tr('monitor.gpu.ai.onCpu') : tr('monitor.gpu.ai.backendUnknown'),
  }) + (voice.quality ? ` · ${tr(`settings.ai.quality.${voice.quality}`)}` : '') : tr('monitor.gpu.ai.idle');
  const vms = view.vms.count ? tr('monitor.gpu.vms', { n: view.vms.count, cpu: view.vms.cpu != null ? formatNumber(Math.round(view.vms.cpu * 100), 0) : '—', mem: formatBytes(view.vms.rss) }) : null;
  return (
    <StatRow style={style}>
      <Stat icon="ai" label={tr('monitor.gpu.ai.title')} value={voice ? tr(`monitor.gpu.ai.state.${voice.state}`) : tr('monitor.gpu.ai.idleShort')}>
        <Hint>{ai}</Hint>
      </Stat>
      {vms && <Stat icon="cpu" label={tr('monitor.gpu.machines')} value={formatNumber(view.vms.count, 0)}><Hint>{vms}</Hint></Stat>}
    </StatRow>
  );
}
