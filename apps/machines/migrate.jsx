// Migrate from another app (docs/VIRTUALIZATION.md 6.22, the approved mock "Import"): a machine of another
// virtualization app (.vmx, .vbox, config.pvs, .ova, .ovf, or one disk image) is read by lord-vm --import-plan, shown
// as a plan (what is kept, what changes, what can't come along, what stops it), then imported with progress into a new
// .lordvm by lord-vm --import-machine. The original is only read. Its identity (the SMBIOS UUID, every MAC address,
// the disks' serials, firmware, CPU topology) is kept so the guest sees as little change as possible; it starts on
// devices every system has drivers for, and Configure offers the faster ones later.
import { View, Text, TextInput, ScrollView, t as tr } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { parsePlan, importProgress, uniqueName, planNotes, planBlockers, systemOf } from './model.js';
import { usePal, Btn, Bar, gb, bytes, OsMark } from './ui.jsx';
import { writeMeta } from './meta.js';

const KIND = { vmx: '.vmx', vbox: '.vbox', pvs: 'config.pvs', ova: '.ova', ovf: '.ovf', disk: '' };

function Line({ what, from, to, last }) {
  const p = usePal();
  return (
    <View style={{ flexDirection: 'row', gap: 12, paddingHorizontal: 14, paddingVertical: 9, borderBottomWidth: last ? 0 : 1, borderColor: p.line }}>
      <Text style={{ width: 130, fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{what}</Text>
      <Text style={{ flex: 1, minWidth: 0, fontSize: 13, color: p.dim, textAlign: 'start', direction: 'ltr' }}>{from}</Text>
      <Text style={{ flex: 1, minWidth: 0, fontSize: 13, color: p.text, textAlign: 'start' }}>{to}</Text>
    </View>
  );
}

function Banner({ tone, children }) {
  const p = usePal();
  const bg = tone === 'bad' ? (p.dark ? '#4a1c1c' : '#ffe5e3') : tone === 'warn' ? p.warnBg : p.card;
  return <View style={{ padding: 12, borderRadius: 10, backgroundColor: bg }}><Text style={{ fontSize: 13, lineHeight: 1.5, color: p.text, textAlign: 'start' }}>{children}</Text></View>;
}

export function Migrate({ host, path, names, onClose, onDone, onStart }) {
  const p = usePal();
  const [arch, setArch] = useState('');
  const [planJob, setPlanJob] = useState(0);
  const [plan, setPlan] = useState(null);
  const [name, setName] = useState('');
  const [job, setJob] = useState(0);
  const [prog, setProg] = useState({ pct: 0, disk: 0, disks: 0 });
  const [done, setDone] = useState(null);
  const [fail, setFail] = useState(null);
  const file = path.split('/').pop();

  // the plan: again when the architecture is switched
  useEffect(() => {
    setPlan(null); setFail(null);
    const id = lord.vm.importPlan(path, arch ? { arch } : {});
    if (!id) { setFail(tr('machines.mig.unreadable')); return undefined; }
    setPlanJob(id);
    const timer = setInterval(() => {
      const r = lord.vm.job(id);
      if (!r || !r.done) return;
      clearInterval(timer);
      setPlanJob(0);
      const pl = parsePlan(r.out);
      if (!pl) { setFail(r.out.trim().split('\n').pop() || tr('machines.mig.unreadable')); return; }
      setPlan(pl);
      setName((n) => n || uniqueName(pl.name || file.replace(/\.[^.]+$/, ''), names));
    }, 250);
    return () => clearInterval(timer);
  }, [arch]);

  // the import's progress
  useEffect(() => {
    if (!job) return undefined;
    const timer = setInterval(() => {
      const r = lord.vm.job(job);
      if (!r) return;
      const pr = importProgress(r.out);
      setProg(pr);
      if (!r.done) return;
      clearInterval(timer);
      setJob(0);
      if (r.ok && pr.done) {
        const sys = systemOf(plan.guest_os, plan.name);
        writeMeta(pr.done.dir, { os: sys.id, osName: sys.name || plan.guest_os || '', importedFrom: plan.source });
        setDone(lord.vm.list().find((x) => x.dir === pr.done.dir) || { dir: pr.done.dir, name });
      } else setFail(r.out.trim().split('\n').filter((l) => l.startsWith('lord-vm:')).pop()?.replace(/^lord-vm:\s*/, '') || tr('machines.mig.failed'));
    }, 300);
    return () => clearInterval(timer);
  }, [job]);

  const start = () => {
    setFail(null);
    const req = { path, name, devices: 'compat', network: 'shared' };
    if (arch) req.arch = arch;
    if (plan.kind === 'disk') req.firmware = plan.firmware;
    const id = lord.vm.importMachine(req);
    if (!id) { setFail(tr('machines.mig.refused')); return; }
    setProg({ pct: 0, disk: 1, disks: plan.disks.length });
    setJob(id);
  };

  const frame = (title, sub, body, actions) => (
    <View style={{ flex: 1, paddingHorizontal: 30, paddingVertical: 26, gap: 14, borderRadius: 18, backgroundColor: p.raised }}>
      <Text style={{ fontSize: 22, fontWeight: 700, color: p.text, textAlign: 'start' }}>{title}</Text>
      {sub ? <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start' }}>{sub}</Text> : null}
      {body}
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>{actions}</View>
    </View>
  );

  if (done) return frame(tr('machines.mig.doneTitle', { name: done.name }), tr('machines.mig.doneSub'), <>
    <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: 12 }}>
      <OsMark id={systemOf(plan?.guest_os, plan?.name).id} size={72} />
      <Text style={{ fontSize: 13, color: p.dim, textAlign: 'center', maxWidth: 560, lineHeight: 1.5 }}>{tr('machines.mig.doneNote')}</Text>
    </View>
  </>, <>
    <View style={{ flex: 1 }} />
    <Btn label={tr('machines.mig.doneLibrary')} onClick={() => onDone(done)} />
    <Btn primary label={tr('machines.mig.doneStart')} onClick={() => onStart(done)} />
  </>);

  if (job) return frame(tr('machines.mig.importing', { name }), tr('machines.mig.originalStays'), <>
    <View style={{ flex: 1, justifyContent: 'center', gap: 10 }}>
      <Text style={{ fontSize: 13, color: p.text, textAlign: 'start' }}>{prog.disks > 1 ? tr('machines.mig.diskOf', { n: prog.disk || 1, of: prog.disks }) : tr('machines.mig.disk')}</Text>
      <Bar frac={prog.pct / 100} color={p.accent} height={8} />
      <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.mig.percent', { n: Math.floor(prog.pct) })}</Text>
    </View>
  </>, <View style={{ flex: 1 }} />);

  if (!plan) return frame(tr('machines.mig.reading', { file }), null, <>
    <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center' }}>
      {fail ? <Banner tone="bad">{fail}</Banner> : <Text style={{ fontSize: 13, color: p.dim }}>{tr('machines.mig.readingNote')}</Text>}
    </View>
  </>, <><View style={{ flex: 1 }} /><Btn label={tr('machines.cancel')} onClick={onClose} /></>);

  // the plan sheet
  const blockers = planBlockers(plan), notes = planNotes(plan);
  const codes = (plan.notes || []).map((n) => n.code);
  const fmt = (d) => `${(d.format || '?').toUpperCase()}, ${bytes(d.vsize)}${d.layers > 1 ? `, ${tr('machines.mig.layers', { n: d.layers - 1 })}` : ''}`;
  const here = (d) => tr('machines.mig.diskHere', { size: bytes(d.vsize), used: bytes(d.used) }) + (d.serial_kept ? `, ${tr('machines.mig.serialKept')}` : '');
  const macs = (plan.nics || []).map((n) => n.mac || tr('machines.mig.noMac')).join(', ');
  const cpuFrom = plan.sockets ? tr('machines.mig.cpuTopo', { n: plan.vcpus, s: plan.sockets, c: plan.cores, mem: gb(plan.memory_mb) }) : tr('machines.mig.cpu', { n: plan.vcpus, mem: gb(plan.memory_mb) });
  const lines = [
    [tr('machines.mig.r.name'), plan.name || file, <TextInput key="n" value={name} onChange={setName} />],
    plan.guest_os ? [tr('machines.mig.r.system'), plan.guest_os, tr('machines.mig.same')] : null,
    [tr('machines.mig.r.cpu'), plan.kind === 'disk' ? '—' : cpuFrom, tr('machines.mig.cpu', { n: plan.vcpus_here ?? plan.vcpus, mem: gb(plan.memory_mb_here ?? plan.memory_mb) })],
    ...(plan.disks || []).map((d, i) => [plan.disks.length > 1 ? tr('machines.mig.r.diskN', { n: i + 1 }) : tr('machines.mig.r.disk'), `${d.file}: ${fmt(d)}`, here(d)]),
    plan.nics?.length ? [tr('machines.mig.r.network'), tr('machines.mig.cards', { n: plan.nics.length, macs }),
      codes.includes('new-mac') ? tr('machines.mig.netNew') : tr('machines.mig.netSame')] : null,
    [tr('machines.mig.r.firmware'), plan.firmware === 'uefi' ? 'UEFI' : 'BIOS', plan.firmware === 'uefi' ? 'UEFI' : 'BIOS'],
    plan.kind !== 'disk' ? [tr('machines.mig.r.identity'), plan.uuid || '—', plan.uuid ? tr('machines.mig.idKept') : tr('machines.mig.idNew')] : null,
    [tr('machines.mig.r.devices'), '', tr('machines.mig.devicesCompat', { nic: plan.nic_model || 'e1000e' })],
  ].filter(Boolean);
  const free = host?.libFreeBytes, need = plan.needs_bytes || 0;
  const other = (plan.arch === 'amd64' ? 'arm64' : 'amd64');
  const can = !blockers.length && !!name.trim() && !names.includes(name.trim());
  const sub = [KIND[plan.kind] ? tr('machines.mig.sub', { kind: KIND[plan.kind], n: plan.disks?.length || 0 }) : tr('machines.mig.subDisk'), tr('machines.mig.originalStays')].join(' ');
  return frame(tr('machines.mig.title', { name: plan.name || file }), sub, <>
    <ScrollView style={{ flex: 1 }} contentStyle={{ gap: 12 }}>
      <View style={{ borderRadius: 12, backgroundColor: p.card }}>
        <Line what="" from={tr('machines.mig.from')} to={tr('machines.mig.to')} />
        {lines.map(([w, f, t], i) => <Line key={i} what={w} from={f} to={t} last={i === lines.length - 1} />)}
      </View>
      {blockers.map((b, i) => <Banner key={`b${i}`} tone="bad">{b.code === 'error' ? b.text : tr(`machines.mig.block.${b.code}`)}</Banner>)}
      {codes.includes('tpm') ? <Banner tone="warn">{tr('machines.mig.tpmWarn')}</Banner> : null}
      {plan.emulated ? <Banner tone="warn">{tr('machines.mig.emulated', { arch: plan.arch, host: host?.arch || '' })}</Banner> : null}
      {notes.length ? <>
        <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{tr('machines.mig.lost')}</Text>
        {notes.map((c) => <Text key={c} style={{ fontSize: 13, lineHeight: 1.6, color: p.dim, textAlign: 'start' }}>{`•  ${tr(`machines.mig.note.${c}`, { n: plan.snapshots || 0 })}`}</Text>)}
      </> : null}
      <Text style={{ fontSize: 13, lineHeight: 1.6, color: p.dim, textAlign: 'start' }}>{`•  ${tr('machines.mig.note.devices')}`}</Text>
      {codes.includes('license') ? <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr('machines.mig.license')}</Text> : null}
    </ScrollView>
  </>, <>
    <Btn link label={tr('machines.as.otherArch', { arch: other })} disabled={!!planJob} onClick={() => setArch(other)} />
    <Text style={{ flex: 1, fontSize: 12, color: p.dim, textAlign: 'end' }}>{free != null ? tr('machines.mig.space', { need: bytes(need), free: bytes(free) }) : ''}</Text>
    {fail ? <Text style={{ fontSize: 12, color: '#ff453a' }}>{fail}</Text> : null}
    <Btn label={tr('machines.cancel')} onClick={onClose} />
    <Btn primary label={tr('machines.mig.import')} disabled={!can} onClick={start} />
  </>);
}
