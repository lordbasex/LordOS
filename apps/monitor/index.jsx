// Monitor: CPU, memory, network, disk and process usage of this computer, and the network
// connections of each app on a map (see docs/MONITOR.md). It follows TangoOS's Monitor: tabs,
// dark cards with a big number and a live chart, a row of small stats and a footer with the
// sampling rate. axia-server (lord.monitor) reads the metrics only while this window is open,
// four times per second.
import {
  View, Text, Image, ScrollView, Segmented, Toolbar, typeScale, useTheme, useLocale, useStore, useSettings, useAppWindow,
  useMonitor, monitor, monitorSamples, formatBytes, formatNumber, AxiaLogo, iconFromFile, getApp,
} from '@lordos/sdk';
import { registerDockActions } from '@lordos/sdk';
import * as sdk from '@lordos/sdk';
import { useEffect, useMemo, useState } from 'react';
import { Chart } from './Chart.jsx';
import { Glyph, MonitorIcon } from './icons.jsx';
import { NetworkTab } from './Network.jsx';
import { RemoteGlyph } from './RemoteDesktop.jsx';
import { MonitorPrivacy } from './Privacy.jsx';
import { BlocklistsPrivacy } from './Blocklists.jsx';
import { FirewallPrivacy } from './Firewall.jsx';
import { FirewallRulesPrivacy } from './FwRules.jsx';
import { CoolingTab } from './Cooling.jsx';
import { ProcessesTab, ProcessLayers } from './Processes.jsx';
import { DisksTab } from './DisksTab.jsx';
import { VoltageTab } from './Voltage.jsx';

export function Icon({ size }) { return <MonitorIcon size={size} />; }

// Series colors (TangoOS's)
export const C = { mem: '#8b5cf6', cache: '#c4b5fd', rx: '#22c55e', tx: '#f97316', read: '#2f7cf6', write: '#ec4899' };

// Settings → Privacy → Monitor (the page belongs to another module: registered if it exists).
if (typeof sdk.registerPrivacySection === 'function') sdk.registerPrivacySection({ id: 'monitor', order: 10, Component: MonitorPrivacy });
// … and "App network": trusted and restricted apps (lord.fw, docs/FIREWALL.md)
if (typeof sdk.registerPrivacySection === 'function') sdk.registerPrivacySection({ id: 'firewall', order: 11, Component: FirewallPrivacy });
// … and the rules of each app: domains, addresses and networks, with the app's default
if (typeof sdk.registerPrivacySection === 'function') sdk.registerPrivacySection({ id: 'fwrules', order: 12, Component: FirewallRulesPrivacy });
// … and (0.8.5) the subscribed blocklists (docs/FIREWALL.md, "Blocklists")
if (typeof sdk.registerPrivacySection === 'function') sdk.registerPrivacySection({ id: 'blocklists', order: 13, Component: BlocklistsPrivacy });

// ---- formats ----
export const fmt = {
  pct: (tr, v) => tr('monitor.pct', { v: formatNumber(Math.round((v || 0) * 100), 0) }),
  mem: (n) => formatBytes(Math.max(0, n || 0)),
  rate: (tr, n) => tr('monitor.rate', { v: formatBytes(Math.max(0, n || 0)) }),
  load: (v) => formatNumber(v || 0, 2),
};

// Dark card with an icon and a title (tm-card)
export function useCardStyle() {
  const t = useTheme();
  const dark = t.mode === 'dark';
  return {
    bg: dark ? '#26272c' : '#f4f4f6',
    style: { borderRadius: 12, backgroundColor: dark ? '#26272c' : '#f4f4f6', border: `1px solid ${t.separator}`, padding: 16, gap: 12, minWidth: 0 },
  };
}

