// Storage (the approved mock "Storage", 6.16): one question, what would really be freed. A bar for the volume
// (machines, disc images, the Trash, other files, free), the machines with each clone under the one it came from, what
// each takes and what deleting it would give back (its files less what it shares by reflink, plus its shared base when
// it's the last machine on it, the Trash counted), and for the selected one the split and the ways to reclaim space:
// compaction (with an estimate first, and compression if wanted), its snapshots and deleting the old ones, the Trash.
// Figures read from compressed extents (btrfs with compression) are shown as "about". Under the machines, the systems the assistant downloaded
// (the mock's "Downloads" in the bar; 3.9, "The Linux download cards"), shared by every account: each with its size and
// the machines of this account whose CD drive has it; Remove (vm-image-remove, an administrator's action) asks first and
// is refused while a running machine has it in its drive.
// The look (2026-11-20, the maintainer): hypervisaur's "See where your space goes" (kit.jsx): the volume on a card with
// its bar, each machine a row with its system's mark, who it shares disk data with and a small bar of its size, and for
// the one chosen three tiles (takes up, frees if deleted, shared) and what it's made of.
import { View, Text, ScrollView, ConfirmDialog, Toggle, useAppWindow, wm, fs, admin, useNarrow, t as tr } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { storageRows, storageBar, stateView, systemOf, oldSnapshots, unusedImages } from './model.js';
import { usePal, has, bytes, Btn, Sheet, OsMark } from './ui.jsx';
import { Sym, Round, Pill } from './kit.jsx';
import { readMeta } from './meta.js';
import CATALOG from '../../assets/machines/systems.json';
import { downloadedImages, IMAGES_DIR } from './catalog.js';

