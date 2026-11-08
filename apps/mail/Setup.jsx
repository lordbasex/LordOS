// Setup.jsx — adding and managing accounts: the setup sheet (our provider table, app
// passwords, manual servers, CalDAV/CardDAV), the certificate warning, the password prompt and
// the accounts panel. Passwords go to the system keyring (Passwords), never to a file.
import { View, Text, useTheme, useStore, Button, TextInput, Segmented, Toggle, ScrollView, t, mailAccounts, mailStatus, mailConfig, autoconfig, OAUTH_PROVIDERS, DAV_PRESETS, davPreset } from '@lordos/sdk';
import { useEffect, useState } from 'react';

export function Sheet({ children, onClose, width = 460 }) {
  const th = useTheme();
  return (
    <View onMouseDown={(e) => { e.stopPropagation?.(); }} style={{ position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.32)', alignItems: 'center', justifyContent: 'center', zIndex: 50 }}>
      <View onClick={(e) => e.stopPropagation?.()} style={{ width, maxHeight: '92%', padding: 18, gap: 10, borderRadius: 14, backgroundColor: th.windowBg,
        border: `1px solid ${th.separator}`, boxShadow: '0 20px 50px rgba(0,0,0,0.45)' }}>
        {children}
        {onClose ? null : null}
      </View>
    </View>
  );
}

function Field({ label, children }) {
  const th = useTheme();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
      <Text style={{ width: 110, fontSize: 12, color: th.textDim, textAlign: 'end' }}>{label}</Text>
      <View style={{ flex: 1 }}>{children}</View>
    </View>
  );
}

function ServerFields({ value, onChange, label }) {
  const th = useTheme();
  const opts = [['ssl', 'SSL/TLS'], ['starttls', 'STARTTLS']];
  return (
    <View style={{ gap: 6 }}>
      <Text style={{ fontSize: 12, fontWeight: 600, color: th.text }}>{label}</Text>
      <Field label={t('mail.setup.server')}><TextInput value={value.host} onChange={(host) => onChange({ ...value, host })} placeholder="imap.example.com" /></Field>
      <Field label={t('mail.setup.port')}><TextInput value={String(value.port || '')} onChange={(p) => onChange({ ...value, port: Number(p.replace(/\D/g, '')) || 0 })} /></Field>
      <Field label={t('mail.setup.security')}>
        <Segmented options={opts} value={value.security} onChange={(security) => onChange({ ...value, security, port: value.port === 993 || value.port === 143 ? (security === 'ssl' ? 993 : 143) : value.port === 465 || value.port === 587 ? (security === 'ssl' ? 465 : 587) : value.port })} />
      </Field>
    </View>
  );
}

// Why the password was not kept in the keyring (sdk/mailauthmodel.js keyringTrouble), for the person
const keyringNote = (trouble) => t(`mail.password.${trouble === 'locked' ? 'notSavedLocked' : trouble === 'missing' ? 'notSavedMissing' : 'notSavedError'}`);

// Waits for the new account's first status: connected, wrong password, certificate or offline.
function useAccountResult(id) {
  const st = useStore(mailStatus);
  const s = id ? st.accounts[id]?.status : null;
  const cert = id ? st.certs.find((c) => c.acct === id) : null;
  const authFailed = id ? st.authFailed[id] : false;
  if (!id) return null;
  if (cert) return 'cert';
  if (authFailed || s === 'auth') return 'auth';
  if (s === 'idle') return 'ok';
  if (s === 'offline') return 'offline';
  return 'working';
}

