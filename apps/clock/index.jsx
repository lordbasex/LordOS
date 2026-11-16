// Clock — world clock, alarms, timer and stopwatch (docs/SYSTEM.md).
// Alarms, the timer and the stopwatch live in the SDK (sdk/alarms.js): they keep running and ring
// with the window closed. Other cities' times come from tzdata through our own reader
// (lord.time.zoneOffset, server/src/ui/tzif.c), without changing the session time zone.
import {
  View, Text, useTheme, useSettings, useStore, useAppWindow, t as tr, formatTime, weekdayName, formatNumber,
  Segmented, Toggle, Button, Stepper, TextInput, ScrollView, clockAlarms, clockTimer, timerStart, timerPause, timerResume,
  timerReset, timerLeft, stopwatch, swStart, swStop, swLap, swReset, swElapsed, nextFire,
  Toolbar, ToolbarButton, EmptyState, Hairline, spacing, typeScale,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';

// ---- analog face without rotations: the hands are dots along the radius ----

function Face({ size, h, m, s, dark, accent }) {
  const r = size / 2;
  const fg = dark ? '#f2f2f7' : '#1c1c1e';
  const dots = [];
  for (let i = 0; i < 12; i++) {
    const a = (i / 12) * Math.PI * 2;
    const d = i % 3 ? size * 0.035 : size * 0.06;
    dots.push(<View key={`t${i}`} style={{ position: 'absolute', left: r + Math.sin(a) * r * 0.82 - d / 2, top: r - Math.cos(a) * r * 0.82 - d / 2,
      width: d, height: d, borderRadius: d / 2, backgroundColor: fg, opacity: i % 3 ? 0.45 : 0.9 }} />);
  }
  const hand = (key, frac, len, w, color) => {
    const a = frac * Math.PI * 2, n = Math.max(4, Math.round(len * r / (w * 0.7)));
    const out = [];
    for (let k = 1; k <= n; k++) {
      const q = (k / n) * len * r;
      out.push(<View key={`${key}${k}`} style={{ position: 'absolute', left: r + Math.sin(a) * q - w / 2, top: r - Math.cos(a) * q - w / 2,
        width: w, height: w, borderRadius: w / 2, backgroundColor: color }} />);
    }
    return out;
  };
  return (
    <View style={{ width: size, height: size, borderRadius: r, backgroundColor: dark ? '#2c2c2e' : '#ffffff', border: `1px solid ${dark ? '#48484a' : '#d1d1d6'}`, direction: 'ltr' }}>
      {dots}
      {hand('h', ((h % 12) + m / 60) / 12, 0.48, Math.max(2, size * 0.07), fg)}
      {hand('m', (m + s / 60) / 60, 0.7, Math.max(1.5, size * 0.05), fg)}
      {s != null && hand('s', s / 60, 0.76, Math.max(1, size * 0.025), accent)}
      <View style={{ position: 'absolute', left: r - size * 0.05, top: r - size * 0.05, width: size * 0.1, height: size * 0.1, borderRadius: size * 0.05, backgroundColor: accent }} />
    </View>
  );
}

export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(180deg, #2b2b30, #0c0c0e)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, alignItems: 'center', justifyContent: 'center' }}>
      <Face size={s * 0.78} h={10} m={9} s={36} dark={false} accent="#ff9f0a" />
    </View>
  );
}

// ---- utilities ----

const pad = (n) => String(n).padStart(2, '0');
const localOffset = (d) => -d.getTimezoneOffset() * 60;

// Date "shifted" to the zone: its getHours() give that city's local time.
function zoneDate(zone, now) {
  let z = null;
  try { z = lord.time.zoneOffset(zone, Math.floor(now / 1000)); } catch (e) { z = null; }
  if (!z) return null;
  return { date: new Date(now + (z.offset - localOffset(new Date(now))) * 1000), offset: z.offset, abbr: z.abbr };
}

const cityName = (zone) => (zone.split('/').pop() || zone).replace(/_/g, ' ');
const regionName = (zone) => zone.split('/').slice(0, -1).join(' / ').replace(/_/g, ' ');

