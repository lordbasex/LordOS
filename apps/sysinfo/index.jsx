// System Information (0.8.5, docs/BENCHMARK.md): every piece of hardware detected and what LordOS
// makes of it. (0.8.7) Report → Share on GitHub (share.js) and community data in Compatibility (community.js). Overview, Hardware (a source list and a detail pane) and Compatibility. The report
// comes from lord.sysinfo.report() (server/src/sysreport, the same collector as lordos-sysreport and
// lordos-fetch), read once on open and again with Refresh; model.js works out the GPU layers and the
// Compatibility panel; Performance (0.8.6, performance.jsx) runs LordBench. Nothing here holds serial numbers, addresses or names: the report has none.
import {
  View, Text, ScrollView, Button, useTheme, useLocale, LordOSLogo, systemReport, systemInfo, lordosVersion, formatBytes, formatNumber,
  Toolbar, ToolbarSpacer, ToolbarButton, Segmented, SidebarItem, GroupBox, FormRow, EmptyState, Hairline, spacing, typeScale,
  fs, userDirs, clipboard, wm, admin, remote, codeStatus, launchApp, useAppWindow,
} from '@lordos/sdk';
import { useMemo, useState } from 'react';
import { evaluate as evaluateChecks, evaluateProbe, evaluateBench, PENDING as PENDING_CHECKS, PROBE_CHECKS } from './checks.js';
import Performance from './performance.jsx';
import { Scene } from './scene.jsx';
import { cardLayers, sessionLayers, compatibility, hardwareSections, piStatus, machineName, compositionReason, reportFileName, reportJson, parseMemoryLines } from './model.js';
import { buildShareReport, summaryRows, issueUrl, shareFileNames, shareJson, sharePage } from './share.js';
import { communityFor, communitySummary } from './community.js';
import HWREPORT_SCHEMA from './hwreport.schema.json';

// The app's icon: a chip on the brand gradient.
export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.22, alignItems: 'center', justifyContent: 'center',
      background: 'linear-gradient(135deg, #B46CFF, #4F46E5)' }}>
      <View style={{ width: s * 0.5, height: s * 0.5, borderRadius: s * 0.08, border: `${Math.max(1, s * 0.04)}px solid rgba(255,255,255,0.95)`,
        alignItems: 'center', justifyContent: 'center' }}>
        <View style={{ width: s * 0.22, height: s * 0.22, borderRadius: s * 0.03, backgroundColor: '#FFD60A' }} />
      </View>
      {[0.32, 0.5, 0.68].map((p) => [
        <View key={`t${p}`} style={{ position: 'absolute', left: s * p - s * 0.02, top: s * 0.16, width: s * 0.04, height: s * 0.08, backgroundColor: 'rgba(255,255,255,0.8)' }} />,
        <View key={`b${p}`} style={{ position: 'absolute', left: s * p - s * 0.02, bottom: s * 0.16, width: s * 0.04, height: s * 0.08, backgroundColor: 'rgba(255,255,255,0.8)' }} />,
      ])}
    </View>
  );
}

const camel = (s) => String(s).replace(/-([a-z0-9])/g, (_, c) => c.toUpperCase());
const STATE_COLOR = { yes: '#30d158', works: '#30d158', limits: '#ff9f0a', no: '#ff453a', absent: '#8e8e93', unchecked: '#8e8e93' };
const ghz = (mhz) => formatNumber(mhz / 1000, 2);
// "Intel(R) Core(TM) i7 CPU @ 2.80GHz" → "Intel Core i7 @ 2.80GHz", as About shows it
const cpuName = (s) => (s || '').replace(/\(R\)|\(TM\)|\bCPU\b|\bProcessor\b/g, '').replace(/\s+/g, ' ').trim();

function Badge({ state }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 8, height: 22, borderRadius: 11,
      backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.07)' : 'rgba(0,0,0,0.05)' }}>
      <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: STATE_COLOR[state] || '#8e8e93' }} />
      <Text style={{ fontSize: typeScale.caption, fontWeight: 500, color: t.text, whiteSpace: 'nowrap' }}>{tr(`sysinfo.state.${state}`)}</Text>
    </View>
  );
}

function Row({ label, value }) {
  const t = useTheme();
  return (
    <FormRow label={label} labelWidth={170} minHeight={30}>
      <Text style={{ fontSize: typeScale.body, color: value ? t.text : t.textDim, textAlign: 'start' }}>{value || '—'}</Text>
    </FormRow>
  );
}

// A layer or compatibility row: name, state and what it means.
function StateRow({ label, state, detail }) {
  const t = useTheme();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m, minHeight: 40, paddingVertical: 6 }}>
      <Text style={{ width: 170, fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{label}</Text>
      <Badge state={state} />
      <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.callout, color: t.textDim, textAlign: 'start', lineHeight: 1.35 }}>{detail || ''}</Text>
    </View>
  );
}

// ---- Overview ----

