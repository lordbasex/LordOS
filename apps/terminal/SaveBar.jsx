// SaveBar.jsx — Save to Passwords from the Terminal (docs/PASSWORDS.md): after a program accepted a password the
// user typed (lord-server decides that, tcapture.c), "Save the password for ssh ana@host in Passwords?". This code
// has the program, host and user only; Save asks lord-server to send the value it keeps to lord-vault
// (lord.term.saveSecret, termfill.c), Not Now has it wiped. Not shown when Passwords already has that host and
// user; the value is wiped by lord-server after a minute either way.
import { View, Text, useTheme, launchApp, vaultRequest, t as T } from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { patchMeta, toast } from './state.js';
import { alreadySaved, offerTitle } from './secret.js';

const OFFER_MS = 60000;                    // as long as lord-server keeps the value (SAVE_OFFER_MS)

export function SaveBar({ sid, wid, offer }) {
  const th = useTheme();
  const [locked, setLocked] = useState(false);
  const close = () => patchMeta(sid, { saveOffer: null });
  const notNow = () => { try { lord.term.dropSecret(sid, offer.token); } catch (e) { /* gone */ } close(); };
  useEffect(() => {
    let live = true;
    (async () => {
      try {
        const st = await vaultRequest('status');
        if (!st.main?.unlocked) { if (live) setLocked(true); return; }
        const list = (await vaultRequest('list')).entries || [];
        if (live && alreadySaved(list, offer)) notNow();
      } catch (e) { if (live) setLocked(true); }
    })();
    const tmr = setTimeout(close, OFFER_MS);
    return () => { live = false; clearTimeout(tmr); };
  }, [offer.token]);
  const save = () => {
    if (!lord.term.saveSecret(sid, offer.token)) { toast(wid, T('terminal.save.failed')); close(); return; }
    close();
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
          {T('terminal.save.ask', { title: offerTitle(offer) })}
        </Text>
        {locked && btn(T('terminal.secret.open'), () => launchApp('passwords'))}
        {btn(T('terminal.save.save'), save, true)}
        {btn(T('terminal.save.notNow'), notNow)}
      </View>
      {locked && <Text style={{ fontSize: 12, color: th.textDim }}>{T('terminal.save.locked')}</Text>}
    </View>
  );
}