// Calendars & Contacts: a generic CalDAV/CardDAV server, or a provider preset (DAV_PRESETS). A
// provider that needs OAuth shows "not available yet" instead of a form.
function DavFields({ dav, setDav, preset }) {
  const th = useTheme();
  const p = davPreset(preset);
  if (p.oauth && !OAUTH_PROVIDERS[p.oauth]?.enabled) {
    return (
      <View style={{ padding: 10, borderRadius: 8, backgroundColor: th.control, gap: 6 }}>
        <Text style={{ fontSize: 13, fontWeight: 600, color: th.text }}>{t('mail.setup.davNotYetTitle', { name: p.name })}</Text>
        <Text style={{ fontSize: 12, color: th.textDim }}>{t('mail.setup.davNotYet', { name: p.name })}</Text>
      </View>
    );
  }
  const toggles = (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 14, marginTop: 2 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
        <Toggle small on={dav.cal} onChange={(cal) => setDav({ ...dav, cal })} /><Text style={{ fontSize: 12, color: th.text }}>{t('mail.setup.syncCalendars')}</Text>
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
        <Toggle small on={dav.card} onChange={(card) => setDav({ ...dav, card })} /><Text style={{ fontSize: 12, color: th.text }}>{t('mail.setup.syncContacts')}</Text>
      </View>
    </View>
  );
  if (p.id !== 'other') {
    return (
      <View style={{ gap: 8 }}>
        <View style={{ padding: 10, borderRadius: 8, backgroundColor: th.control, gap: 4 }}>
          <Text style={{ fontSize: 12, color: th.text }}>{t('mail.setup.davAppPassword', { name: p.name })}</Text>
          {p.appPassword ? <Text style={{ fontSize: 11, color: th.accent }}>{p.appPassword}</Text> : null}
        </View>
        <Field label={t('mail.setup.davAccountEmail')}><TextInput value={dav.user} onChange={(user) => setDav({ ...dav, user })} placeholder={t('mail.setup.emailPh')} /></Field>
        {toggles}
      </View>
    );
  }
  return (
    <View style={{ gap: 8 }}>
      <Field label={t('mail.setup.url')}><TextInput value={dav.url} onChange={(url) => setDav({ ...dav, url })} placeholder="https://cloud.example.com/remote.php/dav" /></Field>
      <Field label={t('mail.setup.cardUrl')}><TextInput value={dav.carddav} onChange={(carddav) => setDav({ ...dav, carddav })} placeholder={t('mail.setup.cardUrlPh')} /></Field>
      <Field label={t('mail.setup.user')}><TextInput value={dav.user} onChange={(user) => setDav({ ...dav, user })} /></Field>
      {toggles}
    </View>
  );
}