function Card({ title, value, sub }) {
  const t = useTheme();
  return (
    <View style={{ flex: 1, minWidth: 0, minHeight: 104, gap: 4, padding: spacing.m, borderRadius: t.groupRadius, border: `1px solid ${t.separator}`,
      backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.025)' }}>
      <Text style={{ fontSize: typeScale.caption, fontWeight: 600, color: t.textDim, textAlign: 'start' }}>{title}</Text>
      <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{value || '—'}</Text>
      {sub ? <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start', lineHeight: 1.35 }}>{sub}</Text> : null}
    </View>
  );
}

function Overview({ r, info }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const ver = lordosVersion(info);
  const c = r.cpu || {};
  const s = r.session || {};
  const reason = compositionReason(s);
  const gpu = (r.gpus || []).find((g) => g.connected) || (r.gpus || [])[0];
  const disks = r.storage || [];
  const sw = r.software || {};
  const m = r.machine || {};
  const kind = m.virtualization ? tr('sysinfo.kind.vm', { name: m.virtualization }) : m.chassis ? tr(`sysinfo.kind.${camel(m.chassis)}`) : '';
  return (
    <ScrollView style={{ flex: 1 }}>
      <View style={{ padding: spacing.xl, gap: spacing.xl }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.l }}>
          <LordOSLogo size={64} />
          <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
            <Text style={{ fontSize: typeScale.largeTitle, fontWeight: 600, color: t.text, textAlign: 'start' }}>{machineName(r) || tr('sysinfo.thisComputer')}</Text>
            <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>
              {[kind, ver.channel ? tr('sysinfo.lordosChannel', { v: ver.version, channel: ver.channel }) : tr('sysinfo.lordos', { v: ver.version }), sw.os].filter(Boolean).join(' · ')}
            </Text>
            {s.fixture ? <Text style={{ fontSize: typeScale.caption, color: '#ff9f0a', textAlign: 'start' }}>{tr('sysinfo.fixture')}</Text> : null}
          </View>
        </View>
        <View style={{ flexDirection: 'row', gap: spacing.m }}>
          <Card title={tr('sysinfo.section.cpu')} value={cpuName(c.model) || c.arch}
            sub={[tr('sysinfo.coresThreads', { cores: c.cores, threads: c.threads }), c.maxMHz ? tr('sysinfo.upTo', { ghz: ghz(c.maxMHz) }) : ''].filter(Boolean).join(' · ')} />
          <Card title={tr('sysinfo.section.memory')} value={formatBytes((r.memory?.totalKB || 0) * 1024)}
            sub={(r.memory?.zram || []).length ? tr('sysinfo.withZram', { size: formatBytes(r.memory.zram[0].sizeKB * 1024) }) : r.memory?.swapKB ? tr('sysinfo.withSwap', { size: formatBytes(r.memory.swapKB * 1024) }) : ''} />
          <Card title={tr('sysinfo.section.graphics')} value={gpu ? (gpu.device || gpu.vendor || gpu.driver) : tr('sysinfo.none')}
            sub={s.composition === 'gpu' ? tr('sysinfo.composedGpu') : tr('sysinfo.composedCpu', { reason: tr(`sysinfo.reason.${camel(reason)}`) })} />
        </View>
        <View style={{ flexDirection: 'row', gap: spacing.m }}>
          <Card title={tr('sysinfo.section.displays')} value={(r.displays || []).length ? (r.displays || []).map((d) => d.name || d.connector).join(', ') : tr('sysinfo.none')}
            sub={(r.displays || []).map((d) => d.nativeWidth ? `${d.nativeWidth} × ${d.nativeHeight}` : d.mode).filter(Boolean).join(', ')} />
          <Card title={tr('sysinfo.section.storage')} value={formatBytes(disks.reduce((a, d) => a + d.bytes, 0))}
            sub={tr('sysinfo.disks', { count: disks.length })} />
          <Card title={tr('sysinfo.isolation')} value={sw.landlockAbi > 0 ? tr('sysinfo.landlock', { abi: sw.landlockAbi }) : tr('sysinfo.landlockUnknown')}
            sub={[sw.bpfLsm ? tr('sysinfo.bpfLsm') : '', sw.cgroup2 ? tr('sysinfo.cgroup2') : '', sw.services?.['lord-netperm'] ? tr('sysinfo.appFirewall') : ''].filter(Boolean).join(' · ')} />
        </View>
      </View>
    </ScrollView>
  );
}

// ---- Hardware ----

function layerDetail(tr, l) {
  if (l.note) return tr(`sysinfo.note.${camel(l.note)}`, l.vars || {});
  if (l.state !== 'yes') return '';
  return { device: () => l.vars.name, driver: () => l.vars.driver, kms: () => tr('sysinfo.detail.kms', l.vars), egl: () => l.vars.renderer }[l.id]?.() || '';
}

function Graphics({ r }) {
  const { t: tr } = useLocale();
  return (
    <View style={{ gap: spacing.xl }}>
      {(r.gpus || []).map((g) => (
        <GroupBox key={g.card} title={tr('sysinfo.cardTitle', { card: g.card, bus: g.bus })}>
          {cardLayers(g).map((l) => <StateRow key={l.id} label={tr(`sysinfo.layer.${l.id}`)} state={l.state} detail={layerDetail(tr, l)} />)}
        </GroupBox>
      ))}
      <GroupBox title={tr('sysinfo.thisSession')} footer={tr('sysinfo.uncheckedFooter')}>
        {sessionLayers(r.session).map((l) => <StateRow key={l.id} label={tr(`sysinfo.layer.${l.id}`)} state={l.state}
          detail={l.id === 'composition' && l.note ? tr(`sysinfo.reason.${camel(l.note)}`) : layerDetail(tr, l)} />)}
      </GroupBox>
    </View>
  );
}

function List({ items, render }) {
  return <View style={{ gap: spacing.l }}>{items.map(render)}</View>;
}

function Section({ id, r }) {
  const { t: tr } = useLocale();
  const yesNo = (v) => (v ? tr('sysinfo.yes') : tr('sysinfo.no'));
  const m = r.machine || {}, c = r.cpu || {}, mem = r.memory || {}, sw = r.software || {};
  switch (id) {
    case 'computer': return (
      <GroupBox>
        <Row label={tr('sysinfo.f.vendor')} value={m.vendor} />
        <Row label={tr('sysinfo.f.product')} value={[m.product, m.version].filter(Boolean).join(' ')} />
        <Row label={tr('sysinfo.f.model')} value={m.model} />
        <Row label={tr('sysinfo.f.board')} value={[m.boardVendor, m.boardName].filter(Boolean).join(' ')} />
        <Row label={tr('sysinfo.f.kind')} value={m.virtualization ? tr('sysinfo.kind.vm', { name: m.virtualization }) : m.chassis ? tr(`sysinfo.kind.${camel(m.chassis)}`) : ''} />
        <Row label={tr('sysinfo.f.firmware')} value={[m.firmware ? tr(`sysinfo.firmware.${m.firmware}`) : '', m.biosVendor, m.biosVersion, m.biosDate].filter(Boolean).join(' · ')} />
        <Row label={tr('sysinfo.f.secureBoot')} value={m.secureBoot == null ? '' : m.secureBoot ? tr('sysinfo.on') : tr('sysinfo.off')} />
      </GroupBox>);
    case 'cpu': return (
      <GroupBox>
        <Row label={tr('sysinfo.f.model')} value={cpuName(c.model)} />
        <Row label={tr('sysinfo.f.vendor')} value={c.vendor} />
        <Row label={tr('sysinfo.f.arch')} value={c.arch} />
        <Row label={tr('sysinfo.f.layout')} value={tr('sysinfo.layout', { sockets: c.sockets, cores: c.cores, threads: c.threads })} />
        <Row label={tr('sysinfo.f.frequency')} value={c.maxMHz ? tr('sysinfo.freqRange', { min: ghz(c.minMHz), max: ghz(c.maxMHz) }) : ''} />
        <Row label={tr('sysinfo.f.governor')} value={c.governor} />
        <Row label={tr('sysinfo.f.cache')} value={[c.l1dKB && `L1d ${c.l1dKB} KB`, c.l1iKB && `L1i ${c.l1iKB} KB`, c.l2KB && `L2 ${c.l2KB} KB`, c.l3KB && `L3 ${c.l3KB} KB`].filter(Boolean).join(' · ')} />
        <Row label={tr('sysinfo.f.instructions')} value={c.flags} />
        <Row label={tr('sysinfo.f.microcode')} value={c.microcode} />
      </GroupBox>);
    case 'memory': return (
      <View style={{ gap: spacing.xl }}>
        <GroupBox>
          <Row label={tr('sysinfo.f.total')} value={formatBytes((mem.totalKB || 0) * 1024)} />
          <Row label={tr('sysinfo.f.swap')} value={mem.swapKB ? formatBytes(mem.swapKB * 1024) : tr('sysinfo.none')} />
          {(mem.zram || []).map((z, i) => <Row key={i} label={tr('sysinfo.f.zram')} value={`${formatBytes(z.sizeKB * 1024)}${z.algorithm ? ` · ${z.algorithm}` : ''}`} />)}
        </GroupBox>
        <MemoryModules />
      </View>);
    case 'graphics': return <Graphics r={r} />;
    case 'displays': return <List items={r.displays || []} render={(d, i) => (
      <GroupBox key={i} title={d.connector}>
        <Row label={tr('sysinfo.f.monitor')} value={[d.name, d.manufacturer].filter(Boolean).join(' · ')} />
        <Row label={tr('sysinfo.f.native')} value={d.nativeWidth ? `${d.nativeWidth} × ${d.nativeHeight}` : d.mode} />
        <Row label={tr('sysinfo.f.size')} value={d.diagonalTenths ? tr('sysinfo.diagonal', { inch: formatNumber(d.diagonalTenths / 10, 1), w: d.widthCm, h: d.heightCm }) : ''} />
        <Row label={tr('sysinfo.f.year')} value={d.year ? String(d.year) : ''} />
        <Row label={tr('sysinfo.f.card')} value={d.card} />
      </GroupBox>)} />;
    case 'storage': return <List items={r.storage || []} render={(d) => (
      <GroupBox key={d.name} title={d.name}>
        <Row label={tr('sysinfo.f.model')} value={[d.vendor, d.model].filter(Boolean).join(' ')} />
        <Row label={tr('sysinfo.f.size')} value={formatBytes(d.bytes)} />
        <Row label={tr('sysinfo.f.connection')} value={[d.transport ? tr(`sysinfo.bus.${d.transport}`) : '', d.link].filter(Boolean).join(' · ')} />
        <Row label={tr('sysinfo.f.kind')} value={d.removable ? tr('sysinfo.removable') : d.rotational ? tr('sysinfo.hdd') : tr('sysinfo.ssd')} />
      </GroupBox>)} />;
    case 'network': return (
      <GroupBox>
        {(r.network || []).map((n, i) => <Row key={i} label={tr(`sysinfo.net.${n.kind}`)}
          value={[n.driver, n.up ? (n.speedMbps ? tr('sysinfo.upAt', { mbps: n.speedMbps }) : tr('sysinfo.up')) : tr('sysinfo.down')].filter(Boolean).join(' · ')} />)}
      </GroupBox>);
    case 'audio': return <GroupBox>{(r.audio || []).map((a, i) => <Row key={i} label={tr('sysinfo.f.card')} value={a} />)}</GroupBox>;
    case 'bluetooth': return <GroupBox>{(r.bluetooth || []).map((b, i) => <Row key={i} label={tr('sysinfo.f.controller')} value={b.driver} />)}</GroupBox>;
    case 'power': return <List items={r.power || []} render={(p, i) => (
      <GroupBox key={i} title={tr(`sysinfo.power.${p.kind}`)}>
        {p.kind === 'battery' ? [
          <Row key="m" label={tr('sysinfo.f.model')} value={[p.vendor, p.model].filter(Boolean).join(' ')} />,
          <Row key="t" label={tr('sysinfo.f.technology')} value={p.technology} />,
          <Row key="c" label={tr('sysinfo.f.charge')} value={p.capacity != null ? `${p.capacity} %` : ''} />,
          <Row key="h" label={tr('sysinfo.f.health')} value={p.health != null ? tr('sysinfo.health', { health: p.health }) : ''} />,
          <Row key="s" label={tr('sysinfo.f.status')} value={p.status} />,
        ] : <Row label={tr('sysinfo.f.plugged')} value={p.online == null ? '' : yesNo(p.online)} />}
      </GroupBox>)} />;
    case 'thermal': return (
      <GroupBox>
        {(r.thermal || []).map((x, i) => <Row key={i} label={x.sensor}
          value={[x.milliC != null ? `${formatNumber(x.milliC / 1000, 1)} °C` : '', x.fans ? tr('sysinfo.fans', { count: x.fans }) : ''].filter(Boolean).join(' · ')} />)}
      </GroupBox>);
    case 'pci': return (
      <GroupBox>
        {(r.pci || []).map((p, i) => <Row key={i} label={p.className || p.class} value={[[p.vendor, p.device].filter(Boolean).join(' ') || `${p.vendorId}:${p.deviceId}`, p.driver].filter(Boolean).join(' · ')} />)}
      </GroupBox>);
    case 'usb': return (
      <GroupBox>
        {(r.usb || []).map((u, i) => <Row key={i} label={u.speedMbps ? tr('sysinfo.usbSpeed', { mbps: u.speedMbps }) : 'USB'}
          value={[[u.vendor, u.product].filter(Boolean).join(' ') || `${u.vendorId}:${u.productId}`, u.driver].filter(Boolean).join(' · ')} />)}
      </GroupBox>);
    case 'raspberryPi': {
      const pi = r.raspberryPi;
      const st = piStatus(pi);
      return (
        <GroupBox>
          <Row label={tr('sysinfo.f.model')} value={pi.model} />
          <Row label={tr('sysinfo.f.revision')} value={pi.revision} />
          {st.power && <Row label={tr('sysinfo.f.power')} value={tr(`sysinfo.pi.${st.power}`)} />}
          {st.throttling && <Row label={tr('sysinfo.f.throttling')} value={tr(`sysinfo.pi.${st.throttling}`)} />}
        </GroupBox>);
    }
    case 'software': return (
      <View style={{ gap: spacing.xl }}>
        <GroupBox>
          <Row label={tr('sysinfo.f.system')} value={sw.os} />
          <Row label={tr('sysinfo.f.kernel')} value={sw.kernel} />
          <Row label={tr('sysinfo.f.lsm')} value={sw.lsm} />
          <Row label={tr('sysinfo.f.landlock')} value={sw.landlockAbi == null ? '' : sw.landlockAbi ? String(sw.landlockAbi) : tr('sysinfo.unavailable')} />
          <Row label={tr('sysinfo.f.cgroup2')} value={yesNo(sw.cgroup2)} />
        </GroupBox>
        <GroupBox title={tr('sysinfo.services')}>
          {Object.entries(sw.services || {}).map(([k, v]) => <Row key={k} label={k} value={v ? tr('sysinfo.running') : tr('sysinfo.notRunning')} />)}
        </GroupBox>
        <GroupBox title={tr('sysinfo.kernelOptions')}>
          {Object.entries(sw.kernelConfig || {}).map(([k, v]) => <Row key={k} label={k.replace(/^CONFIG_/, '')} value={['y', 'm', 'n'].includes(v) ? tr(`sysinfo.kconf.${v}`) : tr('sysinfo.unknown')} />)}
        </GroupBox>
      </View>);
    default: return null;
  }
}

// The memory modules come from the firmware's tables, which only root can read: the read-only
// hw-memory-modules action, on request.
function MemoryModules() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [state, setState] = useState({ busy: false, list: null, error: null });
  const read = () => {
    const lines = [];
    setState({ busy: true, list: null, error: null });
    admin.run('hw-memory-modules', '', (l) => lines.push(l), { quiet: true })
      .then(() => setState({ busy: false, list: parseMemoryLines(lines), error: null }), (e) => setState({ busy: false, list: null, error: e.message }));
  };
  const list = state.list;
  return (
    <GroupBox title={tr('sysinfo.modules.title')} footer={tr('sysinfo.modules.footer')}>
      {list && list.length ? list.map((m, i) => (
        <Row key={i} label={m.slot || tr('sysinfo.modules.slot', { n: i + 1 })}
          value={m.sizeMB ? [formatBytes(m.sizeMB * 1048576), m.type, m.speed ? tr('sysinfo.modules.speed', { mts: m.configured && m.configured !== m.speed ? `${m.configured}/${m.speed}` : m.speed }) : '', m.maker].filter(Boolean).join(' · ')
            : tr('sysinfo.modules.empty')} />
      )) : (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m, minHeight: 40, paddingVertical: 6 }}>
          <Text style={{ flex: 1, fontSize: typeScale.callout, color: state.error ? '#ff453a' : t.textDim, textAlign: 'start' }}>
            {state.error || (list ? tr('sysinfo.modules.none') : state.busy ? tr('sysinfo.modules.reading') : tr('sysinfo.modules.hint'))}</Text>
          <Button small label={tr('sysinfo.modules.read')} disabled={state.busy || !admin.available()} onClick={read} />
        </View>
      )}
    </GroupBox>
  );
}

