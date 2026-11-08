// Monitor → GPU (docs/MONITOR.md, "GPU"): each graphics card drawn live (its fans turn with the real fan, the heat
// line and the load light follow the card), its load, temperature and fan over the last minutes in Cooling's chart,
// the apps using it (each engine's share and their GPU memory, with the APIs they use), its memory, clocks and power,
// the APIs installed for it, LordOS's AI engines, and — for administrators, on amdgpu — its fan by hand (lord-fand).
// lord.monitor.gpu() is read once per second only while this tab or the Overview is on screen and the window isn't
// minimized; the model is gpumodel.js.
import {
  View, Text, Segmented, Slider, Toggle, useTheme, useLocale, useStore, createStore, useAppWindow, useWindowHidden, monitor, formatBytes,
  formatNumber, typeScale, useFan, fanControl, GpuArt, scanVoices, voiceConf, VOICE_LANGS, appName, wm,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { Card, Big, Stat, useCardStyle, AppGlyph, isolatedAppOf } from './index.jsx';
import { Chart } from './Chart.jsx';
import { Glyph } from './icons.jsx';
import { createGpuState, gpuStep, pushHistory, mainCard, topApp, GPU_HISTORY } from './gpumodel.js';

const C = { load: '#30d158', temp: '#ff9f0a', fan: '#32ade6', vram: '#bf5af2', gtt: '#c4b5fd' };
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
    if (v) { gpuView.set(() => v); gpuHistory.set((h) => pushHistory(h, v)); }
  }
  if (users > 0) timer = setTimeout(poll, raw ? POLL_MS : 250);
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