export function AccountSetup({ onClose, kind: kind0 = 'mail', preset: preset0 = 'other' }) {
  const th = useTheme();
  const [kind, setKind] = useState(kind0);
  const [step, setStep] = useState('who');           // who → servers → result
  const [name, setName] = useState('');
  const [email, setEmail] = useState('');
  const [pw, setPw] = useState('');
  const [cfg, setCfg] = useState(null);
  const [cfgFor, setCfgFor] = useState('');            // the address cfg was found for (kept when the form comes back)
  const [manual, setManual] = useState(false);
  const [dav, setDav] = useState({ url: '', carddav: '', user: '', cal: true, card: true });
  const [preset, setPreset] = useState(DAV_PRESETS.some((p) => p.id === preset0) ? preset0 : 'other');
  const pre = davPreset(preset);
  const davBlocked = kind === 'dav' && !!pre.oauth && !OAUTH_PROVIDERS[pre.oauth]?.enabled;
  const [withDav, setWithDav] = useState(true);
  const [created, setCreated] = useState(null);
  const [createdAll, setCreatedAll] = useState([]);    // the account, and its calendars and contacts when added along
  const [err, setErr] = useState('');
  const result = useAccountResult(created);
  const resultError = useStore(mailStatus).accounts[created]?.error || '';
  const keyringError = useStore(mailStatus).keyringError;

  const next = () => {
    setErr('');
    if (kind === 'dav') return save();
    if (!/^[^@\s]+@[^@\s]+\.[^@\s]+$/.test(email.trim())) { setErr(t('mail.setup.badEmail')); return; }
    // back from a failed attempt with the same address: keep the servers as they were set
    if (cfg && cfgFor === email.trim()) { setStep('servers'); return; }
    const c = autoconfig(email.trim());
    setCfg({ ...c, user: email.trim() });
    setCfgFor(email.trim());
    setManual(!c.known);
    setStep('servers');
  };

  // The server said no (or could not be reached): the half-made account goes away and the form comes
  // back with everything as it was typed, to fix only the field that was wrong (issue #50).
  const discard = async () => {
    const ids = createdAll;
    setCreated(null);
    setCreatedAll([]);
    for (const id of ids) await mailAccounts.remove(id).catch(() => {});
  };
  const edit = async () => { await discard(); setErr(''); setStep('who'); };

  const save = async () => {
    setErr('');
    try {
      if (kind === 'dav') {
        if (davBlocked) return;
        if (!dav.cal && !dav.card) { setErr(t('mail.setup.davNothing')); return; }
        const user = dav.user.trim();
        if (!user) { setErr(t('mail.setup.davNoUser')); return; }
        if (pre.userIsEmail && !/^[^@\s]+@[^@\s]+\.[^@\s]+$/.test(user)) { setErr(t('mail.setup.badEmail')); return; }
        if (!pw) { setErr(t('mail.setup.davNoPassword')); return; }
        let url = pre.caldav || dav.url.trim(), carddav = pre.id === 'other' ? dav.carddav.trim() : pre.carddav || '';
        if (pre.id === 'other' && !dav.cal && carddav) url = carddav;        // contacts only, on their own address
        if (!/^https:\/\/[^/\s]+/i.test(url) || (carddav && !/^https:\/\/[^/\s]+/i.test(carddav))) { setErr(t('mail.setup.badUrl')); return; }
        const acct = { kind: 'dav', name: name.trim() || (pre.id === 'other' ? url.replace(/^https:\/\//i, '').split(/[/:]/)[0] : `${pre.name} — ${t('mail.setup.calContacts')}`),
          url, user, cal: !!dav.cal, card: !!dav.card };
        if (carddav && carddav !== url) acct.carddav = carddav;
        const id = await mailAccounts.add(acct, pw);
        setCreated(id);
        setCreatedAll([id]);
        setStep('result');
        return;
      }
      const acct = {
        kind: 'mail', name: cfg.known ? `${cfg.name}` : email.trim(), email: email.trim(), fullName: name.trim(), user: cfg.user.trim(),
        imap: cfg.imap, smtp: cfg.smtp, sentSaved: !!cfg.sentSaved, auth: 'password',
      };
      const id = await mailAccounts.add(acct, pw);
      const ids = [id];
      if (withDav && (cfg.caldav || cfg.carddav) && cfg.davNote !== 'oauth') {
        ids.push(await mailAccounts.add({ kind: 'dav', name: `${cfg.name} — ${t('mail.setup.calContacts')}`, url: cfg.caldav || cfg.carddav, user: cfg.user.trim(), cal: !!cfg.caldav, card: true,
          ...(cfg.caldav && cfg.carddav ? { carddav: cfg.carddav } : {}) }, pw));
      }
      setCreated(id);
      setCreatedAll(ids);
      setStep('result');
    } catch (e) { setErr(`${t('mail.setup.failed')} ${e.message || ''}`); }
  };

  const title = kind === 'dav' ? t('mail.setup.titleDav') : t('mail.setup.title');
  const failed = step === 'result' && !!created && (result === 'auth' || result === 'offline' || result === 'cert');
  return (
    <Sheet>
      <Text style={{ fontSize: 17, fontWeight: 700, color: th.text }}>{title}</Text>
      {step === 'who' && (
        <View style={{ gap: 8 }}>
          <Segmented options={[['mail', t('mail.setup.kindMail')], ['dav', t('mail.setup.kindDav')]]} value={kind} onChange={setKind} />
          {kind === 'dav' && (
            <Field label={t('mail.setup.davService')}>
              <Segmented small options={DAV_PRESETS.map((p) => [p.id, p.id === 'other' ? t('mail.setup.davOther') : p.name])} value={preset} onChange={(v) => { setPreset(v); setErr(''); }} />
            </Field>
          )}
          {!davBlocked && <Field label={t('mail.setup.name')}><TextInput value={name} onChange={setName} placeholder={kind === 'dav' ? (pre.id === 'other' ? t('mail.setup.davNamePh') : `${pre.name} — ${t('mail.setup.calContacts')}`) : t('mail.setup.namePh')} autoFocus /></Field>}
          {kind === 'mail' ? (
            <Field label={t('mail.setup.email')}><TextInput value={email} onChange={setEmail} placeholder={t('mail.setup.emailPh')} onSubmit={next} /></Field>
          ) : <DavFields dav={dav} setDav={setDav} preset={preset} />}
          {!davBlocked && <Field label={kind === 'dav' && pre.id !== 'other' ? t('mail.setup.appSpecificPassword') : t('mail.setup.password')}><TextInput value={pw} onChange={setPw} secure onSubmit={next} /></Field>}
          {!davBlocked && <Text style={{ fontSize: 11, color: th.textDim }}>{t('mail.setup.keyringNote')}</Text>}
        </View>
      )}
      {step === 'servers' && cfg && (
        <ScrollView style={{ maxHeight: 420 }}>
          <View style={{ gap: 10 }}>
            {cfg.known && (
              <View style={{ padding: 10, borderRadius: 8, backgroundColor: th.control, gap: 4 }}>
                <Text style={{ fontSize: 13, fontWeight: 600, color: th.text }}>{t('mail.setup.found', { name: cfg.name })}</Text>
                <Text style={{ fontSize: 12, color: th.textDim }}>{t('mail.setup.appPassword', { name: cfg.name })}</Text>
                {cfg.appPassword ? <Text style={{ fontSize: 11, color: th.accent }}>{cfg.appPassword}</Text> : null}
              </View>
            )}
            {cfg.oauth && (
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
                <Button label={t('mail.setup.oauth', { name: cfg.name })} disabled onClick={() => {}} />
                <Text style={{ flex: 1, fontSize: 11, color: th.textDim }}>{OAUTH_PROVIDERS[cfg.oauth]?.enabled ? '' : t('mail.setup.oauthNeedsId')}</Text>
              </View>
            )}
            {(cfg.caldav || cfg.carddav) && (
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
                <Toggle on={withDav && cfg.davNote !== 'oauth'} onChange={setWithDav} />
                <Text style={{ flex: 1, fontSize: 12, color: cfg.davNote === 'oauth' ? th.textDim : th.text }}>
                  {cfg.davNote === 'oauth' ? t('mail.setup.davOauth', { name: cfg.name }) : t('mail.setup.alsoDav', { name: cfg.name })}
                </Text>
              </View>
            )}
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
              <Toggle on={manual} onChange={setManual} />
              <Text style={{ fontSize: 12, color: th.text }}>{t('mail.setup.manual')}</Text>
            </View>
            {manual && (
              <>
                <Field label={t('mail.setup.user')}><TextInput value={cfg.user} onChange={(user) => setCfg({ ...cfg, user })} /></Field>
                <ServerFields label={t('mail.setup.incoming')} value={cfg.imap} onChange={(imap) => setCfg({ ...cfg, imap })} />
                <ServerFields label={t('mail.setup.outgoing')} value={cfg.smtp} onChange={(smtp) => setCfg({ ...cfg, smtp })} />
              </>
            )}
          </View>
        </ScrollView>
      )}
      {step === 'result' && (
        <View style={{ gap: 8, paddingVertical: 6 }}>
          <Text style={{ fontSize: 14, color: result === 'ok' ? '#30d158' : result === 'working' ? th.text : '#ff453a' }}>
            {t(`mail.setup.result.${result === 'ok' && kind === 'dav' ? 'okDav' : result || 'working'}`)}
          </Text>
          {result === 'auth' && <Text style={{ fontSize: 12, color: th.textDim }}>{t('mail.setup.authHelp')}</Text>}
          {(result === 'offline' || result === 'cert') && resultError ? <Text style={{ fontSize: 12, color: th.textDim }}>{resultError}</Text> : null}
          {failed && <Text style={{ fontSize: 12, color: th.textDim }}>{t('mail.setup.editHelp')}</Text>}
          {created && keyringError ? <Text style={{ fontSize: 12, color: '#ff9f0a' }}>{keyringNote(keyringError)}</Text> : null}
        </View>
      )}
      {err ? <Text style={{ fontSize: 12, color: '#ff453a' }}>{err}</Text> : null}
      <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 4 }}>
        {step === 'result' && created && result !== 'ok' ? <Button label={t('mail.setup.removeAccount')} onClick={async () => { await discard(); onClose(); }} /> : null}
        <Button label={step === 'result' ? t('mail.close') : t('mail.cancel')} onClick={onClose} />
        {failed ? <Button primary label={t('mail.setup.edit')} onClick={edit} /> : null}
        {step === 'who' && <Button primary disabled={davBlocked} label={kind === 'dav' ? t('mail.setup.add') : t('mail.setup.next')} onClick={next} />}
        {step === 'servers' && <Button primary label={t('mail.setup.add')} onClick={save} />}
      </View>
    </Sheet>
  );
}

