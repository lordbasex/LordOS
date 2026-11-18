// Monitor → Disks: read/write rates, and for each physical disk its live temperature (from the
// kernel's hwmon sensors: the nvme driver, or drivetemp for SATA; lord.monitor, no root) and a
// simple health light (OK / Check / Failing) from a cached SMART summary (smart.refresh, at most
// every 5 minutes per disk, never waking a resting disk). The chosen disk shows its temperature over
// the last 30 minutes, Cooling style, and "Open in Disks" for the full SMART report.
import {
  View, Text, Button, Toggle, useTheme, useLocale, useStore, useSettings, useAppWindow, wm, formatNumber, formatTemp, heatColor,
  diskTempHistory, DISK_TEMP_HISTORY, smart, useSmartSummaries, diskTempLimit, diskAlertPrefs, setDiskAlerts, typeScale,
} from '@lordos/sdk';
import { useEffect, useMemo, useState } from 'react';
import { Card, Big, Pair, Stat, StatRow, Table, Cell, C, fmt, useCardStyle } from './index.jsx';
import { Chart } from './Chart.jsx';

export const HEALTH_COLOR = { ok: '#30d158', warning: '#ff9f0a', failing: '#ff453a', unknown: '#8e8e93' };

function tempColor(t, d) {
  if (d.temp == null) return t.textDim;
  const lim = diskTempLimit(d);
  return d.temp >= lim.max ? '#ff453a' : d.temp >= lim.max - 8 ? '#ff9f0a' : t.text;
}

// "38 °C", "No sensor", "Resting" (a spinning disk that is not read while it rests)
function tempText(tr, d) {
  if (!d.sensor) return tr('monitor.temp.noSensor');
  if (d.temp == null) return d.tempIdle ? tr('monitor.temp.resting') : '—';
  return formatTemp(d.temp);
}

function HealthDot({ health, size = 8 }) {
  return <View style={{ width: size, height: size, borderRadius: size / 2, backgroundColor: HEALTH_COLOR[health] || HEALTH_COLOR.unknown }} />;
}

// Refreshes the summaries of the disks on screen, one at a time (each is a smartctl run as root).
function useHealthRefresh(disks) {
  const names = disks.map((d) => `${d.name}:${d.awake ? 1 : 0}`).join(',');
  useEffect(() => {
    let alive = true;
    const run = async () => {
      for (const d of disks) { if (!alive) return; await smart.refresh(d, { awake: d.awake }); }
    };
    run();
    const id = setInterval(run, 60 * 1000);
    return () => { alive = false; clearInterval(id); };
  }, [names]);
}