function AppsCard({ view, card, style }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const apps = view.apps.filter((a) => card.apps.includes(a.key)).slice(0, 8);
  const top = Math.max(0.01, ...apps.map((a) => a.total));
  return (
    <Card icon="gpu" title={tr('monitor.gpu.apps')} style={style}>
      {!apps.length && <Hint>{tr('monitor.gpu.noApps')}</Hint>}
      {apps.map((a) => {
        const engines = catText(tr, a.cats);
        const mem = (a.mem || 0) + (card.shared ? a.sysMem || 0 : 0);
        return (
          <View key={a.key} style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
            <AppGlyph item={a.item} size={18} />
            <View style={{ flex: 1, minWidth: 0, gap: 3 }}>
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
                <Text style={{ flex: 1, minWidth: 0, fontSize: 12, color: t.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis', overflow: 'hidden' }}>
                  {appLabel(tr, a.item)}{a.count > 1 ? ` ${tr('monitor.memTopCount', { count: a.count })}` : ''}
                </Text>
                {a.mem != null && <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap' }}>{formatBytes(mem)}</Text>}
                <Text style={{ width: 44, fontSize: 12, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', textAlign: 'end' }}>{pct(tr, a.total)}</Text>
              </View>
              <View style={{ height: 5, borderRadius: 3, backgroundColor: t.control }}>
                <View style={{ width: `${Math.min(100, (a.total / top) * 100)}%`, height: 5, borderRadius: 3, backgroundColor: C.load }} />
              </View>
              {(engines || a.apis.length > 0) && (
                <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
                  {[a.apis.map((x) => apiText(tr, x)).join(', '), engines].filter(Boolean).join(' — ')}
                </Text>
              )}
            </View>
          </View>
        );
      })}
      <Hint>{tr('monitor.gpu.appsHint')}</Hint>
    </Card>
  );
}

function HistoryCard({ card, hist, style }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const cs = useCardStyle();
  const s = useMemo(() => ({
    times: hist.map((x) => x.t), busy: hist.map((x) => x.busy ?? 0), temp: hist.map((x) => x.temp ?? 0), fan: hist.map((x) => x.fan ?? 0),
  }), [hist]);
  const series = [
    ...(card.busy != null ? [{ label: tr('monitor.gpu.load'), values: s.busy, color: C.load }] : []),
    ...(card.temp != null ? [{ label: tr('cooling.temperature'), values: s.temp, color: C.temp }] : []),
    ...(card.fan ? [{ label: tr('cooling.fanSpeed'), values: s.fan, color: C.fan }] : []),
  ];
  return (
    <Card icon="temp" title={tr('cooling.history')} style={style}>
      {series.length ? <Chart series={series} max={100} format={(v) => formatNumber(Math.round(v), 0)} times={s.times} capacity={GPU_HISTORY} height={150} bg={cs.bg} />
        : <Hint>{tr('monitor.gpu.nothingToChart')}</Hint>}
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 14 }}>
        {series.map((x) => (
          <View key={x.label} style={{ flexDirection: 'row', alignItems: 'center', gap: 5 }}>
            <View style={{ width: 8, height: 8, borderRadius: 2, backgroundColor: x.color }} />
            <Text style={{ fontSize: 11, color: t.textDim }}>{x.label}</Text>
          </View>
        ))}
      </View>
    </Card>
  );
}

function MemoryCard({ card, hist }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const cs = useCardStyle();
  const v = card.vram;
  if (!v && !card.gtt) {
    return <Card icon="memory" title={tr('monitor.gpu.memory')}><Hint>{tr(card.shared ? 'monitor.gpu.sharedMemory' : 'monitor.gpu.noMemory')}</Hint></Card>;
  }
  const vals = hist.map((x) => x.vram ?? 0);
  return (
    <Card icon="memory" title={tr('monitor.gpu.memory')} right={v ? <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.inTotal', { total: formatBytes(v.total) })}</Text> : null}>
      {v && (
        <>
          <Big value={formatBytes(v.used || 0)} small={tr('monitor.ofTotal', { total: formatBytes(v.total) })} />
          <View style={{ flexDirection: 'row', height: 14, borderRadius: 5, overflow: 'hidden', backgroundColor: t.control }}>
            <View style={{ width: `${Math.min(100, ((v.used || 0) / v.total) * 100)}%`, height: 14, backgroundColor: C.vram }} />
          </View>
          <Chart series={[{ label: tr('monitor.gpu.vram'), values: vals, color: C.vram }]} max={v.total} format={formatBytes} times={hist.map((x) => x.t)} capacity={GPU_HISTORY} height={90} bg={cs.bg} />
        </>
      )}
      {card.gtt && <Hint>{tr('monitor.gpu.gtt', { used: formatBytes(card.gtt.used || 0), total: formatBytes(card.gtt.total) })}</Hint>}
    </Card>
  );
}

// The fan by hand (lord-fand's GPUFAN): administrators only, amdgpu only; everyone else sees its state.
function FanControl({ card, gf, canChange }) {
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
              e.lang && ['es', 'en', 'pt'].includes(e.lang) ? tr(`settings.ai.lang.${e.lang}`) : '',
              compact || e.backend !== 'gpu' ? '' : e.mem != null ? tr('monitor.gpu.ai.mem', { mem: formatBytes(e.mem) }) : e.hidden ? tr('monitor.gpu.ai.memHidden') : '',
            ].filter(Boolean).join(' · ')}
          </Hint>
        </View>
      ))}
      {!ai.length && !compact && <VoiceSummary />}
    </Card>
  );
}

