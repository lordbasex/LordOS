// Compose.jsx — the compose window: From, To/Cc/Bcc with autocomplete (Contacts and people you
// wrote to), subject, a body in rich text (formatting buttons and a live preview), markdown or
// plain text, attachments, signature, drafts saved to the server, send now or later.
import { View, Text, useTheme, useAppWindow, Button, TextInput, Segmented, ScrollView, Markdown, FilePicker, t, wm, fs, mailRequest, mailAccounts, contacts, useKeyboard, isolatedPim, pickFileFd, closeFileFd, ContextMenu } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { BodyEditor } from './BodyEditor.jsx';
import { mdToHtml, mdToText } from './md.js';
import { formatSize } from './util.js';

// An attachment is a path (Mail in the session) or { fd, name, size } (Mail in its own process: a
// file from the system dialog, or a cached attachment lord-server opened for it).
const attName = (p) => (typeof p === 'object' ? p.name : fs.basename(p));
const attKey = (p) => (typeof p === 'object' ? `fd:${p.fd}` : p);
function attSize(p) {
  if (typeof p === 'object') return p.size || 0;
  try { return fs.stat(p)?.size || 0; } catch (e) { return 0; }
}

const emailOk = (e) => /^[^@\s<>,;"]+@[^@\s<>,;"]+\.[^@\s<>,;"]+$/.test(e);

function parseTyped(s) {
  const m = /^\s*"?([^"<]*?)"?\s*<([^>]+)>\s*$/.exec(s);
  if (m) return { name: m[1].trim(), email: m[2].trim() };
  return { name: '', email: s.trim() };
}

// One recipients row: chips + a field that suggests addresses as you type.
function Recipients({ label, list, onChange, autoFocus }) {
  const th = useTheme();
  const [q, setQ] = useState('');
  const [sugg, setSugg] = useState([]);
  const [i, setI] = useState(0);
  useEffect(() => {
    let live = true;
    if (q.trim().length < 1) { setSugg([]); return undefined; }
    contacts.autocomplete(q.trim()).then((r) => { if (live) { setSugg(r.filter((x) => !list.some((y) => y.email.toLowerCase() === x.email.toLowerCase())).slice(0, 8)); setI(0); } }).catch(() => {});
    return () => { live = false; };
  }, [q]);
  const add = (a) => { if (a?.email && emailOk(a.email)) onChange([...list, a]); setQ(''); setSugg([]); };
  return (
    <View style={{ zIndex: sugg.length ? 20 : 1 }}>
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, minHeight: 32, paddingHorizontal: 12 }}>
      <Text style={{ width: 54, fontSize: 12, color: th.textDim }}>{label}</Text>
      <View style={{ flex: 1, flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 4, paddingVertical: 4 }}>
        {list.map((a, k) => (
          <View key={a.email + k} style={{ flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 8, height: 22, borderRadius: 11, backgroundColor: emailOk(a.email) ? th.control : '#ff453a55' }}>
            <Text style={{ fontSize: 12, color: th.text }}>{a.name || a.email}</Text>
            <Text onClick={() => onChange(list.filter((_, j) => j !== k))} style={{ fontSize: 11, color: th.textDim }}>✕</Text>
          </View>
        ))}
        <View style={{ flex: 1, minWidth: 140 }}>
          <TextInput value={q} onChange={(v) => { if (/[,;]$/.test(v)) add(parseTyped(v.slice(0, -1))); else setQ(v); }} autoFocus={autoFocus}
            onSubmit={() => (sugg[i] ? add(sugg[i]) : q.trim() && add(parseTyped(q)))}
            onKeyDown={(e) => {
              if (e.key === 'ArrowDown' && sugg.length) { setI((i + 1) % sugg.length); return true; }
              if (e.key === 'ArrowUp' && sugg.length) { setI((i + sugg.length - 1) % sugg.length); return true; }
              if (e.key === 'Tab' && (sugg[i] || q.trim())) { add(sugg[i] || parseTyped(q)); return true; }
              if (e.key === 'Backspace' && !q && list.length) { onChange(list.slice(0, -1)); return true; }
              return false;
            }} style={{ backgroundColor: 'transparent', borderWidth: 0 }} />
        </View>
      </View>
      {sugg.length > 0 && (
        <View style={{ position: 'absolute', left: 66, top: 32, width: 360, padding: 4, borderRadius: 8, backgroundColor: th.windowBg, border: `1px solid ${th.separator}`,
          boxShadow: '0 10px 26px rgba(0,0,0,0.35)', zIndex: 30 }}>
          {sugg.map((s, k) => (
            <View key={s.email} onMouseDown={(e) => { e.stopPropagation?.(); add(s); }} style={{ padding: 6, borderRadius: 6, backgroundColor: k === i ? th.accent : 'transparent' }}>
              <Text style={{ fontSize: 13, color: k === i ? '#fff' : th.text }}>{s.name || s.email}</Text>
              <Text style={{ fontSize: 11, color: k === i ? '#ffffffcc' : th.textDim }}>{`${s.email}${s.src === 'contacts' ? ` · ${t('mail.compose.fromContacts')}` : ''}`}</Text>
            </View>
          ))}
        </View>
      )}
    </View>
    <View style={{ height: 1, backgroundColor: th.separator }} />
    </View>
  );
}

