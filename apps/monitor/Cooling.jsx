// Monitor → Cooling (docs/FANS.md): the cooling hardware drawn live (the fan turns with the real
// duty, the heatsink warms with the temperature), the active profile and step on its curve, a
// temperature/fan history and a quick "full speed" button. The profile and the curve are changed in
// Settings → Cooling; both use lord.fan (axia-fand).
import {
  View, Text, Button, useTheme, useLocale, useFan, useStore, fanHistory, fanCase, fanControl, coolingVariant, CoolingArt,
  FanCurveChart, formatTemp, formatNumber, typeScale, wm, FAN_HISTORY,
} from '@lordos/sdk';
import { useMemo, useState } from 'react';
import { Card, Big, Stat, useCardStyle } from './index.jsx';
import { Chart } from './Chart.jsx';

const C = { temp: '#ff9f0a', fan: '#32ade6' };

export function CoolingTab() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const s = useFan();
  const caseId = useStore(fanCase);
  const hist = useStore(fanHistory);
  const card = useCardStyle();
  const [err, setErr] = useState('');
  const series = useMemo(() => ({ times: hist.map((x) => x.t), temp: hist.map((x) => x.temp), pct: hist.map((x) => x.pct) }), [hist]);
  if (s === undefined) return null;
  const variant = coolingVariant(s, caseId);
  const fan = s?.fans?.[0];
  const pct = fan && fan.pct >= 0 ? fan.pct : 0;
  const forced = !!s && s.force !== 'off';
  const profile = !s ? '—' : forced ? tr('cooling.forcedNow') : tr(`cooling.profiles.${s.profile}`);
  const step = !s || !s.fans.length || s.profile === 'auto' || forced ? '—'
    : s.step < 0 ? tr('cooling.stepOff') : tr('cooling.stepOf', { n: s.step + 1, total: s.curve.length });
  const toggleForce = () => {
    const r = fanControl.force(forced ? 'off' : 30);
    setErr(r ? tr(`errors.${r}`) : '');
  };
  const openSettings = () => wm.open('settings', { args: { section: 'cooling', nonce: Date.now() } });
  return (
    <>
      <View style={{ flexDirection: 'row', gap: 12 }}>
        <Card icon="fan" title={tr(`cooling.variant.${variant}`)} style={{ flex: 1 }}
          right={s && s.fans.length ? <Button small primary={!forced} label={forced ? tr('cooling.stopForce') : tr('cooling.force100')} onClick={toggleForce} /> : null}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 16 }}>
            <CoolingArt variant={variant} pct={pct} temp={s?.temp} width={280} height={156} />
            <View style={{ flex: 1, minWidth: 0, gap: 10 }}>
              <Big value={formatTemp(s?.temp)} small={tr('monitor.cpuTemp')} />
              <Big value={fan ? tr('cooling.fanPct', { v: pct }) : '—'} small={fan && fan.rpm >= 0 ? tr('cooling.rpm', { v: formatNumber(fan.rpm, 0) }) : null} />
              <Text style={{ fontSize: 11, color: err ? '#ff453a' : t.textDim }}>{err || (!s ? tr('cooling.serviceOff') : !s.fans.length ? tr(s.firmwareFans?.length ? 'cooling.firmwareManaged' : 'cooling.noFans') : '')}</Text>
            </View>
          </View>
        </Card>
      </View>
      {s && s.fans.length > 0 && (
        <View style={{ flexDirection: 'row', gap: 12 }}>
          <Card icon="activity" title={tr('cooling.curve')} style={{ flex: 1 }}
            right={<View onClick={openSettings}><Text style={{ fontSize: 11, color: t.accent, whiteSpace: 'nowrap' }}>{tr('cooling.customize')}</Text></View>}>
            {s.profile === 'auto'
              ? <Text style={{ fontSize: 12, color: t.textDim }}>{tr('cooling.systemManaged')}</Text>
              : <FanCurveChart curve={s.curve} floor={s.floor} hyst={s.hyst} step={forced ? -1 : s.step} temp={s.temp} pct={pct} height={150} />}
          </Card>
          <Card icon="temp" title={tr('cooling.history')} style={{ flex: 1 }}>
            <Chart series={[{ label: tr('cooling.temperature'), values: series.temp, color: C.temp }, { label: tr('cooling.fanSpeed'), values: series.pct, color: C.fan }]}
              max={100} format={(v) => formatNumber(Math.round(v), 0)} times={series.times} capacity={FAN_HISTORY} height={150} bg={card.bg} />
          </Card>
        </View>
      )}
      <View style={{ flexDirection: 'row', gap: 10 }}>
        <Stat icon="temp" label={tr('monitor.cpuTemp')} value={s?.temp != null ? formatNumber(Math.round(s.temp), 0) : '—'} unit={s?.temp != null ? '°C' : null} />
        <Stat label={tr('cooling.profile')} value={profile} />
        <Stat label={tr('cooling.step')} value={step} />
        {(s?.fans || []).map((f) => (
          <Stat key={f.id} icon="fan" label={tr('cooling.fanN', { n: f.id + 1 })} value={f.pct >= 0 ? `${f.pct}` : '—'} unit={f.rpm >= 0 ? tr('cooling.pctRpm', { v: formatNumber(f.rpm, 0) }) : '%'} />
        ))}
        {(s?.firmwareFans || []).map((f, i) => (
          <Stat key={`r${i}`} icon="fan" label={tr('cooling.firmwareFan', { n: i + 1 })} value={f.rpm >= 0 ? formatNumber(f.rpm, 0) : '—'} unit={tr('monitor.rpm')} />
        ))}
      </View>
    </>
  );
}