export function Card({ icon, title, right, children, style }) {
  const t = useTheme();
  const card = useCardStyle();
  return (
    <View style={{ ...card.style, ...style }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        {icon ? <Glyph name={icon} color={t.accent} /> : null}
        <Text style={{ fontSize: typeScale.callout, fontWeight: 600, color: t.textDim, whiteSpace: 'nowrap' }}>{title}</Text>
        <View style={{ flex: 1 }} />
        {right}
      </View>
      {children}
    </View>
  );
}

export function Big({ value, small }) {
  const t = useTheme();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'flex-end', gap: 6 }}>
      <Text style={{ fontSize: 24, fontWeight: 600, color: t.text, lineHeight: 1.05 }}>{value}</Text>
      {small ? <Text style={{ fontSize: 13, fontWeight: 500, color: t.textDim, marginBottom: 2 }}>{small}</Text> : null}
    </View>
  );
}

// Pair of values with a color legend (Received / Sent)
export function Pair({ items }) {
  const t = useTheme();
  return (
    <View style={{ flexDirection: 'row', gap: 20 }}>
      {items.map((i) => (
        <View key={i.label} style={{ gap: 2 }}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 5 }}>
            {i.color && <View style={{ width: 8, height: 8, borderRadius: 2, backgroundColor: i.color }} />}
            <Text style={{ fontSize: 11, color: t.textDim }}>{i.label}</Text>
          </View>
          <Text style={{ fontSize: typeScale.title, fontWeight: 600, color: t.text }}>{i.value}</Text>
        </View>
      ))}
    </View>
  );
}

// Small stat (tm-stat)
export function Stat({ label, value, unit, icon, children }) {
  const t = useTheme();
  const card = useCardStyle();
  return (
    <View style={{ flex: 1, minWidth: 120, borderRadius: 10, backgroundColor: card.bg, border: `1px solid ${t.separator}`, paddingHorizontal: 12, paddingVertical: 10, gap: 4 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 5 }}>
        {icon ? <Glyph name={icon} color={t.textDim} size={11} /> : null}
        <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap' }}>{label}</Text>
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'flex-end', gap: 4 }}>
        <Text style={{ fontSize: typeScale.title, fontWeight: 600, color: t.text, whiteSpace: 'nowrap' }}>{value}</Text>
        {unit ? <Text style={{ fontSize: 11, color: t.textDim, marginBottom: 2 }}>{unit}</Text> : null}
      </View>
      {children}
    </View>
  );
}

// Series derived from the sample history
// The Dock's quick actions: the network map and Cooling (sdk/dock.js).
registerDockActions('monitor', () => [
  { id: 'network', label: 'monitor.network', args: { tab: 'network' } },
  { id: 'cooling', label: 'monitor.tab.cooling', args: { tab: 'cooling' } },
  { id: 'disks', label: 'monitor.disks', args: { tab: 'disks' } },
]);

function useSeries(samples) {
  return useMemo(() => {
    const pick = (f) => samples.map(f);
    const ncores = samples.length ? samples[samples.length - 1].cores.length : 0;
    return {
      times: pick((m) => m.t),
      cpu: pick((m) => m.cpu),
      mem: pick((m) => m.mem.used),
      cache: pick((m) => m.mem.used + m.mem.cache),
      rx: pick((m) => m.net.rx),
      tx: pick((m) => m.net.tx),
      read: pick((m) => m.disk.read),
      write: pick((m) => m.disk.write),
      cores: Array.from({ length: ncores }, (_, i) => samples.map((m) => m.cores[i] || 0)),
    };
  }, [samples]);
}

// ---- Overview ----

function TempStat({ last }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const temp = last.temp;
  const tone = temp > 80 ? '#ff453a' : temp > 68 ? '#ff9f0a' : '#30d158';
  return (
    <Stat icon="temp" label={tr('monitor.cpuTemp')} value={temp != null ? formatNumber(Math.round(temp), 0) : '—'} unit={temp != null ? '°C' : null}>
      {temp != null && (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 5, marginTop: 4 }}>
          <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: tone }} />
          <Text style={{ fontSize: 11, color: t.textDim }}>{tr(temp > 80 ? 'monitor.tempHigh' : temp > 68 ? 'monitor.tempRaised' : 'monitor.tempNormal')}</Text>
        </View>
      )}
    </Stat>
  );
}

