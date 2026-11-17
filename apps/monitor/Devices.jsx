// Monitor → Devices and Battery (docs/DEVICES.md, docs/BATTERY.md): what happens now, never history (that is in
// Settings → Battery). Devices: the battery of every connected device, live (useDevices: the kernel's uevents,
// BlueZ's signals, lord-hidpp once a minute). Battery: the computer's battery read every 2 s while the tab is shown
// (lord.power.live()), with the power of the last 60 s; only on a computer that has one.
import {
  View, Text, useTheme, useLocale, useDevices, deviceName, deviceValue, deviceVia, deviceFraction, deviceTone,
  formatNumber, wm,
} from '@lordos/sdk';
import { useEffect, useMemo, useState } from 'react';
import { Card, Stat, StatRow, useCardStyle } from './index.jsx';
import { Chart } from './Chart.jsx';

const TONE = { ok: '#30d158', warn: '#ffd60a', crit: '#ff453a' };

function DeviceLine({ t, d }) {
  const tone = TONE[deviceTone(d)];
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 14, paddingVertical: 10 }}>
      <View style={{ width: 10, height: 10, borderRadius: 5, backgroundColor: tone }} />
      <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
        <Text style={{ fontSize: 13, fontWeight: 600, color: t.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{deviceName(d)}</Text>
        <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>{deviceVia(d)}</Text>
      </View>
      <View style={{ width: 200, height: 6, borderRadius: 3, backgroundColor: t.separator, direction: 'ltr' }}>
        <View style={{ width: `${Math.round(Math.max(0.02, deviceFraction(d)) * 100)}%`, height: 6, borderRadius: 3, backgroundColor: tone }} />
      </View>
      <Text style={{ width: 96, fontSize: 14, fontWeight: 650, color: t.text, textAlign: 'end' }}>{deviceValue(d)}</Text>
    </View>
  );
}

export function DevicesTab() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const list = useDevices();
  return (
    <Card icon="activity" title={tr('monitor.devices.title')}>
      {list.length ? list.map((d, i) => (
        <View key={d.id} style={{ borderTopWidth: i ? 1 : 0, borderTopColor: t.separator }}><DeviceLine t={t} d={d} /></View>
      )) : <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.devices.none')}</Text>}
      <Text style={{ fontSize: 11, color: t.textDim, marginTop: 8, textAlign: 'start' }}>{tr('monitor.devices.note')}</Text>
    </Card>
  );
}

// the system battery among the supplies (scope not Device, a battery, present)
export const systemBattery = (p) => (p?.supplies || []).find((s) => s.battery && !s.device && s.present !== false) || null;
const SAMPLES = 30;                                  // 60 s at one reading every 2 s

export function BatteryTab() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const card = useCardStyle();
  const read = () => { try { return lord.power.live(); } catch (e) { return null; } };
  const [p, setP] = useState(read);
  const [hist, setHist] = useState([]);
  useEffect(() => {
    const id = setInterval(() => {
      const x = read();
      setP(x);
      const b = systemBattery(x);
      setHist((h) => [...h.slice(-(SAMPLES - 1)), { t: Date.now(), w: b && b.watts >= 0 ? b.watts : 0 }]);
    }, 2000);
    return () => clearInterval(id);
  }, []);
  const series = useMemo(() => ({ times: hist.map((x) => x.t), w: hist.map((x) => x.w) }), [hist]);
  const b = systemBattery(p);
  if (!b) return <Card icon="activity" title={tr('monitor.tab.battery')}><Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.battery.none')}</Text></Card>;
  const charging = b.status === 'Charging', full = b.status === 'Full', held = b.status === 'Not charging';
  const state = b.behaviour === 2 ? tr('monitor.battery.state.paused') : b.behaviour === 4 ? tr('monitor.battery.state.forcedDischarge')
    : charging ? tr('monitor.battery.state.charging') : full ? tr('monitor.battery.state.full') : held ? tr('monitor.battery.state.held') : tr('monitor.battery.state.discharging');
  const minutes = b.minutesLeft >= 0 ? b.minutesLeft : -1;
  const hm = (m) => tr('monitor.battery.hm', { h: Math.floor(m / 60), m: m % 60 });
  const num = (v, d) => (v >= 0 ? formatNumber(v, d) : '—');
  const openSettings = () => wm.open('settings', { args: { section: 'battery', nonce: Date.now() } });
  return (
    <>
      <StatRow>
        <Stat icon="activity" label={tr('monitor.battery.charge')} value={b.percent >= 0 ? `${b.percent}` : '—'} unit="%" />
        <Stat label={tr('monitor.battery.power')} value={num(b.watts, 1)} unit="W" />
        <Stat icon="clock" label={tr(charging ? 'monitor.battery.toFull' : 'monitor.battery.left')} value={minutes >= 0 ? hm(minutes) : '—'} />
        <Stat icon="temp" label={tr('monitor.battery.temp')} value={b.tempC > -1000 ? formatNumber(b.tempC, 1) : '—'} unit={b.tempC > -1000 ? '°C' : null} />
      </StatRow>
      <StatRow>
        <Stat label={tr('monitor.battery.voltage')} value={num(b.volts, 2)} unit="V" />
        <Stat label={tr('monitor.battery.current')} value={num(b.amps, 2)} unit="A" />
        <Stat label={tr('monitor.battery.energy')} value={num(b.whNow, 1)} unit={b.whFull > 0 ? tr('monitor.battery.energyOf', { full: formatNumber(b.whFull, 1) }) : 'Wh'} />
        <Stat label={tr('monitor.battery.state')} value={state} />
      </StatRow>
      <Card icon="activity" title={tr('monitor.battery.chart')}
        right={<View onClick={openSettings}><Text style={{ fontSize: 11, color: t.accent, whiteSpace: 'nowrap' }}>{tr('monitor.battery.settings')}</Text></View>}>
        <Chart series={[{ label: tr('monitor.battery.power'), values: series.w, color: '#bf5af2' }]} format={(v) => `${formatNumber(v, 1)} W`}
          times={series.times} capacity={SAMPLES} height={150} bg={card.bg} />
      </Card>
    </>
  );
}
