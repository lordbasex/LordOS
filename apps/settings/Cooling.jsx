// Settings → Cooling (docs/FANS.md): the detected cooling hardware drawn at the top with the live
// temperature and fan speed, the fan profile (or "let the system decide"), the curve (draggable in
// the custom profile), "full speed for a while" and, on a Raspberry Pi 5, the case. axia-fand (root)
// does the work; this page only sends validated requests through lord.fan.
import {
  View, Text, useTheme, useLocale, Segmented, Toggle, Button, admin, useFan, useStore, fanCase, fanControl, refreshFan,
  coolingVariant, CoolingArt, FanCurveChart, formatTemp, formatNumber, spacing, typeScale,
} from '@lordos/sdk';
import { useState } from 'react';
import { GroupBox, SettingRow, ValueRow, PopUp, Stack, Note, tint } from './ui.jsx';

const PROFILES = ['quiet', 'balanced', 'performance', 'always', 'custom'];
const PRESET_CURVE = [[55, 30], [60, 55], [65, 100]];

const minutesLabel = (tr, s) => {
  const m = Math.ceil(s / 60);
  return m >= 60 ? tr('cooling.leftHM', { h: Math.floor(m / 60), m: m % 60 }) : tr('cooling.leftM', { m });
};

function Header({ s, variant, tr }) {
  const t = useTheme();
  const fan = s?.fans?.[0];
  const pct = fan && fan.pct >= 0 ? fan.pct : 0;
  const forced = s && s.force !== 'off';
  const stepText = !s ? tr('cooling.serviceOff')
    : !s.fans.length ? tr(s.firmwareFans?.length ? 'cooling.firmwareManaged' : 'cooling.noFans')
      : forced ? tr('cooling.forcedNow')
        : s.profile === 'auto' ? tr('cooling.systemManaged')
          : s.step < 0 ? tr('cooling.belowFirst', { temp: s.curve[0]?.[0] ?? '' })
            : tr('cooling.stepOf', { n: s.step + 1, total: s.curve.length });
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.l, padding: spacing.m, borderRadius: 12,
      backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.025)', border: `1px solid ${t.separator}` }}>
      <CoolingArt variant={variant} pct={pct} temp={s?.temp} width={300} height={164} />
      <View style={{ flex: 1, minWidth: 0, gap: spacing.s }}>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr(`cooling.variant.${variant}`)}</Text>
        <Text style={{ fontSize: 30, fontWeight: 600, color: t.text, textAlign: 'start' }}>{formatTemp(s?.temp)}</Text>
        {fan ? (
          <View style={{ gap: 2 }}>
            <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('cooling.fanPct', { v: pct })}</Text>
            {fan.rpm >= 0 ? <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{tr('cooling.rpm', { v: formatNumber(fan.rpm, 0) })}</Text> : null}
          </View>
        ) : null}
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
          <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: !s ? tint.off : forced ? tint.warn : tint.ok }} />
          <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{stepText}</Text>
        </View>
      </View>
    </View>
  );
}

