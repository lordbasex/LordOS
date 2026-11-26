// Encrypted machines' keys and Export, the app's side (docs/VIRTUALIZATION.md 8 and 6.4; the logic is storagemodel.js).
//
// A key never comes to JS: lord-server makes it, keeps it in the session keyring (Passwords shows it as "Machines: <name>",
// its recovery key) and hands it to lord-vm itself. The app only asks for it (lord.vm.keyState(…, true)) before a start, a
// copy or an export, and when the keyring is locked shows the system's own unlock prompt. The one place a key is typed is
// the recovery key of a machine brought from another computer: a window of its own, protected whole (never captured).
import { View, Text, TextInput, Toggle, Segmented, FilePicker, wm, useAppWindow, t as tr } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { usePal, Btn, Sheet } from './ui.jsx';
import { normalizeRecoveryKey, exportRefusal, exportName, exportTarget, jobProgress, jobError } from './storagemodel.js';
import { ensureKey, answerRecoveryKey as answer } from './keyflow.js';

export function RecoveryKeyWindow({ id, name }) {
  const p = usePal();
  const { id: winId } = useAppWindow();
  const [text, setText] = useState('');
  const [bad, setBad] = useState(false);
  useEffect(() => () => answer(id, false), []);
  const ok = !!normalizeRecoveryKey(text);
  const submit = () => {
    if (!ok) { setBad(true); return; }
    const kept = lord.vm.keyEnter(id, text, name || 'Machine');
    setText('');
    if (!kept) { setBad(true); return; }
    answer(id, true);
    wm.close(winId, true);
  };
  return (
    // handsDeny: a machine's recovery key is never an AI agent's to read (docs/HANDS.md)
    <View handsDeny style={{ flex: 1, padding: 22, gap: 12, backgroundColor: p.bg }}>
      <Text style={{ fontSize: 16, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.key.recoveryTitle', { name })}</Text>
      <Text style={{ fontSize: 13, lineHeight: 1.45, color: p.dim, textAlign: 'start' }}>{tr('machines.key.recoveryText', { name })}</Text>
      <TextInput secure value={text} onChange={(x) => { setText(x); setBad(false); }} onSubmit={submit} placeholder={tr('machines.key.recoveryPlaceholder')} />
      {bad ? <Text style={{ fontSize: 12, color: '#e5483f', textAlign: 'start' }}>{tr('machines.key.recoveryBad')}</Text> : null}
      <View style={{ flex: 1 }} />
      <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 8 }}>
        <Btn label={tr('machines.cancel')} onClick={() => { answer(id, false); wm.close(winId, true); }} />
        <Btn primary label={tr('machines.key.recoveryOk')} disabled={!ok} onClick={submit} />
      </View>
    </View>
  );
}

