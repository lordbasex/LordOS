// Monitor → GPU (docs/MONITOR.md, "GPU"), the 0.8.9.6 layout approved by the maintainer on 2026-11-24: a chip per GPU
// with its sparkline and a One / Side-by-side switch; the card's header with its tiles (load, temperature, power, fan
// or RC6 idle, core and memory clocks, PCIe link); utilization by engine with three minutes of history; graphics
// memory split by type of process; the processes using it (another user's shown as "Hidden"); and AI on this computer.
// At about 1100×600 and up everything fits the window without scrolling (`fit`): the process table and the AI list
// show the rows that fit. Smaller, the page scrolls like every other tab. The data is gpumodel.js's view, sampled by
// Gpu.jsx's useGpu (once per second, only while this tab or the Overview is on screen and the window isn't minimized).
import {
  View, Text, Image, Segmented, Button, useTheme, useLocale, useStore, formatBytes, formatNumber, wm, useFan, VOICE_LANGS,
  clipboard, toast, systemInfo, lordosVersion,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { useCardStyle, AppGlyph } from './index.jsx';
import { Chart } from './Chart.jsx';
import { canvas, drawArea } from './draw.js';
import { useGpu, gpuHistory, gpuLabel, appLabel, FanControl, ApisCard } from './Gpu.jsx';
import { ENGINE_CATS, GPU_HISTORY, mesaVersion, rowsThatFit } from './gpumodel.js';
import { nvFailure, nvReportText } from './nvreport.js';

// the mockup's colors, per theme
const PALETTE = {
  dark: { '3d': '#30d158', compute: '#ff375f', video: '#32ade6', encode: '#ffd60a', copy: '#bf5af2', vram: '#bf5af2', temp: '#ff9f0a', power: '#ff6b3d', fan: '#32ade6', ok: '#30d158' },
  light: { '3d': '#1fa846', compute: '#e0244d', video: '#1d95cf', encode: '#c9a200', copy: '#9b3fd6', vram: '#9b3fd6', temp: '#ff9f0a', power: '#ff6b3d', fan: '#1d95cf', ok: '#1fa846' },
};
const VENDOR = { amdgpu: '#ed1c24', radeon: '#ed1c24', i915: '#0071c5', xe: '#0071c5', nvidia: '#76b900', nouveau: '#76b900' };
const TYPE_COLOR = { compute: 'compute', graphics: '3d', video: 'video' };

function usePalette() {
  const t = useTheme();
  return PALETTE[t.mode === 'dark' ? 'dark' : 'light'];
}

// a color at about 22 % opacity ('#rrggbb' + alpha; any other form is left as it is, drawn solid by the tint below it)
const tint = (c) => (/^#[0-9a-f]{6}$/i.test(String(c)) ? `${c}38` : c);
const pctText = (tr, v) => (v == null ? '—' : tr('monitor.pct', { v: formatNumber(Math.round(v * 100), 0) }));
const n0 = (v) => formatNumber(Math.round(v), 0);

// ---- small pieces ----

function GCard({ title, right, children, style, glyph }) {
  const t = useTheme();
  const cs = useCardStyle();
  return (
    <View style={{ borderRadius: 12, backgroundColor: cs.bg, border: `1px solid ${t.separator}`, paddingHorizontal: 12, paddingVertical: 10, gap: 7, minWidth: 0, ...style }}>
      {title != null && (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 7 }}>
          <View style={{ width: 14, height: 14, borderRadius: 4, border: `1.5px solid ${glyph || t.accent}` }} />
          <Text style={{ fontSize: 12, fontWeight: 600, color: t.textDim, whiteSpace: 'nowrap' }}>{title}</Text>
          <View style={{ flex: 1 }} />
          {right}
        </View>
      )}
      {children}
    </View>
  );
}

function Bar({ value, color, height = 4, width }) {
  const t = useTheme();
  return (
    <View style={{ height, borderRadius: height / 2, backgroundColor: t.control, overflow: 'hidden', ...(width ? { width } : { flex: 1 }) }}>
      <View style={{ width: `${Math.max(0, Math.min(100, (value || 0) * 100))}%`, height, borderRadius: height / 2, backgroundColor: color }} />
    </View>
  );
}

// a sparkline (drawn in pixels like Chart, always left to right)
function Spark({ values, color, width = 60, height = 16, max = 100 }) {
  const pixels = useMemo(() => (values.length > 1 ? drawArea(canvas(width, height), [{ values, color }], max, GPU_HISTORY) : null), [values, color, width, height, max]);
  return <View style={{ width, height, direction: 'ltr' }}>{pixels && <Image pixels={pixels} style={{ width, height }} />}</View>;
}

