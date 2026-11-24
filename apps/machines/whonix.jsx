// whonix.jsx — the New Machine assistant for a machine group (docs/VIRTUALIZATION.md 19.8; the approved mockups,
// 2026-11-23): the card "Whonix (Tor)" with its "Group · 2 machines" badge is chosen on step 1; then
//   2 About Whonix: the diagram (Internet ↔ Gateway ↔ private link ↔ Workstation), what is locked off, the random group
//     identity, what it doesn't protect against, and the "Disposable session" switch;
//   3 the name; 4 review; 5 "Download and create": the verified archive (vm-image-get, SHA-512 from Whonix's signed
//     list), then lord.vm.groupCreate (lord-vm --group-create: lord-vmunpack, the bases, the two members, group.json),
//     then the group starts (the gateway first).
// resolveGroup finds the newest verified release, never older than the catalog's floor (the admin helper checks again).
import { View, Text, TextInput, Toggle, ScrollView, admin, fs, openUrl, t as tr } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { entryOf, releaseNames, newestRelease, listingNames, newest, pickImage, progressOf, resumedOf, groupMachines } from './catalog.js';
import { usePal, Btn, Bar, gb, bytes } from './ui.jsx';
import { SheetHeader, Pill, Round } from './kit.jsx';
import { sessionCapMb, archiveMatches } from './group.js';
import { GroupMark, groupColors } from './groupui.jsx';
import { OVERHEAD_MB } from './model.js';

// the newest release's archive: the releases' folder, that release's folder, its signed SHA-512 list → { name, sha, size,
// release }. With `only` (a group's own release, to fetch its archive again: 19.6), that release's folder alone
export async function resolveGroup(cat, id, arch, only = '') {
  const s = (cat?.systems || []).find((x) => x?.id === id);
  const e = entryOf(cat, id, arch);
  const rel = s?.arch?.[arch]?.releases;
  if (!e || !rel) throw new Error(tr('machines.dl.noList', { why: tr('machines.dl.noImage') }));
  let release = only;
  if (!release) {
    const top = await admin.run('vm-image-list', `${id} ${arch}`, null, { quiet: true });
    release = newestRelease(releaseNames(fs.read(String(top)), rel.pattern), rel.floor);
  }
  if (!release) throw new Error(tr('machines.dl.noList', { why: tr('machines.dl.noImage') }));
  const idx = await admin.run('vm-image-list', `${id} ${arch} ${release}`, null, { quiet: true });
  const list = newest(listingNames(fs.read(String(idx)), e.sumsPattern), e.sumsPattern);
  if (!list) throw new Error(tr('machines.dl.noList', { why: tr('machines.dl.noImage') }));
  const sums = await admin.run('vm-image-list', `${id} ${arch} ${release} ${list}`, null, { quiet: true });
  const img = pickImage(e, fs.read(String(sums)));
  if (!img) throw new Error(tr('machines.dl.noList', { why: tr('machines.dl.noImage') }));
  return { ...img, release };
}

// a group's verified download fetched again (Repair or Start Fresh making a base again, 19.6, 19.7): its own release's
// signed list must still name the archive group.json records (archiveSha512), or nothing is fetched; then vm-image-get as
// in the assistant. onProgress({ done, total }). Resolves the archive's path in the image store
export async function fetchGroupArchive(cat, group, onProgress) {
  const img = await resolveGroup(cat, group.catalog || 'whonix', 'amd64', group.release || '');
  if (!archiveMatches(img, group)) throw new Error(tr('machines.group.repair.otherImage'));
  return String(await admin.run('vm-image-get', `${group.catalog || 'whonix'} amd64 ${img.name} ${img.sha}`, (l) => {
    const g = progressOf(l);
    if (g) onProgress?.(g);
  }, { quiet: true }));
}

// lord-vm --group-create's progress lines ({"progress":{"step":"verify|unpack|base",…}}) → { step, frac }
export function groupProgress(out) {
  let r = null;
  for (const l of String(out || '').split('\n')) {
    try {
      const o = JSON.parse(l);
      if (o.progress) r = { step: o.progress.step, frac: o.progress.total ? Math.min(1, o.progress.done / o.progress.total) : r?.frac || 0, role: o.progress.role || '' };
      if (o.created) r = { step: 'done', frac: 1, dir: o.dir };
    } catch (e) { /* a partial line */ }
  }
  return r;
}

