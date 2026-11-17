// Settings → Battery (docs/BATTERY.md; every theme, any laptop: the kernel's standard power_supply attributes).
// On a computer with a battery: its state with the plug-in, resume and limit marks, the daily use and the level by hour
// (lord.power.history(), kept by lord-server), the estimated use per app, the battery's condition and its real cycles,
// and the optimized charging (only an administrator changes it: the battery-* admin actions; what the battery can't do
// shows disabled with why). Everywhere something connected has a battery: the devices and their notices.
import {
  View, Text, useTheme, useLocale, useSettings, Toggle, Slider, Button, admin, useBattery, useDevices, deviceName, deviceValue,
  deviceVia, deviceFraction, deviceTone, batteryPrefs, formatNumber, formatDate, formatTime, weekdayName, monthName, appName, wm, typeScale,
} from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { GroupBox, SettingRow, PageHeader, Note, Stack, Badge, tint } from './ui.jsx';

const BEH_INHIBIT = 2, BEH_DISCHARGE = 4;
const systemBat = (b) => (b?.hasBattery ? (b.supplies || []).find((x) => x.battery && !x.device && x.present !== false) || null : null);
const ymdOf = (d) => d.getFullYear() * 10000 + (d.getMonth() + 1) * 100 + d.getDate();
const dateOfYmd = (n) => new Date(Math.floor(n / 10000), Math.floor(n / 100) % 100 - 1, n % 100);
// a session scope to a name: app-lordos-<app>.scope → the app's name; lord-server's own leaf and LordOS's helpers are "the system"
function scopeName(tr, scope) {
  if (scope === 'server' || /^lordos-/.test(scope)) return tr('prefs.battery.appSystem');
  const key = scope.replace(/^app-lordos-/, '').replace(/\.scope$/, '').replace(/^lpk-/, '');
  const known = appName(key);
  if (known && known !== key) return known;
  const tail = key.replace(/^lordos-web-/, '').replace(/^lordos-/, '');
  return tail.split(/[-_.]/).filter(Boolean).map((w) => w[0].toUpperCase() + w.slice(1)).join(' ');
}