function useTick(ms, on = true) {
  const [now, set] = useState(Date.now());
  useEffect(() => { if (!on) return undefined; const id = setInterval(() => set(Date.now()), ms); return () => clearInterval(id); }, [ms, on]);
  return now;
}

function fmtDuration(ms, cents = false) {
  const total = Math.max(0, ms);
  const sec = Math.floor(total / 1000), hh = Math.floor(sec / 3600), mm = Math.floor(sec / 60) % 60, ss = sec % 60;
  const base = `${hh ? `${hh}:${pad(mm)}` : pad(mm)}:${pad(ss)}`;
  return cents ? `${base}${formatNumber(0.5, 1)[1] || '.'}${pad(Math.floor(total / 10) % 100)}` : base;
}

function inWords(ms) {
  const min = Math.max(1, Math.round(ms / 60000));
  const hh = Math.floor(min / 60), mm = min % 60;
  return hh ? tr('clock.inHours', { h: hh, m: mm }) : tr('clock.inMinutes', { m: mm });
}

// ---- world clock ----

function ZonePicker({ onPick, onClose }) {
  const t = useTheme();
  const [q, setQ] = useState('');
  const zones = useMemo(() => { try { return lord.time.zones(); } catch (e) { return ['UTC']; } }, []);
  const needle = q.trim().toLowerCase();
  const shown = (needle ? zones.filter((z) => z.toLowerCase().replace(/_/g, ' ').includes(needle)) : zones).slice(0, 200);
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.35)', alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: 380, maxWidth: '92%', height: 400, maxHeight: '92%', borderRadius: 12, backgroundColor: t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 12px 40px rgba(0,0,0,0.45)', padding: spacing.xl, gap: spacing.m }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('clock.world.add')}</Text>
        <TextInput value={q} onChange={setQ} autoFocus placeholder={tr('clock.world.search')} onCancel={onClose} onSubmit={() => shown[0] && onPick(shown[0])} />
        <View style={{ flex: 1, borderRadius: 8, border: `1px solid ${t.separator}` }}>
          <ScrollView style={{ padding: 4 }}>
            {shown.map((z) => (
              <View key={z} onClick={() => onPick(z)} style={{ height: 28, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 8, borderRadius: 6 }}>
                <Text style={{ flex: 1, fontSize: 13, color: t.text, textAlign: 'start' }}>{cityName(z)}</Text>
                <Text style={{ fontSize: 11, color: t.textDim }}>{regionName(z)}</Text>
              </View>
            ))}
          </ScrollView>
        </View>
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end' }}><Button label={tr('clock.cancel')} onClick={onClose} /></View>
      </View>
    </View>
  );
}

function CityRow({ zone, now, local, onRemove }) {
  const t = useTheme();
  const [hover, setHover] = useState(false);
  const z = zone ? zoneDate(zone, now) : { ...(zoneDate(local, now) || {}), date: new Date(now), offset: localOffset(new Date(now)) };
  if (!z) return null;
  const d = z.date, here = new Date(now);
  const dayDiff = Math.round((new Date(d.getFullYear(), d.getMonth(), d.getDate()) - new Date(here.getFullYear(), here.getMonth(), here.getDate())) / 86400000);
  const diffH = (z.offset - localOffset(here)) / 3600;
  const diff = diffH === 0 ? tr('clock.world.same') : tr('clock.world.diff', { h: `${diffH > 0 ? '+' : '−'}${formatNumber(Math.abs(diffH))}` });
  const day = dayDiff === 0 ? tr('clock.world.today') : dayDiff > 0 ? tr('clock.world.tomorrow') : tr('clock.world.yesterday');
  return (
    <View onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.l, minHeight: 68, paddingHorizontal: spacing.m, borderRadius: 10, backgroundColor: hover ? t.hover : 'transparent' }}>
      <Face size={44} h={d.getHours()} m={d.getMinutes()} s={d.getSeconds()} dark={t.mode === 'dark' && !(d.getHours() >= 7 && d.getHours() < 19)} accent={t.accent} />
      <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{local ? tr('clock.world.local') : cityName(zone)}</Text>
        <Text style={{ fontSize: typeScale.callout, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{local ? `${cityName(local)}${z.abbr ? ` · ${z.abbr}` : ''}` : `${day}, ${diff}${z.abbr && !/^[+-]/.test(z.abbr) ? ` · ${z.abbr}` : ''}`}</Text>
      </View>
      <Text style={{ fontSize: 28, fontWeight: 300, color: t.text, whiteSpace: 'nowrap', textAlign: 'end' }}>{formatTime(d)}</Text>
      <View onClick={onRemove} style={{ width: 22, height: 22, borderRadius: 11, alignItems: 'center', justifyContent: 'center', backgroundColor: hover && onRemove ? t.control : 'transparent' }}>
        <Text style={{ fontSize: 11, fontWeight: 600, color: hover && onRemove ? t.text : 'transparent' }}>✕</Text>
      </View>
    </View>
  );
}

