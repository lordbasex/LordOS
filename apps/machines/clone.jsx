// Copy a machine (the approved mock "Clone"): Clone, a new machine that runs beside this one with its own identity,
// or Duplicate, an exact copy with the same identity (6.3). The sheet says how the disk is copied on this computer's
// file system before anything happens: shared by reflink (btrfs, XFS), a frozen shared base with an overlay for each
// machine (ext4), or a full copy. lord-vm does it with the machine off.
import { View, Text, TextInput, t as tr } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { usePal, Btn, Sheet } from './ui.jsx';
import { cloneMethod } from './model.js';
import { writeMeta } from './meta.js';

function Kind({ on, title, text, foot, onClick }) {
  const p = usePal();
  return (
    <View onClick={onClick} style={{ flex: 1, minHeight: 150, padding: 14, gap: 6, borderRadius: 12, backgroundColor: on ? (p.dark ? '#1d3049' : '#e6f1ff') : p.card,
      border: `2px solid ${on ? '#0a84ff' : p.line}` }}>
      <Text style={{ fontSize: 15, fontWeight: 700, color: p.text, textAlign: 'start' }}>{title}</Text>
      <Text style={{ fontSize: 13, lineHeight: 1.45, color: p.dim, textAlign: 'start' }}>{text}</Text>
      <View style={{ flex: 1 }} />
      <Text style={{ fontSize: 12, fontWeight: 700, color: p.text, textAlign: 'start' }}>{foot}</Text>
    </View>
  );
}

// m: the machine (from lord.vm.list); fromSnapshot: { tag, name } to make a Clone of that moment
export function CloneSheet({ m, fromSnapshot, running, onClose, onDone }) {
  const p = usePal();
  const [kind, setKind] = useState('clone');
  const [name, setName] = useState(fromSnapshot ? `${m.name} ${fromSnapshot.name}`.slice(0, 64) : tr('machines.clone.defaultName', { name: m.name }).slice(0, 64));
  const [info] = useState(() => lord.vm.cloneInfo(m.dir) || {});
  const [job, setJob] = useState(0);
  const [fail, setFail] = useState('');
  const how = cloneMethod(info, fromSnapshot ? 'clone' : kind, !!fromSnapshot);
  useEffect(() => {
    if (!job) return undefined;
    const timer = setInterval(() => {
      const r = lord.vm.job(job);
      if (!r || !r.done) return;
      setJob(0);
      if (!r.ok) { setFail(r.out.trim().split('\n').filter((l) => l.startsWith('lord-vm:')).pop()?.replace(/^lord-vm:\s*/, '') || r.out.trim()); return; }
      let out = {};
      try { out = JSON.parse(r.out.trim().split('\n').pop()); } catch (e) { out = {}; }
      if (out.dir) writeMeta(out.dir, { cloneOf: m.id, cloneKind: fromSnapshot ? 'clone' : kind });
      onDone(out);
    }, 400);
    return () => clearInterval(timer);
  }, [job]);
  const go = () => {
    setFail('');
    const id = lord.vm.clone(m.dir, { name: name.trim(), duplicate: kind === 'duplicate' && !fromSnapshot, linked: how === 'linked', fromSnapshot: fromSnapshot?.tag });
    if (!id) setFail(tr('machines.cfg.refused')); else setJob(id);
  };
  const blocked = running ? tr('machines.clone.running', { name: m.name }) : how === 'blocked' ? tr('machines.clone.suspended', { name: m.name }) : '';
  return (
    <Sheet width={640} title={fromSnapshot ? tr('machines.clone.fromTitle', { name: fromSnapshot.name }) : tr('machines.clone.title', { name: m.name })} onCancel={onClose} actions={[
      <Btn key="c" label={tr('machines.cancel')} onClick={onClose} />,
      <Btn key="o" primary label={job ? tr('machines.clone.copying') : tr(kind === 'duplicate' && !fromSnapshot ? 'machines.clone.duplicate' : 'machines.clone.clone')}
        disabled={!!job || !!blocked || !name.trim()} onClick={go} />,
    ]}>
      {!fromSnapshot ? (
        <View style={{ flexDirection: 'row', gap: 12 }}>
          <Kind on={kind === 'clone'} onClick={() => setKind('clone')} title={tr('machines.clone.clone')} text={tr('machines.clone.cloneText')}
            foot={tr(info.reflink || cloneMethod(info, 'clone') === 'linked' ? 'machines.clone.changesOnly' : 'machines.clone.fullCopy')} />
          <Kind on={kind === 'duplicate'} onClick={() => setKind('duplicate')} title={tr('machines.clone.duplicate')} text={tr('machines.clone.duplicateText')}
            foot={tr(info.reflink ? 'machines.clone.alsoShares' : 'machines.clone.fullCopy')} />
        </View>
      ) : null}
      <View style={{ gap: 4 }}>
        <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.new.name')}</Text>
        <TextInput value={name} onChange={setName} />
      </View>
      <View style={{ paddingHorizontal: 12, paddingVertical: 10, borderRadius: 10, backgroundColor: blocked ? p.warnBg : p.card }}>
        <Text style={{ fontSize: 13, lineHeight: 1.5, color: p.text, textAlign: 'start' }}>
          {blocked || tr(`machines.clone.how.${how}`, { fs: info.fs || '', name: m.name })}
        </Text>
      </View>
      {fail ? <Text style={{ fontSize: 13, color: '#ff453a', textAlign: 'start' }}>{fail}</Text> : null}
    </Sheet>
  );
}
