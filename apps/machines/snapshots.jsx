// Snapshots (the approved mock "Snapshots"): a machine's timeline, each snapshot with its picture, name, when and what
// it holds, then Now; the selected one can be gone back to or deleted. lord-vm does the work (6.2): qcow2 internal
// snapshots of the system disk and the UEFI variables together, with the memory when the machine runs, under
// operation.json, the entry in snapshots.json written last. Going back to a snapshot with memory on a machine that's
// off starts it from there.
import { View, Text, Image, TextInput, ScrollView, Toggle, useAppWindow, wm, fs, formatDate, formatTime, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { stateView, snapName, snapSize, thumbSrc } from './model.js';
import { usePal, has, bytes, Btn, Sheet, markColors } from './ui.jsx';
import { readMeta } from './meta.js';
import { openMachine } from './library.jsx';
import { CloneSheet } from './clone.jsx';

const when = (iso) => { const d = new Date(iso); return Number.isNaN(d.getTime()) ? '' : `${formatDate(d)}, ${formatTime(d)}`; };
const errOf = (out) => (out || '').trim().split('\n').filter((l) => l.startsWith('lord-vm:')).pop()?.replace(/^lord-vm:\s*/, '') || (out || '').trim();

// the node on the timeline, over the line that joins one snapshot to the next
function Node({ on, last }) {
  return (
    <View style={{ height: 25, marginTop: 6 }}>
      <View style={{ position: 'absolute', left: 14, right: last ? undefined : -12, width: last ? 0 : undefined, top: 11, height: 3, backgroundColor: '#44444c' }} />
      <View style={{ position: 'absolute', left: 14, top: 6, width: 13, height: 13, borderRadius: 7, backgroundColor: on ? '#0a84ff' : '#8a8a94' }} />
    </View>
  );
}

function Pic({ src, colors, on }) {
  return (
    <View style={{ height: 130, borderRadius: 10, overflow: 'hidden', background: `linear-gradient(135deg, ${colors[0]}, ${colors[1]})`,
      border: `2px solid ${on ? '#0a84ff' : 'transparent'}` }}>
      {src ? <Image src={src} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, objectFit: 'cover' }} /> : null}
    </View>
  );
}