export function CoolingSettings() {
  const { t: tr } = useLocale();
  const s = useFan();
  const caseId = useStore(fanCase);
  const [err, setErr] = useState('');
  const [caseMsg, setCaseMsg] = useState(null);
  const variant = coolingVariant(s, caseId);
  const run = (code) => setErr(code ? tr(`errors.${code}`) : '');

  if (s === undefined) return <Stack><Note>{tr('cooling.loading')}</Note></Stack>;
  // axia-fand says whether this user may change the fans (an administrator); the case is an admin action
  const isAdmin = !!s?.canChange;
  const controllable = !!s && s.fans.length > 0;
  const custom = s?.custom?.length ? s.custom : PRESET_CURVE;
  const shown = s?.profile === 'custom' ? custom : s?.curve || [];
  const fan = s?.fans?.[0];
  const forceValue = !s || s.force === 'off' ? 'off' : s.force === 'on' ? 'on' : 'timed';
  const forceOpts = [['off', tr('cooling.forceOff')], ['30', tr('cooling.force30')], ['60', tr('cooling.force60')], ['120', tr('cooling.force120')], ['on', tr('cooling.forceOn')]];
  if (forceValue === 'timed') forceOpts.unshift(['timed', minutesLabel(tr, s.forceLeft)]);
  const setForce = (v) => { if (v !== 'timed') run(fanControl.force(v === 'off' || v === 'on' ? v : Number(v))); };
  const editCurve = (pts) => run(fanControl.setCurve(pts));
  const addPoint = () => {
    const last = custom[custom.length - 1];
    if (custom.length >= 8 || last[0] >= 90) return;
    editCurve([...custom, [Math.min(90, last[0] + 5), Math.min(100, last[1] + 10)]]);
  };
  const removePoint = () => { if (custom.length > 1) editCurve(custom.slice(0, -1)); };

  return (
    <Stack>
      <Header s={s} variant={variant} tr={tr} />
      {!s && <Note tone="warn">{tr('cooling.serviceOffHint')}</Note>}
      {s && !controllable && (
        <GroupBox title={tr('cooling.fans')} footer={tr(s.firmwareFans?.length ? 'cooling.firmwareHint' : 'cooling.noFansHint')}>
          {s.firmwareFans?.length
            ? s.firmwareFans.map((f, i) => <ValueRow key={i} label={tr('cooling.fanN', { n: i + 1 })} value={f.rpm >= 0 ? tr('cooling.rpm', { v: formatNumber(f.rpm, 0) }) : '—'} />)
            : <ValueRow label={tr('cooling.cooling')} value={tr('cooling.passive')} />}
        </GroupBox>
      )}
      {controllable && (
        <GroupBox title={tr('cooling.profile')} footer={err || (isAdmin ? tr(`cooling.profileHint.${s.profile}`) : tr('cooling.needAdmin'))}>
          <SettingRow label={tr('cooling.systemControl')} hint={tr('cooling.systemControlHint')} disabled={!isAdmin}>
            <Toggle small on={s.profile === 'auto'} onChange={(on) => isAdmin && run(fanControl.setProfile(on ? 'auto' : 'balanced'))} />
          </SettingRow>
          {s.profile !== 'auto' && (
            <SettingRow label={tr('cooling.curveProfile')} stack disabled={!isAdmin}>
              <Segmented small value={s.profile} onChange={(p) => isAdmin && run(fanControl.setProfile(p))}
                options={PROFILES.map((p) => [p, tr(`cooling.profiles.${p}`)])} />
            </SettingRow>
          )}
        </GroupBox>
      )}
      {controllable && s.profile !== 'auto' && (
        <GroupBox title={tr('cooling.curve')} footer={tr(s.profile === 'custom' ? 'cooling.curveHintCustom' : 'cooling.curveHint', { hyst: s.hyst })}>
          <View style={{ paddingVertical: spacing.m }}>
            <FanCurveChart curve={shown} floor={s.profile === 'custom' ? 0 : s.floor} hyst={s.hyst} step={s.force === 'off' ? s.step : -1}
              temp={s.temp} pct={fan ? fan.pct : null} height={190} onChange={s.profile === 'custom' && isAdmin ? editCurve : undefined} />
          </View>
          {s.profile === 'custom' && isAdmin && (
            <View style={{ flexDirection: 'row', gap: spacing.s, paddingBottom: spacing.s, justifyContent: 'flex-end' }}>
              <Button small label={tr('cooling.addPoint')} onClick={addPoint} disabled={custom.length >= 8} />
              <Button small label={tr('cooling.removePoint')} onClick={removePoint} disabled={custom.length <= 1} />
              <Button small label={tr('cooling.resetCurve')} onClick={() => editCurve(PRESET_CURVE)} />
            </View>
          )}
        </GroupBox>
      )}
      {controllable && (
        <GroupBox title={tr('cooling.boost')} footer={tr('cooling.boostHint')}>
          <SettingRow label={tr('cooling.force')} disabled={!isAdmin}>
            <PopUp value={forceValue} options={forceOpts} onChange={setForce} disabled={!isAdmin} />
          </SettingRow>
        </GroupBox>
      )}
      {s && s.board === 'pi5' && caseId != null && (
        <GroupBox title={tr('cooling.case')} footer={caseMsg ? caseMsg.text : tr('cooling.caseHint')}>
          <SettingRow label={tr('cooling.caseModel')} disabled={!isAdmin}>
            <PopUp value={caseId} width={210} disabled={!isAdmin}
              options={[['none', tr('cooling.caseNone')], ['argon-one-v5', tr('cooling.caseArgonV5')]]}
              onChange={(id) => admin.run('rpi-case', id).then(() => { fanCase.set(() => id); setCaseMsg({ ok: true, text: tr('cooling.caseRestart') }); },
                (e) => setCaseMsg({ ok: false, text: e.message }))} />
          </SettingRow>
        </GroupBox>
      )}
      {s && (
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end' }}>
          <Button small label={tr('cooling.refresh')} onClick={refreshFan} />
        </View>
      )}
    </Stack>
  );
}
