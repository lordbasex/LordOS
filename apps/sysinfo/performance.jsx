// System Information → Performance (0.8.6, docs/BENCHMARK.md "0.8.6 — LordBench"). Quick (about a minute)
// or Full (storage on a scratch file, a sustained load and every video codec too) runs of the lord-bench
// helper and of lord-media's video measurement through lord.sysinfo.bench (server/src/bench/lbrun.c), and
// Desktop impact: the compositor's frame times while a test window moves (scene.jsx), alone and with every
// core loaded at the lowest priority. While a run goes on this view polls every 500 ms. A peak result is
// shown by category as "×N" of each reference machine (perf.js, references.js), with the three verdicts;
// a desktop result against the display's refresh budget. Both with the conditions they ran under and the
// change since the previous run of the same kind, and the boot times per version. Nothing is sent
// anywhere; Save writes the JSON to Documents.
import {
  View, Text, ScrollView, Button, useTheme, useLocale, GroupBox, FormRow, Segmented, formatNumber, spacing, typeScale, fs, userDirs, wm, admin, session,
} from '@lordos/sdk';
import { useEffect, useMemo, useState } from 'react';
import { CATEGORIES, DESKTOP_CATEGORIES, ratio, categoryRatios, comparableReferences, stability, compatibilityVerdict, desktopVerdict, conditionDiffs, changes, sustainedSeries, fileName, piFirmware } from './perf.js';
import { startScene } from './scene.jsx';
import { compatibility } from './model.js';

const VERDICT_COLOR = { pass: '#30d158', warn: '#ff9f0a', fail: '#ff453a', unknown: '#8e8e93' };
const key = (id) => id.replace(/\./g, '_');
const SUSTAINED_S = 180;

const bench = () => (globalThis.lord && lord.sysinfo && lord.sysinfo.bench) || null;
function readSaved(which) { try { const b = bench(); return b ? b.saved(which) : null; } catch (e) { return null; } }

function fmt(v) {
  if (v == null) return '—';
  return formatNumber(v, v >= 100 ? 0 : v >= 10 ? 1 : 2);
}

// A poll that shows nothing new: the same keys with the same primitive values (state, mode, step,
// progress; a result or a start error always counts as new). Most 500 ms polls are this while a
// step runs, and keeping the old state then spares a render of the whole view.
function samePoll(a, b) {
  const ka = Object.keys(a), kb = Object.keys(b);
  return ka.length === kb.length && ka.every((k, i) => k === kb[i] && a[k] === b[k] && (a[k] === null || typeof a[k] !== 'object'));
}

function useBench() {
  const [st, setSt] = useState(() => {
    let p = null;
    try { p = bench() && bench().poll(); } catch (e) { p = null; }
    return p || { state: 'unavailable' };
  });
  const running = st.state === 'waiting' || st.state === 'running';
  useEffect(() => {
    if (!running) return undefined;
    const id = setInterval(() => { try { const p = bench().poll(); if (p) setSt((x) => (samePoll(x, p) ? x : p)); } catch (e) { /* keep the last */ } }, 500);
    return () => clearInterval(id);
  }, [running]);
  const start = (mode) => {
    let ok = false;
    try { ok = !!bench().start(mode, mode === 'full' ? SUSTAINED_S : 0); } catch (e) { ok = false; }
    if (ok) setSt({ state: 'waiting', mode, step: '', progress: 0 });
    else setSt((x) => ({ ...x, error: 'start' }));
  };
  const cancel = () => { try { bench().cancel(); } catch (e) { /* nothing to cancel */ } };
  return [st, running, start, cancel];
}