export function DisksTab({ s, last }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const card = useCardStyle();
  const { args } = useAppWindow() || {};
  useSettings();                                       // redraw when the alert switch changes
  const list = last.disk.disks;
  const sums = useSmartSummaries();
  const hist = useStore(diskTempHistory);
  useHealthRefresh(list);
  const [pick, setPick] = useState(args?.disk || null);
  useEffect(() => { if (args?.disk) setPick(args.disk); }, [args?.disk, args?.nonce]);
  const sel = list.find((d) => d.name === pick) || list.find((d) => d.sensor) || list[0];
  const cols = [
    { key: 'name', label: tr('monitor.diskCol'), flex: 1 },
    { key: 'dev', label: tr('monitor.device'), width: 100 },
    { key: 'size', label: tr('monitor.size'), width: 84, num: true },
    { key: 'temp', label: tr('monitor.temp.col'), width: 100, num: true },
    { key: 'health', label: tr('monitor.health.col'), width: 100 },
    { key: 'read', label: tr('monitor.read'), width: 96, num: true },
    { key: 'write', label: tr('monitor.write'), width: 96, num: true },
  ];
  const healthOf = (d) => sums[d.name]?.health || 'unknown';
  const healthText = (d) => {
    const x = sums[d.name];
    if (!smart.smartable(d.name)) return tr('monitor.health.none');
    if (!x || x.idle) return '—';
    return tr(`monitor.health.${x.health}`);
  };
  return (
    <>
      <Card icon="disk" title={tr('monitor.allDisks')}>
        <Pair items={[{ label: tr('monitor.read'), value: fmt.rate(tr, last.disk.read), color: C.read }, { label: tr('monitor.write'), value: fmt.rate(tr, last.disk.write), color: C.write }]} />
        <Chart series={[{ label: tr('monitor.read'), values: s.read, color: C.read }, { label: tr('monitor.write'), values: s.write, color: C.write }]}
          format={(v) => fmt.rate(tr, v)} times={s.times} height={150} bg={card.bg} />
      </Card>
      <Table cols={cols} rows={list.map((d, i) => (
        <View key={d.name} onClick={() => setPick(d.name)}
          style={{ flexDirection: 'row', height: 30, alignItems: 'center', paddingHorizontal: 10,
            backgroundColor: sel && sel.name === d.name ? (t.mode === 'dark' ? 'rgba(10,132,255,0.24)' : 'rgba(0,122,255,0.12)') : i % 2 ? t.hover : 'transparent' }}>
          <Cell col={cols[0]}>{d.model || tr('monitor.diskUnknown')}</Cell>
          <Cell col={cols[1]} color={t.textDim}>{`/dev/${d.name}`}</Cell>
          <Cell col={cols[2]}>{fmt.mem(d.size)}</Cell>
          <Cell col={cols[3]} color={d.sensor && d.temp != null ? tempColor(t, d) : t.textDim}>{tempText(tr, d)}</Cell>
          <Cell col={cols[4]}>
            {smart.smartable(d.name) && sums[d.name] && !sums[d.name].idle ? <HealthDot health={healthOf(d)} /> : null}
            <Text style={{ fontSize: 12, color: sums[d.name] ? t.text : t.textDim, whiteSpace: 'nowrap', textAlign: 'start' }}>{healthText(d)}</Text>
          </Cell>
          <Cell col={cols[5]}>{fmt.rate(tr, d.read)}</Cell>
          <Cell col={cols[6]}>{fmt.rate(tr, d.write)}</Cell>
        </View>
      ))} />
      {!list.length && <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.noDisks')}</Text>}
      {sel ? <DiskDetail d={sel} sum={sums[sel.name]} h={hist[sel.name]} /> : null}
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
        <Toggle small on={diskAlertPrefs()} onChange={(on) => setDiskAlerts(on)} />
        <Text style={{ flex: 1, fontSize: 12, color: t.textDim, textAlign: 'start' }}>{tr('monitor.diskAlert.toggle')}</Text>
      </View>
    </>
  );
}

function DiskDetail({ d, sum, h }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const card = useCardStyle();
  const lim = diskTempLimit(d);
  const series = useMemo(() => ({ times: h ? h.t : [], v: h ? h.v : [] }), [h]);
  const peak = series.v.reduce((a, v) => Math.max(a, v), 0);
  const top = Math.ceil(Math.max(lim.crit || lim.max, peak + 5) / 10) * 10;
  const health = sum?.health || 'unknown';
  const open = () => wm.open('disks', { args: { disk: d.name, nonce: Date.now() } });
  const tempHint = !d.sensor ? tr('monitor.temp.noSensorHint')
    : d.temp == null && d.tempIdle ? tr('monitor.temp.restingHint')
    : tr(lim.own ? 'monitor.temp.limitOwn' : 'monitor.temp.limitDefault', { max: lim.max });
  return (
    <View style={{ flexDirection: 'row', gap: 12 }}>
      <Card icon="temp" title={tr('monitor.temp.title', { name: d.model || `/dev/${d.name}` })} style={{ flex: 3 }}>
        <View style={{ flexDirection: 'row', alignItems: 'flex-end', gap: 12 }}>
          <Big value={d.sensor ? (d.temp != null ? formatTemp(d.temp) : '—') : tr('monitor.temp.noSensor')} small={d.sensor ? tr(`monitor.temp.sensor.${d.sensor}`) : null} />
          {d.temp != null ? <View style={{ width: 10, height: 10, borderRadius: 5, marginBottom: 6, backgroundColor: heatColor(d.temp) }} /> : null}
        </View>
        <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>{tempHint}</Text>
        {d.sensor && series.v.length ? (
          <Chart series={[{ label: tr('cooling.temperature'), values: series.v, color: '#ff9f0a' }]} max={top}
            format={(v) => formatNumber(Math.round(v), 0)} times={series.times} capacity={DISK_TEMP_HISTORY} height={130} bg={card.bg} />
        ) : null}
      </Card>
      <Card icon="disk" title={tr('monitor.health.title')} style={{ flex: 2 }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
          <HealthDot health={health} size={12} />
          <Text style={{ fontSize: 20, fontWeight: 600, color: t.text }}>{smart.smartable(d.name) ? (sum && !sum.idle ? tr(`monitor.health.${health}`) : '—') : tr('monitor.health.none')}</Text>
        </View>
        <Text style={{ fontSize: 12, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>
          {!smart.smartable(d.name) ? tr('monitor.health.noneHint')
            : !sum ? tr('monitor.health.waiting')
            : sum.standby ? tr('monitor.health.standby')
            : sum.idle ? tr('monitor.health.idle')
            : sum.error ? tr('monitor.health.errorHint')
            : tr(`monitor.health.hint.${health}`)}
        </Text>
        <StatRow gap={8}>
          <Stat label={tr('monitor.health.hours')} value={sum?.hours != null ? formatNumber(sum.hours, 0) : '—'} unit={sum?.hours != null ? tr('monitor.health.hoursUnit') : null} />
          {!d.rota ? <Stat label={tr('monitor.health.wear')} value={sum?.wear != null ? formatNumber(sum.wear, 0) : '—'} unit={sum?.wear != null ? '%' : null} /> : null}
        </StatRow>
        {!smart.installed() && smart.smartable(d.name) ? <Text style={{ fontSize: 11, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{tr('monitor.health.basic')}</Text> : null}
        <Button small label={tr('monitor.health.openDisks')} onClick={open} />
      </Card>
    </View>
  );
}
