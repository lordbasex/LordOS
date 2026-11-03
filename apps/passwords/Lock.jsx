// Lock.jsx — creating the vault (choosing how it unlocks), unlocking it (password, key file,
// FIDO2 security key, password + key file, recovery key) and showing the recovery key to print
// or save. A centered column (360 px; 440 px for the forms with more choices), the padlock, a
// 22 px title, a neutral method switch and one primary button (docs/DESIGN.md).
import { View, Text, useTheme, useStore, Button, TextInput, Segmented, requestPermission, print, fs, t } from '@lordos/sdk';
import * as React from 'react';
import { useEffect, useState } from 'react';
import { app, req, refreshStatus, errorText, APP_ID, toast, appMode } from './api.js';
import { StrengthBar, Hint, Warning, FilePicker, Label, Check, RED, fileReq, fileName } from './ui.jsx';
import { Glyph } from './glyphs.jsx';

// The padlock of the lock screens, in the app icon's colors; the shackle is a neutral gray that
// reads on light and dark backgrounds.
function Padlock({ size = 64, open }) {
  const th = useTheme();
  const shackle = th.mode === 'dark' ? 'rgba(235,235,245,0.62)' : '#a8a8b0';
  return (
    <View style={{ width: size, height: size * 1.04, alignItems: 'center', direction: 'ltr' }}>
      <View style={{ width: size * 0.5, height: size * 0.5, borderRadius: size * 0.25, border: `${Math.max(3, Math.round(size * 0.085))}px solid ${shackle}`,
        marginBottom: -size * 0.25, marginLeft: open ? size * 0.34 : 0 }} />
      <View style={{ width: size * 0.8, height: size * 0.56, borderRadius: size * 0.13, background: 'linear-gradient(180deg, #38c6d4, #1f6fd1)',
        alignItems: 'center', justifyContent: 'center', boxShadow: '0 4px 12px rgba(0,0,0,0.22)' }}>
        <View style={{ width: size * 0.12, height: size * 0.12, borderRadius: size * 0.06, backgroundColor: '#fff' }} />
        <View style={{ width: size * 0.05, height: size * 0.12, marginTop: -1, borderRadius: size * 0.02, backgroundColor: '#fff' }} />
      </View>
    </View>
  );
}

// The header of a lock screen: picture, title and one line under it.
function Head({ pic, title, sub }) {
  const th = useTheme();
  return (
    <View style={{ alignItems: 'center', gap: 8, marginBottom: 4 }}>
      {pic}
      <Text style={{ marginTop: 6, fontSize: 22, fontWeight: 600, color: th.text, textAlign: 'center' }}>{title}</Text>
      {sub ? <Text style={{ fontSize: 13, color: th.textDim, textAlign: 'center', lineHeight: 18, maxWidth: 400 }}>{sub}</Text> : null}
    </View>
  );
}

