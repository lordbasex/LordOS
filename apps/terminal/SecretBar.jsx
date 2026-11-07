// SecretBar.jsx — Passwords in the Terminal (docs/TERMINAL.md): when the program in a pane asks for a
// password (lord-server reads it from the pty: echo off, and a prompt that names one), a bar offers the
// Passwords entries. The one picked goes from lord-vault to the pty in C (lord.term.fill, termfill.c):
// this code only ever sees the entries' titles and user names, never a password. "Type It" closes the bar.
// No text field here: whatever is typed while the bar is open goes to the program, which is asking for
// a password.
import { View, Text, useTheme, launchApp, vaultRequest, t as T } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { meta, patchMeta, toast } from './state.js';
import { secretHost, rankEntries } from './secret.js';

export function SecretBar({ sid, wid, prompt }) {
  const th = useTheme();
  const [list, setList] = useState(null);     // null: closed; 'locked' | 'none' | [entries]
  // who asks, from its own executable and arguments (lord-server reads /proc), not from the text it printed: a
  // script that prints "ana@bank's password:" shows as itself, and its host is not taken from that text
  const [who, setWho] = useState(null);
  useEffect(() => { try { setWho(lord.term.asking(sid)); } catch (e) { setWho(null); } }, [sid, prompt]);
  const program = who?.program || '';
  const host = who?.host || (program === 'git' ? secretHost(prompt, '') : '') || (program ? '' : secretHost(prompt, meta.get()[sid]?.host));

  const load = async () => {
    try {
      const st = await vaultRequest('status');
      if (!st.main?.unlocked) { setList('locked'); return; }
      const all = (await vaultRequest('list')).entries || [];
      const ranked = rankEntries(all, host).slice(0, 12);
      setList(ranked.length ? ranked : 'none');
    } catch (e) { setList('locked'); }
  };
  // the program stopped asking (a new prompt is a new bar): what was open closes
  useEffect(() => { setList(null); }, [prompt]);

  const pick = (e) => {
    setList(null);
    if (!lord.term.fill(sid, 'main', e.id, 'password', 0)) toast(wid, T('terminal.secret.failed'));
  };
  const btn = (label, onClick, primary) => (
    <View onClick={onClick} style={{ paddingHorizontal: 10, height: 24, borderRadius: 6, alignItems: 'center', justifyContent: 'center',
      backgroundColor: primary ? th.accent : th.control }}>
      <Text style={{ fontSize: 12, fontWeight: 600, color: primary ? '#fff' : th.text, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
  return (
    <View style={{ position: 'absolute', left: 10, right: 10, bottom: 10, padding: 8, gap: 6, borderRadius: 10, backgroundColor: th.windowBg,
      border: `1px solid ${th.separator}`, boxShadow: '0 8px 24px rgba(0,0,0,0.45)' }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        <Text style={{ fontSize: 14 }}>🔑</Text>
        <Text style={{ flex: 1, minWidth: 0, fontSize: 12, color: th.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
          {program ? T(host ? 'terminal.secret.askedBy' : 'terminal.secret.askedByNoHost', { program, host })
            : host ? T('terminal.secret.askedHost', { host }) : T('terminal.secret.asked')}
        </Text>
        {btn(T('terminal.secret.usePasswords'), () => (list == null ? load() : setList(null)), true)}
        {btn(T('terminal.secret.typeIt'), () => patchMeta(sid, { secretDismissed: prompt }))}
      </View>
      {list === 'locked' && (
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
          <Text style={{ flex: 1, fontSize: 12, color: th.textDim }}>{T('terminal.secret.locked')}</Text>
          {btn(T('terminal.secret.open'), () => launchApp('passwords'))}
          {btn(T('terminal.secret.retry'), load)}
        </View>
      )}
      {list === 'none' && <Text style={{ fontSize: 12, color: th.textDim }}>{T('terminal.secret.none')}</Text>}
      {Array.isArray(list) && list.map((e) => (
        <View key={e.id} onClick={() => pick(e)} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 6, paddingVertical: 4, borderRadius: 6,
          backgroundColor: e.match ? th.control : 'transparent' }}>
          <Text style={{ flex: 1, minWidth: 0, fontSize: 12, color: th.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{e.title}</Text>
          {e.sub ? <Text style={{ fontSize: 11, color: th.textDim, whiteSpace: 'nowrap' }}>{e.sub}</Text> : null}
        </View>
      ))}
    </View>
  );
}
