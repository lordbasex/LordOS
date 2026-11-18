// dialogs.jsx — the Disks dialogs: format, rename, erase and create a bootable USB.
//
// Everything that erases data goes through the same confirmation: it says exactly what is lost
// (model, size, each partition with its label, file system and mount point) and, for a whole
// disk, asks to type its name (sdb). The helper validates everything again on the root side.
import { View, Text, ScrollView, Button, TextInput, Segmented, useTheme, useLocale, admin, fs, formatBytes, formatNumber } from '@lordos/sdk';
import { useMemo, useState } from 'react';
import { PartGlyph } from './icons.jsx';
import { fsName, diskTitle, partTitle } from './index.jsx';

const RED = '#ff453a';

function Card({ title, children }) {
  const t = useTheme();
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.35)', alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: 480, maxWidth: '94%', maxHeight: '90%', borderRadius: 12, backgroundColor: t.windowBg, border: `1px solid ${t.separator}`,
        boxShadow: '0 12px 40px rgba(0,0,0,0.45)', padding: 20, gap: 12 }}>
        <Text style={{ fontSize: 15, fontWeight: 600, color: t.text, textAlign: 'start' }}>{title}</Text>
        {children}
      </View>
    </View>
  );
}

// What will be erased, with all its details.
function WhatErased({ disk, part }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const parts = part ? [part] : disk.partitions;
  return (
    <View style={{ borderRadius: 8, backgroundColor: 'rgba(255,69,58,0.10)', border: `1px solid rgba(255,69,58,0.35)`, padding: 10, gap: 6 }}>
      <Text style={{ fontSize: 12, fontWeight: 600, color: t.text, textAlign: 'start' }}>
        {part ? tr('disks.confirm.part', { part: part.name, disk: diskTitle(disk), size: formatBytes(part.size) })
          : tr('disks.confirm.disk', { disk: disk.name, model: diskTitle(disk), size: formatBytes(disk.size) })}
      </Text>
      {parts.length ? parts.map((p) => (
        <View key={p.name} style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
          <PartGlyph fstype={p.fstype} size={10} />
          <Text style={{ flex: 1, fontSize: 11, color: t.text, textAlign: 'start' }}>
            {[p.name, partTitle(p, tr), fsName(p.fstype) || tr('disks.noFs'), formatBytes(p.size),
              p.mountpoint ? tr('disks.confirm.mountedAt', { path: p.mountpoint }) : '',
              p.used != null ? tr('disks.confirm.usedData', { used: formatBytes(p.used) }) : ''].filter(Boolean).join(' · ')}
          </Text>
        </View>
      )) : <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>{disk.fstype ? `${fsName(disk.fstype)}${disk.label ? ` · ${disk.label}` : ''}` : tr('disks.confirm.empty')}</Text>}
      <Text style={{ fontSize: 11, color: RED, textAlign: 'start' }}>{tr('disks.confirm.irreversible')}</Text>
    </View>
  );
}

// Common final step: what gets erased, the typed name (whole disk) and the red button.
function Confirm({ disk, part, action, onBack, onGo }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [typed, setTyped] = useState('');
  const need = !part;
  const ok = !need || typed.trim() === disk.name;
  return (
    <View style={{ gap: 10 }}>
      <WhatErased disk={disk} part={part} />
      {need ? (
        <View style={{ gap: 6 }}>
          <Text style={{ fontSize: 12, color: t.text, textAlign: 'start' }}>{tr('disks.confirm.typeName', { name: disk.name })}</Text>
          <TextInput value={typed} onChange={setTyped} placeholder={disk.name} autoFocus onSubmit={() => ok && onGo()} />
        </View>
      ) : null}
      <View style={{ flexDirection: 'row', gap: 8, justifyContent: 'flex-end', marginTop: 4 }}>
        <Button label={tr('disks.back')} onClick={onBack} />
        <Button label={action} onClick={() => ok && onGo()} style={{ backgroundColor: ok ? RED : t.control, opacity: ok ? 1 : 0.5 }} />
      </View>
    </View>
  );
}

