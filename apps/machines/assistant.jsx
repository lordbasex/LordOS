// The New Machine assistant (the approved mock "Assistant"), in the library window: 1 Choose what to run, 2 a profile
// (CPU, memory, disk), 3 the name, 4 review, 5 create. The system is recognized from the ISO's volume label or the
// file's name (name and mark), the architecture from the file's name; another architecture is emulated and says so.
// Downloading a system and migrating from another computer are shown as coming later.
import { View, Text, TextInput, FilePicker, Toggle, t as tr } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { validateDraft, createRequest, missingFor, systemOf, versionOf, archOf, suggestName, PROFILES, profileSize } from './model.js';
import { usePal, OsMark, Btn, gb } from './ui.jsx';
import { writeMeta } from './meta.js';

const STEPS = ['choose', 'profile', 'name', 'review', 'install'];

function Tile({ title, hint, onClick, selected, disabled, mark, height = 76, children }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  return (
    <View onClick={disabled ? undefined : onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flex: 1, height, padding: 12, gap: 6, justifyContent: mark ? 'space-between' : 'center', borderRadius: 10, opacity: disabled ? 0.5 : 1,
        border: `1px solid ${selected ? p.accent : p.line}`, backgroundColor: selected ? (p.dark ? '#1c3550' : '#e6f1ff') : hover && !disabled ? p.btn : p.card }}>
      {mark}
      <View>
        <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{title}</Text>
        {hint ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{hint}</Text> : null}
      </View>
      {children}
    </View>
  );
}