function Overview({ s, last }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const info = monitor.info();
  const card = useCardStyle();
  return (
    <>
      <View style={{ flexDirection: 'row', gap: 12 }}>
        <Card icon="cpu" title={tr('monitor.cpu')} style={{ flex: 1 }}
          right={<Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap' }}>{info ? tr('monitor.cpuModel', { model: info.cpuModel || tr('monitor.cpu'), count: info.cores }) : ''}</Text>}>
          <Big value={fmt.pct(tr, last.cpu)} />
          <Chart series={[{ label: tr('monitor.cpu'), values: s.cpu, color: t.accent }]} max={1} format={(v) => fmt.pct(tr, v)} times={s.times} height={86} bg={card.bg} />
        </Card>
        <Card icon="memory" title={tr('monitor.memory')} style={{ flex: 1 }}>
          <Big value={fmt.mem(last.mem.used)} small={tr('monitor.ofTotal', { total: fmt.mem(last.mem.total) })} />
          <Chart series={[{ label: tr('monitor.used'), values: s.mem, color: C.mem }]} max={last.mem.total} format={fmt.mem} times={s.times} height={86} bg={card.bg} />
        </Card>
      </View>
      <View style={{ flexDirection: 'row', gap: 12 }}>
        <Card icon="network" title={tr('monitor.network')} style={{ flex: 1 }}>
          <Pair items={[{ label: tr('monitor.received'), value: fmt.rate(tr, last.net.rx), color: C.rx }, { label: tr('monitor.sent'), value: fmt.rate(tr, last.net.tx), color: C.tx }]} />
          <Chart series={[{ label: tr('monitor.received'), values: s.rx, color: C.rx }, { label: tr('monitor.sent'), values: s.tx, color: C.tx }]}
            format={(v) => fmt.rate(tr, v)} times={s.times} height={86} bg={card.bg} />
        </Card>
        <Card icon="disk" title={tr('monitor.disk')} style={{ flex: 1 }}>
          <Pair items={[{ label: tr('monitor.read'), value: fmt.rate(tr, last.disk.read), color: C.read }, { label: tr('monitor.write'), value: fmt.rate(tr, last.disk.write), color: C.write }]} />
          <Chart series={[{ label: tr('monitor.read'), values: s.read, color: C.read }, { label: tr('monitor.write'), values: s.write, color: C.write }]}
            format={(v) => fmt.rate(tr, v)} times={s.times} height={86} bg={card.bg} />
        </Card>
      </View>
      <View style={{ flexDirection: 'row', gap: 10 }}>
        <TempStat last={last} />
        <Stat icon="fan" label={tr('monitor.fan')} value={last.fan != null ? formatNumber(last.fan, 0) : '—'} unit={last.fan != null ? tr('monitor.rpm') : null} />
        <Stat label={tr('monitor.load1')} value={fmt.load(last.load[0])} />
        <Stat label={tr('monitor.load515')} value={`${fmt.load(last.load[1])} · ${fmt.load(last.load[2])}`} />
        <Stat label={tr('monitor.cached')} value={fmt.mem(last.mem.cache)} />
      </View>
    </>
  );
}

// ---- CPU ----

function CpuTab({ s, last }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const info = monitor.info();
  const card = useCardStyle();
  const cols = 4;
  const rows = [];
  for (let i = 0; i < s.cores.length; i += cols) rows.push(s.cores.slice(i, i + cols).map((v, k) => [i + k, v]));
  return (
    <>
      <Card icon="cpu" title={tr('monitor.totalUse')} right={<Text style={{ fontSize: 20, fontWeight: 600, color: t.text }}>{fmt.pct(tr, last.cpu)}</Text>}>
        <Chart series={[{ label: tr('monitor.cpu'), values: s.cpu, color: t.accent }]} max={1} format={(v) => fmt.pct(tr, v)} times={s.times} height={160} bg={card.bg} />
      </Card>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text }}>{tr('monitor.perCore')}</Text>
        <Text style={{ fontSize: 12, color: t.textDim }}>{info?.cpuModel || ''}</Text>
      </View>
      {rows.map((row, r) => (
        <View key={r} style={{ flexDirection: 'row', gap: 10 }}>
          {row.map(([i, vals]) => (
            <View key={i} style={{ flex: 1, minWidth: 0, borderRadius: 10, backgroundColor: card.bg, border: `1px solid ${t.separator}`, paddingHorizontal: 12, paddingTop: 10, paddingBottom: 8, gap: 6 }}>
              <View style={{ flexDirection: 'row' }}>
                <Text style={{ flex: 1, fontSize: 11, color: t.textDim }}>{tr('monitor.core', { n: i })}</Text>
                <Text style={{ fontSize: 11, fontWeight: 600, color: t.text }}>{fmt.pct(tr, last.cores[i])}</Text>
              </View>
              <Chart series={[{ label: tr('monitor.core', { n: i }), values: vals, color: t.accent }]} max={1} format={(v) => fmt.pct(tr, v)} times={s.times} height={54} compact bg={card.bg} />
            </View>
          ))}
          {row.length < cols && Array.from({ length: cols - row.length }, (_, k) => <View key={`e${k}`} style={{ flex: 1 }} />)}
        </View>
      ))}
      <View style={{ flexDirection: 'row', gap: 10 }}>
        <Stat label={tr('monitor.load1m')} value={fmt.load(last.load[0])} />
        <Stat label={tr('monitor.load5m')} value={fmt.load(last.load[1])} />
        <Stat label={tr('monitor.load15m')} value={fmt.load(last.load[2])} />
        <Stat label={tr('monitor.temperature')} value={last.temp != null ? formatNumber(Math.round(last.temp), 0) : '—'} unit={last.temp != null ? '°C' : null} />
        <Stat label={tr('monitor.tasks')} value={formatNumber(last.tasks.total, 0)} unit={tr('monitor.running', { n: last.tasks.running })} />
      </View>
    </>
  );
}

