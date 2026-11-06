// The New Machine assistant (the approved mock "Assistant"), in the library window: 1 Choose what to run, 2 a profile
// (CPU, memory, disk), 3 the name, 4 review, 5 create. The system is recognized from the ISO's volume label or the
// file's name (name and mark), the architecture from the file's name; another architecture is emulated and says so.
// Download and install: the cards of assets/machines/systems.json for the chosen architecture (catalog.js); choosing
// one reads its maker's SHA-256 list (vm-image-list), the newest image of the release is named with its hash, and step 5
// downloads it (vm-image-get, which keeps it only with that hash) before the machine is made from it.
// Migrate from another app opens the import plan (migrate.jsx).
import { View, Text, TextInput, FilePicker, Toggle, admin, fs, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import CATALOG from '../../assets/machines/systems.json';
import { systemsFor, entryOf, listingNames, newest, pickImage, versionIn, progressOf, resumedOf, sizeMb } from './catalog.js';
import { validateDraft, createRequest, missingFor, systemOf, versionOf, archOf, suggestName, PROFILES, profileSize } from './model.js';
import { usePal, OsMark, Btn, Bar, gb } from './ui.jsx';
import { writeMeta } from './meta.js';
import { Migrate } from './migrate.jsx';

// what Migrate from another app takes: other apps' settings files and packages, and their disk formats
const MIGRATE_FILES = /(\.(vmx|vbox|pvs|ovf|ova|vmdk|vdi|vhdx|vhd|hds)|\/config\.pvs)$/i;

const STEPS = ['choose', 'profile', 'name', 'review', 'install'];
const rowsOf = (list, n) => { const out = []; for (let i = 0; i < list.length; i += n) out.push(list.slice(i, i + n)); return out; };

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
  const [profile, setProfile] = useState('productivity');
  const [name, setName] = useState('');
  const [rootPassword, setRootPassword] = useState('');
  const [customize, setCustomize] = useState(false);
  const [shareClip, setShareClip] = useState(false);
  const [job, setJob] = useState(0);
  const [fail, setFail] = useState(null);
  // a download card: { id, name, phase: 'list' | 'ready' | 'error', image: { name, sha, size }, err }
  const [dl, setDl] = useState(null);
  const [got, setGot] = useState(null);          // the download's progress, { done, total }, in step 5
  const pickSeq = useRef(0);
  const cancelled = useRef(false);
  const other = host.arch === 'amd64' ? 'arm64' : 'amd64';
  const size = profileSize(profile, host);
  const downloading = source === 'download';
  // a download is checked as the ISO it will be (its file name), and made from the downloaded path
  const d = { name, arch, vcpus: size.vcpus, memoryMb: size.memoryMb, diskGb: size.diskGb, network: size.network, source: source === 'iso' || downloading ? 'iso' : 'import',
    path: downloading ? (dl?.image?.name || '') : path, rootPassword, shareClipboard: shareClip };
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
  // a card: its maker's list (and, when the list's name changes per build, the folder's listing first), the newest image
  const pickDownload = async (s, a = arch) => {
    const seq = ++pickSeq.current;
    setSource('download'); setPath(''); setPicking(false);
    setSys({ id: s.id, name: s.name }); setVer(s.release); setName(suggestName({ name: s.name }, s.release, names));
    setDl({ id: s.id, name: s.name, phase: 'list' });
    const e = entryOf(CATALOG, s.id, a);
    try {
      if (!e) throw new Error(tr('machines.dl.noList', { why: tr('machines.dl.noImage') }));
      let p = await admin.run('vm-image-list', `${s.id} ${a}`, null, { quiet: true });
      if (!e.sums) {
        const list = newest(listingNames(fs.read(String(p)), e.sumsPattern), e.sumsPattern);
        if (!list) throw new Error(tr('machines.dl.noList', { why: tr('machines.dl.noImage') }));
        p = await admin.run('vm-image-list', `${s.id} ${a} ${list}`, null, { quiet: true });
      }
      const img = pickImage(e, fs.read(String(p)));
      if (!img) throw new Error(tr('machines.dl.noList', { why: tr('machines.dl.noImage') }));
      if (seq !== pickSeq.current) return;
      const vv = versionIn(img.name, e.release);
      setVer(vv); setName(suggestName({ name: s.name }, vv, names));
      setDl({ id: s.id, name: s.name, phase: 'ready', image: img });
    } catch (err) {
      if (seq === pickSeq.current) setDl({ id: s.id, name: s.name, phase: 'error', err: err.message || String(err) });
    }
  };
  // another architecture: the same card for it
  useEffect(() => {
    if (!downloading || !dl) return;
    const s = systemsFor(CATALOG, arch).find((x) => x.id === dl.id);
    if (s) pickDownload(s, arch); else { setSource(null); setDl(null); }
  }, [arch]);
  useEffect(() => {
    if (!job) return undefined;
    const timer = setInterval(() => {
      const r = lord.vm.job(job);
      if (!r || !r.done) return;
      setJob(0);
      if (r.ok) {
        writeMeta(r.dir, { os: sys.id, osName: [sys.name, ver].filter(Boolean).join(' '), profile });
        const m = lord.vm.list().find((x) => x.dir === r.dir);
        if (customize) onConfigure(m || null); else onCreated(m || null);
      } else setFail(r.out.trim().split('\n').filter((l) => l.startsWith('lord-vm:')).pop()?.replace(/^lord-vm:\s*/, '') || r.out.trim() || '?');
    }, 300);
    return () => clearInterval(timer);
  }, [job]);
  const create = async () => {
    setFail(null);
    let iso = null;
    if (downloading) {
      setStep(4); setGot({ done: 0, total: dl.image.size || 0 }); cancelled.current = false;
      try {
        // a download stopped before is continued: the helper says from where first ("vm.downloadResumed")
        iso = String(await admin.run('vm-image-get', `${dl.id} ${arch} ${dl.image.name} ${dl.image.sha}`, (l) => {
          const g = progressOf(l);
          if (g) setGot((o) => ({ ...g, resumed: o?.resumed || 0 }));
          const r = resumedOf(l);
          if (r) setGot((o) => ({ ...(o || { done: r, total: 0 }), resumed: r }));
        }, { quiet: true }));
      } catch (e) {
        setGot(null);
        setFail(cancelled.current ? tr('machines.dl.cancelled') : e.code === 'admin.unavailable' ? tr('machines.dl.noHelper') : e.message || tr('machines.dl.failed'));
        return;
      }
      setGot(null);
    }
    const id = lord.vm.create(createRequest(iso ? { ...d, path: iso } : d));
    if (!id) { setFail(tr('machines.new.refused')); setStep(4); return; }
    setJob(id);
    setStep(4);
  };
  const stopDownload = () => { cancelled.current = true; admin.run('vm-image-cancel', '', null, { quiet: true }).catch(() => {}); };

  if (source === 'migrate' && path && !picking) return <Migrate host={host} path={path} names={names} onClose={onClose} onDone={() => onClose()} onStart={(m) => onCreated(m)} />;
  const can = [downloading ? dl?.phase === 'ready' : !!source && !!path && !v.errors.path, true, !v.errors.name, v.ok && !missing.length, false][step];
  const title = tr(`machines.as.title.${STEPS[step]}`);
  const fileHint = (k) => (source === k && path ? path.split('/').pop() : tr(`machines.as.${k}Hint`));
  let body = null;
  if (step === 0) body = <>
    <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'start' }}>{tr('machines.as.download')}</Text>
    {/* the mock's grid of four: one row per four systems of the catalog, for the chosen architecture */}
    {rowsOf(systemsFor(CATALOG, arch), 4).map((row, i) => (
      <View key={i} style={{ flexDirection: 'row', gap: 12 }}>
        {row.map((s) => {
          const mine = downloading && dl?.id === s.id;
          const exact = mine && dl.image?.size > 0;
          const title = [s.name, mine && dl.phase === 'ready' ? ver : s.release].filter(Boolean).join(' ');
          const sz = gb(sizeMb(s.approxMb, exact ? dl.image.size : 0));
          return <Tile key={s.id} height={118} selected={mine} mark={<OsMark id={s.id} size={40} />} title={title}
            hint={tr(exact ? 'machines.dl.size' : 'machines.dl.sizeAbout', { arch, size: sz })} onClick={() => pickDownload(s)} />;
        })}
        {row.length < 4 ? Array.from({ length: 4 - row.length }, (_, k) => <View key={`f${k}`} style={{ flex: 1 }} />) : null}
      </View>
    ))}
    <Text style={{ fontSize: 12, fontWeight: 600, color: p.dim, textAlign: 'start' }}>{tr('machines.as.own')}</Text>
    <View style={{ flexDirection: 'row', gap: 12 }}>
      <Tile title={tr('machines.as.iso')} hint={fileHint('iso')} selected={source === 'iso'} onClick={() => { pickSeq.current++; setDl(null); if (source !== 'iso') setPath(''); setSource('iso'); setPicking(true); }} />
      <Tile title={tr('machines.as.migrate')} hint={tr('machines.as.migrateHint')} selected={source === 'migrate'} onClick={() => { pickSeq.current++; setDl(null); setPath(''); setSource('migrate'); setPicking(true); }} />
      <Tile title={tr('machines.as.import')} hint={fileHint('import')} selected={source === 'import'} onClick={() => { pickSeq.current++; setDl(null); if (source !== 'import') setPath(''); setSource('import'); setPicking(true); }} />
    </View>
    {downloading && dl ? (
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, padding: 12, borderRadius: 10, backgroundColor: p.raised }}>
        <OsMark id={dl.id} size={36} />
        <View style={{ flex: 1 }}>
          <Text style={{ fontSize: 13, fontWeight: 700, color: p.text, textAlign: 'start' }}>{`${[dl.name, ver].filter(Boolean).join(' ')}, ${arch}`}</Text>
          <Text style={{ fontSize: 12, color: dl.phase === 'error' ? '#ff453a' : p.dim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>
            {dl.phase === 'list' ? tr('machines.dl.reading', { name: dl.name }) : dl.phase === 'error' ? dl.err : tr('machines.dl.found', { file: dl.image.name })}</Text>
        </View>
        {dl.phase === 'error' ? <Btn label={tr('machines.dl.retry')} onClick={() => { const s = systemsFor(CATALOG, arch).find((x) => x.id === dl.id); if (s) pickDownload(s); }} /> : null}
      </View>
    ) : null}
    {path && !downloading ? (
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
  else if (step === 1) {
    // the mock "Profiles": two columns of cards, each with what it's for and what it gives on this computer
    const rows = [];
    for (let i = 0; i < PROFILES.length; i += 2) rows.push(PROFILES.slice(i, i + 2));
    body = <>
    <Text style={{ fontSize: 13, color: p.dim, textAlign: 'start' }}>{tr('machines.as.profileFits', { cpus: host.cpus, mem: gb(host.ramMb) })}</Text>
    {rows.map((r, i) => (
      <View key={i} style={{ flexDirection: 'row', gap: 12 }}>
        {r.map((k) => { const s = profileSize(k, host); return (
          <View key={k} onClick={() => setProfile(k)} style={{ flex: 1, minHeight: 92, paddingHorizontal: 14, paddingVertical: 10, gap: 4, borderRadius: 10,
            backgroundColor: p.card, border: `2px solid ${profile === k ? p.accent : p.line}` }}>
            <Text style={{ fontSize: 14, fontWeight: 600, color: p.text, textAlign: 'start' }}>{tr(`machines.as.profile.${k}`)}</Text>
            <Text style={{ fontSize: 12, lineHeight: 1.4, color: p.dim, textAlign: 'start' }}>{tr(`machines.as.profileHint.${k}`)}</Text>
            <Text style={{ fontSize: 12, fontWeight: 600, color: p.text, textAlign: 'start' }}>{tr('machines.as.spec', { cpus: s.vcpus, mem: gb(s.memoryMb), gfx: tr(`machines.as.gfx.${s.gfx}`) })}</Text>
          </View>); })}
        {r.length < 2 ? <View style={{ flex: 1 }} /> : null}
      </View>
    ))}
    {size.gfx === '3d' ? <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.as.no3d')}</Text> : null}
    {emulated ? <View style={{ padding: 12, borderRadius: 10, backgroundColor: p.warnBg }}><Text style={{ fontSize: 13, color: p.text, textAlign: 'start' }}>{tr('machines.as.emulatedShort')}</Text></View> : null}
  </>;
  }
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
      [tr('machines.as.r.from'), downloading ? dl.image.name : path.split('/').pop()],
      ...(downloading ? [[tr('machines.as.r.download'), dl.image.size ? tr('machines.dl.checked', { size: gb(Math.round(dl.image.size / 1048576)) }) : tr('machines.dl.checkedNoSize')]] : []),
      [tr('machines.as.r.arch'), emulated ? `${arch}, ${tr('machines.emulatedLower')}` : arch],
      [tr('machines.as.r.cpu'), tr('machines.vcpus', { n: size.vcpus })],
      [tr('machines.as.r.memory'), gb(size.memoryMb)],
      [tr('machines.as.r.disk'), source === 'iso' || downloading ? tr('machines.as.r.diskNew', { n: size.diskGb }) : tr('machines.as.r.diskCopy', { n: size.diskGb })],
      [tr('machines.as.r.profile'), tr(`machines.as.profile.${profile}`)],
      [tr('machines.as.r.network'), tr(`machines.cfg.net.${size.network}`)],
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
      {profile === 'productivity' ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, padding: 12, borderRadius: 10, backgroundColor: p.card }}>
          <Toggle on={shareClip} onChange={setShareClip} />
          <View style={{ flex: 1 }}>
            <Text style={{ fontSize: 13, color: p.text, textAlign: 'start' }}>{tr('machines.as.shareClip')}</Text>
            <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('machines.as.shareClipHint')}</Text>
          </View>
        </View>
      ) : null}
      {missing.length ? (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, padding: 12, borderRadius: 10, backgroundColor: p.warnBg }}>
          <Text style={{ flex: 1, fontSize: 13, color: p.text, textAlign: 'start' }}>{tr('machines.new.needs', { list: missing.join(', ') })}</Text>
          <Btn label={tr('machines.req.button')} onClick={onNeeds} />
        </View>
      ) : null}
      {Object.keys(v.errors).length ? <Text style={{ fontSize: 13, color: '#ff453a', textAlign: 'start' }}>{Object.values(v.errors).map((e) => tr(`machines.new.err.${e}`, { max: host.memMaxMb })).join(' ')}</Text> : null}
    </>;
  } else if (got && !fail) {
    // the download, with its progress; the helper checks the SHA-256 at the end
    const mb = (x) => gb(Math.round(x / 1048576));
    const checking = got.total > 0 && got.done >= got.total;
    body = (
      <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: 12 }}>
        <OsMark id={sys.id} size={72} />
        <Text style={{ fontSize: 16, fontWeight: 700, color: p.text }}>{tr('machines.dl.downloading', { name: [sys.name, ver].filter(Boolean).join(' ') })}</Text>
        <View style={{ width: 360 }}><Bar frac={got.total ? got.done / got.total : 0} color={p.accent} /></View>
        <Text style={{ fontSize: 13, color: p.dim }}>{checking ? tr('machines.dl.checking') : got.total ? tr('machines.dl.progress', { a: mb(got.done), b: mb(got.total) }) : mb(got.done)}</Text>
        {got.resumed > 0 && !checking ? <Text style={{ fontSize: 12, color: p.dim }}>{tr('machines.dl.resumed', { size: mb(got.resumed) })}</Text> : null}
        <Btn label={tr('machines.dl.cancel')} onClick={stopDownload} />
      </View>
    );
  } else body = (
    <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', gap: 12 }}>
      <OsMark id={sys.id} size={72} />
      <Text style={{ fontSize: 16, fontWeight: 700, color: p.text }}>{fail ? (downloading && !job ? tr('machines.dl.failed') : tr('machines.as.failed')) : source === 'iso' || downloading ? tr('machines.new.creating') : tr('machines.new.importing')}</Text>
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
      {picking ? <FilePicker title={source === 'iso' ? tr('machines.new.pickIso') : source === 'migrate' ? tr('machines.mig.pick') : tr('machines.new.pickImage')}
        filter={source === 'iso' ? /\.iso$/i : source === 'migrate' ? MIGRATE_FILES : /\.(qcow2|img|raw)$/i}
        onPick={(x) => { const pth = Array.isArray(x) ? x[0] : x; if (source === 'migrate') setPath(pth); else chose(pth); setPicking(false); }}
        onClose={() => { setPicking(false); if (source === 'migrate' && !path) setSource(null); }} /> : null}
    </View>
  );
}