const readInfo = (sha) => JSON.parse(fs.read(`${IMAGES_DIR}/${sha}.json`));
function listImages(ms) {
  let entries = [];
  try { entries = lord.fs.list(IMAGES_DIR); } catch (e) { entries = []; }     // none downloaded yet: no folder
  let gs = [];
  try { gs = typeof lord.vm.groups === 'function' ? lord.vm.groups() || [] : []; } catch (e) { gs = []; }
  return downloadedImages(CATALOG, entries, readInfo, ms, (m) => stateView(m.state, m.reason).active, gs);
}
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
  const [imgs, setImgs] = useState([]);
  const [rm, setRm] = useState(null);           // the image whose removal is being confirmed
  const [removing, setRemoving] = useState(false);
  const [zip, setZip] = useState(false);        // Reclaim space compresses the disk too
  const [old, setOld] = useState(null);         // { m, list, size }: the old snapshots asked about
  const [rmAll, setRmAll] = useState(null);     // the unused downloads asked about
  const narrow = useNarrow();       // a small window: the details under the list
  const load = () => {
    if (!has()) return;
    const s = lord.vm.storage();
    const ms = lord.vm.list();
    for (const m of s.machines) {
      const x = ms.find((y) => y.dir === m.dir);
      m.runState = x?.state; m.runReason = x?.reason;
      m.meta = readMeta(m.dir);
    }
    setSt(s); setList(ms); setImgs(listImages(ms));
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
      if (busy?.what === 'snaps') {
        // the old snapshots, one lord-vm job each, in order; a refusal stops the rest
        if (!r.ok) { setBusy(null); load(); setMsg({ ok: false, text: errOf(r.out) }); return; }
        const [next, ...rest] = busy.left;
        if (next) { const job = lord.vm.snapDelete(busy.dir, next); if (job) { setBusy({ ...busy, job, left: rest }); return; } }
        setBusy(null); load();
        setMsg(next ? { ok: false, text: tr('machines.cfg.refused') } : { ok: true, text: tr('machines.storage.oldDone', { n: busy.n }) });
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
  const bar = storageBar(st, imgs);
  const cur = rows.find((r) => r.key === sel) || null;
  const img = imgs.find((x) => `img:${x.sha}` === sel) || null;
  const imgName = (x) => (x.label ? `${x.label}${x.arch ? `, ${x.arch}` : ''}` : tr('machines.storage.dlUnknown'));
  const removeImage = async (x) => {
    setRm(null); setRemoving(true); setMsg(null);
    try {
      await admin.run('vm-image-remove', x.sha, null, { quiet: true });
      for (const d of x.dirs) { try { lord.vm.eject(d); } catch (e) { /* its card says the disc is missing */ } }   // off: their drives emptied
      setSel(null); setMsg({ ok: true, text: tr('machines.storage.dlRemoved', { name: imgName(x) }) }); }
    catch (e) { setMsg({ ok: false, text: e.code === 'admin.unavailable' ? tr('machines.dl.noHelper') : e.message }); }
    setRemoving(false); load();
  };
  const m = cur?.machine;
  const on = m && stateView(m.runState, m.runReason).active;
  // a figure read from compressed extents is "about" (6.16)
  const sz = (x, v) => (x?.approx ? tr('machines.storage.aboutSize', { size: bytes(v) }) : bytes(v));
  const unused = unusedImages(imgs);
  const askOld = (x) => {
    const s = lord.vm.snaps(x.dir);
    const list = oldSnapshots(s?.list || []);
    if (list.length) setOld({ m: x, list, size: list.reduce((a, y) => a + (y.vmstate || 0), 0) });
  };
  const deleteOld = (o) => {
    setOld(null); setMsg(null);
    const [first, ...rest] = o.list.map((y) => y.tag);
    const job = first ? lord.vm.snapDelete(o.m.dir, first) : 0;
    if (job) setBusy({ job, what: 'snaps', dir: o.m.dir, left: rest, n: o.list.length }); else setMsg({ ok: false, text: tr('machines.cfg.refused') });
  };
  const removeUnused = async (list) => {
    setRmAll(null); setRemoving(true); setMsg(null);
    let n = 0;
    for (const x of list) {
      try { await admin.run('vm-image-remove', x.sha, null, { quiet: true }); n++; }
      catch (e) { setMsg({ ok: false, text: e.code === 'admin.unavailable' ? tr('machines.dl.noHelper') : e.message }); break; }
    }
    if (n === list.length) setMsg({ ok: true, text: tr('machines.storage.unusedDone', { n }) });
    setSel(null); setRemoving(false); load();
  };
  const colors = { machines: '#0a84ff', isos: '#5ac8a8', downloads: '#ffb340', trash: '#ff6b6b', other: '#8a8a94' };
  const trash = (x) => {
    setDel(null);
    try { fs.trash(x.dir); setSel(null); setMsg({ ok: true, text: tr('machines.storage.trashed', { name: x.name }) }); } catch (e) { setMsg({ ok: false, text: tr('machines.deleteFailed', { name: x.name }) }); }
    load();
  };
  // who a machine shares disk data with: the machines on the same frozen disk, or the copy it came from and its copies
  const machinesOf = rows.filter((r) => r.machine).map((r) => r.machine);
  const partners = (x) => {
    if (!x) return [];
    if (x.base) return machinesOf.filter((y) => y.base === x.base && y.dir !== x.dir);
    if (!(x.shared > 0)) return [];
    return machinesOf.filter((y) => y.dir !== x.dir && (y.id === x.meta?.cloneOf || y.meta?.cloneOf === x.id));
  };
  const osId = (x) => x?.meta?.os || systemOf(x?.meta?.osName, x?.name).id;
  const maxSize = Math.max(1, ...rows.map((r) => r.size || 0));
  const card = { borderRadius: 14, backgroundColor: p.dark ? 'rgba(255,255,255,0.05)' : '#ffffff', border: `1px solid ${p.dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.08)'}` };
  const sectionTitle = (text) => <Text style={{ fontSize: 12, fontWeight: 700, color: p.dim, textAlign: 'start', paddingHorizontal: 4, paddingTop: 6, paddingBottom: 6 }}>{text}</Text>;
  const miniBar = (frac, lit) => (
    <View style={{ height: 4, borderRadius: 2, marginTop: 5, backgroundColor: lit ? 'rgba(255,255,255,0.3)' : p.track }}>
      <View style={{ width: `${Math.max(2, Math.round(frac * 1000) / 10)}%`, height: 4, borderRadius: 2, backgroundColor: lit ? '#ffffff' : p.accent }} />
    </View>
  );
  const pick = (key) => { setSel(key); setEst(null); setMsg(null); };
  const tile = (label, value, color) => (
    <View style={{ flex: 1, minWidth: 0, padding: 10, gap: 2, borderRadius: 10, backgroundColor: p.dark ? 'rgba(255,255,255,0.06)' : 'rgba(0,0,0,0.04)' }}>
      <Text style={{ fontSize: 11, color: p.dim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{label}</Text>
      <Text style={{ fontSize: 16, fontWeight: 700, color: color || p.text, textAlign: 'start', whiteSpace: 'nowrap' }}>{value}</Text>
    </View>
  );
  const part = (label, v, total) => (
    <View key={label} style={{ gap: 4 }}>
      <View style={{ flexDirection: 'row' }}>
        <Text style={{ flex: 1, fontSize: 13, color: p.text, textAlign: 'start' }}>{label}</Text>
        <Text style={{ fontSize: 13, color: p.dim }}>{bytes(v)}</Text>
      </View>
      <View style={{ height: 4, borderRadius: 2, backgroundColor: p.track }}>
        <View style={{ width: `${total > 0 ? Math.min(100, Math.max(v > 0 ? 1 : 0, Math.round((v / total) * 1000) / 10)) : 0}%`, height: 4, borderRadius: 2, backgroundColor: p.accent }} />
      </View>
    </View>
  );
  const msgLine = msg ? <Text style={{ fontSize: 12, color: msg.ok ? p.dim : '#ff453a', textAlign: 'start' }}>{msg.text}</Text> : null;
  const shareNames = (list) => list.map((y) => y.name).join(', ');
  const mates = partners(m);
  return (
    <View style={{ flex: 1, backgroundColor: p.bg }}>
      <View style={{ paddingHorizontal: 20, paddingTop: 16, paddingBottom: 14, gap: 10, borderBottomWidth: 1, borderColor: p.line }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
          <Sym name="storage" size={20} color={p.text} />
          <Text style={{ flex: 1, fontSize: 15, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.storage.disk')}</Text>
          <Text style={{ fontSize: 13, color: p.dim }}>{tr('machines.storage.freeOf', { free: bytes(st.free || 0), total: bytes(st.total || 0) })}</Text>
          <Round title={tr('machines.storage.refresh')} onClick={load}><Sym name="refresh" size={16} color={p.text} /></Round>
        </View>
        <View style={{ flexDirection: 'row', height: 14, borderRadius: 7, overflow: 'hidden', backgroundColor: p.track }}>
          {bar.map((b) => <View key={b.key} style={{ width: `${Math.round(b.frac * 1000) / 10}%`, height: 14, backgroundColor: b.key === 'machines' ? p.accent : colors[b.key] }} />)}
        </View>
        <View style={{ flexDirection: 'row', gap: 16, flexWrap: 'wrap' }}>
          {bar.map((b) => (
            <View key={b.key} style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
              <View style={{ width: 9, height: 9, borderRadius: 3, backgroundColor: b.key === 'machines' ? p.accent : colors[b.key] }} />
              <Text style={{ fontSize: 12, color: p.text }}>{tr(`machines.storage.cat.${b.key}`)}</Text>
              <Text style={{ fontSize: 12, color: p.dim }}>{bytes(b.bytes)}</Text>
            </View>
          ))}
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
            <View style={{ width: 9, height: 9, borderRadius: 3, border: `1px solid ${p.dim}` }} />
            <Text style={{ fontSize: 12, color: p.text }}>{tr('machines.storage.available')}</Text>
            <Text style={{ fontSize: 12, color: p.dim }}>{bytes(st.free || 0)}</Text>
          </View>
        </View>
        <Text style={{ fontSize: 11, lineHeight: 1.45, color: p.dim, textAlign: 'start' }}>{tr('machines.storage.note')}</Text>
      </View>
      <View style={{ flex: 1, minHeight: 0, flexDirection: narrow ? 'column' : 'row' }}>
        <ScrollView style={{ flex: 1, minWidth: 0, minHeight: 0 }}>
          <View style={{ paddingHorizontal: narrow ? 12 : 16, paddingVertical: 10, gap: 4 }}>
            {sectionTitle(tr('machines.storage.cat.machines'))}
            {rows.map((r) => {
              const lit = sel === r.key;
              const x = r.machine;
              const mates = partners(x);
              const sub = r.base ? (r.names.join(', ') || tr('machines.storage.baseUnused'))
                : mates.length ? tr('machines.storage.sharesWith', { names: shareNames(mates) }) : (x?.meta?.osName || '');
              return (
                <View key={r.key} onClick={() => pick(r.key)} style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 10, paddingVertical: 8,
                  marginLeft: r.level * 22, borderRadius: 10, backgroundColor: lit ? p.accent : 'transparent' }}>
                  {r.level ? <Text style={{ fontSize: 13, color: lit ? '#fff' : p.dim }}>↳</Text> : null}
                  {r.base ? <View style={{ width: 30, height: 30, borderRadius: 7, alignItems: 'center', justifyContent: 'center', backgroundColor: p.dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.06)' }}>
                    <Sym name="disks" size={18} color={lit ? '#ffffff' : p.text} /></View> : <OsMark id={osId(x)} size={30} />}
                  <View style={{ flex: 1, minWidth: 0 }}>
                    <Text style={{ fontSize: 13, fontWeight: 600, color: lit ? '#fff' : p.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>
                      {r.base ? tr('machines.storage.base', { n: r.users }) : r.name}</Text>
                    {sub ? <Text style={{ fontSize: 11, color: lit ? 'rgba(255,255,255,0.82)' : p.dim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{sub}</Text> : null}
                    {miniBar((r.size || 0) / maxSize, lit)}
                  </View>
                  <View style={{ width: 100, alignItems: 'flex-end' }}>
                    <Text style={{ fontSize: 13, fontWeight: 700, color: lit ? '#fff' : p.text, textAlign: 'end' }}>{bytes(r.size)}</Text>
                    <Text style={{ fontSize: 11, color: lit ? 'rgba(255,255,255,0.82)' : p.dim, textAlign: 'end' }}>
                      {r.base ? (r.users ? '—' : tr('machines.storage.freesShort', { size: bytes(r.size) })) : tr('machines.storage.freesShort', { size: sz(x, r.frees) })}</Text>
                  </View>
                </View>
              );
            })}
            {!rows.length ? <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start', padding: 10 }}>{tr('machines.empty')}</Text> : null}
            {imgs.length ? <>
              <View style={{ height: 10 }} />
              {sectionTitle(tr('machines.storage.downloads'))}
              {imgs.map((x) => {
                const key = `img:${x.sha}`;
                const lit = sel === key;
                return (
                  <View key={key} onClick={() => pick(key)} style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 10, paddingVertical: 7, borderRadius: 10,
                    backgroundColor: lit ? p.accent : 'transparent', borderTopWidth: 0 }}>
                    <Sym name="download" size={18} color={lit ? '#ffffff' : p.dim} />
                    <Text style={{ flex: 1, fontSize: 13, color: lit ? '#fff' : p.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>
                      {x.partial ? tr('machines.storage.dlPartialRow', { name: imgName(x) }) : imgName(x)}</Text>
                    <Text style={{ fontSize: 13, color: lit ? '#fff' : p.dim, textAlign: 'end' }}>{bytes(x.size)}</Text>
                    <Round size={26} title={tr('machines.storage.dlRemoveOk')} onClick={(e) => { e?.stopPropagation?.(); if (!removing && !x.running.length) setRm(x); }}>
                      <Sym name="trash" size={14} color={lit ? '#ffffff' : p.dim} /></Round>
                  </View>
                );
              })}
              <Text style={{ fontSize: 11, lineHeight: 1.45, color: p.dim, textAlign: 'start', paddingHorizontal: 4, paddingTop: 6 }}>{tr('machines.storage.dlShared')}</Text>
              {unused.length ? <View style={{ flexDirection: 'row', paddingHorizontal: 4, paddingTop: 8 }}>
                <Pill icon="trash" label={removing ? tr('machines.storage.dlRemoving') : tr('machines.storage.unused', { size: bytes(unused.reduce((a, x) => a + (x.size || 0), 0)) })}
                  disabled={removing} onClick={() => setRmAll(unused)} /></View> : null}
            </> : null}
          </View>
        </ScrollView>
        <View style={{ width: narrow ? undefined : 340, padding: 18, gap: 12, borderLeftWidth: narrow ? 0 : 1, borderTopWidth: narrow ? 1 : 0, borderColor: p.line,
          backgroundColor: p.dark ? 'rgba(255,255,255,0.02)' : 'rgba(0,0,0,0.015)' }}>
          {cur && m ? <>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12 }}>
              <OsMark id={osId(m)} size={40} />
              <Text style={{ flex: 1, fontSize: 16, fontWeight: 700, color: p.text, textAlign: 'start' }}>{m.name}</Text>
            </View>
            <View style={{ flexDirection: 'row', gap: 8 }}>
              {tile(tr('machines.storage.takes'), bytes(cur.size))}
              {tile(tr('machines.storage.freesIf'), sz(m, m.frees), p.accent)}
              {tile(tr('machines.storage.sharedTile'), m.base ? bytes(cur.baseBytes) : sz(m, m.shared || 0))}
            </View>
            <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>
              {m.base ? tr('machines.storage.aboutLinked', { size: bytes(cur.baseBytes), n: cur.baseUsers, frees: bytes(m.frees) })
                : m.shared > 0 ? tr('machines.storage.aboutShared', { size: bytes(m.shared), frees: bytes(m.frees) }) : tr('machines.storage.aboutOwn', { frees: bytes(m.frees) })}</Text>
            {mates.length ? <View style={{ gap: 6 }}>
              <Text style={{ fontSize: 12, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.storage.sharesTitle')}</Text>
              {mates.map((y) => (
                <View key={y.dir} style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
                  <OsMark id={osId(y)} size={20} />
                  <Text style={{ flex: 1, fontSize: 13, color: p.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{y.name}</Text>
                </View>
              ))}
            </View> : null}
            <View style={{ ...card, padding: 12, gap: 10 }}>
              <Text style={{ fontSize: 12, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.storage.madeOf')}</Text>
              {[['disks', m.disks], ['state', m.savedBytes], ['snapshots', m.snapshots], ['other', m.other]].filter(([k, v]) => k === 'disks' || v > 0)
                .map(([k, v]) => part((k === 'snapshots' ? `${tr(`machines.storage.split.${k}`)} (${tr('machines.storage.about')})` : tr(`machines.storage.split.${k}`)), v, m.total || cur.size))}
            </View>
            {m.approx ? <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr('machines.storage.approxNote')}</Text> : null}
            <View style={{ flex: 1 }} />
            {msgLine}
            {busy?.what === 'snaps' ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.storage.oldBusy', { n: busy.left.length + 1 })}</Text> : null}
            {on ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.storage.whenOff')}</Text> : null}
            <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8 }}>
              {m.snapshotCount ? <Pill icon="snapshot" label={tr('machines.storage.snapshots', { n: m.snapshotCount })} onClick={() => openSnapshots(m)} /> : null}
              {m.snapshotCount > 1 ? <Pill icon="trash" label={tr('machines.storage.old')} disabled={!!busy} onClick={() => askOld(m)} /> : null}
              <Pill label={busy ? tr('machines.storage.compacting') : tr('machines.storage.reclaim')} disabled={!!busy || !!est?.job || on}
                onClick={() => { setMsg(null); const j = lord.vm.compact(m.dir, true); if (j) setEst({ job: j }); else setMsg({ ok: false, text: tr('machines.cfg.refused') }); }} />
              <Pill danger icon="trash" label={tr('machines.storage.trash', { size: bytes(m.frees) })} disabled={on || !!busy} onClick={() => setDel(m)} />
            </View>
          </> : cur && cur.base ? <>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12 }}>
              <Sym name="disks" size={32} color={p.text} />
              <Text style={{ flex: 1, fontSize: 16, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.storage.base', { n: cur.users })}</Text>
            </View>
            <View style={{ flexDirection: 'row', gap: 8 }}>
              {tile(tr('machines.storage.takes'), bytes(cur.size))}
              {tile(tr('machines.storage.freesIf'), cur.users || cur.trash ? '—' : bytes(cur.size), p.accent)}
            </View>
            <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{cur.users || cur.trash ? tr('machines.storage.baseUsed', { names: cur.names.join(', ') || '—', trash: cur.trash }) : tr('machines.storage.baseUnused')}</Text>
            <View style={{ flex: 1 }} />
            {msgLine}
            {!cur.users && !cur.trash ? <View style={{ flexDirection: 'row' }}><Pill danger icon="trash" label={tr('machines.storage.removeBase', { size: bytes(cur.size) })}
              onClick={() => { setMsg(lord.vm.removeBase(cur.path) ? null : { ok: false, text: tr('machines.cfg.refused') }); setSel(null); load(); }} /></View> : null}
          </> : img ? <>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12 }}>
              <OsMark id={img.id} size={40} />
              <Text style={{ flex: 1, fontSize: 16, fontWeight: 700, color: p.text, textAlign: 'start' }}>{imgName(img)}</Text>
            </View>
            <View style={{ flexDirection: 'row', gap: 8 }}>
              {tile(tr('machines.storage.takes'), bytes(img.size))}
              {tile(tr('machines.storage.freesIf'), bytes(img.size), p.accent)}
            </View>
            {img.file ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start', direction: 'ltr', whiteSpace: 'nowrap', overflow: 'hidden' }}>{img.file}</Text> : null}
            <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>
              {img.partial ? tr('machines.storage.dlPartial', { size: bytes(img.size) })
                : img.archive ? tr(img.users.length ? (img.basesThere ? 'machines.group.storage.archiveDone' : 'machines.group.storage.archiveNeeded') : 'machines.group.storage.archiveUnused', { names: img.users.join(', ') })
                : img.users.length ? tr('machines.storage.dlUsed', { names: img.users.join(', ') }) : tr('machines.storage.dlUnused')}</Text>
            <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr('machines.storage.dlShared')}</Text>
            <View style={{ flex: 1 }} />
            {msgLine}
            {img.running.length ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.storage.dlRunning', { names: img.running.join(', ') })}</Text> : null}
            <View style={{ flexDirection: 'row' }}>
              <Pill danger icon="trash" label={removing ? tr('machines.storage.dlRemoving') : tr('machines.storage.dlRemove', { size: bytes(img.size) })} disabled={removing || img.running.length > 0} onClick={() => setRm(img)} />
            </View>
          </> : <>
            <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: 12 }}>
              <Sym name="storage" size={40} color={p.dim} />
              <Text style={{ fontSize: 13, lineHeight: 1.5, color: p.dim, textAlign: 'center', maxWidth: 260 }}>{tr('machines.storage.pick')}</Text>
            </View>
            {msgLine}
          </>}
        </View>
      </View>
      {est && !est.job ? (
        <Sheet title={tr('machines.storage.reclaimTitle', { name: m?.name || '' })} onCancel={() => setEst(null)} actions={[
          <Btn key="c" label={tr('machines.cancel')} onClick={() => setEst(null)} />,
          <Btn key="r" primary label={tr('machines.storage.reclaim')} disabled={!est.possible || (est.allocated - est.required <= 0 && !zip)} onClick={() => {
            const j = lord.vm.compact(m.dir, false, zip && !m.encrypted);
            setEst(null);
            if (j) setBusy({ job: j, what: 'compact' }); else setMsg({ ok: false, text: tr('machines.cfg.refused') });
          }} />,
        ]}>
          <Text style={{ fontSize: 13, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{est.possible
            ? tr('machines.storage.estimate', { frees: bytes(Math.max(0, est.allocated - est.required)), now: bytes(est.allocated) })
            : tr('machines.storage.cant', { why: est.why })}</Text>
          {est.possible && !m?.encrypted ? <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
            <Toggle on={zip} onChange={setZip} />
            <View style={{ flex: 1 }}>
              <Text style={{ fontSize: 13, color: p.text, textAlign: 'start' }}>{tr('machines.storage.compress')}</Text>
              <Text style={{ fontSize: 12, lineHeight: 1.4, color: p.dim, textAlign: 'start' }}>{tr('machines.storage.compressHint')}</Text>
            </View>
          </View> : null}
        </Sheet>
      ) : null}
      {del ? <ConfirmDialog title={tr('machines.deleteTitle', { name: del.name })} detail={tr('machines.storage.trashDetail', { size: bytes(del.frees) })} okLabel={tr('machines.deleteOk')}
        cancelLabel={tr('machines.cancel')} destructive onOk={() => trash(del)} onCancel={() => setDel(null)} /> : null}
      {old ? <ConfirmDialog title={tr('machines.storage.oldTitle', { name: old.m.name })}
        detail={tr('machines.storage.oldDetail', { n: old.list.length, size: bytes(old.size) })} okLabel={tr('machines.snap.delete')}
        cancelLabel={tr('machines.cancel')} destructive onOk={() => deleteOld(old)} onCancel={() => setOld(null)} /> : null}
      {rmAll ? <ConfirmDialog title={tr('machines.storage.unusedTitle')}
        detail={tr('machines.storage.unusedDetail', { n: rmAll.length, size: bytes(rmAll.reduce((a, x) => a + (x.size || 0), 0)) })} okLabel={tr('machines.storage.dlRemoveOk')}
        cancelLabel={tr('machines.cancel')} destructive onOk={() => removeUnused(rmAll)} onCancel={() => setRmAll(null)} /> : null}
      {rm ? <ConfirmDialog title={tr('machines.storage.dlRemoveTitle', { name: imgName(rm) })}
        detail={[rm.partial ? tr('machines.storage.dlRemovePartial', { size: bytes(rm.size) }) : rm.users.length ? tr('machines.storage.dlRemoveUsed', { size: bytes(rm.size), names: rm.users.join(', ') }) : tr('machines.storage.dlRemoveDetail', { size: bytes(rm.size) }), tr('machines.storage.dlRemoveOthers')].join(' ')}
        okLabel={tr('machines.storage.dlRemoveOk')} cancelLabel={tr('machines.cancel')} destructive onOk={() => removeImage(rm)} onCancel={() => setRm(null)} /> : null}
    </View>
  );
}