function World({ picking, setPicking }) {
  const t = useTheme();
  const [s, set] = useSettings();
  const now = useTick(1000);
  const cities = s.clock?.cities || [];
  let localZone = 'UTC';
  try { localZone = lord.time.zone(); } catch (e) { /* no zone */ }
  const save = (list) => set({ clock: { ...(s.clock || {}), cities: list } });
  return (
    <View style={{ flex: 1 }}>
      <ScrollView style={{ flex: 1 }}>
        <View style={{ paddingHorizontal: spacing.s, paddingVertical: spacing.s }}>
          <CityRow zone={null} local={localZone} now={now} />
          <View style={{ height: 1, marginHorizontal: spacing.m, marginVertical: spacing.xs }}><Hairline edge="bottom" /></View>
          {cities.map((z) => <CityRow key={z} zone={z} now={now} onRemove={() => save(cities.filter((x) => x !== z))} />)}
        </View>
        {!cities.length ? <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'center', lineHeight: 1.4, paddingHorizontal: spacing.xxl, paddingTop: spacing.l }}>{tr('clock.world.empty')}</Text> : null}
      </ScrollView>
      {picking && <ZonePicker onClose={() => setPicking(false)} onPick={(z) => { if (!cities.includes(z)) save([...cities, z]); setPicking(false); }} />}
    </View>
  );
}

// ---- alarms ----

// the week starts on Monday (as in most es and pt regions)
const weekOrder = () => [1, 2, 3, 4, 5, 6, 0];

function daysText(days) {
  if (!days?.length) return tr('clock.alarm.once');
  if (days.length === 7) return tr('clock.alarm.everyDay');
  const set = [...days].sort().join(',');
  if (set === '1,2,3,4,5') return tr('clock.alarm.weekdays');
  if (set === '0,6') return tr('clock.alarm.weekends');
  return weekOrder().filter((d) => days.includes(d)).map((d) => weekdayName(d)).join(' ');
}