// A certificate that did not verify: shown as is, never accepted without the user saying so.
export function CertDialog({ cert, onClose }) {
  const th = useTheme();
  const row = (k, v) => (
    <View style={{ flexDirection: 'row', gap: 8 }}>
      <Text style={{ width: 110, fontSize: 11, color: th.textDim, textAlign: 'end' }}>{k}</Text>
      <Text style={{ flex: 1, fontSize: 11, color: th.text, fontFamily: 'monospace' }}>{v}</Text>
    </View>
  );
  return (
    <Sheet width={520}>
      <Text style={{ fontSize: 16, fontWeight: 700, color: '#ff453a' }}>{t('mail.cert.title')}</Text>
      <Text style={{ fontSize: 13, color: th.text }}>{t('mail.cert.body', { host: `${cert.host}:${cert.port}` })}</Text>
      <View style={{ padding: 10, borderRadius: 8, backgroundColor: th.control, gap: 3 }}>
        {row(t('mail.cert.reason'), cert.reason)}
        {row(t('mail.cert.subject'), cert.subject)}
        {row(t('mail.cert.issuer'), cert.issuer)}
        {cert.notAfter ? row(t('mail.cert.expires'), cert.notAfter) : null}
        {row('SHA-256', cert.sha256)}
      </View>
      <Text style={{ fontSize: 12, color: th.textDim }}>{t('mail.cert.advice')}</Text>
      <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 8 }}>
        <Button label={t('mail.cancel')} onClick={onClose} />
        <Button label={t('mail.cert.trust')} onClick={() => { mailAccounts.trustCertificate(cert.acct, cert.smtp ? 'smtp' : mailAccounts.get(cert.acct)?.kind === 'dav' ? 'dav' : 'imap', cert.sha256); onClose(); }} />
      </View>
    </Sheet>
  );
}