function Hardware({ r }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const sections = useMemo(() => hardwareSections(r), [r]);
  const [sel, setSel] = useState('computer');
  const cur = sections.includes(sel) ? sel : sections[0];
  return (
    <View style={{ flex: 1, flexDirection: 'row' }}>
      <View style={{ width: 200, backgroundColor: t.sidebarBg }}>
        <ScrollView style={{ flex: 1 }} contentStyle={{ padding: spacing.s, gap: 1 }}>
          {sections.map((id) => <SidebarItem key={id} label={tr(`sysinfo.section.${id}`)} selected={id === cur} onClick={() => setSel(id)} />)}
        </ScrollView>
        <Hairline edge="end" />
      </View>
      <ScrollView style={{ flex: 1 }}>
        <View style={{ padding: spacing.xl, gap: spacing.l }}>
          <Text style={{ fontSize: typeScale.title, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr(`sysinfo.section.${cur}`)}</Text>
          <Section id={cur} r={r} />
        </View>
      </ScrollView>
    </View>
  );
}

// ---- Compatibility ----

function compatNote(tr, c) {
  if (!c.note) return c.vars && c.vars.count != null && c.state === 'works' ? tr(`sysinfo.compat.count.${c.id}`, c.vars) : '';
  if (c.note.startsWith('cpu.')) return tr('sysinfo.compat.cpuBecause', { reason: tr(`sysinfo.reason.${camel(c.note.slice(4))}`) });
  return tr(`sysinfo.compat.${c.note.replace('.', '_')}`, c.vars);
}

// (0.8.7) the catalogue's snapshot, verified in C (lord.sysinfo.community(), sysreport/hwsnap.c): { state, snapshot }
function useCommunity(r) {
  return useMemo(() => {
    try { return (typeof lord !== 'undefined' && lord.sysinfo.community && lord.sysinfo.community()) || { state: 'missing' }; } catch (e) { return { state: 'missing' }; }
  }, [r]);
}

// what other LordOS users confirmed for this row's devices with this version, marked as community data
function communityNote(tr, s) {
  if (!s) return '';
  if (s.onlyReferences) return tr(`sysinfo.community.reference.${s.state}`);
  return tr('sysinfo.community.counts', { count: s.reports, works: s.works, limits: s.limits, no: s.no });
}

function Compatibility({ r }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const rows = useMemo(() => compatibility(r), [r]);
  const comm = useCommunity(r);
  const version = (r.software && r.software.lordos) || '';
  const byRow = useMemo(() => (comm.state === 'ok' ? communityFor(comm.snapshot, r, version) : {}), [comm, r, version]);
  const sn = comm.state === 'ok' ? comm.snapshot : null;
  return (
    <ScrollView style={{ flex: 1 }}>
      <View style={{ padding: spacing.xl, gap: spacing.l }}>
        <GroupBox footer={tr('sysinfo.compatFooter')}>
          {rows.map((c) => {
            const cs = communitySummary(byRow[c.id]);
            return (
              <View key={c.id}>
                <StateRow label={tr(`sysinfo.component.${c.id}`)} state={c.state} detail={compatNote(tr, c)} />
                {cs ? (
                  <View style={{ flexDirection: 'row', gap: spacing.m, paddingBottom: 6 }}>
                    <View style={{ width: 170 }} />
                    <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.caption, color: t.textDim, textAlign: 'start', lineHeight: 1.35 }}>
                      {`${tr('sysinfo.community.label')} · ${communityNote(tr, cs)}`}</Text>
                  </View>
                ) : null}
              </View>
            );
          })}
        </GroupBox>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start', lineHeight: 1.4 }}>
          {sn ? tr('sysinfo.community.footer', { date: sn.generated, count: sn.reports, version: version.replace(/-.*$/, '') })
            : tr(`sysinfo.community.none.${comm.state === 'missing' ? 'missing' : 'unverified'}`)}</Text>
      </View>
    </ScrollView>
  );
}