function Progress({ p }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  if (!p) return null;
  const frac = p.total ? p.done / p.total : null;
  return (
    <View style={{ gap: 6 }}>
      <Text style={{ fontSize: 12, color: t.text, textAlign: 'start' }}>{tr(`disks.phase.${p.phase}`)}</Text>
      {frac != null ? (
        <View style={{ direction: 'ltr', height: 8, borderRadius: 4, backgroundColor: t.control, overflow: 'hidden', flexDirection: 'row' }}>
          <View style={{ flexGrow: frac, backgroundColor: t.accent }} />
          <View style={{ flexGrow: 1 - frac }} />
        </View>
      ) : null}
      {frac != null ? <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>
        {tr('disks.progress', { done: formatBytes(p.done), total: formatBytes(p.total), pct: formatNumber(Math.floor(frac * 100), 0) })}</Text> : null}
    </View>
  );
}

const parseProgress = (line) => {
  if (!line.startsWith('progress ')) return null;
  const o = {};
  for (const kv of line.slice(9).split(' ')) { const [k, v] = kv.split('='); o[k] = k === 'phase' ? v : Number(v); }
  return o;
};

// Runs a long action with progress; ends with the result text or the error.
function useJob() {
  const [state, setState] = useState({ running: false, progress: null, result: null, error: null });
  const start = (action, arg) => {
    setState({ running: true, progress: null, result: null, error: null });
    admin.run(action, arg, (line) => { const p = parseProgress(line); if (p) setState((s) => ({ ...s, progress: p })); }, { quiet: true })
      .then((r) => setState((s) => ({ ...s, running: false, result: r || 'ok' })), (e) => setState((s) => ({ ...s, running: false, error: e.message })));
  };
  return [state, start];
}

function Result({ job, okText, onClose, onCancel }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  return (
    <View style={{ gap: 10 }}>
      {job.running ? <Progress p={job.progress} /> : null}
      {job.running && !job.progress ? <Text style={{ fontSize: 12, color: t.textDim, textAlign: 'start' }}>{tr('disks.working')}</Text> : null}
      {job.error ? <Text style={{ fontSize: 12, color: RED, textAlign: 'start' }}>{job.error}</Text> : null}
      {job.result ? <Text style={{ fontSize: 12, color: '#30d158', textAlign: 'start' }}>{okText(job.result)}</Text> : null}
      <View style={{ flexDirection: 'row', gap: 8, justifyContent: 'flex-end', marginTop: 4 }}>
        {job.running && onCancel ? <Button label={tr('disks.cancel')} onClick={onCancel} /> : null}
        {!job.running ? <Button primary label={tr('disks.done')} onClick={onClose} /> : null}
      </View>
    </View>
  );
}

