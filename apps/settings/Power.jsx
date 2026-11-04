// Settings → Energy (Battery on laptops): battery state and health, power mode, screen off / lock /
// sleep after N minutes, what the power button and the lid do (logind, closed admin action
// power-buttons), the low-battery notice and the percentage in the menu bar. See docs/SETTINGS.md.
import {
  View, Text, useTheme, useLocale, useSettings, Toggle, Segmented, admin, useBattery, usePowerProfiles, onPowerProfileResult,
  BatteryGlyph, powerPrefs,
} from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { GroupBox, SettingRow, PageHeader, PopUp, Stack, Badge } from './ui.jsx';

const mins = (m) => m * 60;
const duration = (tr, m) => (m >= 60 ? tr('prefs.power.hm', { h: Math.floor(m / 60), m: m % 60 }) : tr('prefs.power.m', { m }));

function BatteryCard({ b, tr }) {
  const t = useTheme();
  const main = b.supplies.find((s) => s.battery && !s.device) || {};
  const status = b.charging ? (b.minutesLeft > 0 ? tr('prefs.power.chargingLeft', { time: duration(tr, b.minutesLeft) }) : tr('prefs.power.charging'))
    : b.onAC ? tr('prefs.power.onACFull') : b.minutesLeft > 0 ? tr('prefs.power.left', { time: duration(tr, b.minutesLeft) }) : tr('prefs.power.onBattery');
  const health = main.health >= 0 ? tr(main.health >= 80 ? 'prefs.power.healthNormal' : 'prefs.power.healthService', { pct: main.health }) : tr('prefs.power.healthUnknown');
  return (
    <PageHeader icon={<View style={{ width: 60, alignItems: 'center' }}><BatteryGlyph percent={b.percent} charging={b.charging} color={t.text} width={52} /></View>}
      title={b.percent >= 0 ? `${b.percent} %` : tr('prefs.power.battery')} subtitle={`${status}\n${health}${main.cycles > 0 ? ` · ${tr('prefs.power.cycles', { n: main.cycles })}` : ''}`} />
  );
}

function ProfileRow({ tr }) {
  const p = usePowerProfiles();
  const [err, setErr] = useState('');
  const [pending, setPending] = useState(null);
  useEffect(() => onPowerProfileResult((r) => {
    if (r.ok || !pending) { setErr(''); return; }
    // the daemon refused (polkit: the session is not "active" for logind): the closed admin action
    admin.run('power-profile', pending).then(() => setErr(''), (e) => setErr(e.message));
  }), [pending]);
  const list = p?.profiles || [];
  if (!list.length) return <SettingRow label={tr('prefs.power.mode')} hint={tr('prefs.power.noProfiles')} disabled />;
  const set = (v) => {
    setPending(v);
    if (!(p.daemon && lord.power.setProfile(v))) admin.run('power-profile', v).then(() => setErr(''), (e) => setErr(e.message));
  };
  return (
    <SettingRow label={tr('prefs.power.mode')} hint={err || (p.degraded ? tr('prefs.power.degraded') : tr(p.daemon ? 'prefs.power.modeHint' : 'prefs.power.modeGovernor', { gov: p.governor }))}>
      <Segmented small value={p.active} onChange={set} options={['power-saver', 'balanced', 'performance'].filter((x) => list.includes(x)).map((x) => [x, tr(`prefs.power.profile.${x}`)])} />
    </SettingRow>
  );
}

function Buttons({ hasLid, tr }) {
  const [b, setB] = useState(() => lord.power.buttons());
  const [err, setErr] = useState('');
  const isAdmin = admin.available();
  const acts = () => ['suspend', 'hibernate', 'poweroff', 'ignore'].map((a) => [a, tr(`prefs.power.act.${a}`)]);
  const save = (x) => {
    const n = { ...b, ...x };
    admin.run('power-buttons', `${n.power} ${n.lid} ${n.lidAC}`).then(() => { setB(lord.power.buttons()); setErr(''); }, (e) => setErr(e.message));
    setB(n);
  };
  return (
    <GroupBox title={tr('prefs.power.buttons')} footer={err || (isAdmin ? tr('prefs.power.buttonsHint') : tr('prefs.power.needAdmin'))}>
      <SettingRow label={tr('prefs.power.powerKey')} disabled={!isAdmin}>
        <PopUp value={b.power} options={acts(false)} onChange={(power) => save({ power })} disabled={!isAdmin} />
      </SettingRow>
      {hasLid && (
        <SettingRow label={tr('prefs.power.lid')} disabled={!isAdmin}>
          <PopUp value={b.lid} options={acts(true)} onChange={(lid) => save({ lid })} disabled={!isAdmin} />
        </SettingRow>
      )}
      {hasLid && (
        <SettingRow label={tr('prefs.power.lidAC')} disabled={!isAdmin}>
          <PopUp value={b.lidAC} options={acts(true)} onChange={(lidAC) => save({ lidAC })} disabled={!isAdmin} />
        </SettingRow>
      )}
    </GroupBox>
  );
}