// ---- Checks ----

const CHECK_COLOR = { pass: '#30d158', warn: '#ff9f0a', fail: '#ff453a', info: '#0a84ff', unknown: '#8e8e93', pending: '#8e8e93' };

function CheckRow({ c }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const note = !c.note ? '' : c.note.startsWith('gpuNot.')
    ? tr('sysinfo.compat.cpuBecause', { reason: tr(`sysinfo.reason.${camel(c.note.slice(7))}`) })
    : tr(`sysinfo.checkNote.${c.note}`, c.vars);
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m, minHeight: 44, paddingVertical: 6 }}>
      <View style={{ width: 10, height: 10, borderRadius: 5, backgroundColor: CHECK_COLOR[c.state] || '#8e8e93' }} />
      <View style={{ width: 220, gap: 2 }}>
        <Text style={{ fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{tr(`sysinfo.check.${c.id}`)}</Text>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>
          {[tr(`sysinfo.level.${c.level}`), c.paper ? tr('sysinfo.paperRef', { ref: c.paper }) : ''].filter(Boolean).join(' · ')}</Text>
      </View>
      <Text style={{ width: 120, fontSize: typeScale.callout, fontWeight: 600, color: CHECK_COLOR[c.state], textAlign: 'start' }}>{tr(`sysinfo.checkState.${c.state}`)}</Text>
      <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.callout, color: t.textDim, textAlign: 'start', lineHeight: 1.35 }}>{note}</Text>
    </View>
  );
}