function FormatDialog({ disk, part, tools, onClose }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const kinds = [['fat32', 'FAT32'], ['exfat', 'exFAT'], ['ext4', 'ext4'], ['ntfs', 'NTFS']].filter(([k]) => tools[k]);
  const [kind, setKind] = useState(kinds.length ? kinds[0][0] : '');
  const [label, setLabel] = useState('');
  const [step, setStep] = useState('choose');
  const [job, start] = useJob();
  const max = { fat32: 11, exfat: 15, ext4: 16, ntfs: 32 }[kind] || 11;
  const clean = kind === 'fat32' ? label.toUpperCase() : label;
  const badLabel = clean.length > max || (kind === 'fat32' ? /[^A-Z0-9 _-]/.test(clean) : /[\/\\:*?"<>|\x00-\x1f]/.test(clean)) || clean.startsWith('-');
  const go = () => { setStep('run'); start('disk-format', [part ? part.name : disk.name, kind, clean].join('\n')); };
  return (
    <Card title={tr('disks.formatTitle', { name: part ? partTitle(part, tr) : diskTitle(disk) })} onClose={step === 'run' && job.running ? null : onClose}>
      {step === 'choose' ? (
        <View style={{ gap: 10 }}>
          {!kinds.length ? <Text style={{ fontSize: 12, color: RED, textAlign: 'start' }}>{tr('disks.noTools')}</Text> : (
            <View style={{ gap: 10 }}>
              <Text style={{ fontSize: 12, color: t.textDim, textAlign: 'start' }}>{tr('disks.fsChoose')}</Text>
              <View style={{ flexDirection: 'row' }}><Segmented options={kinds} value={kind} onChange={setKind} /></View>
              <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>{tr(`disks.fsHint.${kind}`)}</Text>
              <TextInput value={label} onChange={setLabel} placeholder={tr('disks.labelPlaceholder', { n: max })} />
              {badLabel ? <Text style={{ fontSize: 11, color: RED, textAlign: 'start' }}>{tr('disks.badLabel', { n: max })}</Text> : null}
              {!part ? <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>{tr('disks.formatWholeHint')}</Text> : null}
            </View>
          )}
          <View style={{ flexDirection: 'row', gap: 8, justifyContent: 'flex-end', marginTop: 4 }}>
            <Button label={tr('disks.cancel')} onClick={onClose} />
            {kinds.length ? <Button primary label={tr('disks.continue')} onClick={() => !badLabel && setStep('confirm')} /> : null}
          </View>
        </View>
      ) : step === 'confirm' ? (
        <Confirm disk={disk} part={part} action={tr('disks.formatGo')} onBack={() => setStep('choose')} onGo={go} />
      ) : <Result job={job} okText={(r) => tr('disks.formatted', { name: r })} onClose={onClose} />}
    </Card>
  );
}

function EraseDialog({ disk, part, onClose }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [mode, setMode] = useState('quick');
  const [step, setStep] = useState('choose');
  const [job, start] = useJob();
  const go = () => { setStep('run'); start('disk-erase', `${part ? part.name : disk.name}\n${mode}`); };
  return (
    <Card title={tr('disks.eraseTitle', { name: part ? partTitle(part, tr) : diskTitle(disk) })} onClose={job.running ? null : onClose}>
      {step === 'choose' ? (
        <View style={{ gap: 10 }}>
          <View style={{ flexDirection: 'row' }}><Segmented options={[['quick', tr('disks.eraseQuick')], ['zero', tr('disks.eraseZero')]]} value={mode} onChange={setMode} /></View>
          <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>{tr(`disks.eraseHint.${mode}`)}</Text>
          <View style={{ flexDirection: 'row', gap: 8, justifyContent: 'flex-end', marginTop: 4 }}>
            <Button label={tr('disks.cancel')} onClick={onClose} />
            <Button primary label={tr('disks.continue')} onClick={() => setStep('confirm')} />
          </View>
        </View>
      ) : step === 'confirm' ? (
        <Confirm disk={disk} part={part} action={tr('disks.eraseGo')} onBack={() => setStep('choose')} onGo={go} />
      ) : <Result job={job} okText={() => tr('disks.erased')} onClose={onClose} onCancel={mode === 'zero' ? () => admin.run('disk-cancel', disk.name, null, { quiet: true }).catch(() => {}) : null} />}
    </Card>
  );
}

function LabelDialog({ part, onClose }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [label, setLabel] = useState(part.label || '');
  const [job, start] = useJob();
  const [started, setStarted] = useState(false);
  const fat = part.fstype === 'vfat';
  const max = { vfat: 11, exfat: 15, ntfs: 32 }[part.fstype] || 16;
  const clean = fat ? label.toUpperCase() : label;
  const bad = clean.length > max || (fat ? /[^A-Z0-9 _-]/.test(clean) : /[\/\\:*?"<>|\x00-\x1f]/.test(clean)) || clean.startsWith('-');
  const needUnmount = part.mountpoint && !/^ext/.test(part.fstype);
  return (
    <Card title={tr('disks.renameTitle', { name: partTitle(part, tr) })} onClose={job.running ? null : onClose}>
      {!started ? (
        <View style={{ gap: 10 }}>
          <TextInput value={label} onChange={setLabel} autoFocus placeholder={tr('disks.labelPlaceholder', { n: max })} onSubmit={() => !bad && !needUnmount && (setStarted(true), start('disk-label', `${part.name}\n${clean}`))} />
          {bad ? <Text style={{ fontSize: 11, color: RED, textAlign: 'start' }}>{tr('disks.badLabel', { n: max })}</Text> : null}
          {needUnmount ? <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>{tr('disks.renameUnmount')}</Text> : null}
          <View style={{ flexDirection: 'row', gap: 8, justifyContent: 'flex-end', marginTop: 4 }}>
            <Button label={tr('disks.cancel')} onClick={onClose} />
            <Button primary label={tr('disks.rename')} onClick={() => !bad && !needUnmount && (setStarted(true), start('disk-label', `${part.name}\n${clean}`))} />
          </View>
        </View>
      ) : <Result job={job} okText={(r) => tr('disks.renamed', { label: r })} onClose={onClose} />}
    </Card>
  );
}

function ImageDialog({ disk, onClose }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const home = fs.home();
  const found = useMemo(() => {
    const out = [];
    for (const ext of ['.iso', '.img']) {
      try { for (const f of fs.search(home, ext, { limit: 40, depth: 4 })) if (!f.dir && f.name.toLowerCase().endsWith(ext)) out.push(f); } catch (e) { /* no permission */ }
    }
    return out.sort((a, b) => (b.mtime || 0) - (a.mtime || 0)).slice(0, 12);
  }, [home]);
  const [path, setPath] = useState(found.length ? found[0].path : '');
  const [step, setStep] = useState('choose');
  const [job, start] = useJob();
  const full = path.replace(/^~(?=\/|$)/, home);
  const st = useMemo(() => { try { return full ? fs.stat(full) : null; } catch (e) { return null; } }, [full]);
  const tooBig = st && st.size > disk.size;
  const okFile = st && !st.dir && st.size > 0 && !tooBig;
  const go = () => { setStep('run'); start('disk-write-image', `${disk.name}\n${full}`); };
  return (
    <Card title={tr('disks.imageTitle', { name: diskTitle(disk) })} onClose={job.running ? null : onClose}>
      {step === 'choose' ? (
        <View style={{ gap: 10 }}>
          <Text style={{ fontSize: 12, color: t.textDim, textAlign: 'start' }}>{tr('disks.imageHint')}</Text>
          <TextInput value={path} onChange={setPath} placeholder={tr('disks.imagePath')} />
          {found.length ? (
            <ScrollView style={{ maxHeight: 150 }}>
              <View style={{ gap: 2 }}>
                {found.map((f) => (
                  <View key={f.path} onClick={() => setPath(f.path)} style={{ flexDirection: 'row', gap: 8, paddingVertical: 4, paddingHorizontal: 8, borderRadius: 6,
                    backgroundColor: f.path === full ? t.accent : 'transparent' }}>
                    <Text style={{ flex: 1, fontSize: 12, color: f.path === full ? '#fff' : t.text, whiteSpace: 'nowrap', textAlign: 'start' }}>{f.name}</Text>
                    <Text style={{ fontSize: 11, color: f.path === full ? '#fff' : t.textDim }}>{formatBytes(f.size)}</Text>
                  </View>
                ))}
              </View>
            </ScrollView>
          ) : <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>{tr('disks.imageNone')}</Text>}
          {st && tooBig ? <Text style={{ fontSize: 11, color: RED, textAlign: 'start' }}>{tr('disks.imageTooBig', { size: formatBytes(st.size), disk: formatBytes(disk.size) })}</Text> : null}
          {full && !st ? <Text style={{ fontSize: 11, color: RED, textAlign: 'start' }}>{tr('disks.imageMissing')}</Text> : null}
          <View style={{ flexDirection: 'row', gap: 8, justifyContent: 'flex-end', marginTop: 4 }}>
            <Button label={tr('disks.cancel')} onClick={onClose} />
            <Button primary label={tr('disks.continue')} onClick={() => okFile && setStep('confirm')} style={{ opacity: okFile ? 1 : 0.5 }} />
          </View>
        </View>
      ) : step === 'confirm' ? (
        <View style={{ gap: 8 }}>
          <Text style={{ fontSize: 12, color: t.text, textAlign: 'start' }}>{tr('disks.imageWillWrite', { file: fs.basename(full), size: formatBytes(st.size) })}</Text>
          <Confirm disk={disk} part={null} action={tr('disks.imageGo')} onBack={() => setStep('choose')} onGo={go} />
        </View>
      ) : <Result job={job} okText={(r) => tr('disks.imageDone', { sha: (/sha256=([0-9a-f]+)/.exec(r) || [])[1] || '', speed: (/mbps=([\d.]+)/.exec(r) || [])[1] || '' })}
        onClose={onClose} onCancel={() => admin.run('disk-cancel', disk.name, null, { quiet: true }).catch(() => {})} />}
    </Card>
  );
}

export function DiskDialog({ kind, disk, part, tools, onClose }) {
  if (kind === 'format') return <FormatDialog disk={disk} part={part} tools={tools} onClose={onClose} />;
  if (kind === 'erase') return <EraseDialog disk={disk} part={part} onClose={onClose} />;
  if (kind === 'label' && part) return <LabelDialog part={part} onClose={onClose} />;
  if (kind === 'image') return <ImageDialog disk={disk} onClose={onClose} />;
  return null;
}