export function PowerSettings() {
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const b = useBattery();
  const p = powerPrefs(s);
  const patch = (x) => set({ power: { ...p, ...x } });
  const hasBattery = !!b?.hasBattery;
  const offOpts = [[0, tr('prefs.power.never')], [mins(1), duration(tr, 1)], [mins(2), duration(tr, 2)], [mins(3), duration(tr, 3)], [mins(5), duration(tr, 5)],
    [mins(10), duration(tr, 10)], [mins(15), duration(tr, 15)], [mins(30), duration(tr, 30)], [mins(60), duration(tr, 60)]];
  // shorter choices for a quicker sleep: 10 and 20 min on power, 5 and 10 min on battery (#17)
  const sleepList = (list) => [[0, tr('prefs.power.never')], ...list.map((m) => [mins(m), duration(tr, m)])];
  const sleepACOpts = sleepList([10, 15, 20, 30, 60, 120, 180]);
  const sleepBatteryOpts = sleepList([5, 10, 15, 30, 60, 120, 180]);
  const lockOpts = [[0, tr('prefs.power.immediately')], [5, tr('prefs.power.secs', { s: 5 })], [60, duration(tr, 1)], [300, duration(tr, 5)], [900, duration(tr, 15)], [3600, duration(tr, 60)], [-1, tr('prefs.power.never')]];
  return (
    <Stack>
      {hasBattery ? <BatteryCard b={b} tr={tr} />
        : <PageHeader icon={<Badge glyph="⚡" color="#30d158" size={44} />} title={tr('prefs.power.onPower')} subtitle={tr('prefs.power.noBattery')} />}
      <GroupBox>
        <ProfileRow tr={tr} />
      </GroupBox>
      <GroupBox title={tr('prefs.power.screenSleep')} footer={tr('prefs.power.screenSleepHint')}>
        <SettingRow label={hasBattery ? tr('prefs.power.offAC') : tr('prefs.power.off')}>
          <PopUp value={p.offAC} options={offOpts} onChange={(offAC) => patch({ offAC })} />
        </SettingRow>
        {hasBattery && (
          <SettingRow label={tr('prefs.power.offBattery')}>
            <PopUp value={p.offBattery} options={offOpts} onChange={(offBattery) => patch({ offBattery })} />
          </SettingRow>
        )}
        <SettingRow label={tr('prefs.power.lock')}>
          <PopUp value={p.lockDelay} options={lockOpts} onChange={(lockDelay) => patch({ lockDelay })} />
        </SettingRow>
        <SettingRow label={hasBattery ? tr('prefs.power.sleepAC') : tr('prefs.power.sleep')}>
          <PopUp value={p.sleepAC} options={sleepACOpts} onChange={(sleepAC) => patch({ sleepAC })} />
        </SettingRow>
        {hasBattery && (
          <SettingRow label={tr('prefs.power.sleepBattery')}>
            <PopUp value={p.sleepBattery} options={sleepBatteryOpts} onChange={(sleepBattery) => patch({ sleepBattery })} />
          </SettingRow>
        )}
      </GroupBox>
      <Buttons hasLid={hasBattery} tr={tr} />
      {hasBattery && (
        <GroupBox title={tr('prefs.power.battery')}>
          <SettingRow label={tr('prefs.power.lowBattery')} hint={tr('prefs.power.lowBatteryHint')}>
            <Toggle small on={p.lowBattery} onChange={(lowBattery) => patch({ lowBattery })} />
          </SettingRow>
          <SettingRow label={tr('prefs.power.menuPercent')}>
            <Toggle small on={p.menuPercent} onChange={(menuPercent) => patch({ menuPercent })} />
          </SettingRow>
        </GroupBox>
      )}
    </Stack>
  );
}