// The Sandbox Probe (apps/sysprobe): started like any isolated app; its result arrives through
// lord-server (lord.sysinfo.probe()). It takes well under a second; after 10 s it didn't report.
function useProbe() {
  const [st, setSt] = useState(() => { let p = null; try { p = lord.sysinfo.probe(); } catch (e) { /* older server */ } return { busy: false, result: p, error: null }; });
  const run = () => {
    const since = Date.now() - 1000;
    setSt((x) => ({ ...x, busy: true, error: null }));
    let ok = false;
    try { ok = !!lord.apps.launchPackage('sysprobe'); } catch (e) { ok = false; }
    if (!ok) { setSt((x) => ({ ...x, busy: false, error: 'start' })); return; }
    const t0 = Date.now();
    const id = setInterval(() => {
      let p = null;
      try { p = lord.sysinfo.probe(); } catch (e) { p = null; }
      if (p && p.at >= since) { clearInterval(id); setSt({ busy: false, result: p, error: null }); }
      else if (Date.now() - t0 > 10000) { clearInterval(id); setSt((x) => ({ ...x, busy: false, error: 'timeout' })); }
    }, 250);
  };
  return [st, run];
}

// The Test Bench (a Wayland client of its own, lordos-testbench): its last report, read from the user's state
// folder, and a button that opens it from the launcher like any Linux app.
function useBenchReport(n) {
  return useMemo(() => {
    try { return JSON.parse(fs.read(`${fs.home()}/.local/state/lordos/testbench.json`)); } catch (e) { return null; }
  }, [n]);
}