export function PasswordPrompt({ acct, onClose }) {
  const th = useTheme();
  const [pw, setPw] = useState('');
  const [busy, setBusy] = useState(false);
  const [notSaved, setNotSaved] = useState(null);     // the keyring did not keep it: say so instead of closing
  const a = mailAccounts.get(acct);
  const go = async () => {
    setBusy(true);
    try { const trouble = await mailAccounts.setPassword(acct, pw); if (trouble) { setBusy(false); setNotSaved(trouble); } else onClose(); }
    catch (e) { setBusy(false); }
  };
  if (notSaved) {
    return (
      <Sheet>
        <Text style={{ fontSize: 16, fontWeight: 700, color: th.text }}>{t('mail.password.title')}</Text>
        <Text style={{ fontSize: 13, color: th.textDim }}>{keyringNote(notSaved)}</Text>
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 8 }}>
          <Button primary label={t('mail.close')} onClick={onClose} />
        </View>
      </Sheet>
    );
  }
  return (
    <Sheet>
      <Text style={{ fontSize: 16, fontWeight: 700, color: th.text }}>{t('mail.password.title')}</Text>
      <Text style={{ fontSize: 13, color: th.textDim }}>{t('mail.password.body', { user: a?.user || acct })}</Text>
      <TextInput value={pw} onChange={setPw} secure autoFocus onSubmit={go} onCancel={onClose} />
      <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 8 }}>
        <Button label={t('mail.cancel')} onClick={onClose} />
        <Button primary label={busy ? t('mail.working') : t('mail.password.save')} onClick={go} />
      </View>
    </Sheet>
  );
}

