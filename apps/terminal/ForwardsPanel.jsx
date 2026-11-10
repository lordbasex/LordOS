// ForwardsPanel.jsx — the tab's port forwards (forwards.js): each "server port → 127.0.0.1:port" with its state, Open
// (in the browser), Copy and Close, and a row to add one for this connection only. Opened from the badge on the tab or
// the footer; a click outside closes it. A fixed local port that is taken says so and offers a free one (fwdports.js).
import { View, Text, TextInput, Button, useStore, clipboard, openUrl, t as tr } from '@lordos/sdk';
import { useState } from 'react';
import { patchWin, leaves } from './state.js';
import { forwards, forwardsOf, forwardLabel, localUrl, addForward, closeForward, useFreePort } from './forwards.js';
import { badgeItem, badgeText } from './fwdports.js';

const W = 340;

export function ForwardsPanel({ t, wid, w, panel }) {
  const all = useStore(forwards);
  const [host, setHost] = useState('');
  const [local, setLocal] = useState('');
  const [bad, setBad] = useState(false);
  const tab = w.tabs.find((x) => x.id === panel.tabId);
  const close = () => patchWin(wid, () => ({ fwdPanel: null }));
  if (!tab) return null;
  const sids = leaves(tab.root);
  const items = forwardsOf(all, sids);
  const target = all[tab.focus] ? tab.focus : sids.find((s) => all[s]);     // the ssh the new forward goes through
  const add = () => {
    // "8080", "db:5432" or "db.local:5432": the server side; the local port is optional (automatic when empty)
    const m = /^(?:([A-Za-z0-9._-]+):)?(\d{1,5})$/.exec(host.trim());
    const ok = !!m && target != null && addForward(target, { host: m[1] || 'localhost', port: Number(m[2]), local: local.trim() });
    setBad(!ok);
    if (ok) { setHost(''); setLocal(''); }
  };
  return (
    <View onClick={close} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0 }}>
      <View onClick={(e) => e.stopPropagation()} style={{ position: 'absolute', top: 34, left: Math.max(8, panel.x - W / 2), width: W, padding: 12, gap: 10,
        borderRadius: 10, backgroundColor: t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 12px 32px rgba(0,0,0,0.45)', direction: 'ltr' }}>
        <Text style={{ fontSize: 13, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('terminal.fwd.title')}</Text>
        {!items.length && <Text style={{ fontSize: 12, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{tr(target != null ? 'terminal.fwd.none' : 'terminal.fwd.noSsh')}</Text>}
        {items.map((it) => (
          <View key={it.key} style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
            <Text style={{ fontSize: 11, color: it.state === 'open' ? '#32d74b' : it.state === 'error' ? '#ff453a' : t.textDim }}>●</Text>
            <View style={{ flex: 1 }}>
              <Text style={{ fontSize: 12, fontFamily: 'monospace', color: t.text, textAlign: 'start' }}>{forwardLabel(it)}</Text>
              {stateText(it) ? <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>{stateText(it)}</Text> : null}
            </View>
            {it.state === 'open' && <Button small label={tr('terminal.fwd.open')} onClick={() => openUrl(localUrl(it))} />}
            {it.state === 'open' && <Button small label={tr('terminal.fwd.copy')} onClick={() => clipboard.set(`127.0.0.1:${it.local}`)} />}
            {it.state === 'error' && it.error === 'inuse' && <Button small label={tr('terminal.fwd.useFree')} onClick={() => useFreePort(it.sid, it.key)} />}
            <Button small label="✕" title={tr('terminal.fwd.close')} onClick={() => closeForward(it.sid, it.key)} />
          </View>
        ))}
        {target != null && (
          <View style={{ gap: 6 }}>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
              <TextInput value={host} onChange={(v) => { setHost(v); setBad(false); }} onSubmit={add} placeholder={tr('terminal.fwd.serverPh')} style={{ flex: 1 }} />
              <Text style={{ fontSize: 12, color: t.textDim }}>→</Text>
              <TextInput value={local} onChange={(v) => { setLocal(v); setBad(false); }} onSubmit={add} placeholder={tr('terminal.fwd.auto')} style={{ width: 70 }} />
              <Button small label={tr('terminal.fwd.add')} onClick={add} />
            </View>
            <Text style={{ fontSize: 11, color: bad ? '#ff453a' : t.textDim, lineHeight: 1.35, textAlign: 'start' }}>{tr(bad ? 'terminal.fwd.badInput' : 'terminal.fwd.sessionOnly')}</Text>
          </View>
        )}
      </View>
    </View>
  );
}

// what a forward that isn't open says under its address
const stateText = (it) => (it.state === 'open' ? '' : it.state === 'error'
  ? tr(`terminal.fwd.err.${['failed', 'nossh', 'bad', 'unavailable', 'inuse'].includes(it.error) ? it.error : 'failed'}`, { port: it.want })
  : tr('terminal.fwd.opening'));

// the badge on a tab: "⇄ 80 → 54321" (+ how many more), nothing without forwards
export function ForwardsBadge({ t, wid, tab }) {
  const items = forwardsOf(useStore(forwards), leaves(tab.root));     // (a selector that builds a new array would re-render forever)
  if (!items.length) return null;
  const first = badgeItem(items);
  const anyErr = items.some((x) => x.state === 'error');
  return (
    <View onMouseDown={(e) => e.stopPropagation()} onClick={(e) => { e.stopPropagation(); patchWin(wid, () => ({ fwdPanel: { tabId: tab.id, x: e.x } })); }}
      title={first.state === 'error' ? stateText(first) : tr('terminal.fwd.title')}
      style={{ height: 18, paddingHorizontal: 5, borderRadius: 4, justifyContent: 'center', backgroundColor: anyErr ? 'rgba(255,69,58,0.25)' : 'rgba(50,215,75,0.22)', direction: 'ltr' }}>
      <Text style={{ fontSize: 10, fontWeight: 600, color: t.text, whiteSpace: 'nowrap' }}>{badgeText(items)}</Text>
    </View>
  );
}