// the diagram: Internet ↔ Gateway ↔ private link ↔ Workstation (left to right whatever the language's direction)
function Diagram() {
  const p = usePal();
  const box = (title, sub, accent) => (
    <View style={{ flex: 1, minWidth: 0, padding: 10, gap: 2, borderRadius: 10, alignItems: 'center', backgroundColor: accent ? 'rgba(125,70,152,0.18)' : p.card, border: `1px solid ${accent ? 'rgba(125,70,152,0.6)' : p.line}` }}>
      <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'center' }}>{title}</Text>
      <Text style={{ fontSize: 11, color: p.dim, textAlign: 'center' }}>{sub}</Text>
    </View>
  );
  const link = (label) => (
    <View style={{ width: 92, alignItems: 'center', gap: 2 }}>
      <Text style={{ fontSize: 11, color: p.dim, textAlign: 'center' }}>{label}</Text>
      <View style={{ height: 2, width: 80, backgroundColor: p.accent }} />
      <Text style={{ fontSize: 11, color: p.accent }}>{'⟷'}</Text>
    </View>
  );
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 4, direction: 'ltr' }}>
      {box(tr('machines.whonix.d.internet'), tr('machines.whonix.d.internetSub'))}
      {link(tr('machines.whonix.d.torOnly'))}
      {box(tr('machines.whonix.d.gateway'), tr('machines.whonix.d.gatewaySub'), true)}
      {link(tr('machines.whonix.d.link'))}
      {box(tr('machines.whonix.d.workstation'), tr('machines.whonix.d.workstationSub'), true)}
    </View>
  );
}

const LOCKS = ['clipboard', 'folders', 'agent', 'gpu', 'mic', 'thumbs', 'captures', 'memory'];