function BenchGroup() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [n, setN] = useState(0);
  const b = useBenchReport(n);
  const rows = useMemo(() => evaluateBench(b), [b]);
  return (
    <View style={{ gap: spacing.s }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m }}>
        <View style={{ flex: 1, gap: 2 }}>
          <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('sysinfo.bench.title')}</Text>
          <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start', lineHeight: 1.4 }}>{tr('sysinfo.bench.intro')}</Text>
        </View>
        <Button label={tr('sysinfo.bench.reload')} onClick={() => setN(n + 1)} />
        <Button primary label={tr('sysinfo.bench.open')} onClick={() => launchApp('desktop:lordos-testbench')} />
      </View>
      <GroupBox footer={b && b.at ? tr('sysinfo.probe.when', { time: new Date(b.at).toLocaleString() }) : null}>
        {rows ? rows.map((c) => <CheckRow key={c.id} c={c} />)
          : <CheckRow c={{ id: 'wayland', level: 'effective', state: 'pending', paper: '§3.4' }} />}
      </GroupBox>
    </View>
  );
}

function Checks({ r }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const rows = useMemo(() => {
    let facts = null, rem = null, stats = null;
    try { facts = lord.sysinfo.facts(); } catch (e) { /* older server */ }
    try { rem = remote.status(); } catch (e) { /* no remote desktop */ }
    try { stats = lord.stats(); } catch (e) { /* no stats */ }
    return evaluateChecks({ report: r, facts, code: codeStatus(), remote: rem, stats });
  }, [r]);
  const [probe, runProbe] = useProbe();
  const probeRows = useMemo(() => (probe.result ? evaluateProbe(probe.result) : null), [probe.result]);
  return (
    <ScrollView style={{ flex: 1 }}>
      <View style={{ padding: spacing.xl, gap: spacing.xl }}>
        <GroupBox footer={tr('sysinfo.checksFooter')}>
          {rows.map((c) => <CheckRow key={c.id} c={c} />)}
        </GroupBox>
        <View style={{ gap: spacing.s }}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m }}>
            <View style={{ flex: 1, gap: 2 }}>
              <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('sysinfo.probe.title')}</Text>
              <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start', lineHeight: 1.4 }}>{tr('sysinfo.probe.intro')}</Text>
            </View>
            <Button primary label={probe.busy ? tr('sysinfo.probe.running') : probe.result ? tr('sysinfo.probe.again') : tr('sysinfo.probe.run')} disabled={probe.busy} onClick={runProbe} />
          </View>
          {probe.error ? <Text style={{ fontSize: typeScale.callout, color: '#ff453a', textAlign: 'start' }}>{tr(`sysinfo.probe.error.${probe.error}`)}</Text> : null}
          <GroupBox footer={probe.result ? tr('sysinfo.probe.when', { time: new Date(probe.result.at).toLocaleTimeString() }) : null}>
            {(probeRows || PROBE_CHECKS.map((id) => ({ id, level: 'effective', state: 'pending' }))).map((c) => <CheckRow key={c.id} c={c} />)}
          </GroupBox>
        </View>
        <BenchGroup />
        <GroupBox title={tr('sysinfo.checksPendingTitle')} footer={tr('sysinfo.checksPendingFooter')}>
          {PENDING_CHECKS.map((id) => <CheckRow key={id} c={{ id, level: 'effective', state: 'pending' }} />)}
        </GroupBox>
      </View>
    </ScrollView>
  );
}