function AlarmEditor({ alarm, onClose }) {
  const t = useTheme();
  const [h, setH] = useState(alarm?.h ?? 7);
  const [m, setM] = useState(alarm?.m ?? 0);
  const [days, setDays] = useState(alarm?.days || [1, 2, 3, 4, 5]);
  const [label, setLabel] = useState(alarm?.label || '');
  const save = () => {
    if (alarm) clockAlarms.update(alarm.id, { h, m, days, label, on: true });
    else clockAlarms.add({ h, m, days, label });
    onClose();
  };
  const d = new Date(); d.setHours(h, m, 0, 0);
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.35)', alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: 380, maxWidth: '92%', borderRadius: 12, backgroundColor: t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 12px 40px rgba(0,0,0,0.45)', padding: spacing.xl, gap: spacing.l }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{alarm ? tr('clock.alarm.edit') : tr('clock.alarm.new')}</Text>
        <View style={{ flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 10, direction: 'ltr' }}>
          <Stepper value={h} onChange={setH} min={0} max={23} format={pad} />
          <Text style={{ fontSize: 22, color: t.text }}>:</Text>
          <Stepper value={m} onChange={setM} min={0} max={59} format={pad} />
        </View>
        <Text style={{ fontSize: 28, fontWeight: 300, color: t.text, textAlign: 'center' }}>{formatTime(d)}</Text>
        <View style={{ gap: 6 }}>
          <Text style={{ fontSize: 12, color: t.textDim, textAlign: 'start' }}>{tr('clock.alarm.repeat')}</Text>
          <View style={{ flexDirection: 'row', gap: 6, justifyContent: 'center' }}>
            {weekOrder().map((wd) => {
              const on = days.includes(wd);
              return (
                <View key={wd} onClick={() => setDays(on ? days.filter((x) => x !== wd) : [...days, wd])}
                  style={{ width: 40, height: 30, borderRadius: 8, alignItems: 'center', justifyContent: 'center', backgroundColor: on ? t.accent : t.control }}>
                  <Text style={{ fontSize: 12, fontWeight: 600, color: on ? '#fff' : t.text }}>{weekdayName(wd)}</Text>
                </View>
              );
            })}
          </View>
          <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'center' }}>{daysText(days)}</Text>
        </View>
        <TextInput value={label} onChange={setLabel} placeholder={tr('clock.alarm.label')} onSubmit={save} />
        <View style={{ flexDirection: 'row', gap: 8 }}>
          {alarm && <Button label={tr('clock.alarm.delete')} onClick={() => { clockAlarms.remove(alarm.id); onClose(); }} />}
          <View style={{ flex: 1 }} />
          <Button label={tr('clock.cancel')} onClick={onClose} />
          <Button primary label={tr('clock.save')} onClick={save} />
        </View>
      </View>
    </View>
  );
}

function Alarms({ editing, setEditing }) {
  const t = useTheme();
  const [s] = useSettings();
  const now = useTick(30000);
  const alarms = (s.clock?.alarms || []).slice().sort((a, b) => a.h * 60 + a.m - (b.h * 60 + b.m));
  const next = alarms.map((a) => nextFire(a, now)).filter((x) => x != null).sort((a, b) => a - b)[0];
  return (
    <View style={{ flex: 1 }}>
      {alarms.length ? (
        <ScrollView style={{ flex: 1 }}>
          <View style={{ paddingHorizontal: spacing.xl, paddingTop: spacing.m, paddingBottom: spacing.xl }}>
            <Text style={{ fontSize: typeScale.callout, color: t.textDim, paddingBottom: spacing.xs, textAlign: 'start' }}>
              {next ? tr('clock.alarm.next', { when: inWords(next - now) }) : tr('clock.alarm.none')}</Text>
            {alarms.map((a) => {
              const d = new Date(); d.setHours(a.h, a.m, 0, 0);
              return (
                <View key={a.id} style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m, minHeight: 72, paddingVertical: spacing.s }}>
                  <View onClick={() => setEditing(a)} style={{ flex: 1, minWidth: 0, gap: 2 }}>
                    <Text style={{ fontSize: 34, fontWeight: 300, color: a.on ? t.text : t.textDim, textAlign: 'start' }}>{formatTime(d)}</Text>
                    <Text style={{ fontSize: typeScale.callout, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>
                      {`${a.label || tr('clock.alarm.default')} · ${daysText(a.days)}${a.snooze && a.snooze > now ? ` · ${tr('clock.alarm.snoozed', { time: formatTime(new Date(a.snooze)) })}` : ''}`}</Text>
                  </View>
                  <Toggle on={!!a.on} onChange={(on) => clockAlarms.update(a.id, { on })} />
                  <Hairline edge="bottom" />
                </View>
              );
            })}
            <Text style={{ fontSize: typeScale.caption, color: t.textDim, paddingTop: spacing.m, lineHeight: 1.4, textAlign: 'start' }}>{tr('clock.alarm.closedHint')}</Text>
          </View>
        </ScrollView>
      ) : <EmptyState icon={<Face size={56} h={7} m={0} s={null} dark={t.mode === 'dark'} accent={t.accent} />} title={tr('clock.alarm.empty')} hint={tr('clock.alarm.closedHint')} />}
      {editing && <AlarmEditor alarm={editing === 'new' ? null : editing} onClose={() => setEditing(null)} />}
    </View>
  );
}