function Tile({ label, value, small, bar, color, onClick }) {
  const t = useTheme();
  return (
    <View onClick={onClick} style={{ flex: 1, minWidth: 0, maxWidth: 170, borderRadius: 9, border: `1px solid ${t.separator}`, backgroundColor: t.windowBg, paddingHorizontal: 8, paddingVertical: 5, gap: 3 }}>
      <Text style={{ fontSize: 10.5, color: t.textDim, whiteSpace: 'nowrap', textAlign: 'start' }}>{label}</Text>
      <View style={{ flexDirection: 'row', alignItems: 'flex-end', gap: 4, overflow: 'hidden' }}>
        <Text style={{ fontSize: 15, fontWeight: 650, color: t.text, whiteSpace: 'nowrap' }}>{value}</Text>
        {small ? <Text style={{ fontSize: 10.5, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', marginBottom: 1 }}>{small}</Text> : null}
      </View>
      {bar != null ? <Bar value={bar} color={color} /> : <View style={{ height: 4 }} />}
    </View>
  );
}

function Pill({ text, color }) {
  const t = useTheme();
  return (
    <View style={{ paddingHorizontal: 6, paddingVertical: 1, borderRadius: 9, backgroundColor: color ? tint(color) : t.control }}>
      <Text style={{ fontSize: 10, fontWeight: 600, color: t.text, whiteSpace: 'nowrap' }}>{text}</Text>
    </View>
  );
}

// a box's height as laid out (read after each draw, like Chart's width): what the table and the AI list fill
function useBoxHeight() {
  const ref = useRef(null);
  const [h, setH] = useState(0);
  useEffect(() => {
    if (!ref.current) return;
    const l = lord.getLayout(ref.current.id);
    const nh = l ? Math.round(l[3]) : 0;
    if (nh > 0 && Math.abs(nh - h) > 1) setH(nh);
  });
  return [ref, h];
}

// ---- the parts ----

function cardTiles(tr, c, P, accent, compact) {
  const fan = c.fan;
  const tiles = [
    { k: 'load', label: tr('monitor.gpu.load'), value: pctText(tr, c.busy), bar: c.busy, color: P['3d'] },
    {
      k: 'temp', label: tr('cooling.temperature'), value: c.temp != null ? tr('cooling.temp', { v: n0(c.temp) }) : '—',
      small: c.temp != null ? (c.tempCrit ? tr('monitor.gpu.v.crit', { sensor: c.tempLabel || '', c: n0(c.tempCrit) }).replace(/^ · /, '') : c.tempLabel || '') : tr('monitor.gpu.v.notReported'),
      bar: c.temp != null ? Math.min(1, c.temp / (c.tempCrit || 100)) : null, color: P.temp,
    },
    {
      k: 'power', label: tr('monitor.gpu.power'), value: c.powerW != null ? `${n0(c.powerW)} W` : '—',
      small: c.powerW != null && c.powerCapW ? tr('monitor.gpu.v.ofWatts', { v: n0(c.powerCapW) }) : c.powerW == null ? tr('monitor.gpu.v.notReported') : '',
      bar: c.powerW != null && c.powerCapW ? Math.min(1, c.powerW / c.powerCapW) : null, color: P.power,
    },
    fan ? { k: 'fan', label: tr('monitor.gpu.v.fan'), value: fan.rpm != null ? n0(fan.rpm) : tr('cooling.fanPct', { v: fan.pct }), small: fan.rpm != null && fan.pct != null ? tr('monitor.gpu.v.rpmPct', { v: fan.pct }) : '', bar: fan.pct != null ? fan.pct / 100 : null, color: P.fan }
      : c.idle != null ? { k: 'idle', label: tr('monitor.gpu.v.idle'), value: pctText(tr, c.idle), bar: c.idle, color: P.fan } : null,
    c.sclk != null ? { k: 'core', label: tr('monitor.gpu.coreClock'), value: n0(c.sclk), small: c.sclkMax || c.rp0 ? tr('monitor.gpu.v.ofMhz', { v: n0(c.sclkMax || c.rp0) }) : 'MHz', bar: c.sclkMax || c.rp0 ? Math.min(1, c.sclk / (c.sclkMax || c.rp0)) : null, color: accent } : null,
    c.mclk != null ? { k: 'mem', label: tr('monitor.gpu.memClock'), value: n0(c.mclk), small: c.mclkMax ? tr('monitor.gpu.v.ofMhz', { v: n0(c.mclkMax) }) : 'MHz', bar: c.mclkMax ? Math.min(1, c.mclk / c.mclkMax) : null, color: P.vram } : null,
    c.link?.gen ? {
      k: 'link', label: tr('monitor.gpu.v.link'), value: tr('monitor.gpu.v.linkValue', { gen: c.link.gen, width: c.link.width }),
      small: c.linkMax?.gen && (c.linkMax.gen !== c.link.gen || c.linkMax.width !== c.link.width) ? tr('monitor.gpu.v.ofLink', { gen: c.linkMax.gen, width: c.linkMax.width }) : '',
      bar: c.linkMax?.gen ? Math.min(1, (c.link.gen * c.link.width) / (c.linkMax.gen * c.linkMax.width)) : 1, color: P.ok,
    } : null,
  ].filter(Boolean);
  return compact ? tiles.slice(0, 5) : tiles;
}

function subLine(c, caps) {
  const mesa = mesaVersion(caps?.libs);
  return [c.driver, c.driver === 'nvidia' ? c.nv?.driver : mesa ? `Mesa ${mesa}` : '', c.slot, c.vram?.total ? formatBytes(c.vram.total) : ''].filter(Boolean).join(' · ');
}

function Header({ c, view, compact, onDetails }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const P = usePalette();
  const tiles = cardTiles(tr, c, P, t.accent, compact);
  if (compact) {
    return (
      <GCard style={{ gap: 6 }}>
        <View style={{ minWidth: 0 }}>
          <Text style={{ fontSize: 14, fontWeight: 700, color: t.text, whiteSpace: 'nowrap', textAlign: 'start' }}>{gpuLabel(tr, c)}</Text>
          <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{subLine(c, view.caps)}</Text>
        </View>
        <View style={{ flexDirection: 'row', gap: 7, flexWrap: 'wrap' }}>{tiles.map((x) => <Tile key={x.k} {...x} />)}</View>
      </GCard>
    );
  }
  return (
    <GCard style={{ flexDirection: 'row', alignItems: 'center', gap: 14, paddingVertical: 9 }}>
      <View style={{ width: 12, height: 38, borderRadius: 3, backgroundColor: VENDOR[c.driver] || t.accent }} />
      <View onClick={onDetails} style={{ width: 228, minWidth: 0, flexShrink: 0 }}>
        <Text style={{ fontSize: 16, fontWeight: 700, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{gpuLabel(tr, c)}</Text>
        <Text style={{ fontSize: 11, color: t.textDim, lineHeight: 1.35, marginTop: 2, textAlign: 'start' }}>{subLine(c, view.caps)}</Text>
        <Text style={{ fontSize: 11, color: t.accent, marginTop: 2, textAlign: 'start' }}>{tr('monitor.gpu.v.details')}</Text>
      </View>
      <View style={{ flex: 1, minWidth: 0, flexDirection: 'row', gap: 7, justifyContent: 'flex-end' }}>{tiles.map((x) => <Tile key={x.k} {...x} />)}</View>
    </GCard>
  );
}

function Engines({ c, hist, height = 150, compact, style }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const P = usePalette();
  const cs = useCardStyle();
  const series = useMemo(() => ENGINE_CATS
    .map((k) => ({ k, label: tr(`monitor.gpu.v.e.${k}`), values: hist.map((p) => p.eng?.[k] ?? 0), color: P[k] }))
    .filter((s) => s.values.some((v) => v > 0.5)), [hist, P, tr]);
  const chart = series.length ? <Chart series={series} max={100} format={(v) => tr('monitor.pct', { v: n0(v) })} times={hist.map((p) => p.t)} capacity={GPU_HISTORY} height={height} bg={cs.bg} />
    : <View style={{ height, borderRadius: 8, border: `1px solid ${t.separator}`, backgroundColor: t.windowBg }} />;
  if (compact) {
    return (
      <GCard title={tr('monitor.gpu.v.engines')} style={style}
        right={<Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{ENGINE_CATS.map((k) => `${tr(`monitor.gpu.v.e.${k}`)} ${pctText(tr, c.cats?.[k])}`).join('  ')}</Text>}>
        {chart}
      </GCard>
    );
  }
  return (
    <GCard title={tr('monitor.gpu.v.engines')} style={style} right={<Text style={{ fontSize: 11, color: t.textDim }}>{tr('monitor.gpu.v.last3')}</Text>}>
      <View style={{ flexDirection: 'row', gap: 12 }}>
        <View style={{ width: 170, gap: 6, justifyContent: 'center' }}>
          {ENGINE_CATS.map((k) => (
            <View key={k} style={{ gap: 2 }}>
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
                <View style={{ width: 9, height: 9, borderRadius: 2, backgroundColor: P[k] }} />
                <Text style={{ flex: 1, fontSize: 12, color: t.text, whiteSpace: 'nowrap', textAlign: 'start' }}>{tr(`monitor.gpu.v.e.${k}`)}</Text>
                <Text style={{ fontSize: 12, fontWeight: 600, color: t.text, textAlign: 'end' }}>{pctText(tr, c.cats?.[k] ?? 0)}</Text>
              </View>
              <View style={{ paddingStart: 15 }}><Bar value={c.cats?.[k] ?? 0} color={P[k]} /></View>
            </View>
          ))}
        </View>
        <View style={{ flex: 1, minWidth: 0 }}>{chart}</View>
      </View>
    </GCard>
  );
}

function Memory({ c, hist, compact, style }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const P = usePalette();
  const cs = useCardStyle();
  const split = c.memSplit || { compute: 0, graphics: 0, video: 0 };
  if (!c.vram) {
    const used = split.compute + split.graphics + split.video;
    return (
      <GCard title={tr('monitor.gpu.memory')} style={style}>
        <View style={{ flexDirection: 'row', alignItems: 'flex-end', gap: 6 }}>
          <Text style={{ fontSize: compact ? 14 : 20, fontWeight: 650, color: t.text }}>{formatBytes(used)}</Text>
          <Text style={{ fontSize: 11, color: t.textDim, marginBottom: 2 }}>{tr('monitor.gpu.v.shared')}</Text>
        </View>
        {!compact && <Text style={{ fontSize: 10.5, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{tr('monitor.gpu.v.sharedHint')}</Text>}
      </GCard>
    );
  }
  const total = c.vram.total, used = c.vram.used || 0;
  const known = split.compute + split.graphics + split.video;
  const other = Math.max(0, used - known);
  const parts = [['compute', split.compute], ['graphics', split.graphics + other], ['video', split.video]];
  const free = Math.max(0, total - used);
  return (
    <GCard title={tr('monitor.gpu.memory')} style={style}
      right={c.memBusy != null ? <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap' }}>{tr('monitor.gpu.v.memBusy', { v: n0(c.memBusy * 100) })}</Text> : null}>
      <View style={{ flexDirection: 'row', alignItems: 'flex-end', gap: 16, flexWrap: 'wrap' }}>
        <View style={{ flexDirection: 'row', alignItems: 'flex-end', gap: 5 }}>
          <Text style={{ fontSize: compact ? 14 : 20, fontWeight: 650, color: t.text }}>{formatBytes(used)}</Text>
          <Text style={{ fontSize: 11, color: t.textDim, marginBottom: 2 }}>{tr('monitor.gpu.v.ofVram', { total: formatBytes(total) })}</Text>
        </View>
        {c.gtt && !compact && (
          <View style={{ flexDirection: 'row', alignItems: 'flex-end', gap: 5 }}>
            <Text style={{ fontSize: 14, fontWeight: 650, color: t.text }}>{formatBytes(c.gtt.used || 0)}</Text>
            <Text style={{ fontSize: 11, color: t.textDim, marginBottom: 1 }}>{tr('monitor.gpu.v.ofGtt', { total: formatBytes(c.gtt.total) })}</Text>
          </View>
        )}
      </View>
      <View style={{ flexDirection: 'row', height: 12, borderRadius: 5, overflow: 'hidden', backgroundColor: t.control, direction: 'ltr' }}>
        {parts.map(([k, v]) => (v > 0 ? <View key={k} style={{ width: `${Math.min(100, (v / total) * 100)}%`, height: 12, backgroundColor: P[TYPE_COLOR[k]] }} /> : null))}
      </View>
      {!compact && (
        <>
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 12 }}>
            {[...parts, ['free', free]].map(([k, v]) => (
              <View key={k} style={{ flexDirection: 'row', alignItems: 'center', gap: 5 }}>
                <View style={{ width: 8, height: 8, borderRadius: 2, backgroundColor: k === 'free' ? t.control : P[TYPE_COLOR[k]] }} />
                <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap' }}>{`${tr(k === 'free' ? 'monitor.gpu.v.free' : `monitor.gpu.v.t.${k}`)} ${formatBytes(v)}`}</Text>
              </View>
            ))}
          </View>
          <Chart series={[{ label: tr('monitor.gpu.vram'), values: hist.map((p) => p.vram ?? 0), color: P.vram }]} max={total} format={formatBytes} times={hist.map((p) => p.t)} capacity={GPU_HISTORY} height={56} bg={cs.bg} />
        </>
      )}
    </GCard>
  );
}

const COLS = [
  { k: 'app', flex: 1 }, { k: 'pid', w: 50 }, { k: 'type', w: 74, start: true }, { k: 'gpu', w: 86 }, { k: '3d', w: 28 }, { k: 'comp', w: 38 }, { k: 'dec', w: 30 },
  { k: 'enc', w: 32 }, { k: 'copy', w: 34 }, { k: 'vram', w: 62 }, { k: 'gtt', w: 54 }, { k: 'api', w: 84, start: true },
];
const MINI = ['app', 'type', 'gpu', 'vram', 'api'];
const MINI_W = { type: 78, gpu: 96, vram: 70, api: 84 };

function TypeTag({ type }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const P = usePalette();
  if (type === 'hidden') {
    return <View style={{ paddingHorizontal: 6, paddingVertical: 1, borderRadius: 5, border: `1px dashed ${t.separator}` }}><Text style={{ fontSize: 10, fontWeight: 600, color: t.textDim }}>{tr('monitor.gpu.v.t.hidden')}</Text></View>;
  }
  return (
    <View style={{ paddingHorizontal: 6, paddingVertical: 1, borderRadius: 5, backgroundColor: tint(P[TYPE_COLOR[type] || '3d']) }}>
      <Text style={{ fontSize: 10, fontWeight: 600, color: t.text, whiteSpace: 'nowrap' }}>{tr(`monitor.gpu.v.t.${type}`)}</Text>
    </View>
  );
}

const apiShort = (tr, apis) => apis.map((a) => (a.detail && a.id === 'vulkan' ? tr('monitor.gpu.apiWith', { api: tr('monitor.gpu.api.vulkan'), detail: a.detail }) : tr(`monitor.gpu.api.${a.id}`))).join(', ');

function ProcTable({ view, c, rows, mini }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const P = usePalette();
  const apps = view.apps.filter((a) => c.apps.includes(a.key));
  const cols = mini ? COLS.filter((x) => MINI.includes(x.k)).map((x) => (MINI_W[x.k] ? { ...x, w: MINI_W[x.k] } : x)) : COLS;
  const box = (col) => (col.flex ? { flex: 1, minWidth: 0 } : { width: col.w, flexShrink: 0 });
  const cell = (col, children, dim) => (
    <View key={col.k} style={{ ...box(col), paddingHorizontal: 5, flexDirection: 'row', alignItems: 'center', justifyContent: col.flex || col.start ? 'flex-start' : 'flex-end', overflow: 'hidden' }}>
      {typeof children === 'string' ? <Text style={{ fontSize: 11.5, color: dim ? t.textDim : t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: col.flex || col.start ? 'start' : 'end' }}>{children}</Text> : children}
    </View>
  );
  const shown = apps.slice(0, Math.max(0, rows - Math.min(c.hidden.length, 1)));
  const hidden = c.hidden.slice(0, Math.max(0, rows - shown.length));
  const pctCell = (v) => (v > 0.005 ? n0(v * 100) : '0');
  return (
    <View style={{ gap: 0 }}>
      <View style={{ flexDirection: 'row', height: 22, alignItems: 'center', borderBottom: `1px solid ${t.separator}` }}>
        {cols.map((col) => cell(col, <Text style={{ fontSize: 10.5, fontWeight: 600, color: col.k === 'gpu' ? t.accent : t.textDim, whiteSpace: 'nowrap' }}>{`${tr(`monitor.gpu.v.col.${col.k}`)}${col.k === 'gpu' ? ' ▾' : ''}`}</Text>))}
      </View>
      {!apps.length && !c.hidden.length && <Text style={{ fontSize: 12, color: t.textDim, paddingVertical: 8, textAlign: 'start' }}>{tr('monitor.gpu.v.noProcs')}</Text>}
      {shown.map((a, i) => {
        const cats = a.cats || {};
        const mem = (a.mem || 0) + (c.shared ? a.sysMem || 0 : 0);
        return (
          <View key={a.key} style={{ flexDirection: 'row', height: 23, alignItems: 'center', backgroundColor: i % 2 ? (t.mode === 'dark' ? 'rgba(255,255,255,0.035)' : 'rgba(0,0,0,0.025)') : 'transparent' }}>
            {cols.map((col) => {
              switch (col.k) {
                case 'app': return cell(col, (
                  <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, minWidth: 0, flex: 1 }}>
                    <AppGlyph item={a.item} size={15} />
                    <Text style={{ flex: 1, minWidth: 0, fontSize: 11.5, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>
                      {appLabel(tr, a.item)}<Text style={{ fontWeight: 400, color: t.textDim }}>{` ${a.item.name || ''}`}</Text>
                    </Text>
                  </View>
                ));
                case 'pid': return cell(col, String(a.pids[0] ?? ''), true);
                case 'type': return cell(col, <TypeTag type={a.type} />);
                case 'gpu': return cell(col, (
                  <View style={{ flexDirection: 'row', alignItems: 'center', gap: 5 }}>
                    <Text style={{ fontSize: 11.5, fontWeight: 600, color: t.text }}>{pctText(tr, a.total)}</Text>
                    <View style={{ width: 44, height: 6, borderRadius: 3, backgroundColor: t.control, flexDirection: 'row', overflow: 'hidden', direction: 'ltr' }}>
                      {ENGINE_CATS.map((k) => (cats[k] > 0 ? <View key={k} style={{ width: Math.round(Math.min(1, cats[k]) * 44), height: 6, backgroundColor: P[k] }} /> : null))}
                    </View>
                  </View>
                ));
                case '3d': return cell(col, pctCell(cats['3d'] || 0), !(cats['3d'] > 0.005));
                case 'comp': return cell(col, pctCell(cats.compute || 0), !(cats.compute > 0.005));
                case 'dec': return cell(col, pctCell(cats.video || 0), !(cats.video > 0.005));
                case 'enc': return cell(col, pctCell(cats.encode || 0), !(cats.encode > 0.005));
                case 'copy': return cell(col, pctCell(cats.copy || 0), !(cats.copy > 0.005));
                case 'vram': return cell(col, mem > 0 ? formatBytes(mem) : '—');
                case 'gtt': return cell(col, a.sysMem > 0 ? formatBytes(a.sysMem) : '—', true);
                case 'api': return cell(col, apiShort(tr, a.apis) || '—', true);
                default: return null;
              }
            })}
          </View>
        );
      })}
      {hidden.map((h) => (
        <View key={`h${h.pid}`} style={{ flexDirection: 'row', height: 23, alignItems: 'center' }}>
          {cell(cols[0], (
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, minWidth: 0, flex: 1 }}>
              <View style={{ width: 15, height: 15, borderRadius: 4, backgroundColor: '#636366' }} />
              <Text style={{ flex: 1, minWidth: 0, fontSize: 11.5, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{h.name}</Text>
            </View>
          ))}
          {!mini && cell(cols[1], String(h.pid), true)}
          {cell(mini ? cols[1] : cols[2], <TypeTag type="hidden" />)}
          <View style={{ flex: mini ? 0 : 3, minWidth: 0, paddingHorizontal: 5, ...(mini ? { width: 250 } : {}) }}>
            <Text style={{ fontSize: 10.5, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{tr('monitor.gpu.v.otherUser')}</Text>
          </View>
        </View>
      ))}
      {apps.length > shown.length && <Text style={{ fontSize: 11, color: t.textDim, paddingTop: 3, textAlign: 'start' }}>{tr('monitor.gpu.v.more', { n: apps.length - shown.length })}</Text>}
    </View>
  );
}

function aiTitle(tr, r) {
  if (r.kind === 'voice') return tr('monitor.gpu.v.voice');
  if (r.kind === 'studio') return tr('monitor.gpu.v.studio');
  if (r.kind === 'ocr') return tr('monitor.gpu.v.ocr');
  if (r.kind === 'sentinela') return tr('monitor.gpu.v.sentinela');
  if (r.kind === 'app' && r.item) return appLabel(tr, r.item);
  return r.name;
}

function aiMeta(tr, r) {
  const api = r.apis.length ? r.apis.map((a) => tr(`monitor.gpu.api.${a}`)).join(', ') : tr('monitor.gpu.v.cpu');
  if (r.kind === 'sentinela') return [tr('monitor.gpu.v.sentinelaSub'), api, r.model].filter(Boolean).join(' · ');
  if (r.kind === 'voice') {
    return [r.name, api, r.lang && VOICE_LANGS.some((L) => L.id === r.lang) ? tr(`settings.ai.lang.${r.lang}`) : '', r.quality ? tr(`settings.ai.quality.${r.quality}`) : ''].filter(Boolean).join(' · ');
  }
  if (r.kind === 'studio' || r.kind === 'ocr') return [r.name, api].join(' · ');
  return [r.kind === 'app' ? r.name : tr('monitor.gpu.v.yourProcess'), api, r.model ? tr('monitor.gpu.v.fromArgv', { model: r.model }) : ''].filter(Boolean).join(' · ');
}

function AiPanel({ rows, max, style, boxRef, fit }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const P = usePalette();
  const openSettings = () => wm.open('settings', { args: { section: 'ai', nonce: Date.now() } });
  const list = rows.slice(0, max);
  return (
    <GCard title={tr('monitor.gpu.v.ai')} glyph="#bf5af2" style={style}
      right={<View onClick={openSettings}><Text style={{ fontSize: 11, color: t.accent, whiteSpace: 'nowrap' }}>{tr('monitor.gpu.ai.open')}</Text></View>}>
      <View ref={boxRef} style={{ gap: 6, ...(fit ? { flex: 1, minHeight: 0 } : {}) }}>
      {!list.length && <Text style={{ fontSize: 12, color: t.textDim, textAlign: 'start' }}>{tr('monitor.gpu.v.noAi')}</Text>}
      {list.map((r) => {
        const gpu = r.where === 'gpu';
        const live = r.state === 'R' || (r.cpu || 0) > 0.02 || (r.compute || 0) > 0.005;
        return (
          <View key={r.key} style={{ borderRadius: 9, border: `1px solid ${t.separator}`, backgroundColor: t.windowBg, paddingHorizontal: 9, paddingVertical: 6, gap: 3 }}>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 7 }}>
              <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: live || r.kind !== 'sentinela' ? P.ok : t.textDim }} />
              <Text style={{ flex: 1, minWidth: 0, fontSize: 12, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{aiTitle(tr, r)}</Text>
              <Pill text={gpu ? tr('monitor.gpu.v.onGpu') : tr('monitor.gpu.v.cpu')} color={gpu ? P.compute : null} />
            </View>
            <Text style={{ fontSize: 10.5, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{aiMeta(tr, r)}</Text>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
              {gpu ? (
                <>
                  <Text style={{ fontSize: 11, color: t.text, whiteSpace: 'nowrap' }}><Text style={{ fontWeight: 700 }}>{pctText(tr, r.compute || 0)}</Text>{` ${tr('monitor.gpu.v.e.compute')}`}</Text>
                  <Bar value={r.compute || 0} color={P.compute} />
                  {r.vram != null && <Text style={{ fontSize: 11, color: t.text, whiteSpace: 'nowrap' }}><Text style={{ fontWeight: 700 }}>{formatBytes(r.vram)}</Text>{` ${tr('monitor.gpu.v.col.vram')}`}</Text>}
                </>
              ) : (
                <>
                  <Text style={{ fontSize: 11, color: t.text, whiteSpace: 'nowrap' }}><Text style={{ fontWeight: 700 }}>{r.cpu != null ? tr('monitor.pct', { v: n0(r.cpu * 100) }) : '—'}</Text>{` ${tr('monitor.gpu.v.cpu')}`}</Text>
                  {r.rss != null && r.rss > 64 * 1024 * 1024 && <Text style={{ fontSize: 11, color: t.text, whiteSpace: 'nowrap' }}><Text style={{ fontWeight: 700 }}>{formatBytes(r.rss)}</Text>{` ${tr('monitor.gpu.v.ram')}`}</Text>}
                  {r.kind === 'sentinela' && <Text style={{ flex: 1, minWidth: 0, fontSize: 10.5, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{tr('monitor.gpu.v.cpuOnly')}</Text>}
                </>
              )}
            </View>
          </View>
        );
      })}
      </View>
    </GCard>
  );
}

// ---- NVIDIA: why there is no data, and a report to send us (nvreport.js; docs/MONITOR.md, "NVIDIA") ----

function nvReport(diag) {
  let info = {}, home = '';
  try { info = systemInfo() || {}; } catch (e) { info = {}; }
  try { home = lord.fs.home() || ''; } catch (e) { home = ''; }
  const user = home.split('/').filter(Boolean).pop() || '';
  return nvReportText({ diag, version: lordosVersion(info).version, kernel: info.kernel, arch: info.arch }, { home, user, host: info.hostname });
}

function NvFailCard({ fail, diag }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const copy = () => { clipboard.set(nvReport(diag)); toast.set(tr('monitor.gpu.nv.copied')); };
  return (
    <GCard title={fail.partial ? null : tr('monitor.gpu.nv.title')} glyph={VENDOR.nvidia}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
        <View style={{ flex: 1, minWidth: 240, gap: 3 }}>
          <Text style={{ fontSize: 12.5, color: t.text, textAlign: 'start' }}>{tr(fail.key, fail.params)}</Text>
          <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>{tr('monitor.gpu.nv.hint')}</Text>
        </View>
        <Button small label={tr('monitor.gpu.nv.copy')} onClick={copy} />
      </View>
    </GCard>
  );
}

// ---- the tab ----

let lastCard = 0;
let lastMode = 'one';

export function GpuTab({ fit = false }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const P = usePalette();
  const view = useGpu(true);
  const hist = useStore(gpuHistory);
  const fan = useFan();
  const [sel, setSelState] = useState(lastCard);
  const [mode, setModeState] = useState(lastMode);
  const [details, setDetails] = useState(false);
  const [tableRef, tableH] = useBoxHeight();
  const [aiRef, aiH] = useBoxHeight();
  const [sideRef, sideH] = useBoxHeight();
  const setSel = (i) => { lastCard = i; setSelState(i); };
  const setMode = (m) => { lastMode = m; setModeState(m); };
  if (!view) return <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.waiting')}</Text>;
  const cards = view.cards.filter((c) => c.kind !== 'display');
  const nvFail = nvFailure(view.nvDiag, view.hasNv);
  const nvCard = nvFail ? <NvFailCard fail={nvFail} diag={view.nvDiag} /> : null;
  if (!cards.length) {
    return (
      <View style={{ gap: 10 }}>
        {nvCard}
        <GCard title={tr('monitor.gpu.title')}><Text style={{ fontSize: 13, color: t.textDim }}>{tr('monitor.gpu.none')}</Text></GCard>
      </View>
    );
  }
  const i = Math.min(Number(sel) || 0, cards.length - 1);
  const c = cards[i];
  const side = mode === 'side' && cards.length > 1;
  // the rows that fit (fit mode): the table's rows are 23 px under a 22 px header, an AI row about 64 px with its gap
  const rows = fit ? rowsThatFit(tableH, 23, 22 + 18, 2, 40) : 12;
  const aiMax = fit ? rowsThatFit(aiH + 6, 64, 0, 1, 8) : 8;
  const sideRows = fit ? rowsThatFit(sideH, 23, 22, 2, 40) : 8;
  const gf = (fan?.gpuFans || []).find((f) => f.slot === c.slot);
  const chips = (
    <View style={{ flexDirection: 'row', gap: 8, alignItems: 'center', flexWrap: fit ? 'nowrap' : 'wrap' }}>
      {cards.map((x, k) => {
        const h = hist[x.key] || [];
        const on = k === i && !side;
        return (
          <View key={x.key} onClick={() => { setSel(k); if (side) setMode('one'); }}
            style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 10, paddingVertical: 5, borderRadius: 9, border: `1px solid ${on ? t.accent : t.separator}`, backgroundColor: on ? tint(t.accent) : cardBg(t) }}>
            <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: VENDOR[x.driver] || t.accent }} />
            <Text style={{ fontSize: 12, fontWeight: 700, color: t.text, whiteSpace: 'nowrap' }}>{gpuLabel(tr, x).replace(/^(AMD|Intel|NVIDIA) /, '')}</Text>
            <Spark values={h.map((p) => p.busy ?? 0)} color={P['3d']} />
            <Text style={{ fontSize: 12, color: t.text, whiteSpace: 'nowrap' }}>{pctText(tr, x.busy)}</Text>
            {x.temp != null && <Text style={{ fontSize: 12, color: t.textDim, whiteSpace: 'nowrap' }}>{tr('cooling.temp', { v: n0(x.temp) })}</Text>}
            {x.vram && <Text style={{ fontSize: 12, color: t.textDim, whiteSpace: 'nowrap' }}>{`${formatBytes(x.vram.used || 0)} / ${formatBytes(x.vram.total)}`}</Text>}
          </View>
        );
      })}
      <View style={{ flex: 1 }} />
      {cards.length > 1 && <Segmented small value={side ? 'side' : 'one'} onChange={setMode} options={[['one', tr('monitor.gpu.v.one')], ['side', tr('monitor.gpu.v.side')]]} />}
    </View>
  );
  if (side) {
    return (
      <View style={{ gap: 10, ...(fit ? { flex: 1, minHeight: 0 } : {}) }}>
        {chips}
        {nvCard}
        <View style={{ flexDirection: 'row', gap: 10, ...(fit ? { flex: 1, minHeight: 0 } : {}) }}>
          {cards.slice(0, 2).map((x) => (
            <View key={x.key} style={{ flex: 1, minWidth: 0, gap: 10 }}>
              <Header c={x} view={view} compact />
              <Engines c={x} hist={hist[x.key] || []} height={92} compact />
              <Memory c={x} hist={hist[x.key] || []} compact />
              <GCard title={tr('monitor.gpu.v.procs')} style={fit ? { flex: 1, minHeight: 0, overflow: 'hidden' } : null}>
                <View ref={x === cards[0] ? sideRef : null} style={fit ? { flex: 1, minHeight: 0 } : null}><ProcTable view={view} c={x} rows={sideRows} mini /></View>
              </GCard>
            </View>
          ))}
        </View>
      </View>
    );
  }
  return (
    <View style={{ gap: 10, ...(fit ? { flex: 1, minHeight: 0 } : {}) }}>
      {chips}
      {nvCard}
      <Header c={c} view={view} onDetails={() => setDetails((d) => !d)} />
      {details && (
        <View style={{ gap: 10 }}>
          {gf && <FanControl key={`${gf.id}${gf.mode}`} card={c} gf={gf} canChange={!!fan?.canChange} />}
          <ApisCard card={c} />
          <View onClick={() => setDetails(false)} style={{ alignSelf: 'flex-end' }}><Text style={{ fontSize: 11, color: t.accent }}>{tr('monitor.gpu.v.close')}</Text></View>
        </View>
      )}
      <View style={{ flexDirection: fit ? 'row' : 'row', gap: 10, flexWrap: fit ? 'nowrap' : 'wrap' }}>
        <Engines c={c} hist={hist[c.key] || []} style={{ flex: 1.7, minWidth: 420 }} />
        <Memory c={c} hist={hist[c.key] || []} style={{ flex: 1, minWidth: 300 }} />
      </View>
      <View style={{ flexDirection: 'row', gap: 10, flexWrap: fit ? 'nowrap' : 'wrap', ...(fit ? { flex: 1, minHeight: 0 } : {}) }}>
        <GCard title={tr('monitor.gpu.v.procs')} style={{ flex: 1.7, minWidth: 420, ...(fit ? { overflow: 'hidden' } : {}) }}
          right={<Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap' }}>{tr('monitor.gpu.v.sorted', { n: view.apps.filter((a) => c.apps.includes(a.key)).length + c.hidden.length })}</Text>}>
          <View ref={tableRef} style={fit ? { flex: 1, minHeight: 0 } : null}><ProcTable view={view} c={c} rows={rows} /></View>
        </GCard>
        <AiPanel rows={view.aiRows || []} max={aiMax} boxRef={aiRef} fit={fit} style={{ flex: 1, minWidth: 300, ...(fit ? { overflow: 'hidden' } : {}) }} />
      </View>
    </View>
  );
}

function cardBg(t) {
  return t.mode === 'dark' ? '#26272c' : '#f4f4f6';
}