function Bar({ value }) {
  const t = useTheme();
  return (
    <View style={{ height: 6, borderRadius: 3, overflow: 'hidden', backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.1)' : 'rgba(0,0,0,0.08)' }}>
      <View style={{ width: `${Math.round(Math.max(0, Math.min(1, value)) * 100)}%`, height: 6, backgroundColor: t.accent || '#0a84ff' }} />
    </View>
  );
}

function Verdict({ label, state, detail, states = 'verdictState' }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'flex-start', gap: spacing.m, minHeight: 40, paddingVertical: 8 }}>
      <View style={{ width: 10, height: 10, marginTop: 5, borderRadius: 5, backgroundColor: VERDICT_COLOR[state] || '#8e8e93' }} />
      <Text style={{ width: 150, fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{label}</Text>
      <Text style={{ width: 110, fontSize: typeScale.callout, fontWeight: 600, color: VERDICT_COLOR[state], textAlign: 'start' }}>{tr(`sysinfo.perf.${states}.${state}`)}</Text>
      <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.callout, color: t.textDim, textAlign: 'start', lineHeight: 1.4 }}>{detail}</Text>
    </View>
  );
}

// the stability verdict's text: what went wrong (or that nothing did), then what was only checked (info notes:
// the helper sandbox held)
function stabilityText(stab, tr) {
  const vars = (v) => (Array.isArray(v.ids) ? { ...v, list: v.ids.map((id) => tr(`sysinfo.perf.sbx.${id}`)).join(', ') } : v);
  const say = (ns) => ns.map((n) => tr(`sysinfo.perf.note.${n.id}`, vars(n.vars))).join(' ');
  const warns = stab.notes.filter((n) => !n.info), infos = stab.notes.filter((n) => n.info);
  return [warns.length ? say(warns) : tr('sysinfo.perf.note.none'), say(infos)].filter(Boolean).join(' ');
}