export function GroupFlow({ step, setStep, dl, host, names, groupNames, onCancel, onGroup, catalog }) {
  const p = usePal();
  const [name, setName] = useState('Whonix');
  const [disp, setDisp] = useState(false);
  const [got, setGot] = useState(null);       // { done, total, phase: 'download' | 'verify' | 'unpack' | 'base' }
  const [fail, setFail] = useState('');
  const [job, setJob] = useState(0);
  const members = groupMachines((catalog?.systems || []).find((x) => x.id === dl.id));
  const mem = members.reduce((a, m) => a + m.memoryMb + OVERHEAD_MB.kvm, 0) + (disp ? sessionCapMb(members.find((m) => m.role === 'workstation')) : 0);
  const taken = (n) => [...(names || []), ...(groupNames || [])].some((x) => String(x).toLowerCase() === n.trim().toLowerCase());
  const nameOk = /^[^/\\\u0000-\u001f]{1,40}$/.test(name.trim()) && !name.trim().startsWith('.') && !taken(name);
  const fits = !host?.memMaxMb || mem <= host.memMaxMb;
  useEffect(() => {
    if (!job) return undefined;
    const t = setInterval(() => {
      const r = lord.vm.job(job);
      if (!r) return;
      const g = groupProgress(r.out);
      if (g && !r.done) setGot({ phase: g.step, done: g.frac, total: 1 });
      if (!r.done) return;
      setJob(0);
      if (!r.ok || !g?.dir) { setFail(r.out.trim().split('\n').filter((l) => l.startsWith('lord-vm:')).pop()?.replace(/^lord-vm:\s*/, '') || tr('machines.as.failed')); return; }
      const done = () => onGroup(g.dir);
      if (disp) {
        const sid = lord.vm.groupSet(g.dir, { disposable: true });
        const t2 = setInterval(() => { const q = lord.vm.job(sid); if (q && !q.done) return; clearInterval(t2); done(); }, 300);
      } else done();
    }, 400);
    return () => clearInterval(t);
  }, [job]);
  const create = async () => {
    setStep(4); setFail(''); setGot({ phase: 'download', done: 0, total: dl.image.size || 0 });
    let archive = '';
    try {
      archive = String(await admin.run('vm-image-get', `${dl.id} amd64 ${dl.image.name} ${dl.image.sha}`, (l) => {
        const g = progressOf(l);
        if (g) setGot({ phase: 'download', ...g });
        const r = resumedOf(l);
        if (r) setGot((o) => ({ phase: 'download', total: 0, ...o, done: Math.max(o?.done || 0, r) }));
      }, { quiet: true }));
    } catch (e) { setGot(null); setFail(e.code === 'admin.unavailable' ? tr('machines.dl.noHelper') : e.message || tr('machines.dl.failed')); return; }
    const id = lord.vm.groupCreate({ catalogId: dl.id, archive, sha512: dl.image.sha, release: dl.image.release || dl.release || '', name: name.trim() });
    if (!id) { setFail(tr('machines.new.refused')); return; }
    setGot({ phase: 'verify', done: 0, total: 1 });
    setJob(id);
  };

  let body = null, title = '';
  if (step === 1) {
    title = tr('machines.whonix.title');
    body = <>
      <View style={{ flexDirection: 'row', gap: 14, alignItems: 'center' }}>
        <View style={{ width: 92, height: 64, borderRadius: 12, alignItems: 'center', justifyContent: 'center', background: `linear-gradient(135deg, ${groupColors[0]}, ${groupColors[1]})` }}><GroupMark size={40} /></View>
        <Text style={{ flex: 1, fontSize: 13, lineHeight: 1.5, color: p.text, textAlign: 'start' }}>{tr('machines.whonix.what')}</Text>
      </View>
      <Diagram />
      <View style={{ flexDirection: 'row', gap: 12 }}>
        <View style={{ flex: 1, padding: 12, gap: 6, borderRadius: 10, backgroundColor: p.card }}>
          <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.whonix.lockedTitle')}</Text>
          {LOCKS.map((k) => <Text key={k} style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{`• ${tr(`machines.whonix.lock.${k}`)}`}</Text>)}
        </View>
        <View style={{ flex: 1, padding: 12, gap: 6, borderRadius: 10, backgroundColor: p.card }}>
          <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.whonix.notTitle')}</Text>
          {['host', 'escape', 'fingerprint', 'timing', 'visible'].map((k) => <Text key={k} style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{`• ${tr(`machines.whonix.not.${k}`)}`}</Text>)}
        </View>
      </View>
      <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr('machines.whonix.identity')}</Text>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, padding: 14, borderRadius: 12, backgroundColor: p.dark ? '#2a1f33' : '#f3ecf7', border: `1px solid ${p.dark ? '#5a3a6e' : '#d7bfe4'}` }}>
        <View style={{ flex: 1 }}>
          <Text style={{ fontSize: 14, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.whonix.disposable')}</Text>
          <Text style={{ fontSize: 12, lineHeight: 1.4, color: p.dim, textAlign: 'start' }}>{tr('machines.whonix.disposableHint', { cap: gb(sessionCapMb(members.find((m) => m.role === 'workstation'))) })}</Text>
        </View>
        <Toggle on={disp} onChange={setDisp} />
      </View>
      <View style={{ flexDirection: 'row', gap: 8 }}>
        <Btn link label={tr('machines.whonix.kvmPage')} onClick={() => openUrl('https://www.whonix.org/wiki/KVM')} />
        <Btn link label={tr('machines.whonix.license')} onClick={() => openUrl('https://www.whonix.org/wiki/Whonix_Binary_License_Agreement')} />
      </View>
    </>;
  } else if (step === 2) {
    title = tr('machines.as.title.name');
    body = <>
      <View style={{ gap: 4 }}>
        <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'start' }}>{tr('machines.new.name')}</Text>
        <TextInput value={name} onChange={setName} autoFocus />
        {!nameOk && name.trim() ? <Text style={{ fontSize: 12, color: '#ff453a', textAlign: 'start' }}>{tr(taken(name) ? 'machines.new.err.nameTaken' : 'machines.new.err.nameBad')}</Text> : null}
      </View>
      <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.whonix.nameHint')}</Text>
    </>;
  } else if (step === 3) {
    title = tr('machines.as.title.review');
    const role = (r) => members.find((m) => m.role === r) || { vcpus: 1, memoryMb: 2048 };
    const rows = [
      [tr('machines.as.r.system'), `Whonix ${dl.image.release || dl.release || ''} · LXQt`],
      [tr('machines.as.r.from'), dl.image.name],
      [tr('machines.as.r.download'), dl.image.size ? tr('machines.whonix.r.checked', { size: bytes(dl.image.size) }) : tr('machines.whonix.r.checkedNoSize')],
      [tr('machines.group.role.gateway'), tr('machines.as.spec', { cpus: role('gateway').vcpus, mem: gb(role('gateway').memoryMb), gfx: tr('machines.as.gfx.2d') })],
      [tr('machines.group.role.workstation'), tr('machines.as.spec', { cpus: role('workstation').vcpus, mem: gb(role('workstation').memoryMb), gfx: tr('machines.as.gfx.2d') })],
      [tr('machines.whonix.disposable'), tr(disp ? 'machines.whonix.r.on' : 'machines.whonix.r.off')],
      [tr('machines.whonix.r.memory'), gb(mem)],
      [tr('machines.whonix.r.disk'), tr('machines.whonix.r.diskValue')],
      [tr('machines.as.r.where'), `~/Machines/${name.trim()}.lordgroup`],
    ];
    body = <>
      <View style={{ borderRadius: 12, backgroundColor: p.card }}>
        {rows.map(([k, val], i) => (
          <View key={k} style={{ flexDirection: 'row', paddingHorizontal: 14, paddingVertical: 9, borderBottomWidth: i < rows.length - 1 ? 1 : 0, borderColor: p.line }}>
            <Text style={{ flex: 1, fontSize: 13, color: p.dim, textAlign: 'start' }}>{k}</Text>
            <Text style={{ fontSize: 13, fontWeight: 600, color: p.text, direction: 'ltr' }}>{val}</Text>
          </View>
        ))}
      </View>
      {!fits ? <View style={{ padding: 12, borderRadius: 10, backgroundColor: p.warnBg }}><Text style={{ fontSize: 13, color: p.text, textAlign: 'start' }}>{tr('machines.whonix.noMemory', { need: gb(mem), max: gb(host.memMaxMb) })}</Text></View> : null}
    </>;
  } else {
    title = tr('machines.whonix.creating');
    const ph = got?.phase || 'verify';
    body = (
      <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: 12, paddingTop: 40 }}>
        <View style={{ width: 120, height: 84, borderRadius: 14, alignItems: 'center', justifyContent: 'center', background: `linear-gradient(135deg, ${groupColors[0]}, ${groupColors[1]})` }}><GroupMark size={52} /></View>
        <Text style={{ fontSize: 16, fontWeight: 700, color: p.text }}>{fail ? tr('machines.as.failed') : tr(`machines.whonix.phase.${ph}`)}</Text>
        {fail ? <Text style={{ maxWidth: 560, fontSize: 13, color: '#ff453a', textAlign: 'center' }}>{fail}</Text> : <>
          <View style={{ width: 360 }}><Bar frac={got?.total ? got.done / got.total : 0} color={p.accent} /></View>
          {ph === 'download' && got?.total > 1 ? <Text style={{ fontSize: 13, color: p.dim }}>{tr('machines.dl.progress', { a: bytes(got.done), b: bytes(got.total) })}</Text> : null}
          <Text style={{ fontSize: 12, color: p.dim, textAlign: 'center', maxWidth: 520 }}>{tr('machines.whonix.phaseHint')}</Text>
        </>}
      </View>
    );
  }
  const can = step === 1 ? true : step === 2 ? nameOk : step === 3 ? nameOk && fits : false;
  const STEPN = ['choose', 'about', 'name', 'review', 'install'];
  return (
    <View style={{ flex: 1 }}>
      <SheetHeader onBack={step < 4 || fail ? () => setStep(step - 1) : null} title={tr('machines.newMachine')}
        subtitle={`${tr('machines.as.stepOf', { n: step + 1, total: 5 })} · ${tr(STEPN[step] === 'about' ? 'machines.whonix.step' : `machines.as.step.${STEPN[step]}`)}`}
        right={<Round title={tr('machines.cancel')} onClick={onCancel}><Text style={{ fontSize: 15, color: p.dim }}>✕</Text></Round>} />
      <View style={{ flex: 1, minHeight: 0, paddingHorizontal: 28, paddingVertical: 20, gap: 16 }}>
        <Text style={{ fontSize: 22, fontWeight: 700, color: p.text, textAlign: 'start' }}>{title}</Text>
        <ScrollView style={{ flex: 1 }} contentStyle={{ gap: 14, paddingBottom: 8 }}>{body}</ScrollView>
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 20, paddingVertical: 14, borderTopWidth: 1, borderColor: p.line }}>
        <View style={{ flex: 1 }} />
        {step === 4 && !fail ? null : <Pill label={fail ? tr('machines.cancel') : tr('machines.back')} onClick={() => (fail ? onCancel() : setStep(step - 1))} />}
        {step < 3 ? <Pill primary label={tr('machines.as.continue')} disabled={!can} onClick={() => setStep(step + 1)} /> : null}
        {step === 3 ? <Pill primary label={tr('machines.whonix.create')} disabled={!can} onClick={create} /> : null}
      </View>
    </View>
  );
}
