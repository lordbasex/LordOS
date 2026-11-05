// Storage (the approved mock "Storage", 6.16): one question, what would really be freed. A bar for the volume
// (machines, disc images, the Trash, other files, free), the machines with each clone under the one it came from, what
// each takes and what deleting it would give back (its files less what it shares by reflink, plus its shared base when
// it's the last machine on it, the Trash counted), and for the selected one the split and the ways to reclaim space:
// compaction (with an estimate first), its snapshots, the Trash.
import { View, Text, ScrollView, ConfirmDialog, useAppWindow, wm, fs, t as tr } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { storageRows, storageBar, stateView } from './model.js';
import { usePal, has, bytes, Btn, Sheet } from './ui.jsx';
import { readMeta } from './meta.js';
import { openSnapshots } from './library.jsx';

const errOf = (out) => (out || '').trim().split('\n').filter((l) => l.startsWith('lord-vm:')).pop()?.replace(/^lord-vm:\s*/, '') || (out || '').trim();

export function Storage() {
  const p = usePal();
  const { id: winId } = useAppWindow();
  const [st, setSt] = useState(null);
  const [list, setList] = useState([]);
  const [sel, setSel] = useState(null);
  const [est, setEst] = useState(null);         // { job } while measuring, then { allocated, required, possible, why }
  const [busy, setBusy] = useState(null);       // { job, what }
  const [msg, setMsg] = useState(null);
  const [del, setDel] = useState(null);
  const load = () => {
    if (!has()) return;
    const s = lord.vm.storage();
    const ms = lord.vm.list();
    for (const m of s.machines) {
      const x = ms.find((y) => y.dir === m.dir);
      m.state = x?.state; m.reason = x?.reason;
      m.meta = readMeta(m.dir);
    }
    setSt(s); setList(ms);
  };
  useEffect(() => { wm.setTitle(winId, tr('machines.storage.title')); load(); const timer = setInterval(load, 5000); return () => clearInterval(timer); }, []);
  // lord-vm jobs: the estimate, then the compaction
  useEffect(() => {
    const j = est?.job || busy?.job;
    if (!j) return undefined;
    const timer = setInterval(() => {
      const r = lord.vm.job(j);
      if (!r || !r.done) return;
      if (est?.job) {
        let out = null;
        try { out = JSON.parse(r.out.trim().split('\n').pop()); } catch (e) { out = null; }
        setEst(out && out.required >= 0 ? out : null);
        if (!out) setMsg({ ok: false, text: errOf(r.out) });
        return;
      }
      setBusy(null); load();
      if (!r.ok) { setMsg({ ok: false, text: errOf(r.out) }); return; }
      let out = {};
      try { out = JSON.parse(r.out.trim().split('\n').pop()); } catch (e) { out = {}; }
      setMsg({ ok: true, text: tr('machines.storage.compacted', { size: bytes(Math.max(0, (out.before || 0) - (out.after || 0))) }) });
    }, 500);
    return () => clearInterval(timer);
  }, [est?.job, busy?.job]);

  if (!st) return <View style={{ flex: 1, backgroundColor: p.bg }} />;
  const rows = storageRows(st);
  const bar = storageBar(st);
  const cur = rows.find((r) => r.key === sel) || null;
  const m = cur?.machine;
  const on = m && stateView(m.state, m.reason).active;
  const colors = { machines: '#0a84ff', isos: '#ffb340', trash: '#ff6b6b', other: '#8a8a94' };
  const trash = (x) => {
    setDel(null);
    try { fs.trash(x.dir); setSel(null); setMsg({ ok: true, text: tr('machines.storage.trashed', { name: x.name }) }); } catch (e) { setMsg({ ok: false, text: tr('machines.deleteFailed', { name: x.name }) }); }
    load();
  };
  return (
    <View style={{ flex: 1, backgroundColor: p.bg }}>
      <View style={{ paddingHorizontal: 20, paddingVertical: 16, gap: 10, borderBottomWidth: 1, borderColor: p.line }}>
        <View style={{ flexDirection: 'row', alignItems: 'center' }}>
          <Text style={{ flex: 1, fontSize: 15, fontWeight: 600, color: p.text, textAlign: 'start' }}>{tr('machines.storage.title')}</Text>
          <Text style={{ fontSize: 12, color: p.dim }}>{tr('machines.storage.freeOf', { free: bytes(st.free || 0), total: bytes(st.total || 0) })}</Text>
        </View>
        <View style={{ flexDirection: 'row', height: 16, borderRadius: 4, overflow: 'hidden', backgroundColor: p.track }}>
          {bar.map((b) => <View key={b.key} style={{ width: `${Math.round(b.frac * 1000) / 10}%`, height: 16, backgroundColor: colors[b.key] }} />)}
        </View>
        <View style={{ flexDirection: 'row', gap: 18 }}>
          {bar.map((b) => (
            <View key={b.key} style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
              <View style={{ width: 10, height: 10, borderRadius: 2, backgroundColor: colors[b.key] }} />
              <Text style={{ fontSize: 12, color: p.text }}>{`${tr(`machines.storage.cat.${b.key}`)} ${bytes(b.bytes)}`}</Text>
            </View>
          ))}
        </View>
      </View>
      <View style={{ flex: 1, minHeight: 0, flexDirection: 'row' }}>
        <View style={{ flex: 1, minWidth: 0, paddingHorizontal: 20, paddingVertical: 12 }}>
          <View style={{ flexDirection: 'row', gap: 8, paddingHorizontal: 10, paddingVertical: 6 }}>
            <Text style={{ flex: 1, fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'start' }}>{tr('machines.storage.machine')}</Text>
            <Text style={{ width: 90, fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'end' }}>{tr('machines.storage.size')}</Text>
            <Text style={{ width: 110, fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'end' }}>{tr('machines.storage.frees')}</Text>
          </View>
          <ScrollView style={{ flex: 1 }}>
            {rows.map((r) => {
              const lit = sel === r.key;
              return (
                <View key={r.key} onClick={() => { setSel(r.key); setEst(null); setMsg(null); }} style={{ height: 34, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 10, borderRadius: 6, backgroundColor: lit ? '#0a84ff' : 'transparent' }}>
                  <Text style={{ flex: 1, paddingLeft: r.level * 18, fontSize: 13, color: lit ? '#fff' : r.base ? p.dim : p.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>
                    {r.base ? tr('machines.storage.base', { n: r.users }) : r.name}</Text>
                  <Text style={{ width: 90, fontSize: 13, color: lit ? '#fff' : p.text, textAlign: 'end' }}>{bytes(r.size)}</Text>
                  <Text style={{ width: 110, fontSize: 13, fontWeight: 600, color: lit ? '#fff' : p.text, textAlign: 'end' }}>{r.base ? (r.users ? '—' : bytes(r.size)) : bytes(r.frees)}</Text>
                </View>
              );
            })}
            {!rows.length ? <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start', padding: 10 }}>{tr('machines.empty')}</Text> : null}
          </ScrollView>
        </View>
        <View style={{ width: 300, padding: 16, gap: 10, backgroundColor: p.raised }}>
          {cur && m ? <>
            <Text style={{ fontSize: 14, fontWeight: 600, color: p.text, textAlign: 'start' }}>{m.name}</Text>
            <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>
              {m.base ? tr('machines.storage.aboutLinked', { size: bytes(cur.baseBytes), n: cur.baseUsers, frees: bytes(m.frees) })
                : m.shared > 0 ? tr('machines.storage.aboutShared', { size: bytes(m.shared), frees: bytes(m.frees) }) : tr('machines.storage.aboutOwn', { frees: bytes(m.frees) })}</Text>
            {[['disks', m.disks], ['state', m.savedBytes], ['snapshots', m.snapshots], ['other', m.other]].map(([k, v]) => (
              <View key={k} style={{ flexDirection: 'row' }}>
                <Text style={{ flex: 1, fontSize: 12, color: p.text, textAlign: 'start' }}>{tr(`machines.storage.split.${k}`)}</Text>
                <Text style={{ fontSize: 12, fontWeight: 600, color: p.text }}>{(k === 'snapshots' && v > 0 ? `${tr('machines.storage.about')} ` : '') + bytes(v)}</Text>
              </View>
            ))}
            <View style={{ flex: 1 }} />
            {msg ? <Text style={{ fontSize: 12, color: msg.ok ? p.dim : '#ff453a', textAlign: 'start' }}>{msg.text}</Text> : null}
            {m.snapshotCount ? <Btn label={tr('machines.storage.snapshots', { n: m.snapshotCount })} onClick={() => openSnapshots(m)} /> : null}
            <Btn label={busy ? tr('machines.storage.compacting') : tr('machines.storage.reclaim')} disabled={!!busy || !!est?.job || on}
              onClick={() => { setMsg(null); const j = lord.vm.compact(m.dir, true); if (j) setEst({ job: j }); else setMsg({ ok: false, text: tr('machines.cfg.refused') }); }} />
            <Btn danger label={tr('machines.storage.trash', { size: bytes(m.frees) })} disabled={on || !!busy} onClick={() => setDel(m)} />
            {on ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.storage.whenOff')}</Text> : null}
          </> : cur && cur.base ? <>
            <Text style={{ fontSize: 14, fontWeight: 600, color: p.text, textAlign: 'start' }}>{tr('machines.storage.base', { n: cur.users })}</Text>
            <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{cur.users || cur.trash ? tr('machines.storage.baseUsed', { names: cur.names.join(', ') || '—', trash: cur.trash }) : tr('machines.storage.baseUnused')}</Text>
            <View style={{ flex: 1 }} />
            {!cur.users && !cur.trash ? <Btn danger label={tr('machines.storage.removeBase', { size: bytes(cur.size) })}
              onClick={() => { setMsg(lord.vm.removeBase(cur.path) ? null : { ok: false, text: tr('machines.cfg.refused') }); setSel(null); load(); }} /> : null}
          </> : <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr('machines.storage.pick')}</Text>}
        </View>
      </View>
      {est && !est.job ? (
        <Sheet title={tr('machines.storage.reclaimTitle', { name: m?.name || '' })} onCancel={() => setEst(null)} actions={[
          <Btn key="c" label={tr('machines.cancel')} onClick={() => setEst(null)} />,
          <Btn key="r" primary label={tr('machines.storage.reclaim')} disabled={!est.possible || est.allocated - est.required <= 0} onClick={() => {
            const j = lord.vm.compact(m.dir, false);
            setEst(null);
            if (j) setBusy({ job: j, what: 'compact' }); else setMsg({ ok: false, text: tr('machines.cfg.refused') });
          }} />,
        ]}>
          <Text style={{ fontSize: 13, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{est.possible
            ? tr('machines.storage.estimate', { frees: bytes(Math.max(0, est.allocated - est.required)), now: bytes(est.allocated) })
            : tr('machines.storage.cant', { why: est.why })}</Text>
        </Sheet>
      ) : null}
      {del ? <ConfirmDialog title={tr('machines.deleteTitle', { name: del.name })} detail={tr('machines.storage.trashDetail', { size: bytes(del.frees) })} okLabel={tr('machines.deleteOk')}
        cancelLabel={tr('machines.cancel')} destructive onOk={() => trash(del)} onCancel={() => setDel(null)} /> : null}
    </View>
  );
}