// ---- Memory ----

function MemTab({ s, last }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const card = useCardStyle();
  const m = last.mem;
  const free = Math.max(0, m.total - m.used - m.cache);
  const parts = [
    { key: 'used', label: tr('monitor.memUsed'), v: m.used, color: C.mem },
    { key: 'cache', label: tr('monitor.memCache'), v: m.cache, color: C.cache },
    { key: 'free', label: tr('monitor.memFree'), v: free, color: null },
  ];
  return (
    <>
      <Card icon="memory" title={tr('monitor.memUse')} right={<Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.inTotal', { total: fmt.mem(m.total) })}</Text>}>
        <View style={{ flexDirection: 'row', height: 22, borderRadius: 6, overflow: 'hidden', backgroundColor: t.control }}>
          {parts.filter((p) => p.color).map((p) => <View key={p.key} style={{ width: `${(p.v / Math.max(1, m.total)) * 100}%`, height: 22, backgroundColor: p.color }} />)}
        </View>
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 18 }}>
          {parts.map((p) => (
            <View key={p.key} style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
              <View style={{ width: 8, height: 8, borderRadius: 2, backgroundColor: p.color || t.control }} />
              <Text style={{ fontSize: 12, color: t.textDim }}>{p.label}</Text>
              <Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{fmt.mem(p.v)}</Text>
            </View>
          ))}
          {m.swapTotal > 0 && (
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
              <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.swap')}</Text>
              <Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{tr('monitor.swapValue', { used: fmt.mem(m.swapUsed), total: fmt.mem(m.swapTotal) })}</Text>
            </View>
          )}
        </View>
      </Card>
      <Card icon="activity" title={tr('monitor.last60')}>
        <Chart series={[{ label: tr('monitor.usedCache'), values: s.cache, color: C.cache }, { label: tr('monitor.used'), values: s.mem, color: C.mem }]}
          max={m.total} format={fmt.mem} times={s.times} height={170} bg={card.bg} />
      </Card>
      <Text style={{ fontSize: 12, color: t.textDim, lineHeight: 1.5, maxWidth: 620 }}>{tr('monitor.cacheHint')}</Text>
    </>
  );
}

// ---- Disks ----

// Table column: the flexible one stretches; fixed-width ones don't shrink.
export const colBox = (c) => (c.flex ? { flex: c.flex, minWidth: 0 } : { width: c.width, flexShrink: 0 });