function ChargeBar({ t, tr, bat, prefs }) {
  const p = Math.max(0, Math.min(100, bat.percent));
  const mark = (at, color, label, key) => (
    <View key={key} style={{ position: 'absolute', left: `${at}%`, top: 0, bottom: 0, width: 2, backgroundColor: color }}>
      <Text style={{ position: 'absolute', top: 36, left: -40, width: 80, fontSize: 10.5, color, textAlign: 'center', whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
  const limit = bat.endThreshold >= 0 ? bat.endThreshold : prefs.limit;
  return (
    <View style={{ height: 52, marginTop: 6, direction: 'ltr' }}>
      <View style={{ height: 30, borderRadius: 9, backgroundColor: t.separator, overflow: 'hidden' }}>
        <View style={{ width: `${p}%`, height: 30, background: 'linear-gradient(90deg, #2fb877, #3dd68c)' }} />
      </View>
      {mark(prefs.plugAt, '#f5b43c', tr('prefs.battery.markPlug', { percent: prefs.plugAt }), 'plug')}
      {bat.startThreshold >= 0 ? mark(bat.startThreshold, t.textDim, tr('prefs.battery.markResume', { percent: bat.startThreshold }), 'resume') : null}
      {mark(limit, t.text, tr('prefs.battery.markLimit', { percent: limit }), 'limit')}
    </View>
  );
}

function Usage({ t, tr, hist }) {
  const days = hist?.days || [];
  const today = ymdOf(new Date());
  const last8 = Array.from({ length: 8 }, (_, i) => { const d = new Date(); d.setDate(d.getDate() - (7 - i)); return ymdOf(d); });
  const drainOf = (y) => (days.find((d) => d.ymd === y)?.drain || 0) / 10;
  const past = last8.slice(0, 7).map(drainOf).filter((v) => v > 0);
  const avg = past.length ? Math.round(past.reduce((a, b) => a + b, 0) / past.length) : 0;
  const now = Math.round(drainOf(today));
  const max = Math.max(1, ...last8.map(drainOf));
  const td = days.find((d) => d.ymd === today) || { on: 0, off: 0 };
  // the level by hour today: the last sample of each hour
  const hours = Array(24).fill(null), charging = Array(24).fill(false);
  const start = new Date(); start.setHours(0, 0, 0, 0);
  for (const [ts, p, f] of hist?.samples || []) {
    const ms = ts * 1000;
    if (ms < start.getTime()) continue;
    const h = new Date(ms).getHours();
    hours[h] = p;
    if (f & 1) charging[h] = true;
  }
  const hm = (s) => tr('prefs.power.hm', { h: Math.floor(s / 3600), m: Math.floor(s / 60) % 60 });
  const empty = !days.length;
  return (
    <GroupBox title={tr('prefs.battery.usage')}>
      {empty ? <Note>{tr('prefs.battery.noHistory')}</Note> : (
        <View style={{ padding: 14, gap: 12 }}>
          {past.length ? <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr(now > avg * 1.15 ? 'prefs.battery.usageMore' : now < avg * 0.85 ? 'prefs.battery.usageLess' : 'prefs.battery.usageSame')}</Text> : null}
          <View style={{ flexDirection: 'row', gap: 40 }}>
            <View><Text style={{ fontSize: 11.5, color: t.textDim }}>{tr('prefs.battery.average')}</Text><Text style={{ fontSize: 24, fontWeight: 600, color: t.text }}>{`${avg} %`}</Text></View>
            <View><Text style={{ fontSize: 11.5, color: '#f59e0b' }}>{tr('prefs.battery.today')}</Text><Text style={{ fontSize: 24, fontWeight: 600, color: '#f59e0b' }}>{`${now} %`}</Text></View>
          </View>
          <View style={{ flexDirection: 'row', gap: 14, alignItems: 'flex-end', height: 120, direction: 'ltr' }}>
            {last8.map((y, i) => (
              <View key={y} style={{ alignItems: 'center', gap: 5 }}>
                <View style={{ width: 26, height: Math.max(4, Math.round(drainOf(y) / max * 96)), borderRadius: 6, backgroundColor: i === 7 ? '#f59e0b' : t.separator }} />
                <Text style={{ fontSize: 10.5, color: i === 7 ? '#f59e0b' : t.textDim }}>{weekdayName(dateOfYmd(y).getDay()).slice(0, 1).toLocaleUpperCase()}</Text>
              </View>
            ))}
          </View>
          <Text style={{ fontSize: 12, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('prefs.battery.byHour')}</Text>
          <View style={{ flexDirection: 'row', gap: 3, alignItems: 'flex-end', height: 70, direction: 'ltr' }}>
            {hours.map((p, h) => <View key={h} style={{ width: 11, height: p == null ? 2 : Math.max(2, Math.round(p * 0.68)), borderRadius: 2, backgroundColor: p == null ? t.separator : charging[h] ? tint.ok : t.textDim }} />)}
          </View>
          <View style={{ flexDirection: 'row', gap: 50, borderTopWidth: 1, borderTopColor: t.separator, paddingTop: 10 }}>
            <View><Text style={{ fontSize: 11.5, color: t.textDim }}>{tr('prefs.battery.screenOn')}</Text><Text style={{ fontSize: 20, fontWeight: 600, color: t.text }}>{hm(td.on)}</Text></View>
            <View><Text style={{ fontSize: 11.5, color: t.textDim }}>{tr('prefs.battery.screenOff')}</Text><Text style={{ fontSize: 20, fontWeight: 600, color: t.text }}>{hm(td.off)}</Text></View>
          </View>
        </View>
      )}
    </GroupBox>
  );
}

function Apps({ t, tr, hist }) {
  const today = (hist?.days || []).find((d) => d.ymd === ymdOf(new Date()));
  const drain = today ? today.drain / 10 : 0;
  const byName = new Map();
  for (const [scope, us] of Object.entries(today?.apps || {})) { const n = scopeName(tr, scope); byName.set(n, (byName.get(n) || 0) + us); }
  const total = [...byName.values()].reduce((a, b) => a + b, 0);
  const rows = [...byName.entries()].sort((a, b) => b[1] - a[1]).slice(0, 8).map(([n, us]) => [n, total ? Math.round(drain * us / total) : 0, us]);
  return (
    <GroupBox title={tr('prefs.battery.apps')} footer={tr('prefs.battery.appsHint')}>
      {rows.length ? rows.map(([n, pct, us]) => (
        <SettingRow key={n} label={n} hint={tr('prefs.battery.appCpu', { time: tr('prefs.power.hm', { h: Math.floor(us / 3.6e9), m: Math.floor(us / 6e7) % 60 }) })}>
          <Text style={{ fontSize: 13, fontWeight: 650, color: pct >= 5 ? '#f59e0b' : t.text }}>{`${pct} %`}</Text>
        </SettingRow>
      )) : <Note>{tr('prefs.battery.appsNone')}</Note>}
    </GroupBox>
  );
}

function Condition({ tr, bat, hist }) {
  const service = bat.health >= 0 && bat.health < 80;
  const month = (y, m) => (y ? `${monthName(m - 1)} ${y}` : tr('prefs.battery.unknown'));
  const first = hist?.firstSeen ? new Date(hist.firstSeen * 1000) : null;
  const cycles = bat.cycles > 0 ? String(bat.cycles) : hist ? formatNumber(hist.cycles || 0, 1) : '—';
  return (
    <GroupBox title={tr('prefs.battery.condition')}>
      <SettingRow label={tr('prefs.battery.condition')} hint={tr(service ? 'prefs.battery.condServiceHint' : 'prefs.battery.condNormalHint')}>
        <Text style={{ fontSize: 13, fontWeight: 650, color: service ? tint.warn : undefined }}>{bat.health >= 0 ? tr(service ? 'prefs.battery.condService' : 'prefs.battery.condNormal') : tr('prefs.battery.unknown')}</Text>
      </SettingRow>
      <SettingRow label={tr('prefs.battery.capacity')} hint={bat.whFull > 0 && bat.whDesign > 0 ? tr('prefs.battery.capacityHint', { now: formatNumber(bat.whFull, 1), design: formatNumber(bat.whDesign, 1) }) : null}>
        <Text style={{ fontSize: 13, fontWeight: 650 }}>{bat.health >= 0 ? `${bat.health} %` : '—'}</Text>
      </SettingRow>
      <SettingRow label={tr('prefs.battery.cycles')} hint={bat.cycles > 0 ? tr('prefs.battery.cyclesBattery') : tr('prefs.battery.cyclesLordos', { date: first ? formatDate(first) : '—' })}>
        <Text style={{ fontSize: 13, fontWeight: 650 }}>{cycles}</Text>
      </SettingRow>
      <SettingRow label={tr('prefs.battery.made')}><Text style={{ fontSize: 13 }}>{month(bat.mfgYear, bat.mfgMonth)}</Text></SettingRow>
      <SettingRow label={tr('prefs.battery.firstUse')}><Text style={{ fontSize: 13 }}>{first ? `${monthName(first.getMonth())} ${first.getFullYear()}` : '—'}</Text></SettingRow>
      <SettingRow label={tr('prefs.battery.maker')}><Text style={{ fontSize: 13 }}>{[bat.vendor, bat.model, bat.technology].filter(Boolean).join(' · ') || tr('prefs.battery.unknown')}</Text></SettingRow>
    </GroupBox>
  );
}

function Charging({ tr, bat, prefs, patch }) {
  const isAdmin = admin.available();
  const [err, setErr] = useState('');
  const hasEnd = bat.endThreshold >= 0, hasStart = bat.startThreshold >= 0;
  const canPause = !!(bat.behaviours & BEH_INHIBIT), canDischarge = !!(bat.behaviours & BEH_DISCHARGE);
  const run = (a, arg) => admin.run(a, arg, null, { quiet: true }).then(() => setErr(''), (e) => setErr(e.message));
  const setLimit = (v) => { const limit = Math.round(v); const resume = Math.min(prefs.resume, limit - 5); patch({ limit, resume }); if (hasEnd) run('battery-thresholds', `${resume} ${limit}`); };
  const setResume = (v) => { const resume = Math.min(Math.round(v), prefs.limit - 5); patch({ resume }); if (hasEnd) run('battery-thresholds', `${resume} ${prefs.limit}`); };
  const paused = bat.behaviour === BEH_INHIBIT;
  const no = tr('prefs.battery.notSupported');
  return (
    <GroupBox title={tr('prefs.battery.charge')} footer={err || (!isAdmin ? tr('prefs.battery.adminOnly') : !hasEnd ? tr('prefs.battery.noControl') : null)}>
      <SettingRow label={tr('prefs.battery.limit')} hint={hasEnd ? tr('prefs.battery.limitHint') : no} disabled={!isAdmin || !hasEnd}>
        <Slider value={prefs.limit} min={50} max={100} step={5} width={200} onChange={isAdmin && hasEnd ? setLimit : () => {}} />
        <Text style={{ width: 42, fontSize: 13, fontWeight: 650, textAlign: 'end' }}>{`${prefs.limit} %`}</Text>
      </SettingRow>
      <SettingRow label={tr('prefs.battery.resume')} hint={hasStart ? tr('prefs.battery.resumeHint') : no} disabled={!isAdmin || !hasStart}>
        <Slider value={prefs.resume} min={50} max={95} step={5} width={200} onChange={isAdmin && hasStart ? setResume : () => {}} />
        <Text style={{ width: 42, fontSize: 13, fontWeight: 650, textAlign: 'end' }}>{`${prefs.resume} %`}</Text>
      </SettingRow>
      <SettingRow label={tr('prefs.battery.plug')} hint={tr('prefs.battery.plugHint')}>
        <Slider value={prefs.plugAt} min={5} max={50} step={5} width={200} onChange={(v) => patch({ plugAt: Math.round(v) })} />
        <Text style={{ width: 42, fontSize: 13, fontWeight: 650, textAlign: 'end' }}>{`${prefs.plugAt} %`}</Text>
      </SettingRow>
      <SettingRow label={tr('prefs.battery.fullOnce')} hint={hasEnd ? tr('prefs.battery.fullOnceHint') : no} disabled={!isAdmin || !hasEnd}>
        <Button small label={tr(prefs.fullOnce ? 'prefs.battery.fullOnceCancel' : 'prefs.battery.fullOnceButton')} disabled={!isAdmin || !hasEnd}
          onClick={() => patch({ fullOnce: !prefs.fullOnce })} />
      </SettingRow>
      <SettingRow label={tr('prefs.battery.pause')} hint={canPause ? tr('prefs.battery.pauseHint') : no} disabled={!isAdmin || !canPause}>
        <Toggle small on={paused} onChange={(on) => { if (isAdmin && canPause) run('battery-behaviour', on ? 'inhibit-charge' : 'auto'); }} />
      </SettingRow>
      <SettingRow label={tr('prefs.battery.heat')} hint={canPause ? tr('prefs.battery.heatHint') : no} disabled={!isAdmin || !canPause}>
        <Toggle small on={prefs.heatGuard && canPause} onChange={(heatGuard) => { if (isAdmin && canPause) patch({ heatGuard }); }} />
      </SettingRow>
      <SettingRow label={tr('prefs.battery.discharge')} hint={canDischarge ? tr('prefs.battery.dischargeHint') : no} disabled={!isAdmin || !canDischarge}>
        <Button small label={tr(prefs.dischargeTo ? 'prefs.battery.dischargeCancel' : 'prefs.battery.dischargeButton', { percent: prefs.limit })} disabled={!isAdmin || !canDischarge}
          onClick={() => patch({ dischargeTo: !prefs.dischargeTo })} />
      </SettingRow>
    </GroupBox>
  );
}

function Devices({ t, tr, prefs, patch }) {
  const list = useDevices().filter((d) => d.kind !== 'laptop');
  if (!list.length) return null;
  const tone = (d) => ({ ok: tint.ok, warn: tint.warn, crit: tint.error }[deviceTone(d)]);
  return (
    <GroupBox title={tr('prefs.battery.devices')}>
      {list.map((d) => (
        <SettingRow key={d.id} label={deviceName(d)} hint={deviceVia(d)}>
          <View style={{ width: 140, height: 6, borderRadius: 3, backgroundColor: t.separator, direction: 'ltr' }}>
            <View style={{ width: `${Math.round(Math.max(0.03, deviceFraction(d)) * 100)}%`, height: 6, borderRadius: 3, backgroundColor: tone(d) }} />
          </View>
          <Text style={{ width: 90, fontSize: 13, fontWeight: 650, textAlign: 'end' }}>{deviceValue(d)}</Text>
        </SettingRow>
      ))}
      <SettingRow label={tr('prefs.battery.devicesNotify')} hint={tr('prefs.battery.devicesNotifyHint', { low: prefs.plugAt, high: prefs.limit })}>
        <Toggle small on={prefs.devicesNotify} onChange={(devicesNotify) => patch({ devicesNotify })} />
      </SettingRow>
    </GroupBox>
  );
}

export function BatterySettings() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [s, set] = useSettings();
  const b = useBattery();
  const bat = systemBat(b);
  const prefs = batteryPrefs(s.power?.battery);
  const patch = (x) => set({ power: { ...(s.power || {}), battery: { ...(s.power?.battery || {}), ...x } } });
  const [hist, setHist] = useState(null);
  useEffect(() => {
    if (!bat) return undefined;
    const read = () => { try { setHist(lord.power.history()); } catch (e) { setHist(null); } };
    read();
    const id = setInterval(read, 60000);
    return () => clearInterval(id);
  }, [!!bat]);
  const charging = bat?.status === 'Charging';
  const state = !bat ? '' : bat.behaviour === BEH_INHIBIT ? tr('prefs.battery.state.paused') : bat.behaviour === BEH_DISCHARGE ? tr('prefs.battery.state.forced')
    : charging ? tr('prefs.battery.state.charging') : bat.status === 'Full' ? tr('prefs.battery.state.full')
      : bat.status === 'Not charging' ? tr('prefs.battery.state.held') : tr('prefs.battery.state.discharging');
  const last = hist?.lastCharge?.t ? new Date(hist.lastCharge.t * 1000) : null;
  return (
    <Stack>
      {bat ? (
        <PageHeader icon={<Badge glyph="⚡" color={tint.ok} size={44} />} title={`${bat.percent} %`}
          subtitle={[state, last ? tr('prefs.battery.lastCharge', { percent: hist.lastCharge.p, when: `${formatDate(last)} ${formatTime(last)}` }) : null].filter(Boolean).join('\n')} />
      ) : null}
      {bat ? <View style={{ paddingHorizontal: 8 }}><ChargeBar t={t} tr={tr} bat={bat} prefs={prefs} /></View> : null}
      {bat ? <Usage t={t} tr={tr} hist={hist} /> : null}
      {bat ? <Apps t={t} tr={tr} hist={hist} /> : null}
      {bat ? <Condition tr={tr} bat={bat} hist={hist} /> : null}
      {bat ? <Charging tr={tr} bat={bat} prefs={prefs} patch={patch} /> : null}
      <Devices t={t} tr={tr} prefs={prefs} patch={patch} />
      {bat ? (
        <View onClick={() => wm.open('monitor', { args: { tab: 'battery', nonce: Date.now() } })}>
          <Text style={{ fontSize: 12, color: t.accent, textAlign: 'start' }}>{tr('prefs.battery.more')}</Text>
        </View>
      ) : null}
    </Stack>
  );
}