// Export… (6.4): the format, the snapshots, the place (the system's save dialog), then lord-vm --export with progress
export function ExportSheet({ m, running, onClose }) {
  const p = usePal();
  const [format, setFormat] = useState('lordvm');
  const [snaps, setSnaps] = useState(true);
  const [picking, setPicking] = useState(false);
  const [job, setJob] = useState(0);
  const [pct, setPct] = useState(null);
  const [fail, setFail] = useState('');
  const [done, setDone] = useState(null);
  const why = exportRefusal(m, format, { running });
  useEffect(() => {
    if (!job) return undefined;
    const timer = setInterval(() => {
      const r = lord.vm.job(job);
      if (!r) { setJob(0); return; }
      const g = jobProgress(r.out);
      if (g.pct != null) setPct(g.pct);
      if (!r.done) return;
      setJob(0);
      if (!r.ok || !g.last) { setFail(jobError(r.out) || tr('machines.exp.failed')); return; }
      setDone(g.last);
    }, 400);
    return () => clearInterval(timer);
  }, [job]);
  const go = async (path) => {
    const to = exportTarget(path, format);
    if (!to) return;
    setFail(''); setPct(0);
    try { await ensureKey(m.dir, { id: m.keyId, name: m.name }); } catch (e) { setPct(null); setFail(e.message); return; }
    const id = lord.vm.export(m.dir, { to, format, noSnapshots: !snaps });
    if (!id) { setPct(null); setFail(tr('machines.exp.failed')); return; }
    setJob(id);
  };
  const busy = !!job;
  return (
    <Sheet title={tr('machines.exp.title', { name: m.name })} onCancel={busy || picking ? undefined : onClose}
      over={picking ? <FilePicker mode="save" name={exportName(m, format)} title={tr('machines.exp.choose')} onClose={() => setPicking(false)}
        onPick={(x) => { setPicking(false); go(Array.isArray(x) ? x[0] : x); }} /> : null}
      actions={done ? [<Btn key="ok" primary label={tr('machines.exp.close')} onClick={onClose} />]
        : [<Btn key="c" label={tr('machines.cancel')} disabled={busy} onClick={onClose} />,
          <Btn key="go" primary label={tr('machines.exp.choose')} disabled={busy || !!why} onClick={() => setPicking(true)} />]}>
      <Segmented options={[['lordvm', tr('machines.exp.fmtLordvm')], ['ova', tr('machines.exp.fmtOva')]]} value={format} onChange={setFormat} />
      <Text style={{ fontSize: 13, lineHeight: 1.45, color: p.dim, textAlign: 'start' }}>
        {tr(format === 'ova' ? 'machines.exp.ovaNote' : m.encrypted ? 'machines.exp.lordvmEncNote' : 'machines.exp.lordvmNote', { name: m.name })}
      </Text>
      {format === 'lordvm' ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
          <Text style={{ flex: 1, fontSize: 13, color: p.text, textAlign: 'start' }}>{tr('machines.exp.snapshots')}</Text>
          <Toggle on={snaps} onChange={setSnaps} />
        </View>
      ) : null}
      {why ? <Text style={{ fontSize: 13, color: '#e5483f', textAlign: 'start' }}>{tr(`machines.exp.why.${why}`)}</Text> : null}
      {pct != null && !done ? <Text style={{ fontSize: 13, color: p.text, textAlign: 'start' }}>{tr('machines.exp.progress', { pct: Math.round(pct) })}</Text> : null}
      {done ? <Text style={{ fontSize: 13, color: p.text, textAlign: 'start' }}>{tr('machines.exp.done', { file: String(done.to || '').split('/').pop() })}</Text> : null}
      {fail ? <Text style={{ fontSize: 13, color: '#e5483f', textAlign: 'start' }}>{fail}</Text> : null}
    </Sheet>
  );
}

// Bring in a machine exported from LordOS (6.4): the folder chosen, its manifest read, the key asked for when it's
// encrypted (this computer's keyring, else its recovery key), then lord-vm --import-lordvm checks every file and copies it
export function ImportExportSheet({ onClose, onDone }) {
  const p = usePal();
  const [picking, setPicking] = useState(true);
  const [info, setInfo] = useState(null);
  const [job, setJob] = useState(0);
  const [pct, setPct] = useState(null);
  const [fail, setFail] = useState('');
  useEffect(() => {
    if (!job) return undefined;
    const timer = setInterval(() => {
      const r = lord.vm.job(job);
      if (!r) { setJob(0); return; }
      const g = jobProgress(r.out);
      if (g.pct != null) setPct(g.pct);
      if (!r.done) return;
      setJob(0);
      if (!r.ok || !g.last) { setFail(jobError(r.out) || tr('machines.imp.failed')); return; }
      onDone(g.last);
    }, 400);
    return () => clearInterval(timer);
  }, [job]);
  const chosen = async (path) => {
    const x = lord.vm.exportInfo(path);
    if (!x) { setFail(tr('machines.imp.notExport')); return; }
    setInfo({ ...x, path });
    setFail('');
    try { if (x.encrypted) await ensureKey(x.id, { id: x.id, name: x.name }); } catch (e) { setFail(e.message); return; }
    const id = lord.vm.importExport(path);
    if (!id) { setFail(tr('machines.imp.failed')); return; }
    setPct(0);
    setJob(id);
  };
  return (
    <Sheet title={tr('machines.imp.title')} onCancel={job || picking ? undefined : onClose}
      over={picking ? <FilePicker mode="folder" title={tr('machines.imp.pick')} onClose={() => { setPicking(false); if (!info) onClose(); }}
        onPick={(x) => { setPicking(false); chosen(Array.isArray(x) ? x[0] : x); }} /> : null}
      actions={[<Btn key="c" label={tr('machines.cancel')} disabled={!!job} onClick={onClose} />]}>
      <Text style={{ fontSize: 13, lineHeight: 1.45, color: p.dim, textAlign: 'start' }}>
        {info ? tr(info.encrypted ? 'machines.imp.encrypted' : 'machines.imp.plain', { name: info.name }) : tr('machines.imp.text')}
      </Text>
      {pct != null ? <Text style={{ fontSize: 13, color: p.text, textAlign: 'start' }}>{tr('machines.imp.progress', { pct: Math.round(pct) })}</Text> : null}
      {fail ? <Text style={{ fontSize: 13, color: '#e5483f', textAlign: 'start' }}>{fail}</Text> : null}
    </Sheet>
  );
}