// ---- timer ----

function Timer() {
  const t = useTheme();
  const st = useStore(clockTimer);
  const [hh, setHh] = useState(0);
  const [mm, setMm] = useState(5);
  const [ss, setSs] = useState(0);
  useTick(250, st.state === 'running');
  const left = timerLeft(st);
  const frac = st.duration ? Math.min(1, Math.max(0, left / st.duration)) : 0;
  const presets = [1, 3, 5, 10, 15, 30];
  if (st.state === 'idle') {
    return (
      <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: 18 }}>
        <View style={{ flexDirection: 'row', alignItems: 'flex-end', gap: 12, direction: 'ltr' }}>
          {[[hh, setHh, 23, 'clock.timer.h'], [mm, setMm, 59, 'clock.timer.m'], [ss, setSs, 59, 'clock.timer.s']].map(([v, fn, max, key]) => (
            <View key={key} style={{ alignItems: 'center', gap: 4 }}>
              <Text style={{ fontSize: 11, color: t.textDim }}>{tr(key)}</Text>
              <Stepper value={v} onChange={fn} min={0} max={max} format={pad} />
            </View>
          ))}
        </View>
        <View style={{ flexDirection: 'row', gap: 6, flexWrap: 'wrap', justifyContent: 'center', maxWidth: 420 }}>
          {presets.map((p) => (
            <View key={p} onClick={() => { setHh(0); setMm(p); setSs(0); timerStart(p * 60); }}
              style={{ height: 28, paddingHorizontal: 12, borderRadius: 14, justifyContent: 'center', backgroundColor: t.control }}>
              <Text style={{ fontSize: 12, color: t.text }}>{tr('clock.timer.preset', { n: p })}</Text>
            </View>
          ))}
        </View>
        <Button primary label={tr('clock.timer.start')} onClick={() => { const sec = hh * 3600 + mm * 60 + ss; if (sec > 0) timerStart(sec); }} />
        <Text style={{ fontSize: 11, color: t.textDim, maxWidth: 380, textAlign: 'center', lineHeight: 1.4 }}>{tr('clock.timer.hint')}</Text>
      </View>
    );
  }
  return (
    <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: 20 }}>
      <Text style={{ fontSize: 64, fontWeight: 200, color: t.text, fontFamily: 'monospace' }}>{fmtDuration(left + 999)}</Text>
      <View style={{ width: 320, height: 6, borderRadius: 3, backgroundColor: t.control, direction: 'ltr' }}>
        <View style={{ width: Math.round(320 * frac), height: 6, borderRadius: 3, backgroundColor: t.accent }} />
      </View>
      <Text style={{ fontSize: 12, color: t.textDim }}>
        {st.state === 'paused' ? tr('clock.timer.paused') : tr('clock.timer.endsAt', { time: formatTime(new Date(st.endsAt)) })}</Text>
      <View style={{ flexDirection: 'row', gap: 10 }}>
        <Button label={tr('clock.cancel')} onClick={timerReset} />
        {st.state === 'running' ? <Button primary label={tr('clock.timer.pause')} onClick={timerPause} />
          : <Button primary label={tr('clock.timer.resume')} onClick={timerResume} />}
      </View>
    </View>
  );
}

// ---- stopwatch ----

