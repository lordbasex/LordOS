// rescue.js — a system with no administrator (server/src/users/admin_rescue.h, docs/INSTALL-DEBIAN.md).
// Shown under "<user> is not an administrator". Asks the helper whether that case applies; only then,
// with the root password, the session user joins `sudo`, effective after a restart. The helper decides
// everything again and takes no user name. The password stays in this field and goes in one request.
import { createElement as h, useEffect, useState } from 'react';
import { View, Text, Button, TextInput, useTheme, admin, session, typeScale, spacing, t } from '@lordos/sdk';

export const isNotAdmin = (e) => e?.code === 'admin.notAdmin';

function askState(set) {
  let word = 'no';
  admin.run('admin-rescue-status', '', (line) => { const m = /^users\.rescueState state=([a-z-]+)$/.exec(line); if (m) word = m[1]; }, { quiet: true })
    .then(() => set(word), () => set('no'));
}

export function AdminRescue() {
  const th = useTheme();
  const [state, setState] = useState(null);
  const [pass, setPass] = useState('');
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState('');
  useEffect(() => { askState(setState); }, []);
  if (!state || state === 'no') return null;
  const submit = () => {
    if (busy || !pass) return;
    setBusy(true);
    setErr('');
    admin.run('admin-rescue', pass, null, { quiet: true }).then(
      () => setState('restart'),
      (e) => { setErr(e.message); askState(setState); },
    ).finally(() => { setPass(''); setBusy(false); });
  };
  const canRestart = typeof session.canPower === 'function' && session.canPower();
  const body = { 'no-sudo': 'store.rescue.noSudo', 'root-locked': 'store.rescue.rootLocked', restart: 'store.rescue.restart' }[state];
  return h(View, { style: { gap: spacing.s, padding: spacing.xl, borderRadius: 10, border: `1px solid ${th.separator}`, marginBottom: spacing.m } },
    h(Text, { style: { fontSize: typeScale.body, fontWeight: 600, color: th.text, textAlign: 'start' } },
      t(state === 'restart' ? 'store.rescue.titleRestart' : 'store.rescue.title')),
    h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, lineHeight: 1.4, textAlign: 'start' } }, t(body || 'store.rescue.ask')),
    state === 'offer' ? h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: spacing.s } },
      h(TextInput, { secure: true, value: pass, onChange: setPass, onSubmit: submit, placeholder: t('store.rescue.password'), style: { flex: 1, minWidth: 0 } }),
      h(Button, { primary: true, label: t('store.rescue.button'), disabled: busy || !pass, onClick: submit })) : null,
    err ? h(Text, { style: { fontSize: typeScale.caption, color: '#ff453a', textAlign: 'start' } }, err) : null,
    state === 'restart' ? h(Text, { style: { fontSize: typeScale.callout, color: th.textDim, textAlign: 'start' } },
      t(canRestart ? 'store.restart.save' : 'store.restart.how')) : null,
    state === 'restart' && canRestart ? h(View, { style: { flexDirection: 'row', justifyContent: 'flex-end' } },
      h(Button, { primary: true, label: t('store.restart.now'), onClick: () => session.restart() })) : null);
}