// A picked file: its name (or a dim placeholder) and the button that picks it.
function FileRow({ file, empty, button, onPick }) {
  const th = useTheme();
  return (
    <View style={{ height: 36, flexDirection: 'row', gap: 8, alignItems: 'center', paddingInlineStart: 10, paddingInlineEnd: 4, borderRadius: 8,
      border: `1px solid ${th.separator}` }}>
      <Glyph kind="drive" size={14} color={file ? th.accent : th.textDim} />
      <Text style={{ flex: 1, minWidth: 0, fontSize: 12, color: file ? th.text : th.textDim, whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{fileName(file) || empty}</Text>
      <Button small label={button} onClick={onPick} />
    </View>
  );
}

// picker: a file picker shown in place of the form, under the header (the first child), so the
// lock screen stays recognisable instead of disappearing behind a dialog. In the app process the
// system dialog picks the file and the form stays.
function Shell({ children, wide, picker }) {
  const th = useTheme();
  // the file dialog (picker) covers the window itself: it is only mounted here
  return (
    <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', backgroundColor: th.windowBg, padding: 20 }}>
      <View style={{ width: wide ? 460 : 360, maxWidth: '100%', gap: 14, alignItems: 'stretch' }}>
        {children}
        {picker || null}
      </View>
    </View>
  );
}

// ---- unlock ----

export function UnlockView() {
  const th = useTheme();
  const st = useStore(app);
  const slots = st.status?.main?.slots || [];
  const has = (type) => slots.some((s) => s.type === type);
  const first = has('password') ? 'password' : has('passkey') ? 'passkey' : has('keyfile') ? 'keyfile' : has('fido') ? 'fido' : 'recovery';
  const [method, setMethod] = useState(first);
  const [pw, setPw] = useState('');
  const [rec, setRec] = useState('');
  const [pin, setPin] = useState('');
  const [keyfile, setKeyfile] = useState('');
  const [picking, setPicking] = useState(false);
  const [err, setErr] = useState('');
  const [busy, setBusy] = useState(false);
  const [ms, setMs] = useState(null);
  useEffect(() => { setMethod(first); }, [first]);
  const go = async () => {
    setErr(''); setBusy(true);
    app.set((s) => ({ ...s, touch: false }));
    try {
      const args = { method };
      let files;
      if (method === 'password' || method === 'passkey') args.password = pw;
      if (method === 'recovery') args.recovery = rec;
      if (method === 'keyfile' || method === 'passkey') {
        if (!keyfile) { setBusy(false); setErr(t('passwords.unlock.needKeyfile')); return; }
        files = fileReq([keyfile, 'r']);
      }
      if (method === 'fido') {
        const ok = await requestPermission(APP_ID, 'usb');
        if (!ok) throw Object.assign(new Error('denied'), { code: 'fido-denied' });
        const a = lord.vault.fidoAttach(APP_ID);
        if (!a.ok) throw Object.assign(new Error(a.error), { code: a.error });
        if (pin) args.pin = pin;
      }
      const r = await req('unlock', args, files);
      setMs(r.ms ?? null);
      setPw(''); setRec(''); setPin('');
      await refreshStatus();
    } catch (e) { setErr(errorText(e.code)); }
    if (keyfile && typeof keyfile !== 'string') setKeyfile('');     // an fd is used up: pick it again
    setBusy(false);
    app.set((s) => ({ ...s, touch: false }));
  };
  const opts = [];
  if (has('password')) opts.push(['password', t('passwords.method.password')]);
  if (has('passkey')) opts.push(['passkey', t('passwords.method.passkey')]);
  if (has('keyfile')) opts.push(['keyfile', t('passwords.method.keyfile')]);
  if (has('fido')) opts.push(['fido', t('passwords.method.fido')]);
  if (has('recovery')) opts.push(['recovery', t('passwords.method.recovery')]);
  return (
    <Shell picker={picking && <FilePicker title={t('passwords.unlock.chooseKeyfile')} onClose={() => setPicking(false)} onPick={(p) => { setKeyfile(p); setPicking(false); }} />}>
      <Head pic={<Padlock size={64} />} title={t('passwords.unlock.title')} sub={t('passwords.unlock.subtitle')} />
      {opts.length > 1 && <View style={{ alignItems: 'center' }}><Segmented appearance="neutral" options={opts} value={method} onChange={(m) => { setMethod(m); setErr(''); }} /></View>}
      {(method === 'password' || method === 'passkey') && (
        <TextInput value={pw} onChange={setPw} secure autoFocus placeholder={t('passwords.unlock.password')} onSubmit={go} />
      )}
      {(method === 'keyfile' || method === 'passkey') && (
        <FileRow file={keyfile} empty={t('passwords.unlock.noKeyfile')} button={t('passwords.unlock.chooseKeyfile')} onPick={() => setPicking(true)} />
      )}
      {method === 'fido' && (
        <View style={{ gap: 10 }}>
          <View style={{ flexDirection: 'row', gap: 10, alignItems: 'center', paddingHorizontal: 12, paddingVertical: 10, borderRadius: 8, backgroundColor: th.control }}>
            <Glyph kind="key" size={16} color={th.accent} />
            <Text style={{ flex: 1, fontSize: 12, color: th.text, lineHeight: 17 }}>{st.touch ? t('passwords.fido.touch') : t('passwords.fido.insert')}</Text>
          </View>
          <TextInput value={pin} onChange={setPin} secure placeholder={t('passwords.fido.pinOptional')} onSubmit={go} />
        </View>
      )}
      {method === 'recovery' && (
        <TextInput value={rec} onChange={setRec} placeholder="XXXXX-XXXXX-XXXXX-XXXXX-XXXXX-XXXXX-XXXXX-XXXXX" onSubmit={go} style={{ direction: 'ltr' }} />
      )}
      {err ? <Text style={{ fontSize: 12, color: RED, textAlign: 'center', lineHeight: 17 }}>{err}</Text> : null}
      <Button primary label={busy ? (st.touch ? t('passwords.fido.touchShort') : t('passwords.unlock.working')) : t('passwords.unlock.button')} onClick={go} />
      {ms != null && <Text style={{ fontSize: 11, color: th.textDim, textAlign: 'center' }}>{t('passwords.unlock.took', { ms })}</Text>}
      {st.status?.main?.unreadable && <Warning>{t('passwords.unlock.unreadable')}</Warning>}
    </Shell>
  );
}

// ---- create ----

export function CreateView() {
  const th = useTheme();
  const st = useStore(app);
  const [method, setMethod] = useState('password');
  const [pw, setPw] = useState('');
  const [pw2, setPw2] = useState('');
  const [score, setScore] = useState(null);
  const [keyPath, setKeyPath] = useState('');
  const [picking, setPicking] = useState(false);
  const [pin, setPin] = useState('');
  const [err, setErr] = useState('');
  const [busy, setBusy] = useState(false);
  const [recovery, setRecovery] = useState(null);
  useEffect(() => {
    if (!pw) { setScore(null); return; }
    let live = true;
    req('strength', { password: pw }).then((r) => live && setScore(r)).catch(() => {});
    return () => { live = false; };
  }, [pw]);
  if (recovery) return <RecoveryView code={recovery} onDone={() => { setRecovery(null); refreshStatus(); }} />;
  const needPw = method === 'password' || method === 'passkey';
  const needKey = method === 'keyfile' || method === 'passkey';
  const go = async () => {
    setErr('');
    if (needPw && pw.length < 8) { setErr(t('passwords.create.short')); return; }
    if (needPw && pw !== pw2) { setErr(t('passwords.create.mismatch')); return; }
    if (needKey && !keyPath) { setErr(t('passwords.create.needKeyPath')); return; }
    setBusy(true);
    try {
      const args = { method };
      let files;
      if (needPw) args.password = pw;
      if (needKey) { args.newkey = true; files = fileReq([keyPath, 'w']); }
      if (method === 'fido') {
        const ok = await requestPermission(APP_ID, 'usb');
        if (!ok) throw Object.assign(new Error('denied'), { code: 'fido-denied' });
        const a = lord.vault.fidoAttach(APP_ID);
        if (!a.ok) throw Object.assign(new Error(a.error), { code: a.error });
        if (pin) args.pin = pin;
      }
      const r = await req('create', args, files);
      setPw(''); setPw2('');
      setRecovery(r.recovery);
    } catch (e) { setErr(errorText(e.code)); }
    setBusy(false);
  };
  return (
    <Shell wide picker={picking && <FilePicker save suggest="passwords.key" title={t('passwords.create.chooseKeyPath')} onClose={() => setPicking(false)}
        onPick={(p) => { setKeyPath(p); setPicking(false); }} />}>
      <Head pic={<Padlock size={60} open />} title={t('passwords.create.title')} sub={t('passwords.create.subtitle')} />
      <View style={{ gap: 8, marginTop: 4 }}>
        <Label>{t('passwords.create.how')}</Label>
        <Segmented appearance="neutral" fill value={method} onChange={(m) => { setMethod(m); setErr(''); }} options={[
          ['password', t('passwords.method.password')], ['fido', t('passwords.method.fido')],
          ['keyfile', t('passwords.method.keyfile')], ['passkey', t('passwords.method.passkey')]]} />
        <Hint>{t(`passwords.create.about.${method}`)}</Hint>
      </View>
      {needPw && (
        <View style={{ gap: 8 }}>
          <TextInput value={pw} onChange={setPw} secure autoFocus placeholder={t('passwords.create.password')} />
          <TextInput value={pw2} onChange={setPw2} secure placeholder={t('passwords.create.repeat')} onSubmit={go} />
          {score && <StrengthBar score={score.score} bits={score.bits} />}
        </View>
      )}
      {needKey && (
        <FileRow file={keyPath} empty={t('passwords.create.keyWhere')} button={t('passwords.create.chooseKeyPath')} onPick={() => setPicking(true)} />
      )}
      {method === 'fido' && (
        <View style={{ gap: 8 }}>
          <Hint>{st.touch ? t('passwords.fido.touch') : t('passwords.fido.insertCreate')}</Hint>
          <TextInput value={pin} onChange={setPin} secure placeholder={t('passwords.fido.pinOptional')} />
        </View>
      )}
      {err ? <Text style={{ fontSize: 12, color: RED }}>{err}</Text> : null}
      <Button primary label={busy ? (st.touch ? t('passwords.fido.touchShort') : t('passwords.create.working')) : t('passwords.create.button')} onClick={go} />
      <Text style={{ fontSize: 11, color: th.textDim, textAlign: 'center', lineHeight: 15 }}>{t('passwords.create.local')}</Text>
    </Shell>
  );
}

// ---- recovery key ----

export function RecoveryView({ code, onDone }) {
  const th = useTheme();
  const [saving, setSaving] = useState(false);
  const [ok, setOk] = useState(false);
  const text = `${t('passwords.recovery.printTitle')}\n\n${code}\n\n${t('passwords.recovery.printBody')}\n`;
  return (
    <Shell wide picker={saving && <FilePicker save suggest="recovery-key.txt" title={t('passwords.recovery.save')} onClose={() => setSaving(false)}
        onPick={(p) => {
          try {
            if (typeof p === 'string') fs.write(p, text);
            else { const ok = lord.broker.fdWrite(p.fd, text); lord.broker.fdClose(p.fd); if (!ok) throw new Error('write'); }
            toast(t('passwords.recovery.saved'));
          } catch (e) { toast(errorText('file')); }
          setSaving(false);
        }} />}>
      <Head pic={<View style={{ width: 56, height: 56, borderRadius: 14, alignItems: 'center', justifyContent: 'center', background: 'linear-gradient(180deg, #38c6d4, #1f6fd1)' }}>
        <Glyph kind="key" size={32} color="#fff" /></View>} title={t('passwords.recovery.title')} sub={t('passwords.recovery.body')} />
      <View style={{ paddingHorizontal: 16, paddingVertical: 14, borderRadius: 10, border: `1px solid ${th.separator}`,
        backgroundColor: th.mode === 'dark' ? 'rgba(255,255,255,0.05)' : 'rgba(0,0,0,0.03)', alignItems: 'center', direction: 'ltr' }}>
        <Text style={{ fontSize: 15, fontWeight: 600, color: th.text, fontFamily: 'monospace', textAlign: 'center', lineHeight: 22 }}>{code}</Text>
      </View>
      <View style={{ flexDirection: 'row', gap: 8, justifyContent: 'center' }}>
        {!appMode && <Button small label={t('passwords.recovery.print')} onClick={() => print.open({ title: t('passwords.recovery.printTitle'), text })} />}
        <Button small label={t('passwords.recovery.save')} onClick={() => setSaving(true)} />
      </View>
      <Check on={ok} onChange={setOk} label={t('passwords.recovery.confirm')} />
      <Button primary disabled={!ok} label={t('passwords.recovery.done')} onClick={() => ok && onDone()} />
    </Shell>
  );
}