export function Table({ cols, rows, onSort, sort }) {
  const t = useTheme();
  const card = useCardStyle();
  return (
    <View style={{ borderRadius: 10, border: `1px solid ${t.separator}`, overflow: 'hidden', backgroundColor: t.windowBg }}>
      <View style={{ flexDirection: 'row', height: 28, alignItems: 'center', paddingHorizontal: 10, backgroundColor: card.bg }}>
        {cols.map((c) => (
          <View key={c.key} onClick={onSort && c.sort ? () => onSort(c.key) : undefined} style={{ ...colBox(c), flexDirection: 'row', justifyContent: c.num ? 'flex-end' : 'flex-start', paddingHorizontal: 6 }}>
            <Text style={{ fontSize: 11, fontWeight: 600, color: sort?.key === c.key ? t.text : t.textDim, whiteSpace: 'nowrap' }}>{c.label}{sort?.key === c.key ? (sort.dir > 0 ? ' ▲' : ' ▼') : ''}</Text>
          </View>
        ))}
      </View>
      {rows}
    </View>
  );
}

export function Cell({ col, children, color, bold }) {
  const t = useTheme();
  return (
    <View style={{ ...colBox(col), flexDirection: 'row', alignItems: 'center', gap: 6, justifyContent: col.num ? 'flex-end' : 'flex-start', paddingHorizontal: 6, overflow: 'hidden' }}>
      {typeof children === 'string' || typeof children === 'number'
        ? <Text style={{ fontSize: 12, color: color || t.text, fontWeight: bold ? 600 : 400, whiteSpace: 'nowrap', textAlign: col.num ? 'end' : 'start' }}>{children}</Text>
        : children}
    </View>
  );
}

// ---- Processes ----

// An isolated LordOS app (its own process and scope, "lpk:<id>"): a package or a built-in app such
// as Mail or Passwords; the process is shown with the app's name and icon.
export const isolatedAppOf = (item) => (typeof item?.app === 'string' && item.app.startsWith('lpk:') ? getApp(item.app.slice(4)) : null);

// (0.8.5) LordOS's own components: the built-in app's icon (Mail, Store, Terminal) or a glyph
function ComponentGlyph({ comp, size }) {
  const t = useTheme();
  const a = comp.icon ? getApp(comp.icon) : null;
  if (a?.Icon) return <a.Icon size={size} />;
  return (
    <View style={{ width: size, height: size, borderRadius: size * 0.24, backgroundColor: comp.system ? '#8e8e93' : t.accent, alignItems: 'center', justifyContent: 'center' }}>
      <Glyph name={comp.glyph || 'network'} color="#fff" size={Math.round(size * 0.62)} />
    </View>
  );
}

export function AppGlyph({ item, size = 16 }) {
  if (item.app === 'lordos') return <AxiaLogo size={size} />;
  const comp = sdk.netModel?.COMPONENTS?.[item.app];
  if (comp) return <ComponentGlyph comp={comp} size={size} />;
  if (item.app === 'remote-desktop') return <RemoteGlyph size={size} />;
  const iso = isolatedAppOf(item);
  if (iso?.Icon) return <iso.Icon size={size} />;
  if (item.appIcon) return <Image src={item.appIcon} style={{ width: size, height: size, objectFit: 'contain' }} />;
  const Ico = iconFromFile(null, item.appName || item.name || '?');
  return <Ico size={size} />;
}

// ---- window ----

let lastTab = 'overview';

// Each tab subscribes to what it shows, at the rate it needs: the chart tabs on every sample (4 per
// second in front, 1 in the background); Processes and Network, once per second. The window itself
// doesn't redraw on every sample.
function Charts({ tab, rate }) {
  const samples = useMonitor({ everyMs: rate });
  const s = useSeries(samples);
  const last = samples[samples.length - 1];
  if (!last) return null;
  return (
    <ScrollView style={{ flex: 1 }}>
      <View style={{ paddingHorizontal: 20, paddingTop: 20, paddingBottom: 24, gap: 16 }}>
        {tab === 'overview' && <Overview s={s} last={last} />}
        {tab === 'cpu' && <CpuTab s={s} last={last} />}
        {tab === 'memory' && <MemTab s={s} last={last} />}
        {tab === 'disks' && <DisksTab s={s} last={last} />}
      </View>
    </ScrollView>
  );
}

