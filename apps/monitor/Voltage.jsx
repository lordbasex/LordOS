// Monitor → Voltage (docs/MONITOR.md, "Voltage"): one view over whatever this machine reports. The sources come from
// voltmodel.js: the Raspberry Pi 5's power chip (the closed, read-only rpi-pmic admin action), hwmon chips, batteries
// and adapters (lord.monitor.voltage(), world-readable sysfs read on the Monitor's thread). The tab only exists on
// machines with something to show (the sample's "volt"). It is read about every 2 s, only while this view is mounted
// and its window is not minimized.
import { View, Text, admin, monitor, useAppWindow, useLocale, useStore, useTheme, useWindowHidden, formatNumber } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { Card, Big, Stat, useCardStyle } from './index.jsx';
import { Chart } from './Chart.jsx';
import { VoltageArt } from './VoltageArt.jsx';
import { LOW_5V, parsePmic, boardWatts, powerState, voltSources, primaryRail, traceRange, afterBoot } from './voltmodel.js';

const POLL_MS = 2000, HISTORY = 150;     // 5 minutes
const TONE = { normal: '#30d158', low: '#ff9f0a', happened: '#ff9f0a', under: '#ff453a' };
const TRACE = '#32ade6';

function Dot({ color }) {
  return <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: color }} />;
}

function Hint({ children }) {
  const t = useTheme();
  return <Text style={{ fontSize: 11, color: t.textDim, lineHeight: 1.4 }}>{children}</Text>;
}

function useVoltage() {
  const { id } = useAppWindow() || {};
  const hidden = useWindowHidden(id);
  const [st, setSt] = useState({ d: null, pmic: null, err: '', seen: false, hist: [], key: null });
  const seen = useRef(false);
  useEffect(() => {
    if (hidden) return undefined;
    let stop = false, tm = null, d = null, pmic = null, err = '';
    const hist = [];
    let key = null;
    monitor.wantVoltage(true);
    const show = () => {
      if (stop) return;
      if (d.alarm === 1) seen.current = true;
      const prime = primaryRail(voltSources(d, pmic));
      if (prime && prime.key !== key) { key = prime.key; hist.length = 0; }
      if (prime) {
        hist.push({ t: Date.now(), v: prime.v });
        if (hist.length > HISTORY) hist.shift();
      }
      setSt({ d, pmic, err, seen: seen.current, hist: hist.slice(), key });
      tm = setTimeout(tick, POLL_MS);
    };
    const tick = () => {
      d = monitor.voltage() || d;
      if (!d) { tm = setTimeout(tick, 250); return; }         // the Monitor's thread hasn't read it yet
      if (!d.board) { pmic = null; show(); return; }
      admin.run('rpi-pmic', '', null, { quiet: true }).then((line) => {
        pmic = parsePmic(line);
        err = pmic ? '' : 'errors.hw.pmicFailed';
      }, (e) => { pmic = null; err = e.message; }).then(show);
    };
    tick();
    return () => { stop = true; clearTimeout(tm); monitor.wantVoltage(false); };
  }, [hidden]);
  return st;
}

function Rail({ name, note, v, a, unit }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, minHeight: 22 }}>
      <View style={{ flex: 1, minWidth: 0 }}>
        <Text style={{ fontSize: 12, color: t.text, whiteSpace: 'nowrap' }}>{name}</Text>
        {note ? <Text style={{ fontSize: 11, color: t.textDim }}>{note}</Text> : null}
      </View>
      {a != null && <Text style={{ fontSize: 12, color: t.textDim, whiteSpace: 'nowrap', textAlign: 'end' }}>{tr('monitor.volt.amps', { v: formatNumber(a, 3) })}</Text>}
      <Text style={{ width: 84, fontSize: 12, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', textAlign: 'end' }}>{unit || tr('monitor.volt.volts', { v: formatNumber(v, 3) })}</Text>
    </View>
  );
}

function railName(tr, r) {
  if (r.rail) return tr(`monitor.volt.rail.${r.rail}`);
  return r.label || r.name;
}