function Measurement({ id, res, refs, change }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const m = res.results && res.results[id];
  const skipped = res.skipped && res.skipped[id];
  const vs = refs.map((r) => {
    const x = ratio(res, r, id);
    if (x == null) return null;
    return tr('sysinfo.perf.timesRef', { x: formatNumber(x, 2), ref: tr(`sysinfo.perf.ref.${r.id}`) });
  }).filter(Boolean).join(' · ');
  const ch = change != null && Math.abs(change) >= 0.005
    ? tr(change > 0 ? 'sysinfo.perf.better' : 'sysinfo.perf.worse', { pct: formatNumber(Math.abs(change) * 100, 0) }) : '';
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m, minHeight: 32, paddingVertical: 4 }}>
      <Text style={{ width: 230, fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{tr(`sysinfo.perf.m.${key(id)}`)}</Text>
      <Text style={{ width: 130, fontSize: typeScale.body, fontWeight: 600, color: m ? t.text : t.textDim, textAlign: 'end', fontVariantNumeric: 'tabular-nums' }}>
        {m ? `${fmt(m.value)} ${m.unit}` : '—'}</Text>
      <Text style={{ flex: 1, minWidth: 0, fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{m ? vs : skipped ? tr(`sysinfo.perf.skip.${skipped}`) : ''}</Text>
      <Text style={{ width: 110, fontSize: typeScale.callout, color: change > 0 ? '#30d158' : '#ff9f0a', textAlign: 'end' }}>{ch}</Text>
    </View>
  );
}

function Row({ label, value }) {
  const t = useTheme();
  return (
    <FormRow label={label} labelWidth={190} minHeight={28}>
      <Text style={{ fontSize: typeScale.body, color: value ? t.text : t.textDim, textAlign: 'start' }}>{value || '—'}</Text>
    </FormRow>
  );
}

function Conditions({ res }) {
  const { t: tr } = useLocale();
  const b = res.before || {}, a = res.after || {};
  const temp = (c) => (c.tempC != null ? tr('sysinfo.perf.tempC', { v: formatNumber(c.tempC, 0) }) : '');
  const trips = [b.passiveC ? tr('sysinfo.perf.passive', { v: formatNumber(b.passiveC, 0) }) : '', b.criticalC ? tr('sysinfo.perf.critical', { v: formatNumber(b.criticalC, 0) }) : ''].filter(Boolean).join(' · ');
  const ds = ((res.session && res.session.displays) || []).map((d) => (d.mode ? `${d.mode.w}×${d.mode.h} @ ${formatNumber(d.mode.refresh / 1000, 0)} Hz${d.scale && d.scale !== 100 ? ` (${d.scale} %)` : ''}` : '')).filter(Boolean).join(', ');
  // a Raspberry Pi: what its firmware reported during the run; "none" says what could be watched
  const pf = piFirmware(res);
  const pfVars = { temp: formatNumber((pf && pf.tempLimitC) || 80, 0) };
  const pfText = !pf ? '' : pf.events.length ? pf.events.map((id) => tr(`sysinfo.perf.pi.${id}`, pfVars)).join(', ')
    : tr(pf.flags ? 'sysinfo.perf.pi.none' : pf.voltageAlarm ? 'sysinfo.perf.pi.noneVoltage' : 'sysinfo.perf.pi.noneTemp', pfVars);
  return (
    <GroupBox title={tr('sysinfo.perf.conditions')} footer={tr('sysinfo.perf.conditionsFooter')}>
      <Row label={tr('sysinfo.perf.c.versions')} value={`LordOS ${res.lordos} · ${tr('sysinfo.perf.kernel', { v: res.kernel })} · LordBench ${res.lordbench}`} />
      <Row label={tr('sysinfo.perf.c.threads')} value={tr('sysinfo.perf.threadsOf', { n: res.threads, cpus: res.cpus })} />
      <Row label={tr('sysinfo.perf.c.quiet')} value={tr(res.quiet && res.quiet.quiet ? 'sysinfo.perf.quietYes' : 'sysinfo.perf.quietNo', { pct: formatNumber((res.quiet && res.quiet.busyPct) || 0, 0), s: formatNumber((res.quiet && res.quiet.waitedS) || 0, 0) })} />
      <Row label={tr('sysinfo.perf.c.temps')} value={[temp(b) && tr('sysinfo.perf.startEnd', { a: temp(b), b: temp(a) || '—' }), trips].filter(Boolean).join(' · ')} />
      <Row label={tr('sysinfo.perf.c.power')} value={[tr(`sysinfo.perf.power.${b.power || 'unknown'}`), b.batteryPct != null ? `${b.batteryPct} %` : '', b.platformProfile || ''].filter(Boolean).join(' · ')} />
      {pf ? <Row label={tr('sysinfo.perf.c.piFirmware')} value={pfText} /> : null}
      <Row label={tr('sysinfo.perf.c.cpufreq')} value={[b.governor, b.freqMaxMHz ? tr('sysinfo.perf.maxMHz', { v: formatNumber(b.freqMaxMHz, 0) }) : ''].filter(Boolean).join(' · ')} />
      <Row label={tr('sysinfo.perf.c.gpu')} value={[res.gpuKind ? tr(`sysinfo.perf.gpuKind.${res.gpuKind}`) : '', res.session && res.session.renderer].filter(Boolean).join(' · ')} />
      <Row label={tr('sysinfo.perf.c.displays')} value={[ds, res.session && res.session.composition ? tr('sysinfo.perf.composition', { c: res.session.composition }) : ''].filter(Boolean).join(' · ')} />
      <Row label={tr('sysinfo.perf.c.storage')} value={res.storage && res.storage.kind !== 'none' ? tr('sysinfo.perf.storageOn', { kind: tr(`sysinfo.perf.disk.${res.storage.kind}`), mb: res.storage.mb }) : tr('sysinfo.perf.storageNone')} />
      <Row label={tr('sysinfo.perf.c.live')} value={tr(res.live ? 'sysinfo.perf.liveYes' : 'sysinfo.perf.liveNo')} />
    </GroupBox>
  );
}

// the boot time test (bench/boottest.h): N restarts in a row, each boot's time recorded by lord-sessiond;
// started with the boot-test admin action and then the normal Restart (its confirmation included)
const BOOT_TEST_N = [3, 5, 10];
function BootTest({ onChange }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [n, setN] = useState(5);
  const [state, setState] = useState(() => { try { return bench().bootTest(); } catch (e) { return null; } });
  const [err, setErr] = useState('');
  const refresh = () => { try { setState(bench().bootTest()); } catch (e) { setState(null); } onChange && onChange(); };
  const start = async () => {
    setErr('');
    try { await admin.run('boot-test', String(n)); } catch (e) { setErr(e.message || String(e)); return; }
    refresh();
    session.restart();
  };
  const stop = async () => { try { await admin.run('boot-test-cancel', ''); } catch (e) { setErr(e.message || String(e)); } refresh(); };
  return (
    <View style={{ gap: spacing.s, paddingVertical: 6 }}>
      {state ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m }}>
          <Text style={{ flex: 1, fontSize: typeScale.body, color: t.text, textAlign: 'start', lineHeight: 1.4 }}>
            {tr(state.left === state.total ? 'sysinfo.perf.bootTestSet' : 'sysinfo.perf.bootTestRunning', { n: state.total, done: state.total - state.left })}</Text>
          {state.left === state.total && session.canPower() ? <Button label={tr('sysinfo.perf.bootTestRestart')} onClick={() => session.restart()} /> : null}
          <Button label={tr('sysinfo.perf.bootTestStop')} onClick={stop} />
        </View>
      ) : (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m }}>
          <Text style={{ flex: 1, fontSize: typeScale.body, color: t.text, textAlign: 'start', lineHeight: 1.4 }}>{tr('sysinfo.perf.bootTestAsk')}</Text>
          <Segmented appearance="neutral" value={n} onChange={setN} options={BOOT_TEST_N.map((x) => [x, String(x)])} />
          <Button label={tr('sysinfo.perf.bootTestStart')} disabled={!session.canPower()} onClick={start} />
        </View>
      )}
      {err ? <Text style={{ fontSize: typeScale.callout, color: '#ff453a', textAlign: 'start' }}>{err}</Text> : null}
    </View>
  );
}