function Stopwatch() {
  const t = useTheme();
  const st = useStore(stopwatch);
  useTick(47, st.running);
  const total = swElapsed(st);
  const laps = st.laps;
  const splits = laps.map((v, i) => v - (laps[i + 1] || 0));
  const best = splits.length > 1 ? Math.min(...splits) : null, worst = splits.length > 1 ? Math.max(...splits) : null;
  return (
    <View style={{ flex: 1, alignItems: 'center', paddingTop: spacing.xxl, gap: spacing.xl }}>
      <Text style={{ fontSize: 60, fontWeight: 200, color: t.text, fontFamily: 'monospace' }}>{fmtDuration(total, true)}</Text>
      <View style={{ flexDirection: 'row', gap: 10 }}>
        {st.running ? <Button label={tr('clock.sw.lap')} onClick={swLap} /> : <Button label={tr('clock.sw.reset')} onClick={swReset} />}
        {st.running ? <Button primary label={tr('clock.sw.stop')} onClick={swStop} /> : <Button primary label={tr('clock.sw.start')} onClick={swStart} />}
      </View>
      <ScrollView style={{ alignSelf: 'stretch', paddingHorizontal: spacing.xxl }}>
        {laps.map((v, i) => {
          const split = splits[i];
          const color = split === best ? '#30d158' : split === worst ? '#ff453a' : t.text;
          return (
            <View key={laps.length - i} style={{ flexDirection: 'row', alignItems: 'center', minHeight: 32, paddingHorizontal: spacing.xs }}>
              <Hairline edge="bottom" />
              <Text style={{ flex: 1, fontSize: 13, color, textAlign: 'start' }}>{tr('clock.sw.lapN', { n: laps.length - i })}</Text>
              <Text style={{ width: 120, fontSize: 13, color, fontFamily: 'monospace', textAlign: 'end' }}>{fmtDuration(split, true)}</Text>
              <Text style={{ width: 120, fontSize: 13, color: t.textDim, fontFamily: 'monospace', textAlign: 'end' }}>{fmtDuration(v, true)}</Text>
            </View>
          );
        })}
      </ScrollView>
    </View>
  );
}

// ---- window ----

let lastTab = 'world';

// Width of a box after layout (re-measured on every render).
function useWidth(ref, initial) {
  const [w, setW] = useState(initial);
  useEffect(() => {
    const id = setTimeout(() => {
      if (!ref.current) return;
      const lw = Math.round(lord.getLayout(ref.current.id)[2]);
      if (lw > 0 && Math.abs(lw - w) > 2) setW(lw);
    }, 20);
    return () => clearTimeout(id);
  });
  return w;
}

export default function Clock() {
  const { args } = useAppWindow();
  const box = useRef(null);
  const width = useWidth(box, 560);
  const [tab, setTabState] = useState(lastTab);
  const [picking, setPicking] = useState(false);            // World: the city picker is open
  const [editing, setEditing] = useState(null);             // Alarms: null | 'new' | alarm
  const setTab = (x) => { lastTab = x; setTabState(x); };
  useEffect(() => { if (args?.tab && ['world', 'alarms', 'timer', 'stopwatch'].includes(args.tab)) setTab(args.tab); }, [args?.nonce]);
  const add = tab === 'world' ? [tr('clock.world.add'), () => setPicking(true)] : tab === 'alarms' ? [tr('clock.alarm.add'), () => setEditing('new')] : null;
  return (
    <View ref={box} style={{ flex: 1 }}>
      <Toolbar>
        <View style={{ flex: 1 }} />
        <Segmented appearance="neutral" small={width < 520} value={tab} onChange={setTab} options={[['world', tr('clock.tab.world')], ['alarms', tr('clock.tab.alarms')],
          ['timer', tr('clock.tab.timer')], ['stopwatch', tr('clock.tab.stopwatch')]]} />
        <View style={{ flex: 1, flexDirection: 'row', justifyContent: 'flex-end' }}>
          {add ? <ToolbarButton icon="+" iconSize={20} title={add[0]} onClick={add[1]} /> : null}
        </View>
      </Toolbar>
      {tab === 'world' ? <World picking={picking} setPicking={setPicking} />
        : tab === 'alarms' ? <Alarms editing={editing} setEditing={setEditing} />
        : tab === 'timer' ? <Timer /> : <Stopwatch />}
    </View>
  );
}