export function Snapshots({ dir, name: name0 }) {
  const p = usePal();
  const { id: winId } = useAppWindow();
  const [m, setM] = useState(null);
  const [list, setList] = useState([]);
  const [lim, setLim] = useState({ max: 64, warn: 20 });
  const [sel, setSel] = useState(null);
  const [taking, setTaking] = useState(null);       // the name being typed, or null
  const [goBack, setGoBack] = useState(null);       // the snapshot asked about
  const [first, setFirst] = useState(true);
  const [del, setDel] = useState(null);
  const [busy, setBusy] = useState(null);           // { job, what, then }
  const [msg, setMsg] = useState(null);
  const [cloning, setCloning] = useState(null);
  const thumb = useRef(0);
  const load = () => {
    if (!has()) return;
    const x = lord.vm.list().find((y) => y.dir === dir) || null;
    setM(x);
    const s = lord.vm.snaps(dir);
    if (s) { setList(s.list); setLim({ max: s.max, warn: s.warn }); }
    try { thumb.current = fs.stat(`${dir}/thumb.png`)?.mtime || 0; } catch (e) { thumb.current = 0; }
  };
  useEffect(() => { load(); const timer = setInterval(load, 2000); return () => clearInterval(timer); }, []);
  useEffect(() => { wm.setTitle(winId, tr('machines.snap.title', { name: m?.name || name0 })); }, [m?.name]);
  // the job lord-vm runs; `then` follows a success (a snapshot before going back, then going back)
  useEffect(() => {
    if (!busy) return undefined;
    const timer = setInterval(() => {
      const r = lord.vm.job(busy.job);
      if (!r || !r.done) return;
      const b = busy;
      setBusy(null);
      load();
      if (r.ok) { setMsg({ ok: true, text: tr(`machines.snap.done.${b.what}`) }); if (b.then) b.then(); return; }
      // a snapshot with memory, and the machine off: it starts from there instead
      if (b.what === 'revert' && r.exit === 4) { openMachine({ ...m, dir }, { loadvm: b.tag }); setMsg({ ok: true, text: tr('machines.snap.startingFrom') }); return; }
      setMsg({ ok: false, text: errOf(r.out) || tr('machines.cfg.refused') });
    }, 400);
    return () => clearInterval(timer);
  }, [busy]);

  if (!m) return <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', backgroundColor: p.bg }}><Text style={{ color: p.dim }}>{tr('machines.win.gone')}</Text></View>;
  const v = stateView(m.state, m.reason);
  const meta = readMeta(dir);
  const colors = markColors(meta.os || 'other');
  const total = list.reduce((a, s) => a + (s.vmstate || 0), 0);
  const cur = list.find((s) => s.tag === sel) || null;
  const run = (job, what, extra = {}) => { if (!job) { setMsg({ ok: false, text: tr('machines.cfg.refused') }); return false; } setMsg(null); setBusy({ job, what, ...extra }); return true; };
  const take = (name, then) => {
    const r = lord.vm.snapTake(dir, name);
    if (!r) { setMsg({ ok: false, text: tr('machines.cfg.refused') }); return; }
    run(r.job, 'take', { then });
  };
  const revert = (s) => run(lord.vm.snapRevert(dir, s.tag), 'revert', { tag: s.tag });
  const full = list.length >= lim.max;

  return (
    <View style={{ flex: 1, padding: 22, gap: 16, backgroundColor: p.bg }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
        <Text style={{ flex: 1, fontSize: 20, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.snap.title', { name: m.name })}</Text>
        <Text style={{ fontSize: 12, color: list.length >= lim.warn ? '#ffc56e' : p.dim }}>{tr('machines.snap.count', { n: list.length, warn: lim.warn, size: bytes(total) })}</Text>
        <Btn primary label={busy?.what === 'take' ? tr('machines.snap.taking') : tr('machines.snap.take')} disabled={!!busy || full || m.suspended}
          onClick={() => setTaking(snapName(list, tr('machines.snap.defaultName')))} />
      </View>
      <ScrollView style={{ maxHeight: 520 }} contentStyle={{ flexDirection: 'row', flexWrap: 'wrap', gap: 12, paddingBottom: 6 }}>
        {list.map((s) => (
          <View key={s.tag} onClick={() => setSel(s.tag)} style={{ width: 196, height: 250 }}>
            <Pic src={s.picture} colors={colors} on={sel === s.tag} />
            <Node on={sel === s.tag} />
            <Text style={{ marginTop: 4, fontSize: 13, fontWeight: 600, color: p.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{s.name}</Text>
            <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{when(s.created)}</Text>
            <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{snapSize(s) ? tr('machines.snap.withMemory', { size: bytes(s.vmstate) }) : tr('machines.snap.diskOnly')}</Text>
          </View>
        ))}
        <View style={{ width: 196, height: 250 }}>
          <Pic src={thumbSrc(dir, thumb.current)} colors={colors} on={false} />
          <Node on={false} last />
          <Text style={{ marginTop: 4, fontSize: 13, fontWeight: 600, color: p.text, textAlign: 'start' }}>{tr('machines.snap.now')}</Text>
          <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr(`machines.state.${v.key}`)}</Text>
          <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.snap.current')}</Text>
        </View>
      </ScrollView>
      {!list.length ? <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start' }}>{tr('machines.snap.none')}</Text> : null}
      {cur ? (
        <View style={{ padding: 14, borderRadius: 12, backgroundColor: p.card, flexDirection: 'row', alignItems: 'center', gap: 10 }}>
          <View style={{ flex: 1 }}>
            <Text style={{ fontSize: 14, fontWeight: 700, color: p.text, textAlign: 'start' }}>{cur.name}</Text>
            <Text style={{ marginTop: 2, fontSize: 13, color: p.dim, textAlign: 'start', lineHeight: 1.4 }}>{tr(cur.memory ? 'machines.snap.aboutMemory' : 'machines.snap.aboutDisk')}</Text>
          </View>
          <Btn label={tr('machines.snap.cloneHere')} disabled={!!busy || v.active} onClick={() => setCloning(cur)} />
          <Btn label={tr('machines.snap.delete')} disabled={!!busy} onClick={() => setDel(cur)} />
          <Btn primary label={tr('machines.snap.goBack')} disabled={!!busy || (!cur.memory && v.active)} onClick={() => { setFirst(!full); setGoBack(cur); }} />
        </View>
      ) : null}
      {cur && !cur.memory && v.active ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.snap.offOnly')}</Text> : null}
      <View style={{ flex: 1 }} />
      {msg ? <Text style={{ fontSize: 13, color: msg.ok ? p.dim : '#ff453a', textAlign: 'start' }}>{msg.text}</Text> : null}
      {busy ? <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start' }}>{tr(`machines.snap.busy.${busy.what}`)}</Text> : null}
      <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{list.length >= lim.warn ? tr('machines.snap.many', { n: lim.warn }) : tr('machines.snap.notBackup')}</Text>

      {taking !== null ? (
        <Sheet title={tr('machines.snap.take')} onCancel={() => setTaking(null)} actions={[
          <Btn key="c" label={tr('machines.cancel')} onClick={() => setTaking(null)} />,
          <Btn key="t" primary label={tr('machines.snap.takeOk')} disabled={!taking.trim()} onClick={() => { const n = taking.trim(); setTaking(null); take(n); }} />,
        ]}>
          <TextInput value={taking} onChange={setTaking} autoFocus />
          <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start', lineHeight: 1.4 }}>{tr(v.active ? 'machines.snap.takeRunning' : 'machines.snap.takeOff')}</Text>
        </Sheet>
      ) : null}
      {goBack ? (
        <Sheet title={tr('machines.snap.goBackTitle', { name: goBack.name })} onCancel={() => setGoBack(null)} actions={[
          <Btn key="c" label={tr('machines.cancel')} onClick={() => setGoBack(null)} />,
          <Btn key="g" primary label={tr('machines.snap.goBack')} onClick={() => {
            const s = goBack;
            setGoBack(null);
            if (first) take(tr('machines.snap.beforeGoingBack', { name: s.name }), () => revert(s)); else revert(s);
          }} />,
        ]}>
          <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start', lineHeight: 1.5 }}>{tr(goBack.memory ? 'machines.snap.goBackMemory' : 'machines.snap.goBackDisk')}</Text>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, opacity: full ? 0.45 : 1 }}>
            <Toggle on={first && !full} onChange={(x) => setFirst(x && !full)} />
            <Text style={{ fontSize: 13, color: p.text }}>{tr('machines.snap.takeFirst')}</Text>
          </View>
        </Sheet>
      ) : null}
      {cloning ? <CloneSheet m={m} fromSnapshot={cloning} running={v.active} onClose={() => setCloning(null)}
        onDone={() => { setCloning(null); setMsg({ ok: true, text: tr('machines.clone.done') }); }} /> : null}
      {del ? (
        <Sheet title={tr('machines.snap.deleteTitle', { name: del.name })} onCancel={() => setDel(null)} actions={[
          <Btn key="c" label={tr('machines.cancel')} onClick={() => setDel(null)} />,
          <Btn key="d" danger label={tr('machines.snap.delete')} onClick={() => { const s = del; setDel(null); if (run(lord.vm.snapDelete(dir, s.tag), 'delete')) setSel(null); }} />,
        ]}>
          <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start', lineHeight: 1.5 }}>{tr('machines.snap.deleteText')}</Text>
        </Sheet>
      ) : null}
    </View>
  );
}
