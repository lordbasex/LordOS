// Console (the approved mock): every query a connection sent, with its time, who sent it (you, or the app reading the
// structure, the tables, a page), how long it took and its error. It lives in memory with the connection and is gone
// when it closes; passwords never show (model.redactSql). A window of its own: wm.open('databases', { args: { console } }).
import { View, Text, SearchField, FilePicker, useStore, useAppWindow, wm, fs, formatTime, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { usePal, Btn, Check, Note, MONO, useSize } from './ui.jsx';
import { Grid } from './grid.jsx';
import { store, clearLog } from './conn.js';
import { fmtMs } from './model.js';

const pad = (x, k = 2) => String(x).padStart(k, '0');
const stamp = (at) => { const d = new Date(at); return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}.${pad(d.getMilliseconds(), 3)}`; };

export function ConsoleWindow({ sessionKey }) {
  const p = usePal();
  const { id: winId } = useAppWindow();
  useStore(store);
  const s = store.get().sessions.find((x) => x.key === sessionKey);
  const [query, setQuery] = useState('');
  const [showApp, setShowApp] = useState(true);
  const [errorsOnly, setErrorsOnly] = useState(false);
  const [saving, setSaving] = useState(false);
  const [msg, setMsg] = useState(null);
  const box = useRef(null);
  const size = useSize(box, { w: 900, h: 400 });
  useEffect(() => { if (s) wm.setTitle(winId, tr('databases.console.title', { name: s.fav.name })); }, [s?.key]);

  if (!s) {
    return <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', backgroundColor: p.bg }}><Text style={{ fontSize: 13, color: p.dim }}>{tr('databases.console.gone')}</Text></View>;
  }
  const q = query.trim().toLowerCase();
  const lines = s.log.filter((l) => (showApp || l.src !== 'app') && (!errorsOnly || l.error) && (!q || l.sql.toLowerCase().includes(q) || (l.error || '').toLowerCase().includes(q)));
  const pill = (l) => {
    const [bg, fg, label] = l.error ? ['rgba(255,69,58,0.2)', '#ff6961', tr('databases.console.error')]
      : l.src === 'you' ? ['rgba(10,132,255,0.2)', '#5aa9ff', tr('databases.console.you')] : [p.dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.06)', p.dim, tr('databases.console.app')];
    return <View style={{ paddingHorizontal: 6, paddingVertical: 1, borderRadius: 5, backgroundColor: bg }}><Text style={{ fontSize: 11, fontWeight: 600, color: fg }}>{label}</Text></View>;
  };
  const qw = Math.max(300, size.w - 96 - 80 - 80 - 4);
  const cols = [
    { key: 't', label: tr('databases.console.time'), width: 96, render: (v, l) => <Text style={{ fontSize: 12, fontFamily: MONO, color: p.dim }}>{stamp(l.at)}</Text> },
    { key: 'src', label: tr('databases.console.source'), width: 80, render: (v, l) => pill(l) },
    { key: 'q', label: tr('databases.console.query'), width: qw, render: (v, l) => (
      <Text style={{ fontSize: 12, fontFamily: MONO, color: l.error ? '#ff6961' : l.src === 'you' ? p.text : p.dim, whiteSpace: 'nowrap', overflow: 'hidden' }}>
        {`${l.sql.replace(/\s+/g, ' ')}${l.error ? `  — ${l.error}` : ''}`}
      </Text>
    ) },
    { key: 'ms', label: tr('databases.console.ms'), width: 80, align: 'end', render: (v, l) => <Text style={{ fontSize: 12, fontFamily: MONO, color: p.dim }}>{l.ms === null ? '…' : fmtMs(l.ms)}</Text> },
  ];
  const save = (path) => {
    setSaving(false);
    if (typeof path !== 'string') return;
    const text = s.log.map((l) => `${stamp(l.at)}\t${l.src}\t${l.ms === null ? '' : fmtMs(l.ms)}\t${l.sql.replace(/\s+/g, ' ')}${l.error ? `\t-- ${l.error}` : ''}`).join('\n');
    try { fs.write(path, `${text}\n`); setMsg({ kind: 'ok', text: tr('databases.console.saved', { name: fs.basename(path) }) }); } catch (e) { setMsg({ kind: 'bad', text: tr('databases.console.notSaved') }); }
  };
  return (
    <View style={{ flex: 1, backgroundColor: p.bg }}>
      <View style={{ height: 42, flexDirection: 'row', alignItems: 'center', gap: 12, paddingHorizontal: 12, backgroundColor: p.bar }}>
        <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 1, backgroundColor: p.line }} />
        <SearchField value={query} onChange={setQuery} placeholder={tr('databases.console.filter')} width={240} />
        <Check on={showApp} onChange={setShowApp} label={tr('databases.console.showApp')} />
        <Check on={errorsOnly} onChange={setErrorsOnly} label={tr('databases.console.errorsOnly')} />
        <View style={{ flex: 1 }} />
        <Btn small label={tr('databases.clear')} onClick={() => clearLog(s.key)} />
        <Btn small label={tr('databases.console.saveAs')} onClick={() => setSaving(true)} />
      </View>
      {msg ? <Note kind={msg.kind} style={{ borderRadius: 0 }} action={<Btn small label={tr('databases.dismiss')} onClick={() => setMsg(null)} />}>{msg.text}</Note> : null}
      <View ref={box} style={{ flex: 1, minHeight: 0 }}>
        <Grid columns={cols} rows={lines} cell={() => null} scrollTo={lines.length - 1} emptyText={tr('databases.console.empty')}
          rowStyle={(l) => (l.error ? { backgroundColor: 'rgba(255,69,58,0.1)' } : null)} />
      </View>
      <View style={{ height: 30, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 12, backgroundColor: p.bar }}>
        <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 1, backgroundColor: p.line }} />
        <Text style={{ fontSize: 12, color: p.dim, whiteSpace: 'nowrap', overflow: 'hidden' }}>
          {tr('databases.console.footer', { count: s.log.length, time: formatTime(new Date(s.openedAt)) })}
        </Text>
      </View>
      {saving ? <FilePicker mode="save" name={`${s.fav.name || 'console'}.sql.log`} title={tr('databases.console.saveAs')} onClose={() => setSaving(false)} onPick={save} /> : null}
    </View>
  );
}