function Network({ prefs }) {
  const samples = useMonitor({ everyMs: 1000 });
  const last = samples[samples.length - 1];
  return last ? <NetworkTab last={last} prefs={prefs} /> : null;
}

function Footer() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const card = useCardStyle();
  const samples = useMonitor({ everyMs: 1000 });
  const last = samples[samples.length - 1];
  return (
    <View style={{ height: 28, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 20, backgroundColor: card.bg }}>
      <Glyph name="activity" color={t.textDim} size={11} />
      <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap' }}>{tr('monitor.footer')}</Text>
      <View style={{ flex: 1 }} />
      {last && <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap' }}>{tr('monitor.loadFooter', { load: last.load.map(fmt.load).join(' · ') })}</Text>}
    </View>
  );
}

export default function Monitor() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const { args, focused } = useAppWindow() || {};
  const [settings] = useSettings();
  const prefs = settings.monitor || {};
  const [tab, setTabState] = useState(lastTab);
  const setTab = (v) => { lastTab = v; setTabState(v); };
  useEffect(() => { if (args?.tab) setTab(args.tab); }, [args?.tab, args?.nonce]);
  const showConns = tab === 'network' && prefs.connections !== false;
  // which lists axia-server builds (without redrawing this window on every sample)
  useMonitor({ procs: tab === 'processes', conns: showConns, geo: showConns && prefs.geo !== false, everyMs: 0 });
  const ready = useStore(monitorSamples, (l) => l.length > 0);
  // Voltage only exists where the machine reports something (not in a virtual machine, for one)
  const hasVolt = useStore(monitorSamples, (l) => !!l[l.length - 1]?.volt);
  const view = tab === 'voltage' && !hasVolt ? 'overview' : tab;
  const tabs = [
    ['overview', tr('monitor.tab.overview')], ['cpu', tr('monitor.cpu')], ['memory', tr('monitor.memory')],
    ['network', tr('monitor.network')], ['disks', tr('monitor.disks')], ['processes', tr('monitor.processes')], ['cooling', tr('monitor.tab.cooling')],
    ...(hasVolt ? [['voltage', tr('monitor.tab.voltage')]] : []),
  ];
  if (!monitor.available()) {
    return <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center' }}><Text style={{ fontSize: 13, color: t.textDim }}>{tr('monitor.unavailable')}</Text></View>;
  }
  return (
    <View style={{ flex: 1 }}>
      <Toolbar style={{ paddingHorizontal: 20 }}>
        <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, width: 110, whiteSpace: 'nowrap' }}>{tr('monitor.title')}</Text>
        <View style={{ flex: 1, alignItems: 'center' }}>
          <Segmented value={view} onChange={setTab} options={tabs} />
        </View>
        <View style={{ width: 110 }} />
      </Toolbar>
      {!ready ? (
        <View style={{ flex: 1, padding: 20 }}><Text style={{ fontSize: 13, color: t.textDim }}>{tr('monitor.waiting')}</Text></View>
      ) : view === 'network' ? (
        <Network prefs={prefs} />
      ) : view === 'voltage' ? (
        <ScrollView style={{ flex: 1 }}>
          <View style={{ paddingHorizontal: 20, paddingTop: 20, paddingBottom: 24, gap: 16 }}><VoltageTab /></View>
        </ScrollView>
      ) : view === 'cooling' ? (
        <ScrollView style={{ flex: 1 }}>
          <View style={{ paddingHorizontal: 20, paddingTop: 20, paddingBottom: 24, gap: 16 }}><CoolingTab /></View>
        </ScrollView>
      ) : view === 'processes' ? (
        <ScrollView style={{ flex: 1 }}>
          <View style={{ paddingHorizontal: 20, paddingTop: 20, paddingBottom: 24, gap: 16 }}><ProcessesTab /></View>
        </ScrollView>
      ) : (
        <Charts tab={view} rate={focused === false ? 1000 : 250} />
      )}
      <View style={{ height: 1, backgroundColor: t.separator }} />
      <Footer />
      <ProcessLayers active={tab === 'processes'} />
    </View>
  );
}