// The accounts: mail, and calendars and contacts (CalDAV/CardDAV). Calendar and Contacts open it on
// one of theirs (openMailAccounts), `confirm` straight on the question to remove it.
export function AccountsPanel({ onClose, onAdd, onPassword, initial = null, confirm: confirm0 = false }) {
  const th = useTheme();
  const st = useStore(mailStatus);
  const [list, setList] = useState(mailAccounts.list());
  const [sel, setSel] = useState(list.some((x) => x.id === initial) ? initial : list[0]?.id || null);
  const a = list.find((x) => x.id === sel);
  const [sig, setSig] = useState(a?.signature || '');
  const [confirm, setConfirm] = useState(confirm0 && !!a);
  const [busy, setBusy] = useState(false);
  useEffect(() => { setSig(a?.signature || ''); }, [sel]);
  const refresh = () => setList(mailAccounts.list());
  const cfg = mailConfig.get();
  // Removing: the account leaves the file, its password leaves the keyring, and lord-maild deletes
  // what it had downloaded (mail, calendars, contacts). Nothing is deleted on the server.
  const remove = async () => {
    setBusy(true);
    await mailAccounts.remove(a.id).catch(() => {});
    const l = mailAccounts.list();
    setList(l); setSel(l[0]?.id || null); setConfirm(false); setBusy(false);
  };
  if (confirm && a) {
    return (
      <Sheet width={420}>
        <Text style={{ fontSize: 16, fontWeight: 700, color: th.text }}>{t('mail.accounts.removeTitle', { name: a.name || a.email || a.user })}</Text>
        <Text style={{ fontSize: 13, color: th.textDim }}>{t(a.kind === 'dav' ? 'mail.accounts.removeBodyDav' : 'mail.accounts.removeBody')}</Text>
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 4 }}>
          <Button label={t('mail.cancel')} onClick={() => (confirm0 ? onClose() : setConfirm(false))} />
          <Button destructive disabled={busy} label={busy ? t('mail.working') : t('mail.accounts.removeConfirm')} onClick={async () => { await remove(); if (confirm0) onClose(); }} />
        </View>
      </Sheet>
    );
  }
  return (
    <Sheet width={620}>
      <Text style={{ fontSize: 17, fontWeight: 700, color: th.text }}>{t('mail.accounts.title')}</Text>
      <View style={{ flexDirection: 'row', gap: 12, height: 300 }}>
        <View style={{ width: 200, gap: 2, backgroundColor: th.sidebarBg, borderRadius: 8, padding: 6 }}>
          <ScrollView style={{ flex: 1 }}>
            {list.map((x) => (
              <View key={x.id} onClick={() => setSel(x.id)} style={{ padding: 6, borderRadius: 6, backgroundColor: x.id === sel ? th.accent : 'transparent' }}>
                <Text style={{ fontSize: 13, color: x.id === sel ? '#fff' : th.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{x.name || x.email}</Text>
                <Text style={{ fontSize: 11, color: x.id === sel ? '#ffffffcc' : th.textDim }}>{x.kind === 'dav' ? t('mail.setup.kindDav') : x.email}</Text>
              </View>
            ))}
          </ScrollView>
          <Button label={t('mail.accounts.add')} onClick={onAdd} />
        </View>
        <View style={{ flex: 1, gap: 8 }}>
          {a ? (
            <>
              <Text style={{ fontSize: 14, fontWeight: 600, color: th.text }}>{a.name || a.email}</Text>
              <Text style={{ fontSize: 12, color: th.textDim }}>{`${t('mail.accounts.status')}: ${t(`mail.status.${st.accounts[a.id]?.status || 'idle'}`)} ${st.accounts[a.id]?.error ? `· ${st.accounts[a.id].error}` : ''}`}</Text>
              {a.kind === 'dav' && (
                <>
                  <Text style={{ fontSize: 12, color: th.textDim }}>{`${a.user} · ${a.url}`}</Text>
                  {a.carddav ? <Text style={{ fontSize: 12, color: th.textDim }}>{a.carddav}</Text> : null}
                  <Text style={{ fontSize: 12, color: th.textDim }}>{[a.cal !== false ? t('mail.setup.syncCalendars') : '', a.card !== false ? t('mail.setup.syncContacts') : ''].filter(Boolean).join(' · ')}</Text>
                </>
              )}
              {a.kind === 'mail' && (
                <>
                  <Text style={{ fontSize: 12, color: th.textDim }}>{`IMAP ${a.imap?.host}:${a.imap?.port} · SMTP ${a.smtp?.host}:${a.smtp?.port}`}</Text>
                  <Text style={{ fontSize: 12, fontWeight: 600, color: th.text, marginTop: 6 }}>{t('mail.accounts.signature')}</Text>
                  <TextInput value={sig} onChange={setSig} placeholder={t('mail.accounts.signaturePh')} />
                  <Button label={t('mail.accounts.saveSignature')} onClick={() => { mailAccounts.update(a.id, { signature: sig }); refresh(); }} />
                </>
              )}
              <View style={{ flex: 1 }} />
              <View style={{ flexDirection: 'row', gap: 8 }}>
                <Button label={t('mail.accounts.password')} onClick={() => onPassword(a.id)} />
                <Button label={t('mail.accounts.remove')} onClick={() => setConfirm(true)} />
              </View>
            </>
          ) : <Text style={{ fontSize: 13, color: th.textDim }}>{t('mail.accounts.none')}</Text>}
        </View>
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        <Toggle on={cfg.notify ?? true} onChange={(v) => { mailConfig.save({ ...mailConfig.get(), notify: v }); refresh(); }} />
        <Text style={{ flex: 1, fontSize: 12, color: th.text }}>{t('mail.accounts.notify')}</Text>
        <Button label={t('mail.close')} onClick={onClose} />
      </View>
    </Sheet>
  );
}