export function Assistant({ host, mode, names, onClose, onCreated, onConfigure, onNeeds }) {
  const p = usePal();
  const [step, setStep] = useState(0);
  const [source, setSource] = useState(mode === 'import' ? 'import' : null);
  const [path, setPath] = useState('');
  const [picking, setPicking] = useState(mode === 'import');
  const [sys, setSys] = useState({ id: 'other', name: '' });
  const [ver, setVer] = useState('');
  const [arch, setArch] = useState(host.arch);
  const [profile, setProfile] = useState('standard');
  const [name, setName] = useState('');
  const [rootPassword, setRootPassword] = useState('');
  const [customize, setCustomize] = useState(false);
  const [job, setJob] = useState(0);
  const [fail, setFail] = useState(null);
  const other = host.arch === 'amd64' ? 'arm64' : 'amd64';
  const size = profileSize(profile, host);
  const d = { name, arch, ...size, source: source === 'iso' ? 'iso' : 'import', path, rootPassword };
  const v = validateDraft(d, host, names);
  const missing = missingFor(host, [arch]);
  const emulated = arch !== host.arch;

  const chose = (pth) => {
    setPath(pth);
    const file = pth.split('/').pop();
    const label = source === 'iso' ? (lord.vm.isoInfo(pth)?.label || '') : '';
    const s = systemOf(label, file);
    const vv = versionOf(label) || versionOf(file);
    setSys(s); setVer(vv);
    const a = archOf(file) || archOf(label);
    if (a) setArch(a);
    setName(suggestName(s, vv, names));
  };
  useEffect(() => {
    if (!job) return undefined;
    const timer = setInterval(() => {
      const r = lord.vm.job(job);
      if (!r || !r.done) return;
      setJob(0);
      if (r.ok) {
        writeMeta(r.dir, { os: sys.id, osName: [sys.name, ver].filter(Boolean).join(' ') });
        const m = lord.vm.list().find((x) => x.dir === r.dir);
        if (customize) onConfigure(m || null); else onCreated(m || null);
      } else setFail(r.out.trim().split('\n').filter((l) => l.startsWith('lord-vm:')).pop()?.replace(/^lord-vm:\s*/, '') || r.out.trim() || '?');
    }, 300);
    return () => clearInterval(timer);
  }, [job]);
  const create = () => {
    setFail(null);
    const id = lord.vm.create(createRequest(d));
    if (!id) { setFail(tr('machines.new.refused')); return; }
    setJob(id);
    setStep(4);
  };

  const can = [!!source && !!path && !v.errors.path, true, !v.errors.name, v.ok && !missing.length, false][step];
  const title = tr(`machines.as.title.${STEPS[step]}`);
  const fileHint = (k) => (source === k && path ? path.split('/').pop() : tr(`machines.as.${k}Hint`));
  let body = null;
  if (step === 0) body = <>
    <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'start' }}>{tr('machines.as.download')}</Text>
    <View style={{ flexDirection: 'row', gap: 12 }}>
      {[['lordos', 'LordOS'], ['debian', 'Debian'], ['ubuntu', 'Ubuntu'], ['fedora', 'Fedora']].map(([id, n]) => (
        <Tile key={id} height={118} disabled mark={<OsMark id={id} size={40} />} title={n} hint={tr('machines.later')} />
      ))}
    </View>
    <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'start' }}>{tr('machines.as.own')}</Text>
    <View style={{ flexDirection: 'row', gap: 12 }}>
      <Tile title={tr('machines.as.iso')} hint={fileHint('iso')} selected={source === 'iso'} onClick={() => { if (source !== 'iso') setPath(''); setSource('iso'); setPicking(true); }} />
      <Tile title={tr('machines.as.migrate')} hint={tr('machines.later')} disabled />
      <Tile title={tr('machines.as.import')} hint={fileHint('import')} selected={source === 'import'} onClick={() => { if (source !== 'import') setPath(''); setSource('import'); setPicking(true); }} />
    </View>
    {path ? (
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, padding: 12, borderRadius: 10, backgroundColor: p.raised }}>
        <OsMark id={sys.id} size={36} />
        <View style={{ flex: 1 }}>
          <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{[sys.name || tr('machines.as.unknownSystem'), ver].filter(Boolean).join(' ')}{`, ${arch}`}</Text>
          <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start', direction: 'ltr', whiteSpace: 'nowrap', overflow: 'hidden' }}>{path}</Text>
        </View>
      </View>
    ) : null}
    {emulated ? <View style={{ padding: 12, borderRadius: 10, backgroundColor: p.warnBg }}><Text style={{ fontSize: 13, lineHeight: 1.5, color: p.text, textAlign: 'start' }}>{tr('machines.as.emulated', { arch, host: host.arch })}</Text></View> : null}
  </>;
  else if (step === 1) body = <>
    <View style={{ flexDirection: 'row', gap: 12 }}>
      {PROFILES.map((k) => { const s = profileSize(k, host); return (
        <Tile key={k} height={130} selected={profile === k} onClick={() => setProfile(k)} title={tr(`machines.as.profile.${k}`)}
          hint={tr(`machines.as.profileHint.${k}`)}>
          <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.as.size', { cpus: s.vcpus, mem: gb(s.memoryMb), disk: s.diskGb })}</Text>
        </Tile>); })}
    </View>
    <Text style={{ fontSize: 12, lineHeight: 1.5, color: p.dim, textAlign: 'start' }}>{tr('machines.as.profileNote', { mem: gb(host.memMaxMb) })}</Text>
    {emulated ? <View style={{ padding: 12, borderRadius: 10, backgroundColor: p.warnBg }}><Text style={{ fontSize: 13, color: p.text, textAlign: 'start' }}>{tr('machines.as.emulatedShort')}</Text></View> : null}
  </>;
  else if (step === 2) body = <>
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 14 }}>
      <OsMark id={sys.id} size={56} />
      <View style={{ flex: 1, gap: 4 }}>
        <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'start' }}>{tr('machines.new.name')}</Text>
        <TextInput value={name} onChange={setName} autoFocus placeholder={tr('machines.new.namePlaceholder')} />
        {v.errors.name && name ? <Text style={{ fontSize: 12, color: '#ff453a', textAlign: 'start' }}>{tr(`machines.new.err.${v.errors.name}`)}</Text> : null}
      </View>
    </View>
    {source === 'import' ? (
      <View style={{ gap: 4 }}>
        <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'start' }}>{tr('machines.new.rootPassword')}</Text>
        <TextInput secure value={rootPassword} onChange={setRootPassword} />
        <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start', lineHeight: 1.4 }}>{tr('machines.new.rootPasswordHint')}</Text>
      </View>
    ) : null}
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
      <Toggle on={customize} onChange={setCustomize} />
      <Text style={{ fontSize: 13, color: p.text }}>{tr('machines.as.customize')}</Text>
    </View>
  </>;
  else if (step === 3) {
    const rows = [
      [tr('machines.as.r.system'), [sys.name || tr('machines.as.unknownSystem'), ver].filter(Boolean).join(' ')],
      [tr('machines.as.r.from'), path.split('/').pop()],
      [tr('machines.as.r.arch'), emulated ? `${arch}, ${tr('machines.emulatedLower')}` : arch],
      [tr('machines.as.r.cpu'), tr('machines.vcpus', { n: size.vcpus })],
      [tr('machines.as.r.memory'), gb(size.memoryMb)],
      [tr('machines.as.r.disk'), source === 'iso' ? tr('machines.as.r.diskNew', { n: size.diskGb }) : tr('machines.as.r.diskCopy', { n: size.diskGb })],
      [tr('machines.as.r.network'), tr('machines.as.r.shared')],
      [tr('machines.as.r.where'), `~/Machines/${name}.lordvm`],
    ];
    body = <>
      <View style={{ borderRadius: 12, backgroundColor: p.card }}>
        {rows.map(([k, val], i) => (
          <View key={k} style={{ flexDirection: 'row', paddingHorizontal: 14, paddingVertical: 10, borderBottomWidth: i < rows.length - 1 ? 1 : 0, borderColor: p.line }}>
            <Text style={{ flex: 1, fontSize: 13, color: p.dim, textAlign: 'start' }}>{k}</Text>
            <Text style={{ fontSize: 13, fontWeight: 600, color: p.text, direction: 'ltr' }}>{val}</Text>
          </View>
        ))}
      </View>
      {missing.length ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, padding: 12, borderRadius: 10, backgroundColor: p.warnBg }}>
          <Text style={{ flex: 1, fontSize: 13, color: p.text, textAlign: 'start' }}>{tr('machines.new.needs', { list: missing.join(', ') })}</Text>
          <Btn label={tr('machines.req.button')} onClick={onNeeds} />
        </View>
      ) : null}
      {Object.keys(v.errors).length ? <Text style={{ fontSize: 13, color: '#ff453a', textAlign: 'start' }}>{Object.values(v.errors).map((e) => tr(`machines.new.err.${e}`, { max: host.memMaxMb })).join(' ')}</Text> : null}
    </>;
  } else body = (
    <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: 12 }}>
      <OsMark id={sys.id} size={72} />
      <Text style={{ fontSize: 16, fontWeight: 700, color: p.text }}>{fail ? tr('machines.as.failed') : source === 'iso' ? tr('machines.new.creating') : tr('machines.new.importing')}</Text>
      {fail ? <Text style={{ maxWidth: 560, fontSize: 13, color: '#ff453a', textAlign: 'center' }}>{fail}</Text>
        : <Text style={{ fontSize: 13, color: p.dim }}>{tr('machines.as.thenStart')}</Text>}
    </View>
  );

  return (
    <View style={{ flex: 1, paddingHorizontal: 28, paddingVertical: 24, gap: 16, borderRadius: 12, backgroundColor: p.raised, border: `1px solid ${p.line}` }}>
      <View style={{ flexDirection: 'row', gap: 20 }}>
        {STEPS.map((s, i) => <Text key={s} style={{ fontSize: 12, fontWeight: i === step ? 700 : 400, color: i === step ? p.text : p.dim }}>{`${i + 1} ${tr(`machines.as.step.${s}`)}`}</Text>)}
      </View>
      <Text style={{ fontSize: 22, fontWeight: 600, color: p.text, textAlign: 'start' }}>{title}</Text>
      {body}
      <View style={{ flex: 1 }} />
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        {step === 0 ? <Btn link label={arch === host.arch ? tr('machines.as.otherArch', { arch: other }) : tr('machines.as.thisArch', { arch: host.arch })}
          onClick={() => setArch(arch === host.arch ? other : host.arch)} /> : null}
        <View style={{ flex: 1 }} />
        {step === 4 && !fail ? null : <Btn label={step === 0 || fail ? tr('machines.cancel') : tr('machines.back')} onClick={() => (step === 0 || fail ? onClose() : setStep(step - 1))} />}
        {step < 3 ? <Btn primary label={tr('machines.as.continue')} disabled={!can} onClick={() => setStep(step + 1)} /> : null}
        {step === 3 ? <Btn primary label={customize ? tr('machines.as.createConfigure') : tr('machines.as.createStart')} disabled={!can} onClick={create} /> : null}
      </View>
      {picking ? <FilePicker title={source === 'iso' ? tr('machines.new.pickIso') : tr('machines.new.pickImage')} filter={source === 'iso' ? /\.iso$/i : /\.(qcow2|img|raw)$/i}
        onPick={(x) => { chose(Array.isArray(x) ? x[0] : x); setPicking(false); }} onClose={() => setPicking(false)} /> : null}
    </View>
  );
}