const Line = ({ th }) => <View style={{ height: 1, backgroundColor: th.separator }} />;

function ToolButton({ label, onClick, hint, bold, strike }) {
  const th = useTheme();
  return (
    <View onClick={onClick} style={{ minWidth: 28, height: 26, paddingHorizontal: 6, borderRadius: 6, alignItems: 'center', justifyContent: 'center', backgroundColor: th.control }}>
      <Text style={{ fontSize: 13, fontWeight: bold ? 700 : 500, color: th.text }}>{label}</Text>
      {/* the engine has no strike-through style: a hairline across the letter */}
      {strike ? <View style={{ position: 'absolute', left: 9, right: 9, top: 13, height: 1, backgroundColor: th.text }} /> : null}
    </View>
  );
}

// draft = { acct, to, cc, bcc, subject, body, format, inReplyTo, references, original, attachments: [path], draftUid, draftFolder }
export function Compose({ draft: initial }) {
  const th = useTheme();
  const { id: wid } = useAppWindow();
  const accounts = mailAccounts.list().filter((a) => a.kind === 'mail');
  const [d, setD] = useState(() => {
    const acct = initial?.acct && accounts.some((a) => a.id === initial.acct) ? initial.acct : accounts[0]?.id;
    const a = accounts.find((x) => x.id === acct);
    let body = initial?.body || '';
    if (a?.signature && !initial?.noSignature) body = `${body ? '' : '\n\n'}${body.startsWith('\n') ? '' : ''}\n\n-- \n${a.signature}${body ? `\n\n${body}` : ''}`.replace(/^\n\n\n/, '\n\n');
    return { to: [], cc: [], bcc: [], subject: '', format: 'rich', attachments: [], ...initial, acct, body };
  });
  const [showCc, setShowCc] = useState(!!(initial?.cc?.length || initial?.bcc?.length));
  const [preview, setPreview] = useState(true);
  const [picker, setPicker] = useState(false);
  const [busy, setBusy] = useState('');
  const [err, setErr] = useState('');
  const [later, setLater] = useState(false);
  const dirty = useRef(false);
  const ed = useRef(null);
  const bodyBox = useRef(null);
  const [ctx, setCtx] = useState(null);       // the body's right-click menu: { x, y }
  const set = (patch) => { dirty.current = true; setD((x) => ({ ...x, ...patch })); };

  useEffect(() => { wm.setTitle(wid, d.subject || t('mail.compose.newMessage')); }, [d.subject]);
  // files held as fds (isolated Mail) are closed with the window
  const atts = useRef(d.attachments);
  atts.current = d.attachments;
  useEffect(() => () => { for (const p of atts.current) closeFileFd(p); }, []);

  const payload = () => {
    const html = d.format === 'plain' ? undefined : mdToHtml(d.body);
    const text = d.format === 'plain' ? d.body : mdToText(d.body);
    return {
      acct: d.acct, to: d.to, cc: d.cc, bcc: d.bcc, subject: d.subject, text, ...(html ? { html } : {}),
      attachments: d.attachments.map((p) => ({ name: attName(p) })),
      ...(d.inReplyTo ? { inReplyTo: d.inReplyTo } : {}), ...(d.references ? { references: d.references } : {}),
      ...(d.original ? { original: d.original } : {}),
      ...(d.draftUid ? { draftUid: d.draftUid, draftFolder: d.draftFolder } : {}),
    };
  };

  const saveDraft = async (quiet) => {
    if (!d.acct) return;
    try {
      const r = await mailRequest('draft', { ...payload(), replace: d.draftUid || 0, format: d.format }, { files: d.attachments });
      dirty.current = false;
      if (r.uid) setD((x) => ({ ...x, draftUid: r.uid, draftFolder: x.draftFolder || 'Drafts' }));
      if (!quiet) setBusy(t('mail.compose.draftSaved'));
      setTimeout(() => setBusy(''), 1500);
    } catch (e) { if (!quiet) setErr(e.message); }
  };

  // autosave every 30 s while there are changes
  useEffect(() => { const id = setInterval(() => { if (dirty.current) saveDraft(true); }, 30000); return () => clearInterval(id); }, [d]);

  const send = async (sendAt) => {
    setErr('');
    const all = [...d.to, ...d.cc, ...d.bcc];
    if (!all.length) { setErr(t('mail.compose.noRecipients')); return; }
    const bad = all.find((a) => !emailOk(a.email));
    if (bad) { setErr(t('mail.compose.badAddress', { addr: bad.email })); return; }
    setBusy(t('mail.compose.sending'));
    try {
      const r = await mailRequest('send', { ...payload(), ...(sendAt ? { sendAt } : {}) }, { files: d.attachments });
      dirty.current = false;
      setBusy(r.scheduled ? t('mail.compose.scheduled') : t('mail.compose.sent'));
      setTimeout(() => wm.close(wid), 600);
    } catch (e) { setBusy(''); setErr(`${t('mail.compose.failed')} ${e.message}`); }
  };

  useEffect(() => {
    wm.setCloseGuard?.(wid, () => {
      if (!dirty.current) return true;
      saveDraft(true);
      return true;
    });
  }, [wid, d]);

  useKeyboard((e) => {
    const m = e.ctrl || e.meta;
    const b = (e.base || '').toLowerCase();
    if (m && e.key === 'Enter') { send(); return true; }
    if (m && b === 's') { saveDraft(); return true; }
    return false;
  });

  const tomorrow8 = () => { const x = new Date(); x.setDate(x.getDate() + 1); x.setHours(8, 0, 0, 0); return +x; };
  const monday8 = () => { const x = new Date(); x.setDate(x.getDate() + ((8 - x.getDay()) % 7 || 7)); x.setHours(8, 0, 0, 0); return +x; };
  const colors = { text: th.text, dim: th.textDim, heading: th.text, code: th.accent, accent: th.accent, selection: `${th.accent}55`, caret: th.accent,
    popupBg: th.windowBg, popupText: th.text, border: th.separator };
  const fmtOpts = [['rich', t('mail.compose.rich')], ['markdown', t('mail.compose.markdown')], ['plain', t('mail.compose.plain')]];
  const wrapSel = (a, b = a) => ed.current?.wrap?.(a, b) ?? ed.current?.insert?.(`${a}${b}`);
  return (
    <View style={{ flex: 1, backgroundColor: th.windowBg }}>
      {/* toolbar */}
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 12, height: 46 }}>
        <Button primary label={`➤ ${t('mail.compose.send')}`} onClick={() => send()} />
        <Button label={t('mail.compose.later')} onClick={() => setLater(!later)} />
        <Button label={`📎 ${t('mail.compose.attach')}`} onClick={() => {
          if (!isolatedPim()) { setPicker(true); return; }
          // the system's file dialog: this process receives only the chosen file
          let f = null;
          try { f = pickFileFd(); } catch (e) { setErr(e.message); }
          if (f) set({ attachments: [...d.attachments, f].slice(0, 15) });
        }} />
        <Button label={t('mail.compose.saveDraft')} onClick={() => saveDraft()} />
        <View style={{ flex: 1 }} />
        <Segmented small options={fmtOpts} value={d.format} onChange={(format) => set({ format })} />
      </View>
      <Line th={th} />
      {later && (
        <View style={{ position: 'absolute', left: 110, top: 44, padding: 6, borderRadius: 8, backgroundColor: th.windowBg, border: `1px solid ${th.separator}`, zIndex: 40,
          boxShadow: '0 10px 26px rgba(0,0,0,0.35)', gap: 2 }}>
          {[[t('mail.compose.in1h'), () => Date.now() + 3600000], [t('mail.compose.tomorrow8'), tomorrow8], [t('mail.compose.monday8'), monday8]].map(([label, at]) => (
            <View key={label} onClick={() => { setLater(false); send(at()); }} style={{ padding: 6, borderRadius: 6 }}><Text style={{ fontSize: 13, color: th.text }}>{label}</Text></View>
          ))}
        </View>
      )}
      {/* headers */}
      {accounts.length > 1 && (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, height: 32, paddingHorizontal: 12 }}>
          <Text style={{ width: 54, fontSize: 12, color: th.textDim }}>{t('mail.compose.from')}</Text>
          {accounts.map((a) => (
            <View key={a.id} onClick={() => set({ acct: a.id })} style={{ paddingHorizontal: 8, height: 22, borderRadius: 11, justifyContent: 'center', backgroundColor: a.id === d.acct ? th.accent : th.control }}>
              <Text style={{ fontSize: 12, color: a.id === d.acct ? '#fff' : th.text }}>{a.email}</Text>
            </View>
          ))}
        </View>
      )}
      {accounts.length > 1 && <Line th={th} />}
      <Recipients label={t('mail.compose.to')} list={d.to} onChange={(to) => set({ to })} autoFocus={!d.to.length} />
      {showCc ? (
        <>
          <Recipients label={t('mail.compose.cc')} list={d.cc} onChange={(cc) => set({ cc })} />
          <Recipients label={t('mail.compose.bcc')} list={d.bcc} onChange={(bcc) => set({ bcc })} />
        </>
      ) : (
        <Text onClick={() => setShowCc(true)} style={{ position: 'absolute', right: 14, top: accounts.length > 1 ? 86 : 54, fontSize: 12, color: th.accent, zIndex: 25 }}>{t('mail.compose.ccBcc')}</Text>
      )}
      <View style={{ flexDirection: 'row', alignItems: 'center', height: 34, paddingHorizontal: 12 }}>
        <Text style={{ width: 54, fontSize: 12, color: th.textDim }}>{t('mail.compose.subject')}</Text>
        <View style={{ flex: 1 }}><TextInput value={d.subject} onChange={(subject) => set({ subject })} style={{ backgroundColor: 'transparent', borderWidth: 0 }} /></View>
      </View>
      <Line th={th} />
      {d.format !== 'plain' && (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 12, height: 36 }}>
          <ToolButton label="B" bold onClick={() => wrapSel('**')} />
          <ToolButton label="I" onClick={() => wrapSel('*')} />
          <ToolButton label="S" strike onClick={() => wrapSel('~~')} />
          <ToolButton label="• ≡" onClick={() => ed.current?.list?.('bullet')} />
          <ToolButton label="1. ≡" onClick={() => ed.current?.list?.('num')} />
          <ToolButton label="❝" onClick={() => ed.current?.quote?.()} />
          <ToolButton label="🔗" onClick={() => ed.current?.link?.()} />
          <ToolButton label="</>" onClick={() => wrapSel('`')} />
          <View style={{ flex: 1 }} />
          {d.format === 'rich' && <Button small plain label={preview ? t('mail.compose.hidePreview') : t('mail.compose.showPreview')} onClick={() => setPreview(!preview)} />}
        </View>
      )}
      <View style={{ flex: 1, flexDirection: 'row' }}>
        <View ref={bodyBox} style={{ flex: 1 }}>
          <BodyEditor value={d.body} onChange={(body) => set({ body })} colors={colors} apiRef={ed} autoFocus={!!d.to.length} padding={14} caret={0}
            onContextMenu={(e) => setCtx(bodyBox.current ? e.local(bodyBox.current) : { x: e.x, y: e.y })} />
          {ctx && (
            <ContextMenu x={ctx.x} y={ctx.y} onClose={() => setCtx(null)}
              bounds={{ w: bodyBox.current ? lord.getLayout(bodyBox.current.id)[2] : 600, h: bodyBox.current ? lord.getLayout(bodyBox.current.id)[3] : 400 }}
              items={[
                { label: t('mail.compose.cut'), hint: 'Ctrl+X', disabled: !ed.current?.hasSelection?.(), onClick: () => ed.current?.cut?.() },
                { label: t('mail.compose.copy'), hint: 'Ctrl+C', disabled: !ed.current?.hasSelection?.(), onClick: () => ed.current?.copy?.() },
                { label: t('mail.compose.paste'), hint: 'Ctrl+V', onClick: () => ed.current?.paste?.() },
                '-',
                { label: t('mail.compose.selectAll'), hint: 'Ctrl+A', onClick: () => ed.current?.selectAll?.() },
              ]} />
          )}
        </View>
        {d.format === 'rich' && preview && <View style={{ width: 1, backgroundColor: th.separator }} />}
        {d.format === 'rich' && preview && (
          <View style={{ flex: 1, backgroundColor: th.mode === 'dark' ? '#00000022' : '#f7f7f7' }}>
            <ScrollView style={{ flex: 1 }}><View style={{ padding: 14 }}><Markdown source={d.body} width={360} /></View></ScrollView>
          </View>
        )}
      </View>
      {d.attachments.length > 0 && (
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6, padding: 10 }}>
          {d.attachments.map((p) => {
            const size = attSize(p);
            return (
              <View key={attKey(p)} style={{ flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 10, height: 28, borderRadius: 8, backgroundColor: th.control }}>
                <Text style={{ fontSize: 12, color: th.text }}>{`📄 ${attName(p)}`}</Text>
                <Text style={{ fontSize: 11, color: th.textDim }}>{formatSize(size)}</Text>
                <Text onClick={() => { closeFileFd(p); set({ attachments: d.attachments.filter((x) => x !== p) }); }} style={{ fontSize: 11, color: th.textDim }}>✕</Text>
              </View>
            );
          })}
        </View>
      )}
      {(err || busy) && (
        <View style={{ paddingHorizontal: 12, height: 26, justifyContent: 'center', backgroundColor: err ? '#ff453a22' : th.control }}>
          <Text style={{ fontSize: 12, color: err ? '#ff453a' : th.text }}>{err || busy}</Text>
        </View>
      )}
      {picker && <FilePicker multiple title={t('mail.compose.attachTitle')} onClose={() => setPicker(false)}
        onPick={(paths) => { setPicker(false); set({ attachments: [...d.attachments, ...[].concat(paths).filter((p) => !d.attachments.includes(p))].slice(0, 15) }); }} />}
    </View>
  );
}