// ---- Report ----

// The exact text that would be saved, line by line (leading spaces kept as no-break spaces).
function Preview({ text }) {
  const t = useTheme();
  const lines = useMemo(() => text.split('\n'), [text]);
  return (
    <View style={{ flex: 1, borderRadius: t.groupRadius, border: `1px solid ${t.separator}`, overflow: 'hidden',
      backgroundColor: t.mode === 'dark' ? 'rgba(0,0,0,0.25)' : 'rgba(0,0,0,0.03)' }}>
      <ScrollView style={{ flex: 1 }} contentStyle={{ padding: spacing.m, direction: 'ltr' }}>
        {lines.map((l, i) => (
          <Text key={i} style={{ fontFamily: 'monospace', fontSize: 12, lineHeight: 1.45, color: t.text, textAlign: 'left', minHeight: 17 }}>
            {l.replace(/^ +/, (m) => '\u00a0'.repeat(m.length)) || '\u00a0'}</Text>
        ))}
      </ScrollView>
    </View>
  );
}

// ---- Share on GitHub (0.8.7) ----

const SHARE_ROWS = ['machine', 'kind', 'cpu', 'gpu', 'memory', 'arch', 'lordos', 'kernel', 'live', 'bench', 'compat'];
const PAGE_KEYS = ['title', 'lead', 'summaryHead', 'sharedHead', 'shared1', 'shared2', 'shared3', 'shared4', 'neverHead', 'never1', 'never2', 'never3',
  'stepsHead', 'step1', 'step2', 'step3', 'step4', 'open', 'fileLabel', 'footer'];

function savedBench(which) {
  try { return (typeof lord !== 'undefined' && lord.sysinfo.bench.saved(which)) || null; } catch (e) { return null; }
}

// The summary rows with their labels in the user's language (the values are the report's own, as they travel).
function shareRows(tr, rep) {
  return summaryRows(rep).filter(([k]) => SHARE_ROWS.includes(k)).map(([k, v]) => [tr(`sysinfo.share.row.${k}`),
    k === 'live' ? tr(v === 'yes' ? 'sysinfo.perf.liveYes' : 'sysinfo.perf.liveNo')
      : k === 'kind' && ['board', 'desktop', 'laptop', 'server', 'vm'].includes(v) ? tr(`sysinfo.share.kind.${v}`) : v]);
}

// Preview first, explicit consent on every share (nothing is remembered); then the report and the page are saved to
// Documents and the page opens in the default browser through lord.apps.launchWith('', path). LordOS sends nothing.
function Share({ r, onClose }) {
  const t = useTheme();
  const { t: tr, lang, dir } = useLocale();
  const [msg, setMsg] = useState(null);
  const rep = useMemo(() => buildShareReport({
    report: r, bench: savedBench(0), desktop: savedBench(2), compat: compatibility(r), schema: HWREPORT_SCHEMA,
    date: /^\d{4}-\d{2}-\d{2}$/.test(r.date || '') ? r.date : '',
  }), [r]);
  const json = useMemo(() => shareJson(rep), [rep]);
  const rows = useMemo(() => shareRows(tr, rep), [rep, tr]);
  const save = () => {
    const docs = userDirs().documents;
    try { fs.mkdir(docs); } catch (e) { /* already there */ }
    const names = shareFileNames(rep.createdAt, (n) => { try { return !!fs.stat(`${docs}/${n}`); } catch (e) { return false; } });
    if (!names) { setMsg({ ok: false, text: tr('sysinfo.share.failed') }); return; }
    const text = Object.fromEntries(PAGE_KEYS.map((k) => [k, tr(`sysinfo.share.page.${k}`)]));
    const html = sharePage({ text, rows, url: issueUrl(rep), jsonName: names.json, lang: lang || 'en', rtl: dir === 'rtl' });
    try {
      fs.write(`${docs}/${names.json}`, json);
      fs.write(`${docs}/${names.html}`, html);
    } catch (e) { setMsg({ ok: false, text: tr('sysinfo.share.failed') }); return; }
    let opened = false;
    try { opened = !!lord.apps.launchWith('', `${docs}/${names.html}`); } catch (e) { opened = false; }
    setMsg({ ok: true, dir: docs, text: tr(opened ? 'sysinfo.share.opened' : 'sysinfo.share.savedOnly', { json: names.json, html: names.html }) });
  };
  return (
    <ScrollView style={{ flex: 1 }}>
    <View style={{ padding: spacing.xl, gap: spacing.m }}>
      <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('sysinfo.share.title')}</Text>
      <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start', lineHeight: 1.4 }}>{tr('sysinfo.share.intro')}</Text>
      <GroupBox footer={[rep.live ? tr('sysinfo.share.live') : '', rep.bench || rep.desktop ? tr('sysinfo.share.withBench') : tr('sysinfo.share.noBench')].filter(Boolean).join(' ')}>
        {rows.map(([k, v]) => <Row key={k} label={k} value={v} />)}
      </GroupBox>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
        <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{tr('sysinfo.share.exact')}</Text>
        <Button label={tr('sysinfo.share.back')} onClick={onClose} />
        <Button primary label={tr('sysinfo.share.go')} onClick={save} />
      </View>
      {msg ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
          <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.callout, color: msg.ok ? '#30d158' : '#ff453a', textAlign: 'start' }}>{msg.text}</Text>
          {msg.dir ? <Button small plain label={tr('sysinfo.report.showInFiles')} onClick={() => wm.open('files', { new: true, args: { path: msg.dir } })} /> : null}
        </View>
      ) : null}
      <View style={{ height: 320 }}><Preview text={json} /></View>
    </View>
    </ScrollView>
  );
}