function Boots() {
  const { t: tr } = useLocale();
  const [rev, setRev] = useState(0);
  const rows = useMemo(() => { try { return bench() ? bench().boots() : null; } catch (e) { return null; } }, [rev]);
  return (
    <GroupBox title={tr('sysinfo.perf.boots')} footer={tr('sysinfo.perf.bootsFooter')}>
      {(rows || []).slice(-6).reverse().map((r) => (
        <Row key={r.version} label={r.version} value={(r.boots
          ? tr('sysinfo.perf.bootLine', { median: formatNumber(r.median, 1), min: formatNumber(r.min, 1), max: formatNumber(r.max, 1), n: r.boots })
            + (r.first != null ? ` · ${tr('sysinfo.perf.bootFirst', { s: formatNumber(r.first, 1) })}` : '')
          : tr('sysinfo.perf.bootOnlyFirst', { s: formatNumber(r.first, 1) }))
          + (r.test ? ` · ${tr('sysinfo.perf.bootTestLine', { median: formatNumber(r.test.median, 1), min: formatNumber(r.test.min, 1), max: formatNumber(r.test.max, 1), n: r.test.boots, of: r.test.of })}` : '')} />
      ))}
      <BootTest onChange={() => setRev((x) => x + 1)} />
    </GroupBox>
  );
}