function Trace({ rail, hist, mark }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const card = useCardStyle();
  const height = 130;
  const values = hist.map((h) => h.v);
  const { floor, span } = traceRange(values, mark);
  const fmtV = (x) => tr('monitor.volt.volts', { v: formatNumber(x + floor, 2) });
  const y = mark == null ? 0 : Math.round(height - 1 - ((mark - floor) / span) * (height - 3));
  return (
    <View>
      <Chart series={[{ label: railName(tr, rail), values: values.map((v) => Math.max(0, v - floor)), color: TRACE }]} max={span} format={fmtV}
        times={hist.map((h) => h.t)} capacity={HISTORY} height={height} bg={card.bg} />
      {mark != null && (
        <>
          <View style={{ position: 'absolute', left: 0, right: 0, top: y, height: 1, backgroundColor: TONE.low, pointerEvents: 'none' }} />
          <Text style={{ position: 'absolute', left: 2, top: y - 15, fontSize: 10, color: TONE.low, pointerEvents: 'none' }}>{tr('monitor.volt.lowMark', { v: formatNumber(mark, 1) })}</Text>
        </>
      )}
      <View style={{ height: 4 }} />
      <Text style={{ fontSize: 11, color: t.textDim }}>{tr('monitor.volt.window')}</Text>
    </View>
  );
}

function StateRow({ state }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
      <Dot color={TONE[state]} />
      <Text style={{ fontSize: 12, fontWeight: 600, color: t.text, whiteSpace: 'nowrap' }}>{tr(`monitor.volt.state.${state}`)}</Text>
    </View>
  );
}

export function VoltageTab() {
  const { t: tr } = useLocale();
  const { d, pmic, err, seen, hist, key } = useVoltage();
  if (!d) return null;
  const sources = voltSources(d, pmic);
  const prime = primaryRail(sources);
  const onBoard = prime?.rail === 'EXT5V';
  const state = powerState({ alarm: d.alarm, sinceBoot: d.sinceBoot, input: pmic?.input ?? null, count: pmic?.count ?? null, seen });
  const watts = boardWatts(pmic?.rails);
  const count = pmic ? pmic.count : -1;
  const when = count > 0 && pmic.lastS >= 0 ? afterBoot(pmic.lastS) : null;
  const logged = d.sinceBoot === 1 ? tr('monitor.volt.happened') : count > 0 ? formatNumber(count, 0) : count === 0 || d.sinceBoot === 0 ? tr('monitor.volt.noEvents') : tr('monitor.volt.unknown');
  const main = prime && hist.length && key === prime.key;
  return (
    <>
      {err ? <Hint>{err.startsWith('errors.') ? tr(err) : err}</Hint> : null}
      {(prime || state) && (
        <Card icon="activity" title={prime ? railName(tr, prime) : tr('monitor.volt.board')} right={state ? <StateRow state={state} /> : null}>
          {prime && (
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 16 }}>
              <VoltageArt variant={d.board ? 'pi' : 'mother'} tone={TONE[state] || TONE.normal} />
              <View style={{ flex: 1, minWidth: 0 }}><Big value={tr('monitor.volt.volts', { v: formatNumber(prime.v, 3) })} /></View>
            </View>
          )}
          {main ? <Trace rail={prime} hist={hist} mark={onBoard ? LOW_5V : null} /> : null}
          {state ? <Hint>{tr(`monitor.volt.explain.${state}`, { low: formatNumber(LOW_5V, 1) })}</Hint> : null}
        </Card>
      )}
      {(watts != null || state) && (
        <View style={{ flexDirection: 'row', gap: 10 }}>
          {watts != null && (
            <Stat icon="activity" label={tr('monitor.volt.power')} value={formatNumber(watts, 2)} unit={tr('monitor.volt.wattsUnit')}>
              <Hint>{tr('monitor.volt.powerNote')}</Hint>
            </Stat>
          )}
          {state && (
            <Stat label={tr('monitor.volt.sinceBoot')} value={logged}>
              <Hint>{when ? tr('monitor.volt.lastAt', when) : tr('monitor.volt.sinceBootNote')}</Hint>
            </Stat>
          )}
        </View>
      )}
      {sources.map((s) => (
        <Card key={s.id} icon={s.kind === 'board' ? 'cpu' : 'activity'}
          title={s.kind === 'board' ? tr('monitor.volt.board') : s.kind === 'supply' ? tr('monitor.volt.supplies') : s.title}>
          <View style={{ gap: 2 }}>
            {s.rails.map((r) => (
              <Rail key={r.key} name={railName(tr, r)} v={r.v} a={r.a}
                note={r.raw ? tr('monitor.volt.rawValue') : r.battery === false ? tr('monitor.volt.adapter') : r.min != null ? tr('monitor.volt.minDesign', { v: formatNumber(r.min, 2) }) : r.battery ? tr('monitor.volt.battery') : null} />
            ))}
          </View>
          {s.raw ? <Hint>{tr('monitor.volt.rawNote')}</Hint> : null}
        </Card>
      ))}
    </>
  );
}