function ApisCard({ card }) {
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

// ---- the tab ----

let lastCard = 0;

export function GpuTab() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const view = useGpu(true);
  const hist = useStore(gpuHistory);
  const fan = useFan();
  const [sel, setSelState] = useState(lastCard);
  const setSel = (i) => { lastCard = i; setSelState(i); };
  if (!view) return <Hint>{tr('monitor.waiting')}</Hint>;
  const cards = view.cards.filter((c) => c.kind !== 'display');
  if (!cards.length) {
    return <Card icon="gpu" title={tr('monitor.gpu.title')}><Text style={{ fontSize: 13, color: t.textDim }}>{tr('monitor.gpu.none')}</Text></Card>;
  }
  const i = Math.min(Number(sel) || 0, cards.length - 1);
  const c = cards[i];
  const h = hist[c.key] || [];
  const gf = (fan?.gpuFans || []).find((f) => f.slot === c.slot);
  const fanPct = c.fan && c.fan.spinning ? Math.max(c.fan.pct || 0, 15) : 0;
  const temps = c.temps.filter((x) => x.c != null);
  return (
    <>
      {cards.length > 1 && (
        <View style={{ alignItems: 'center' }}>
          <Segmented value={String(i)} onChange={(v) => setSel(Number(v))} options={cards.map((x, k) => [String(k), gpuLabel(tr, x)])} />
        </View>
      )}
      <View style={{ flexDirection: 'row', gap: 12 }}>
        <Card icon="gpu" title={gpuLabel(tr, c)} style={{ flex: 1 }}
          right={<Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap' }}>{tr(`monitor.gpu.kind.${c.kind}`, { driver: c.driver })}</Text>}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 16 }}>
            <GpuArt kind={c.kind} fans={2} fanPct={fanPct} temp={c.temp} load={c.busy || 0} width={280} height={156} />
            <View style={{ flex: 1, minWidth: 0, gap: 10 }}>
              <Big value={pct(tr, c.busy)} small={tr('monitor.gpu.load')} />
              <Big value={c.temp != null ? tr('cooling.temp', { v: formatNumber(Math.round(c.temp), 0) }) : '—'} small={sensor(tr, c.tempLabel)} />
              {c.fan && <Big value={c.fan.rpm != null ? tr('cooling.rpm', { v: formatNumber(c.fan.rpm, 0) }) : tr('cooling.fanPct', { v: c.fan.pct })}
                small={c.fan.rpm === 0 ? tr('monitor.gpu.fanStopped') : c.fan.pct != null ? tr('cooling.fanPct', { v: c.fan.pct }) : null} />}
              <Hint>{c.busySource === 'apps' ? tr('monitor.gpu.loadFromApps') : c.busySource == null ? tr('monitor.gpu.loadUnknown') : ''}</Hint>
            </View>
          </View>
        </Card>
      </View>
      <View style={{ flexDirection: 'row', gap: 12 }}>
        <AppsCard view={view} card={c} style={{ flex: 1 }} />
        <HistoryCard card={c} hist={h} style={{ flex: 1 }} />
      </View>
      <View style={{ flexDirection: 'row', gap: 10 }}>
        {temps.length > 1
          ? temps.map((x, k) => <Stat key={`${k}${x.label}`} icon="temp" label={sensor(tr, x.label)} value={formatNumber(Math.round(x.c), 0)} unit="°C" />)
          : <Stat icon="temp" label={tr('cooling.temperature')} value={c.temp != null ? formatNumber(Math.round(c.temp), 0) : '—'} unit={c.temp != null ? '°C' : null} />}
        <Stat icon="fan" label={tr('monitor.fan')} value={c.fan?.rpm != null ? formatNumber(c.fan.rpm, 0) : '—'} unit={c.fan?.rpm != null ? tr('monitor.rpm') : null} />
        <Stat icon="activity" label={tr('monitor.gpu.power')} value={c.powerW != null ? formatNumber(c.powerW, 1) : '—'}
          unit={c.powerW != null ? (c.powerCapW ? tr('monitor.gpu.wattsOf', { cap: formatNumber(c.powerCapW, 0) }) : tr('monitor.volt.wattsUnit')) : null} />
        <Stat icon="cpu" label={tr('monitor.gpu.coreClock')} value={mhz(tr, c.sclk)} unit={c.sclkMax ? tr('monitor.gpu.ofMhz', { v: formatNumber(c.sclkMax, 0) }) : null} />
        {(c.mclk != null || c.mclkMax != null) && <Stat icon="memory" label={tr('monitor.gpu.memClock')} value={mhz(tr, c.mclk)} unit={c.mclkMax ? tr('monitor.gpu.ofMhz', { v: formatNumber(c.mclkMax, 0) }) : null} />}
      </View>
      {gf && <FanControl key={`${gf.id}${gf.mode}`} card={c} gf={gf} canChange={!!fan?.canChange} />}
      <View style={{ flexDirection: 'row', gap: 12 }}>
        <View style={{ flex: 1, minWidth: 0, gap: 12 }}><MemoryCard card={c} hist={h} /></View>
        <View style={{ flex: 1, minWidth: 0, gap: 12 }}><AiCard view={view} /></View>
      </View>
      <ApisCard card={c} />
    </>
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
    <View style={{ flexDirection: 'row', gap: 10, ...style }}>
      <Stat icon="ai" label={tr('monitor.gpu.ai.title')} value={voice ? tr(`monitor.gpu.ai.state.${voice.state}`) : tr('monitor.gpu.ai.idleShort')}>
        <Hint>{ai}</Hint>
      </Stat>
      {vms && <Stat icon="cpu" label={tr('monitor.gpu.machines')} value={formatNumber(view.vms.count, 0)}><Hint>{vms}</Hint></Stat>}
    </View>
  );
}
