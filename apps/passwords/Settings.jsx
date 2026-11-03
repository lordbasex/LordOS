// Settings.jsx — vault settings (ways to unlock, recovery key, auto-lock, clipboard, Argon2id),
// import and export, and editing categories and tags. Each sheet is built from inset groups
// (Group) with the one primary action in the footer (docs/DESIGN.md).
import { View, Text, useTheme, useStore, Button, TextInput, Segmented, requestPermission, t, formatDate } from '@lordos/sdk';
import { useState } from 'react';
import { app, req, refreshStatus, reload, errorText, toast, setMeta, setCats, setTags, APP_ID } from './api.js';
import { allCategories, BUILTIN, ICONS, COLORS, FIELD_TYPES, catName, builtinById } from './templates.js';
import { Sheet, Label, Hint, Warning, FilePicker, Choice, CatIcon, StrengthBar, Group, Check, RED, GREEN, fileReq, fileName } from './ui.jsx';
import { Glyph, iconKind } from './glyphs.jsx';
import { RecoveryView } from './Lock.jsx';

// The glyph of each way to unlock (drawn: glyphs.jsx).
const slotIcon = { password: 'lock', keyfile: 'drive', passkey: 'drive', fido: 'key', recovery: 'note', session: 'person' };

// ---- settings ----