function Report({ r }) {
  const [sharing, setSharing] = useState(false);
  if (sharing) return <Share r={r} onClose={() => setSharing(false)} />;
  return <ReportText r={r} onShare={() => setSharing(true)} />;
}

function ReportText({ r, onShare }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [fmt, setFmt] = useState('markdown');
  const [msg, setMsg] = useState(null);
  const md = useMemo(() => { try { return lord.sysinfo.markdown() || ''; } catch (e) { return ''; } }, [r]);
  const text = fmt === 'json' ? reportJson(r) : md;
  const save = () => {
    const dir = userDirs().documents;
    try { fs.mkdir(dir); } catch (e) { /* already there */ }
    const name = reportFileName(r.date, fmt === 'json' ? 'json' : 'md', (n) => { try { return !!fs.stat(`${dir}/${n}`); } catch (e) { return false; } });
    if (!name) { setMsg({ ok: false, text: tr('sysinfo.report.saveFailed') }); return; }
    try {
      fs.write(`${dir}/${name}`, text);
      setMsg({ ok: true, text: tr('sysinfo.report.saved', { name }), dir });
    } catch (e) { setMsg({ ok: false, text: tr('sysinfo.report.saveFailed') }); }
  };
  const copy = () => { clipboard.set(text); setMsg({ ok: true, text: tr('sysinfo.report.copied') }); };
  return (
    <View style={{ flex: 1, padding: spacing.xl, gap: spacing.m }}>
      <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start', lineHeight: 1.4 }}>{tr('sysinfo.report.intro')}</Text>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
        <Segmented small value={fmt} onChange={(v) => { setFmt(v); setMsg(null); }} options={[['markdown', tr('sysinfo.report.markdown')], ['json', 'JSON']]} />
        <View style={{ flex: 1 }} />
        <Button label={tr('sysinfo.report.copy')} onClick={copy} />
        <Button label={tr('sysinfo.share.button')} onClick={onShare} />
        <Button primary label={tr('sysinfo.report.save')} onClick={save} />
      </View>
      {msg ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
          <Text style={{ fontSize: typeScale.callout, color: msg.ok ? '#30d158' : '#ff453a', textAlign: 'start' }}>{msg.text}</Text>
          {msg.dir ? <Button small plain label={tr('sysinfo.report.showInFiles')} onClick={() => wm.open('files', { new: true, args: { path: msg.dir } })} /> : null}
        </View>
      ) : null}
      <Preview text={text} />
    </View>
  );
}

export default function SystemInformation() {
  const { args } = useAppWindow() || {};
  if (args && args.scene) return <Scene />;      // LordBench's desktop scene (scene.jsx)
  return <SystemInformationMain />;
}

function SystemInformationMain() {
  const { t: tr } = useLocale();
  const [tab, setTab] = useState('overview');
  const [n, setN] = useState(0);
  const r = useMemo(() => systemReport(), [n]);
  const info = useMemo(() => systemInfo(), [n]);
  if (!r) return <EmptyState mascot="confused" title={tr('sysinfo.unavailable')} />;
  return (
    <View style={{ flex: 1 }}>
      <Toolbar>
        <ToolbarSpacer />
        <Segmented appearance="neutral" value={tab} onChange={setTab}
          options={[['overview', tr('sysinfo.tab.overview')], ['hardware', tr('sysinfo.tab.hardware')], ['compat', tr('sysinfo.tab.compat')], ['checks', tr('sysinfo.tab.checks')], ['perf', tr('sysinfo.tab.perf')], ['report', tr('sysinfo.tab.report')]]} />
        <ToolbarSpacer />
        <ToolbarButton icon="↻" title={tr('sysinfo.refresh')} onClick={() => setN(n + 1)} />
      </Toolbar>
      {tab === 'overview' ? <Overview r={r} info={info} /> : tab === 'hardware' ? <Hardware r={r} /> : tab === 'compat' ? <Compatibility r={r} /> : tab === 'checks' ? <Checks r={r} /> : tab === 'perf' ? <Performance r={r} /> : <Report r={r} />}
    </View>
  );
}