function Result({ res, report }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const desktop = res.mode === 'desktop', slot = desktop ? 2 : 0;
  const refs = useMemo(() => (desktop ? [] : comparableReferences(res)), [res]);
  const prev = useMemo(() => { const s0 = readSaved(slot); return s0 && s0.at === res.at ? readSaved(slot + 1) : s0; }, [res]);
  const prevComparable = prev && prev.at !== res.at ? prev : null;
  const ch = useMemo(() => changes(res, prevComparable), [res, prevComparable]);
  const diffs = useMemo(() => (ch ? conditionDiffs(res, prevComparable) : []), [res, prevComparable, ch]);
  const stab = useMemo(() => stability(res), [res]);
  const dv = useMemo(() => desktopVerdict(res), [res]);
  const compat = useMemo(() => compatibilityVerdict(compatibility(report)), [report]);
  const perfText = refs.map((r) => {
    const cr = categoryRatios(res, r);
    const parts = CATEGORIES.filter((c) => cr[c.id] != null).map((c) => tr('sysinfo.perf.catTimes', { cat: tr(`sysinfo.perf.cat.${c.id}`), x: formatNumber(cr[c.id], 2) }));
    return parts.length ? tr('sysinfo.perf.againstRef', { ref: tr(`sysinfo.perf.ref.${r.id}`), list: parts.join(', ') }) : '';
  }).filter(Boolean).join('\n') || tr('sysinfo.perf.noRefs');
  const series = sustainedSeries(res);
  const [msg, setMsg] = useState(null);
  const save = () => {
    const dir = userDirs().documents;
    try { fs.mkdir(dir); } catch (e) { /* already there */ }
    const name = fileName(res.at, (n) => { try { return !!fs.stat(`${dir}/${n}`); } catch (e) { return false; } });
    try {
      if (!name) throw new Error('name');
      fs.write(`${dir}/${name}`, `${JSON.stringify(res, null, 2)}\n`);
      setMsg({ ok: true, text: tr('sysinfo.report.saved', { name }), dir });
    } catch (e) { setMsg({ ok: false, text: tr('sysinfo.report.saveFailed') }); }
  };
  return (
    <View style={{ gap: spacing.l }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
        <Text style={{ flex: 1, fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>
          {tr('sysinfo.perf.ranAt', { mode: tr(`sysinfo.perf.mode.${res.mode}`), time: new Date(res.at * 1000).toLocaleString(), s: formatNumber(res.seconds, 0) })}
          {res.status !== 'done' ? ` · ${tr(`sysinfo.perf.status.${res.status}`)}${res.stopReason ? ` (${tr(`sysinfo.perf.why.${res.stopReason}`)})` : ''}` : ''}</Text>
        <Button label={tr('sysinfo.report.save')} onClick={save} />
      </View>
      {msg ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
          <Text style={{ fontSize: typeScale.callout, color: msg.ok ? '#30d158' : '#ff453a', textAlign: 'start' }}>{msg.text}</Text>
          {msg.dir ? <Button small plain label={tr('sysinfo.report.showInFiles')} onClick={() => wm.open('files', { new: true, args: { path: msg.dir } })} /> : null}
        </View>
      ) : null}
      {desktop ? (
        <GroupBox title={tr('sysinfo.perf.verdicts')} footer={tr('sysinfo.perf.deskFooter', { budget: formatNumber((res.frames && res.frames.budgetMs) || 0, 1) })}>
          <Verdict label={tr('sysinfo.perf.v.desktop')} state={dv.state} states="deskState"
            detail={dv.notes.map((n) => tr(`sysinfo.perf.dnote.${n.id}`, Object.fromEntries(Object.entries(n.vars).map(([k, v]) => [k, formatNumber(v, 1)])))).join(' ') || tr('sysinfo.perf.dnote.none')} />
          <Verdict label={tr('sysinfo.perf.v.stability')} state={stab.state}
            detail={stabilityText(stab, tr)} />
        </GroupBox>
      ) : (
        <GroupBox title={tr('sysinfo.perf.verdicts')} footer={tr('sysinfo.perf.verdictsFooter')}>
          <Verdict label={tr('sysinfo.perf.v.performance')} state={refs.length ? 'pass' : 'unknown'} detail={perfText} />
          <Verdict label={tr('sysinfo.perf.v.compatibility')} state={compat.state}
            detail={tr('sysinfo.perf.compatCount', { works: compat.count.works, limits: compat.count.limits, no: compat.count.no })} />
          <Verdict label={tr('sysinfo.perf.v.stability')} state={stab.state}
            detail={stabilityText(stab, tr)} />
        </GroupBox>
      )}
      {(desktop ? DESKTOP_CATEGORIES : CATEGORIES).map((c) => (
        <GroupBox key={c.id} title={tr(`sysinfo.perf.cat.${c.id}`)}>
          {c.ids.map((id) => <Measurement key={id} id={id} res={res} refs={refs} change={ch ? ch[id] : null} />)}
          {c.id === 'sustained' && series.length ? (
            <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start', paddingVertical: 6, lineHeight: 1.5 }}>
              {series.map(([s, g, temp]) => tr(temp == null ? 'sysinfo.perf.sampleNoTemp' : 'sysinfo.perf.sample', { s: formatNumber(s, 0), g: formatNumber(g, 1), temp: temp == null ? '' : formatNumber(temp, 0) })).join(' · ')}</Text>
          ) : null}
        </GroupBox>
      ))}
      {diffs.length ? (
        <GroupBox title={tr('sysinfo.perf.diffs')}>
          {diffs.map((d) => <Row key={d.id} label={tr(`sysinfo.perf.diff.${d.id}`)} value={tr('sysinfo.perf.diffLine', { now: String(d.now ?? '—'), before: String(d.before ?? '—') })} />)}
        </GroupBox>
      ) : null}
      <Conditions res={res} />
    </View>
  );
}

export default function Performance({ r }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [st, running, start, cancel] = useBench();
  const fresh = st.result || null;
  const res = fresh && fresh.mode !== 'desktop' ? fresh : running ? null : readSaved(0);
  const dres = fresh && fresh.mode === 'desktop' ? fresh : running ? null : readSaved(2);
  // the desktop run's scene, while lord-server records frames (comp.* and desk.* steps)
  const scene = running && st.mode === 'desktop' && /^(comp|desk)\./.test(st.step || '');
  useEffect(() => (scene ? startScene() : undefined), [scene]);
  if (st.state === 'unavailable') return <View style={{ padding: spacing.xl }}><Text style={{ color: t.textDim, textAlign: 'start' }}>{tr('sysinfo.perf.unavailable')}</Text></View>;
  return (
    <ScrollView style={{ flex: 1 }}>
      <View style={{ padding: spacing.xl, gap: spacing.l }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m }}>
          <View style={{ flex: 1, gap: 2 }}>
            <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>LordBench</Text>
            <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start', lineHeight: 1.4 }}>{tr('sysinfo.perf.intro')}</Text>
          </View>
          {running ? <Button label={tr('sysinfo.perf.cancel')} onClick={cancel} /> : [
            <Button key="d" label={tr('sysinfo.perf.desktop')} onClick={() => start('desktop')} />,
            <Button key="f" label={tr('sysinfo.perf.full')} onClick={() => start('full')} />,
            <Button key="q" primary label={tr('sysinfo.perf.quick')} onClick={() => start('quick')} />,
          ]}
        </View>
        {!running ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start', lineHeight: 1.4 }}>{tr('sysinfo.perf.fullNote', { min: SUSTAINED_S / 60 })}</Text> : null}
        {st.error ? <Text style={{ fontSize: typeScale.callout, color: '#ff453a', textAlign: 'start' }}>{tr('sysinfo.perf.startFailed')}</Text> : null}
        {running ? (
          <View style={{ gap: spacing.s }}>
            <Text style={{ fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>
              {st.state === 'waiting' ? tr('sysinfo.perf.waiting') : tr('sysinfo.perf.measuring', { what: st.step ? tr(`sysinfo.perf.m.${key(st.step)}`) : '…' })}</Text>
            <Bar value={st.progress || 0} />
            <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr(st.mode === 'desktop' ? 'sysinfo.perf.deskKeepStill' : 'sysinfo.perf.keepStill')}</Text>
          </View>
        ) : null}
        {res ? <Result res={res} report={r} /> : !running && !dres ? <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{tr('sysinfo.perf.none')}</Text> : null}
        {dres ? (
          <View style={{ gap: spacing.l }}>
            <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start', paddingTop: spacing.m }}>{tr('sysinfo.perf.deskTitle')}</Text>
            <Result res={dres} report={r} />
          </View>
        ) : null}
        {!running ? <Boots /> : null}
      </View>
    </ScrollView>
  );
}