export function SettingsSheet({ onClose }) {
  const th = useTheme();
  const st = useStore(app);
  const [tab, setTab] = useState('unlock');
  const [add, setAdd] = useState(null);         // password | keyfile | passkey | fido | change
  const [pw, setPw] = useState('');
  const [pw2, setPw2] = useState('');
  const [old, setOld] = useState('');
  const [pin, setPin] = useState('');
  const [path, setPath] = useState('');
  const [picking, setPicking] = useState(false);
  const [rec, setRec] = useState(null);
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState('');
  const slots = st.status?.main?.slots || [];
  const meta = st.meta || {};
  const reset = () => { setAdd(null); setPw(''); setPw2(''); setOld(''); setPin(''); setPath(''); setErr(''); };
  const done = async (msg) => { await refreshStatus(); toast(msg); reset(); };
  const run = async (fn) => { setBusy(true); setErr(''); try { await fn(); } catch (e) { setErr(errorText(e.code)); } setBusy(false); };
  const submitAdd = () => run(async () => {
    if ((add === 'password' || add === 'passkey' || add === 'change') && (pw.length < 8 || pw !== pw2)) { setErr(pw.length < 8 ? t('passwords.create.short') : t('passwords.create.mismatch')); return; }
    if (add === 'change') { await req('password-change', { old, password: pw }); await done(t('passwords.set.changed')); return; }
    const args = { method: add, label: '' };
    let files;
    if (add === 'password' || add === 'passkey') args.password = pw;
    if (add === 'keyfile' || add === 'passkey') { if (!path) { setErr(t('passwords.create.needKeyPath')); return; } args.newkey = true; files = fileReq([path, 'w']); }
    if (add === 'fido') {
      if (!(await requestPermission(APP_ID, 'usb'))) throw Object.assign(new Error(''), { code: 'fido-denied' });
      const a = lord.vault.fidoAttach(APP_ID);
      if (!a.ok) throw Object.assign(new Error(''), { code: a.error });
      if (pin) args.pin = pin;
    }
    await req('slot-add', args, files);
    await done(t('passwords.set.added'));
  });
  if (rec) return <Sheet title={t('passwords.recovery.title')} onClose={() => setRec(null)} width={600}><RecoveryView code={rec} onDone={() => setRec(null)} /></Sheet>;
  return (
    <Sheet title={t('passwords.set.title')} onClose={onClose} width={560}>
      <Segmented appearance="neutral" fill value={tab} onChange={setTab} options={[['unlock', t('passwords.set.tabUnlock')], ['prefs', t('passwords.set.tabPrefs')]]} />
      {tab === 'unlock' ? (
        <View style={{ gap: 10 }}>
          <Hint>{t('passwords.set.unlockHint')}</Hint>
          <Group>{slots.map((s) => (
            <View key={s.id} style={{ flexDirection: 'row', alignItems: 'center', gap: 10, minHeight: 48 }}>
              <Glyph kind={slotIcon[s.type] || 'key'} size={16} color={th.accent} />
              <View style={{ flex: 1, minWidth: 0 }}>
                <Text style={{ fontSize: 13, color: th.text, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{t(`passwords.method.${s.type}`)}{s.label ? ` · ${s.label}` : ''}</Text>
                <Text style={{ fontSize: 11, color: th.textDim, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>
                  {s.m ? t('passwords.set.kdf', { m: Math.round(s.m / 1024), t: s.t }) : ''}{s.created ? ` ${formatDate(new Date(s.created * 1000))}` : ''}
                </Text>
              </View>
              {s.type === 'recovery'
                ? <Button small label={t('passwords.set.newRecovery')} onClick={() => run(async () => { const r = await req('recovery-new'); await refreshStatus(); setRec(r.recovery); })} />
                : <Button small destructive label={t('passwords.set.remove')} onClick={() => run(async () => { await req('slot-del', { slot: s.id }); await done(t('passwords.set.removed')); })} />}
            </View>
          ))}</Group>
          {!add && (
            <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8 }}>
              {slots.some((s) => s.type === 'password') && <Button small label={t('passwords.set.change')} onClick={() => setAdd('change')} />}
              {!slots.some((s) => s.type === 'password') && <Button small label={t('passwords.set.addPassword')} onClick={() => setAdd('password')} />}
              <Button small label={t('passwords.set.addKeyfile')} onClick={() => setAdd('keyfile')} />
              <Button small label={t('passwords.set.addPasskey')} onClick={() => setAdd('passkey')} />
              <Button small label={t('passwords.set.addFido')} onClick={() => setAdd('fido')} />
            </View>
          )}
          {add && (
            <View style={{ gap: 10, padding: 14, borderRadius: 10, border: `1px solid ${th.separator}` }}>
              <Text style={{ fontSize: 13, fontWeight: 600, color: th.text }}>{t(`passwords.set.adding.${add}`)}</Text>
              <Hint>{t(`passwords.create.about.${add === 'change' ? 'password' : add}`)}</Hint>
              {add === 'change' && <TextInput value={old} onChange={setOld} secure placeholder={t('passwords.set.oldPassword')} />}
              {(add === 'password' || add === 'passkey' || add === 'change') && <>
                <TextInput value={pw} onChange={setPw} secure placeholder={t('passwords.create.password')} />
                <TextInput value={pw2} onChange={setPw2} secure placeholder={t('passwords.create.repeat')} />
              </>}
              {(add === 'keyfile' || add === 'passkey') && (
                <View style={{ flexDirection: 'row', gap: 8, alignItems: 'center' }}>
                  <Glyph kind="drive" size={14} color={path ? th.accent : th.textDim} />
                  <Text style={{ flex: 1, minWidth: 0, fontSize: 12, color: path ? th.text : th.textDim, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{fileName(path) || t('passwords.create.keyWhere')}</Text>
                  <Button small label={t('passwords.create.chooseKeyPath')} onClick={() => setPicking(true)} />
                </View>
              )}
              {add === 'passkey' && <Warning>{t('passwords.set.passkeyWarn')}</Warning>}
              {add === 'fido' && <><Hint>{st.touch ? t('passwords.fido.touch') : t('passwords.fido.insertCreate')}</Hint>
                <TextInput value={pin} onChange={setPin} secure placeholder={t('passwords.fido.pinOptional')} /></>}
              {err ? <Text style={{ fontSize: 12, color: RED }}>{err}</Text> : null}
              <View style={{ flexDirection: 'row', gap: 8, justifyContent: 'flex-end' }}>
                <Button label={t('passwords.cancel')} onClick={reset} />
                <Button primary label={busy ? (st.touch ? t('passwords.fido.touchShort') : t('passwords.saving')) : t('passwords.save')} onClick={submitAdd} />
              </View>
            </View>
          )}
          {!add && err ? <Text style={{ fontSize: 12, color: RED }}>{err}</Text> : null}
        </View>
      ) : (
        <View style={{ gap: 12 }}>
          <Group>
          <Row label={t('passwords.set.autolock')} hint={t('passwords.set.autolockHint')}>
            <Choice value={meta.autolock ?? 300} width={190} onChange={(autolock) => setMeta({ autolock }).catch((e) => toast(errorText(e.code)))} options={[
              [60, t('passwords.set.min', { n: 1 })], [300, t('passwords.set.min', { n: 5 })], [900, t('passwords.set.min', { n: 15 })],
              [3600, t('passwords.set.hour')], [0, t('passwords.set.never')]]} />
          </Row>
          <Row label={t('passwords.set.clip')} hint={t('passwords.set.clipHint')}>
            <Choice value={meta.clip ?? 30} width={190} onChange={(clip) => setMeta({ clip }).catch(() => {})} options={[
              [15, t('passwords.set.sec', { n: 15 })], [30, t('passwords.set.sec', { n: 30 })], [60, t('passwords.set.sec', { n: 60 })], [120, t('passwords.set.sec', { n: 120 })]]} />
          </Row>
          <Row label={t('passwords.set.calibrate')} hint={t('passwords.set.calibrateHint')}>
            <Button small label={busy ? t('passwords.saving') : t('passwords.set.measure')} onClick={() => run(async () => {
              const r = await req('calibrate', { targetMs: 700 });
              toast(t('passwords.set.measured', { m: Math.round(r.m / 1024), t: r.t, ms: r.ms }));
            })} />
          </Row>
          </Group>
          <Hint>{t('passwords.set.memory', { kb: Math.round((st.status?.mem?.inUse || 0) / 1024), locked: Math.round((st.status?.mem?.locked || 0) / 1024) })}</Hint>
        </View>
      )}
      {picking && <FilePicker save suggest="passwords.key" title={t('passwords.create.chooseKeyPath')} onClose={() => setPicking(false)} onPick={(p) => { setPath(p); setPicking(false); }} />}
    </Sheet>
  );
}

// A settings row: the label (and a hint under it) at the start, the control at the end.
function Row({ label, hint, children }) {
  const th = useTheme();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, paddingVertical: 8, minHeight: 36 }}>
      <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
        <Text style={{ fontSize: 13, color: th.text }}>{label}</Text>
        {hint ? <Text style={{ fontSize: 11, color: th.textDim, lineHeight: 15 }}>{hint}</Text> : null}
      </View>
      {children}
    </View>
  );
}

// ---- import ----

export function ImportSheet({ onClose }) {
  const th = useTheme();
  const [file, setFile] = useState('');
  const [picking, setPicking] = useState(null);       // 'file' | 'key'
  const [pw, setPw] = useState('');
  const [key, setKey] = useState('');
  const [busy, setBusy] = useState(false);
  const [res, setRes] = useState(null);
  const [err, setErr] = useState('');
  const isKdbx = /\.kdbx$/i.test(fileName(file));
  const isVault = /\.lvault$/i.test(fileName(file));
  const go = async () => {
    if (!file) return;
    setBusy(true); setErr(''); setRes(null);
    try {
      const files = fileReq([file, 'r'], [key, 'r']);
      const name = fileName(file).split('/').pop();
      const r = isVault ? await req('import-vault', { password: pw, recovery: '' }, files)
        : await req('import', { name, password: pw || undefined }, files);
      setRes(r);
      await reload();
    } catch (e) { setErr(errorText(e.code)); }
    // app process: a picked file is an fd that the request already used up; pick again to retry
    if (typeof file !== 'string') { setFile(''); setKey(''); }
    setBusy(false);
  };
  return (
    <Sheet title={t('passwords.imp.title')} onClose={onClose} width={540} footer={[
      <Button key="c" label={t('passwords.close')} onClick={onClose} />,
      <Button key="g" primary label={busy ? t('passwords.imp.working') : t('passwords.imp.button')} onClick={go} />]}>
      <Hint>{t('passwords.imp.formats')}</Hint>
      <View style={{ height: 40, flexDirection: 'row', gap: 8, alignItems: 'center', paddingInlineStart: 12, paddingInlineEnd: 6, borderRadius: 8, border: `1px solid ${th.separator}` }}>
        <Glyph kind="note" size={15} color={file ? th.accent : th.textDim} />
        <Text style={{ flex: 1, minWidth: 0, fontSize: 12, color: file ? th.text : th.textDim, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{fileName(file) || t('passwords.imp.noFile')}</Text>
        <Button small label={t('passwords.imp.choose')} onClick={() => setPicking('file')} />
      </View>
      {(isKdbx || isVault) && (
        <View style={{ gap: 8 }}>
          <TextInput value={pw} onChange={setPw} secure placeholder={isVault ? t('passwords.imp.vaultPassword') : t('passwords.imp.kdbxPassword')} onSubmit={go} />
          <View style={{ flexDirection: 'row', gap: 8, alignItems: 'center' }}>
            <Text style={{ flex: 1, minWidth: 0, fontSize: 12, color: key ? th.text : th.textDim, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{fileName(key) || t('passwords.imp.noKey')}</Text>
            <Button small label={t('passwords.imp.chooseKey')} onClick={() => setPicking('key')} />
          </View>
        </View>
      )}
      {isKdbx && <Hint>{t('passwords.imp.kdbxHint')}</Hint>}
      {err ? <Text style={{ fontSize: 12, color: RED }}>{err}</Text> : null}
      {res && (
        <View style={{ flexDirection: 'row', gap: 10, paddingHorizontal: 12, paddingVertical: 10, borderRadius: 10, backgroundColor: 'rgba(48,209,88,0.12)',
          border: '1px solid rgba(48,209,88,0.35)' }}>
          <Text style={{ fontSize: 13, fontWeight: 700, color: GREEN, lineHeight: 18 }}>✓</Text>
          <View style={{ flex: 1, gap: 2 }}>
          <Text style={{ fontSize: 13, fontWeight: 600, color: th.text, lineHeight: 18 }}>{t('passwords.imp.done', { count: res.count })}</Text>
          {res.format && <Text style={{ fontSize: 12, color: th.textDim }}>{t('passwords.imp.detected', { format: t(`passwords.fmt.${res.format}`) })}</Text>}
          {res.cats ? <Text style={{ fontSize: 12, color: th.textDim }}>{t('passwords.imp.cats', { count: res.cats })}</Text> : null}
          </View>
        </View>
      )}
      {picking && <FilePicker title={picking === 'file' ? t('passwords.imp.choose') : t('passwords.imp.chooseKey')} onClose={() => setPicking(null)}
        filter={picking === 'file' ? (n) => /\.(csv|json|kdbx|lvault)$/i.test(n) : null}
        onPick={(p) => { if (picking === 'file') setFile(p); else setKey(p); setPicking(null); }} />}
    </Sheet>
  );
}

// ---- export ----

export function ExportSheet({ onClose }) {
  const th = useTheme();
  const [kind, setKind] = useState('vault');
  const [ok, setOk] = useState(false);
  const [picking, setPicking] = useState(false);
  const [err, setErr] = useState('');
  const go = async (p) => {
    setPicking(false); setErr('');
    try {
      const r = await req(kind === 'csv' ? 'export-csv' : 'export-vault', {}, fileReq([p, 'w']));
      toast(kind === 'csv' ? t('passwords.exp.doneCsv', { count: r.count }) : t('passwords.exp.doneVault'));
      onClose();
    } catch (e) { setErr(errorText(e.code)); }
  };
  return (
    <Sheet title={t('passwords.exp.title')} onClose={onClose} width={520} footer={[
      <Button key="c" label={t('passwords.cancel')} onClick={onClose} />,
      <Button key="g" primary disabled={kind === 'csv' && !ok} label={t('passwords.exp.button')} onClick={() => (kind === 'vault' || ok) && setPicking(true)} />]}>
      <Segmented appearance="neutral" fill value={kind} onChange={(k) => { setKind(k); setOk(false); }} options={[['vault', t('passwords.exp.vault')], ['csv', t('passwords.exp.csv')]]} />
      {kind === 'vault' ? <Hint>{t('passwords.exp.vaultHint')}</Hint> : (
        <View style={{ gap: 8 }}>
          <Warning>{t('passwords.exp.csvWarning')}</Warning>
          <Check on={ok} onChange={setOk} color={RED} label={t('passwords.exp.csvConfirm')} />
        </View>
      )}
      {err ? <Text style={{ fontSize: 12, color: RED }}>{err}</Text> : null}
      {picking && <FilePicker save suggest={kind === 'csv' ? 'passwords.csv' : 'passwords-backup.lvault'} title={t('passwords.exp.title')} onClose={() => setPicking(false)} onPick={go} />}
    </Sheet>
  );
}

// ---- categories ----

export function CategorySheet({ cat, onClose }) {
  const th = useTheme();
  const st = useStore(app);
  const cats = allCategories(st.cats);
  const isNew = !cat;
  const [name, setName] = useState(cat?.name || '');
  const [icon, setIcon] = useState(iconKind(cat?.icon));
  const [color, setColor] = useState(cat?.color || COLORS[0]);
  const [parent, setParent] = useState(cat?.parent || '');
  const [template, setTemplate] = useState(cat?.template || 'general');
  const [fields, setFields] = useState(cat?.fields || []);
  const [err, setErr] = useState('');
  const save = async () => {
    if (!name.trim()) { setErr(t('passwords.cats.needName')); return; }
    const id = cat?.id || `u${Date.now().toString(36)}`;
    const c = { id, name: name.trim(), icon, color, template, ...(parent ? { parent } : {}), ...(fields.length ? { fields } : {}) };
    const list = (st.cats || []).filter((x) => x.id !== id).concat([c]);
    try { await setCats(list); toast(t('passwords.cats.saved')); onClose(); } catch (e) { setErr(errorText(e.code)); }
  };
  const remove = async () => {
    const used = st.entries.some((e) => e.cat === cat.id);
    if (used) { setErr(t('passwords.cats.inUse')); return; }
    try { await setCats((st.cats || []).filter((x) => x.id !== cat.id && x.parent !== cat.id)); onClose(); } catch (e) { setErr(errorText(e.code)); }
  };
  const parents = [['', t('passwords.cats.noParent')], ...cats.filter((c) => c.id !== cat?.id).map((c) => [c.id, catName(c)])];
  return (
    <Sheet title={isNew ? t('passwords.cats.new') : t('passwords.cats.edit')} onClose={onClose} width={540} footer={[
      !isNew && <Button key="d" destructive label={t('passwords.cats.delete')} onClick={remove} />, !isNew && <View key="sp" style={{ flex: 1 }} />,
      <Button key="c" label={t('passwords.cancel')} onClick={onClose} />,
      <Button key="s" primary label={t('passwords.save')} onClick={save} />].filter(Boolean)}>
      <View style={{ flexDirection: 'row', gap: 12, alignItems: 'center' }}>
        <CatIcon cat={{ icon, color }} size={36} />
        <TextInput value={name} onChange={setName} placeholder={t('passwords.cats.name')} autoFocus style={{ flex: 1, minWidth: 0 }} />
      </View>
      <View style={{ gap: 6 }}>
        <Label>{t('passwords.cats.icon')}</Label>
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 4 }}>
          {ICONS.map((i) => <View key={i} onClick={() => setIcon(i)} style={{ width: 32, height: 32, borderRadius: 7, alignItems: 'center', justifyContent: 'center',
            backgroundColor: i === icon ? th.accent : th.control }}>
            <Glyph kind={i} size={16} color={i === icon ? '#fff' : th.text} /></View>)}
        </View>
      </View>
      <View style={{ gap: 6 }}>
        <Label>{t('passwords.cats.color')}</Label>
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8 }}>
          {COLORS.map((c) => <View key={c} onClick={() => setColor(c)} style={{ width: 22, height: 22, borderRadius: 11, alignItems: 'center', justifyContent: 'center',
            border: c === color ? `2px solid ${th.accent}` : '2px solid transparent' }}>
            <View style={{ width: 16, height: 16, borderRadius: 8, backgroundColor: c }} /></View>)}
        </View>
      </View>
      <View style={{ flexDirection: 'row', gap: 12 }}>
        <View style={{ flex: 1, minWidth: 0, gap: 6 }}><Label>{t('passwords.cats.parent')}</Label><Choice value={parent} options={parents} onChange={setParent} width={220} /></View>
        <View style={{ flex: 1, minWidth: 0, gap: 6 }}><Label>{t('passwords.cats.template')}</Label>
          <Choice value={template} options={BUILTIN.map((b) => [b.id, t(`passwords.cat.${b.id}`)])} onChange={setTemplate} width={220} /></View>
      </View>
      <Label>{t('passwords.cats.fields')}</Label>
      <Hint>{t('passwords.cats.fieldsHint')}</Hint>
      {fields.map((f, i) => (
        <View key={i} style={{ flexDirection: 'row', gap: 6, alignItems: 'center' }}>
          <TextInput value={f.l || ''} onChange={(l) => setFields(fields.map((x, k) => (k === i ? { ...x, l } : x)))} placeholder={t('passwords.edit.fieldName')} style={{ flex: 1, minWidth: 0 }} />
          <Choice value={f.multiline ? 'multiline' : f.t} options={FIELD_TYPES.map((x) => [x, t(`passwords.ftype.${x}`)])} width={150}
            onChange={(ty) => setFields(fields.map((x, k) => (k === i ? { ...x, t: ty === 'multiline' ? 'text' : ty, multiline: ty === 'multiline' } : x)))} />
          <Button small plain label="✕" onClick={() => setFields(fields.filter((_, k) => k !== i))} />
        </View>
      ))}
      <Button small icon="+" label={t('passwords.edit.addFieldDots')} style={{ alignSelf: 'flex-start' }}
        onClick={() => setFields([...fields, { k: `c${fields.length + 1}`, t: 'text', l: '' }])} />
      {err ? <Text style={{ fontSize: 12, color: RED }}>{err}</Text> : null}
    </Sheet>
  );
}

// ---- tags ----

export function TagSheet({ tag, onClose }) {
  const st = useStore(app);
  const th = useTheme();
  const cur = (st.tags || []).find((x) => x.name === tag) || { name: tag };
  const [name, setName] = useState(tag);
  const [color, setColor] = useState(cur.color || COLORS[0]);
  const save = async () => {
    try {
      if (name.trim() && name.trim() !== tag) await req('rename-tag', { from: tag, to: name.trim() });
      await setTags((st.tags || []).filter((x) => x.name !== tag && x.name !== name.trim()).concat([{ name: name.trim() || tag, color }]));
      await reload();
      onClose();
    } catch (e) { toast(errorText(e.code)); }
  };
  const remove = async () => { try { await req('rename-tag', { from: tag, to: '' }); await setTags((st.tags || []).filter((x) => x.name !== tag)); await reload(); onClose(); } catch (e) { toast(errorText(e.code)); } };
  return (
    <Sheet title={t('passwords.tags.edit')} onClose={onClose} width={420} footer={[
      <Button key="d" destructive label={t('passwords.tags.delete')} onClick={remove} />, <View key="sp" style={{ flex: 1 }} />,
      <Button key="c" label={t('passwords.cancel')} onClick={onClose} />, <Button key="s" primary label={t('passwords.save')} onClick={save} />]}>
      <TextInput value={name} onChange={setName} autoFocus />
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8 }}>
        {COLORS.map((c) => <View key={c} onClick={() => setColor(c)} style={{ width: 22, height: 22, borderRadius: 11, alignItems: 'center', justifyContent: 'center',
          border: c === color ? `2px solid ${th.accent}` : '2px solid transparent' }}>
          <View style={{ width: 16, height: 16, borderRadius: 8, backgroundColor: c }} /></View>)}
      </View>
    </Sheet>
  );
}

export { builtinById, StrengthBar };
