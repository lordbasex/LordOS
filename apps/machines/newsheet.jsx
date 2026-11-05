// The New machine sheet (alpha): name, architecture, vCPUs, memory, disk and where the system comes from (a disk
// image to import or an ISO to boot). lord-vm --create checks it all again; the folder is ~/Machines/<name>.lordvm.
import { View, Text, Button, TextInput, Segmented, Stepper, FilePicker, useTheme, spacing, typeScale, t as tr } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { defaultsFor, validateDraft, createRequest, missingFor } from './model.js';

function Field({ label, children, hint, warn }) {
  const t = useTheme();
  return (
    <View style={{ gap: 4 }}>
      <Text style={{ fontSize: typeScale.caption, fontWeight: 600, color: t.textDim, textAlign: 'start' }}>{label}</Text>
      {warn ? <Text style={{ fontSize: typeScale.caption, color: '#ff9f0a', textAlign: 'start', lineHeight: 1.35 }}>{warn}</Text> : null}
      {children}
      {hint ? <Text style={{ fontSize: typeScale.caption, color: hint.bad ? '#ff453a' : t.textDim, textAlign: 'start', lineHeight: 1.35 }}>{hint.text}</Text> : null}
    </View>
  );
}

export function NewMachine({ host, names: names0, onClose, onCreated }) {
  const t = useTheme();
  const [names] = useState(names0);              // the list goes on refreshing behind the sheet, with this machine in it
  const [d, setD] = useState(() => defaultsFor(host));
  const [picking, setPicking] = useState(false);
  const [job, setJob] = useState(0);
  const [fail, setFail] = useState(null);
  const [tried, setTried] = useState(false);
  const set = (p) => setD((x) => ({ ...x, ...p }));
  const other = host.arch === 'amd64' ? 'arm64' : 'amd64';
  const v = validateDraft(d, host, names);
  const missing = missingFor(host, [d.arch]);
  const err = (k) => (tried || (k === 'name' && d.name) || (k === 'path' && d.path) ? v.errors[k] : null);
  useEffect(() => {
    if (!job) return undefined;
    const timer = setInterval(() => {
      const r = lord.vm.job(job);
      if (!r || !r.done) return;
      setJob(0);
      if (r.ok) {
        const m = lord.vm.list().find((x) => x.dir === r.dir);
        onCreated(m || null);
      } else setFail(r.out.trim().split('\n').filter((l) => l.startsWith('lord-vm:')).pop()?.replace(/^lord-vm:\s*/, '') || r.out.trim() || '?');
    }, 300);
    return () => clearInterval(timer);
  }, [job]);
  const create = () => {
    setTried(true);
    setFail(null);
    if (!v.ok || missing.length) return;
    const id = lord.vm.create(createRequest(d));
    if (!id) setFail(tr('machines.new.refused'));
    else setJob(id);
  };
  const iso = d.source === 'iso';
  return (
    <View style={{ position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(0,0,0,0.35)' }}>
      <View style={{ width: 500, maxHeight: '96%', padding: spacing.xl, gap: spacing.m, borderRadius: 12, backgroundColor: t.windowBg, border: `1px solid ${t.separator}`,
        boxShadow: '0 14px 36px rgba(0,0,0,0.45)' }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('machines.new.title')}</Text>
        <Field label={tr('machines.new.name')} hint={err('name') ? { bad: true, text: tr(`machines.new.err.${err('name')}`) } : null}>
          <TextInput value={d.name} onChange={(x) => set({ name: x })} placeholder={tr('machines.new.namePlaceholder')} autoFocus />
        </Field>
        <Field label={tr('machines.new.arch')} hint={v.emulated ? { text: tr('machines.new.warn.emulated') } : v.warnings.includes('noKvm') ? { text: tr('machines.new.warn.noKvm') } : null}>
          <Segmented small fill value={d.arch} onChange={(x) => set({ arch: x })}
            options={[[host.arch, tr('machines.new.archThis', { arch: host.arch })], [other, tr('machines.new.archOther', { arch: other })]]} />
        </Field>
        {v.warnings.includes('vcpus') ? <Text style={{ fontSize: typeScale.caption, color: '#ff9f0a', textAlign: 'start', lineHeight: 1.35, marginBottom: -6 }}>
          {tr('machines.new.warn.vcpus', { n: host.vcpusRecommended })}</Text> : null}
        <View style={{ flexDirection: 'row', gap: spacing.l }}>
          <Field label={tr('machines.new.vcpus')}
            hint={err('vcpus') ? { bad: true, text: tr('machines.new.err.vcpus', { max: host.cpus }) } : null}>
            <Stepper value={d.vcpus} min={1} max={Math.max(1, host.cpus)} onChange={(x) => set({ vcpus: x })} />
          </Field>
          <Field label={tr('machines.new.memory')} hint={err('memoryMb') ? { bad: true, text: tr('machines.new.err.memory', { max: host.memMaxMb }) } : { text: tr('machines.new.memoryMax', { max: host.memMaxMb }) }}>
            <Stepper value={d.memoryMb} min={256} max={Math.max(256, host.memMaxMb)} step={256} width={72} format={(x) => `${x} MB`} onChange={(x) => set({ memoryMb: x })} />
          </Field>
          <Field label={tr('machines.new.disk')} hint={err('diskGb') ? { bad: true, text: tr('machines.new.err.disk') } : null}>
            <Stepper value={d.diskGb} min={1} max={2048} step={1} width={64} format={(x) => `${x} GB`} onChange={(x) => set({ diskGb: x })} />
          </Field>
        </View>
        <Field label={tr('machines.new.source')} hint={err('path') ? { bad: true, text: tr(`machines.new.err.${err('path')}`) } : { text: iso ? tr('machines.new.isoHint') : tr('machines.new.importHint') }}>
          <Segmented small fill value={d.source} onChange={(x) => set({ source: x, path: '' })}
            options={[['import', tr('machines.new.import')], ['iso', tr('machines.new.iso')]]} />
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
            <Text style={{ flex: 1, fontSize: typeScale.callout, color: d.path ? t.text : t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start', direction: 'ltr' }}>
              {d.path || tr('machines.new.noFile')}</Text>
            <Button small label={tr('machines.new.choose')} onClick={() => setPicking(true)} />
          </View>
        </Field>
        {!iso ? (
          <Field label={tr('machines.new.rootPassword')} hint={{ text: tr('machines.new.rootPasswordHint') }}>
            <TextInput secure value={d.rootPassword} onChange={(x) => set({ rootPassword: x })} />
          </Field>
        ) : null}
        {missing.length ? <Text style={{ fontSize: typeScale.caption, color: '#ff9f0a', textAlign: 'start' }}>{tr('machines.new.needs', { list: missing.join(', ') })}</Text> : null}
        {fail ? <Text style={{ fontSize: typeScale.callout, color: '#ff453a', textAlign: 'start', lineHeight: 1.35 }}>{tr('machines.new.failed', { why: fail })}</Text> : null}
        <View style={{ flexDirection: 'row', gap: spacing.s, justifyContent: 'flex-end', alignItems: 'center' }}>
          {job ? <Text style={{ flex: 1, fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{iso ? tr('machines.new.creating') : tr('machines.new.importing')}</Text> : null}
          <Button label={tr('machines.cancel')} disabled={!!job} onClick={onClose} />
          <Button primary label={tr('machines.new.create')} disabled={!!job || !!missing.length} onClick={create} />
        </View>
      </View>
      {picking ? <FilePicker title={iso ? tr('machines.new.pickIso') : tr('machines.new.pickImage')} filter={iso ? /\.iso$/i : /\.(qcow2|img|raw)$/i}
        onPick={(p) => { set({ path: Array.isArray(p) ? p[0] : p }); setPicking(false); }} onClose={() => setPicking(false)} /> : null}
    </View>
  );
}
