// Logs (the approved mock "Logs", 6.19): the machine's log (lord-vm, QEMU and passt), the guest's serial console, the
// guest's journal when the agent is on (7), and an Overview that brings what they say together (unified diagnostics:
// state, sandbox, limits, network, engine, storage, recoveries, problems) with Copy report. A filter as you type, Level
// (all, warnings, errors), a unit or source filter (click a unit; journal and event logs), Pause with a count of new
// lines, Clear view, Copy, Save. The guest's lines are shown in this computer's local time, its own zone named in the
// footer when it differs (diag.js journalLine). Read again every second; at most
// the last 256 KB of a log and 50 000 lines are kept, and guest text has its escape sequences removed (lord.vm.log).
import { View, Text, TextInput, ScrollView, Segmented, useAppWindow, wm, fs, clipboard, revealPath, useNarrow, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { usePal, Btn, has } from './ui.jsx';
import { Sym, ToolPill, ToolGlyph } from './kit.jsx';
import { readLog, reportText, journalLine, guestZone, tzLabel } from './diag.js';

const levelOf = (l) => (/(fail|error|refus|crash|killed|panic|cannot|can't|err\b|\[err)/i.test(l) ? 'error' : /(warn|note:|lost|retry)/i.test(l) ? 'warn' : 'info');
const LEVELS = ['all', 'warn', 'error'];
const MAX_LINES = 50000;

function Overview({ dir, m, text }) {
  const p = usePal();
  if (!m) return null;
  const log = readLog(text);
  const snaps = has() ? (lord.vm.snaps(dir)?.list || []).length : 0;
  const rows = [
    ['state', `${tr(`machines.state.${m.state === 'stopped' && m.reason === 'suspended' ? 'suspended' : m.state || 'stopped'}`)}${m.detail ? ` — ${m.detail}` : ''}`],
    ['started', log.started || '—'],
    ['engine', log.version ? `QEMU ${log.version}` : '—'],
    ['sandbox', log.sandbox || '—'],
    ['limits', log.cgroup || '—'],
    ['network', `${tr(`machines.cfg.net.${m.network || 'shared'}`)}${log.network.length ? ` — ${log.network[log.network.length - 1].text}` : ''}`],
    ['storage', [tr('machines.disk', { n: m.diskGb }), m.base ? tr('machines.diag.linked') : null, snaps ? tr('machines.diag.snaps', { n: snaps }) : null,
      m.suspended ? tr('machines.doing.suspended') : null].filter(Boolean).join(', ')],
    ['operation', m.operation ? tr('machines.diag.opWaiting', { op: m.operation }) : '—'],
  ];
  const copy = () => clipboard.set(reportText(m, log, { snapshots: snaps }));
  return (
    <ScrollView style={{ flex: 1 }} contentStyle={{ paddingHorizontal: 18, paddingVertical: 14, gap: 12 }}>
      <View style={{ borderRadius: 12, backgroundColor: p.card }}>
        {rows.map(([k, v], i) => (
          <View key={k} style={{ flexDirection: 'row', gap: 12, paddingHorizontal: 14, paddingVertical: 9, borderBottomWidth: i < rows.length - 1 ? 1 : 0, borderColor: p.line }}>
            <Text style={{ width: 150, fontSize: 13, color: p.dim, textAlign: 'start' }}>{tr(`machines.diag.${k}`)}</Text>
            <Text style={{ flex: 1, fontSize: 13, color: p.text, textAlign: 'start', direction: 'ltr' }}>{v}</Text>
          </View>
        ))}
      </View>
      {[['recovery', log.recovery, '#9fd0ff'], ['problems', log.problems, '#ff9a9a'], ['notes', log.notes, '#ffc56e']].map(([k, list, color]) => (list.length ? (
        <View key={k} style={{ gap: 4 }}>
          <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr(`machines.diag.${k}`)}</Text>
          {list.map((x, i) => (
            <View key={i} style={{ flexDirection: 'row', gap: 10 }}>
              <Text style={{ width: 140, fontFamily: 'monospace', fontSize: 12, color: p.dim }}>{x.at}</Text>
              <Text style={{ flex: 1, fontFamily: 'monospace', fontSize: 12, color, textAlign: 'start', direction: 'ltr' }}>{x.text}</Text>
            </View>
          ))}
        </View>
      ) : null))}
      {!log.problems.length && !log.recovery.length ? <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start' }}>{tr('machines.diag.clean')}</Text> : null}
      <View style={{ flexDirection: 'row' }}><Btn label={tr('machines.diag.copy')} onClick={copy} /></View>
    </ScrollView>
  );
}

export function Logs({ dir, name }) {
  const p = usePal();
  const { id: winId } = useAppWindow();
  const [which, setWhich] = useState('overview');
  const [text, setText] = useState('');
  const [filter, setFilter] = useState('');
  const [level, setLevel] = useState('all');
  const [unit, setUnit] = useState('');
  const [paused, setPaused] = useState(false);
  const [saved, setSaved] = useState('');
  const [m, setM] = useState(null);
  const cleared = useRef({ which: '', count: 0 });
  const held = useRef(0);
  useEffect(() => { wm.setTitle(winId, tr('machines.logs.title', { name })); }, [name]);
  useEffect(() => {
    const read = () => {
      if (!has()) return;
      setM(lord.vm.list().find((x) => x.dir === dir) || null);
      const src = which === 'overview' ? 'vm' : which;
      const t = lord.vm.log(dir, src, 262144);
      if (paused) { held.current = Math.max(0, t.split('\n').filter((l) => l.trim()).length - text.split('\n').filter((l) => l.trim()).length); return; }
      held.current = 0;
      setText(t);
    };
    read();
    const timer = setInterval(read, 1000);
    return () => clearInterval(timer);
  }, [which, paused]);
  let all = text.split('\n').filter((l) => l.trim());
  if (all.length > MAX_LINES) all = all.slice(-MAX_LINES);
  if (cleared.current.which === which) all = all.slice(Math.min(all.length, cleared.current.count));
  const f = filter.trim().toLowerCase();
  const jr = which === 'journal';
  const lines = all.filter((l) => (!f || l.toLowerCase().includes(f)) && (level === 'all' || (level === 'error' ? levelOf(l) === 'error' : levelOf(l) !== 'info')) &&
    (!jr || !unit || journalLine(l).unit === unit));
  const zone = jr ? guestZone(all) : null;
  const tone = { info: '#9fd0ff', warn: '#ffc56e', error: '#ff9a9a' };
  const save = () => {
    const out = `${fs.home()}/Documents/${name} ${which === 'vm' ? 'machine' : which === 'serial' ? 'console' : 'journal'} log.txt`;
    try { fs.write(out, `${lines.join('\n')}\n`); setSaved(out); } catch (e) { setSaved(''); }
  };
  const narrow = useNarrow();       // a small window: the controls wrap onto a second line
  const sources = [['overview', tr('machines.logs.overview')], ['vm', tr('machines.logs.machine')], ['serial', tr('machines.logs.console')]];
  if (m?.agent) sources.push(['journal', tr('machines.logs.journal')]);
  return (
    <View style={{ flex: 1, backgroundColor: p.bg }}>
      <View style={{ minHeight: 56, flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 10, paddingHorizontal: 16, paddingVertical: narrow ? 8 : 0, borderBottomWidth: 1, borderColor: p.line }}>
        <Sym name="logs" size={20} color={p.text} />
        <Text style={{ flex: 1, minWidth: 0, fontSize: 15, fontWeight: 700, color: p.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{tr('machines.logs.title', { name })}</Text>
        <Segmented small value={which} onChange={(x) => { setWhich(x); setPaused(false); setUnit(''); }} options={sources} />
        {jr && unit ? <Btn label={tr('machines.logs.unitOnly', { unit })} onClick={() => setUnit('')} style={{ height: 30 }} /> : null}
        {which !== 'overview' ? <>
          <Btn label={tr(`machines.logs.level.${level}`)} onClick={() => setLevel(LEVELS[(LEVELS.indexOf(level) + 1) % LEVELS.length])} style={{ height: 30 }} />
          <ToolPill>
            <ToolGlyph icon={paused ? 'play' : 'pause'} title={paused ? tr('machines.logs.follow') : tr('machines.logs.pause')} on={paused} onClick={() => setPaused(!paused)} />
            <ToolGlyph icon="copy" title={tr('machines.logs.copy')} onClick={() => clipboard.set(lines.join('\n'))} />
            <ToolGlyph icon="download" title={tr('machines.logs.save')} onClick={save} />
            <ToolGlyph icon="trash" title={tr('machines.logs.clear')} onClick={() => { cleared.current = { which, count: text.split('\n').filter((l) => l.trim()).length }; setText(text); }} />
          </ToolPill>
          <View style={{ width: narrow ? 140 : 200, flexDirection: 'row', alignItems: 'center', gap: 6, paddingLeft: 10, height: 32, borderRadius: 16,
            backgroundColor: p.dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.05)', border: `1px solid ${p.dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.06)'}` }}>
            <Sym name="search" size={14} color={p.dim} />
            <View style={{ flex: 1, minWidth: 0 }}><TextInput value={filter} onChange={setFilter} placeholder={tr('machines.logs.filter')}
              style={{ backgroundColor: 'transparent', borderWidth: 0, border: 'none', paddingHorizontal: 2, height: 28 }} /></View>
          </View>
        </> : null}
      </View>
      {which === 'overview' ? <Overview dir={dir} m={m} text={text} /> : (
        <ScrollView style={{ flex: 1, backgroundColor: p.dark ? '#16171b' : 'rgba(0,0,0,0.03)' }} contentStyle={{ paddingHorizontal: 16, paddingVertical: 10 }}>
          {lines.slice(-800).map((l, i) => {
            const jl = jr ? journalLine(l) : null;
            const mt = jl?.at ? null : l.match(/^(\d{4}-\d\d-\d\dT(\d\d:\d\d:\d\d)[.\d]*Z)\s+(.*)$/) || l.match(/^((\d{4}-\d\d-\d\d[T ])?(\d\d:\d\d:\d\d))\S*\s+(.*)$/);
            const lv = levelOf(l);
            const at = jl?.at ? jl.at : mt ? (mt[2] && mt[2].length === 8 ? mt[2] : mt[3]) : null;
            const body = jl?.at ? jl.body : mt ? mt[mt.length - 1] : l;
            const lu = jl?.unit || '';
            return (
              <View key={i} style={{ flexDirection: 'row', gap: 10 }}>
                {at ? <Text style={{ width: 64, fontFamily: 'monospace', fontSize: 12, lineHeight: 1.7, color: p.dim }}>{at}</Text> : null}
                <Text style={{ width: 44, fontFamily: 'monospace', fontSize: 12, lineHeight: 1.7, color: tone[lv] }}>{lv}</Text>
                {jr ? <Text onClick={() => lu && setUnit(unit === lu ? '' : lu)} style={{ width: 120, fontFamily: 'monospace', fontSize: 12, lineHeight: 1.7, color: unit === lu && lu ? p.text : p.dim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{lu}</Text> : null}
                <Text style={{ flex: 1, fontFamily: 'monospace', fontSize: 12, lineHeight: 1.7, color: p.text, textAlign: 'start', direction: 'ltr', whiteSpace: 'nowrap', overflow: 'hidden' }}>{body}</Text>
              </View>
            );
          })}
        </ScrollView>
      )}
      <View style={{ height: 32, flexDirection: 'row', alignItems: 'center', gap: 16, paddingHorizontal: 16, borderTopWidth: 1, borderColor: p.line }}>
        <Text style={{ fontSize: 12, color: p.dim }}>{which === 'overview' ? tr('machines.logs.overviewHint') : paused ? tr('machines.logs.pausedNew', { n: held.current }) : tr(`machines.logs.following.${which}`)}</Text>
        {which !== 'overview' ? <Text style={{ fontSize: 12, color: p.dim }}>{tr('machines.logs.count', { a: lines.length, b: all.length })}</Text> : null}
        {zone != null ? <Text style={{ fontSize: 12, color: p.dim }}>{tr('machines.logs.guestZone', { zone: tzLabel(zone) })}</Text> : null}
        <View style={{ flex: 1 }} />
        {saved ? <Text onClick={() => revealPath(saved)} style={{ fontSize: 12, color: '#6cb6ff' }}>{tr('machines.logs.savedAs', { name: saved.split('/').pop() })}</Text> : null}
        <Text style={{ fontSize: 12, color: p.dim }}>{sources.find(([k]) => k === which)?.[1]}</Text>
      </View>
    </View>
  );
}
