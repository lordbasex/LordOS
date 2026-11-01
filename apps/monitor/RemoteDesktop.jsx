// RemoteDesktop.jsx — the remote desktop as one more app in Monitor → Network (0.8.3, docs/MONITOR.md,
// "Remote desktop"). lord-sessiond serves it as root, outside the session's apps, so its connections
// never show up among them: this file builds its row from lord-sessiond's own state, read-only
// (/run/lordos/status.json through remote.status(): clients with their traffic, blocks and lists),
// and, for administrators, the recent failed sign-ins through the fixed admin action
// remote-failures (an address, the cleaned name that was typed, a count; never a password). Block and
// Unblock use the same validated admin actions as Settings → Sharing (remoteAccess).
import { View, Text, Button, TextInput, useTheme, useLocale, useStore, shareHolder, remote, remoteAccess, remoteCidr, remoteMinutesLeft, admin, formatBytes, formatNumber, useSystemInfo } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';

export const REMOTE_KEY = 'remote-desktop';

// The app's icon: the Sharing glyph of Settings (green, two arrows).
export function RemoteGlyph({ size = 16 }) {
  return (
    <View style={{ width: size, height: size, borderRadius: Math.round(size * 0.24), backgroundColor: '#34c759', alignItems: 'center', justifyContent: 'center' }}>
      <Text style={{ fontSize: Math.round(size * 0.68), fontWeight: 700, color: '#fff', lineHeight: 1 }}>⇄</Text>
    </View>
  );
}

const agoMin = (sec) => Math.max(0, Math.round((Date.now() / 1000 - sec) / 60));

// lord-sessiond's state every 2 s (and the failed sign-ins every 5 s, administrators only) while the
// Network tab is open; the traffic rates are the difference between two readings.
export function useRemoteDesktop() {
  const [st, setSt] = useState(() => remote.status());
  const [fails, setFails] = useState(null);       // null: not available (not an administrator)
  const prev = useRef({ t: 0, bytes: new Map() });
  const rates = useRef(new Map());
  useEffect(() => {
    let alive = true;
    const read = () => {
      const s = remote.status();
      const now = Date.now(), dt = prev.current.t ? Math.max(0.5, (now - prev.current.t) / 1000) : 0;
      const bytes = new Map();
      for (const c of s.clients) {
        const old = prev.current.bytes.get(c.pid);
        bytes.set(c.pid, [c.rx || 0, c.tx || 0]);
        rates.current.set(c.pid, old && dt ? [Math.max(0, (c.rx || 0) - old[0]) / dt, Math.max(0, (c.tx || 0) - old[1]) / dt] : [0, 0]);
      }
      prev.current = { t: now, bytes };
      if (alive) setSt(s);
    };
    const failures = () => {
      if (!admin.available()) return;
      remoteAccess.failures().then((f) => alive && setFails(f), () => alive && setFails(null));
    };
    read();
    failures();
    const a = setInterval(read, 2000), b = setInterval(failures, 5000);
    return () => { alive = false; clearInterval(a); clearInterval(b); };
  }, []);
  return { st, fails, rates: rates.current };
}

// Who holds the screen-sharing API now: a line above the list, with or without remote desktop on.
export function ShareHolderLine({ tr }) {
  const t = useTheme();
  const c = useStore(shareHolder);
  if (!c) return null;
  return <Text style={{ fontSize: 12, color: t.textDim, paddingHorizontal: 12, paddingVertical: 4 }}>{tr('monitor.net.shareHolder', { app: c.name })}</Text>;
}

// The row in the app list (the same shape as Network.jsx's groups). Only while it's on.
export function remoteGroup(rd, tr) {
  const { st, rates } = rd;
  if (!st.enabled) return null;
  const hosts = new Map();
  let sent = 0, recv = 0, rateTx = 0, rateRx = 0;
  const conns = st.clients.map((c) => {
    const r = rates.get(c.pid) || [0, 0];
    sent += c.tx || 0;
    recv += c.rx || 0;
    rateRx += r[0];
    rateTx += r[1];
    const h = hosts.get(c.ip) || { remote: c.ip, cc: '', scope: 'lan', ports: new Set([`TCP ${st.port}`]), sent: 0, recv: 0, rateTx: 0, rateRx: 0, count: 0 };
    h.sent += c.tx || 0;
    h.recv += c.rx || 0;
    h.count++;
    hosts.set(c.ip, h);
    return { id: `rd${c.pid}`, dir: 'in', proto: 'tcp', lport: st.port, rport: 0, remote: c.ip };
  });
  const listen = [{ id: 'rdl', dir: 'listen', proto: 'tcp', lport: st.port }];
  const name = tr('monitor.net.remoteDesktop');
  return { key: REMOTE_KEY, item: { app: REMOTE_KEY, appName: name, name }, name, exe: '', pids: new Set(), conns: [...conns, ...listen], listen, hosts,
    sent, recv, rateTx, rateRx, incoming: conns.length, verdict: 'allow', rule: 'allow', allowed: 0, denied: (rd.fails || []).reduce((a, f) => a + f.count, 0), remote: true };
}

// The detail panel. Section and KV come from Network.jsx (the same folding sections).
export function RemoteDetail({ rd, Section, KV, tr }) {
  const t = useTheme();
  const info = useSystemInfo(60000);
  const { st, fails, rates } = rd;
  const isAdmin = admin.available() && info.admin;
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState(null);
  const [text, setText] = useState('');
  const [bad, setBad] = useState(false);
  const run = (p) => {
    setBusy(true);
    setMsg(null);
    return p.then(() => true, (e) => { setMsg(e.message); return false; }).finally(() => setBusy(false));
  };
  const block = (ip) => run(remoteAccess.block(ip));
  const typed = () => {
    const v = remoteCidr(text);
    if (!v) { setBad(true); return; }
    block(v).then((ok) => { if (ok) setText(''); });
  };
  const temp = st.blocked.filter((b) => !b.manual), manual = st.blocked.filter((b) => b.manual);
  const mono = { fontSize: 11, color: t.text, fontFamily: 'monospace', direction: 'ltr', textAlign: 'start', lineHeight: 1.3 };
  const dim = { fontSize: 10, color: t.textDim, lineHeight: 1.3, textAlign: 'start' };
  const row = { gap: 3, paddingVertical: 2 };
  return (
    <>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
        <RemoteGlyph size={32} />
        <View style={{ flex: 1, minWidth: 0 }}>
          <Text style={{ fontSize: 15, fontWeight: 600, color: t.text, lineHeight: 1.25, textAlign: 'start' }} numberOfLines={2}>{tr('monitor.net.remoteDesktop')}</Text>
          <Text style={{ fontSize: 11, color: t.textDim }}>{tr('monitor.net.remotePort', { port: st.port, count: st.clients.length })}</Text>
        </View>
      </View>
      {msg ? <Text style={{ fontSize: 11, color: '#ff453a', lineHeight: 1.35 }}>{msg}</Text> : null}
      <Section id="remoteClients" title={tr('monitor.net.remoteClients')}>
        {st.clients.map((c) => {
          const r = rates.get(c.pid) || [0, 0];
          return (
            <View key={c.pid} style={row}>
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
                <Text style={{ ...mono, flex: 1, minWidth: 0 }} numberOfLines={2}>{c.ip}</Text>
                {isAdmin ? <Button small label={tr('settings.sharing.block')} disabled={busy} onClick={() => block(c.ip)} /> : null}
              </View>
              <Text style={dim} numberOfLines={2}>{tr('monitor.net.remoteClient', { user: c.user, minutes: agoMin(c.since) })}</Text>
              <Text style={dim}>{`↓ ${formatBytes(c.rx || 0)} · ↑ ${formatBytes(c.tx || 0)} · ${formatBytes(r[0] + r[1])}/s`}</Text>
            </View>
          );
        })}
        {!st.clients.length ? <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.net.remoteNoClients')}</Text> : null}
      </Section>
      <Section id="remoteFailures" title={tr('monitor.net.remoteFailures')}>
        {fails === null ? <Text style={{ ...dim, fontSize: 11 }}>{tr('monitor.net.remoteAdminOnly')}</Text> : null}
        {(fails || []).slice(0, 12).map((f) => (
          <View key={`${f.key} ${f.user}`} style={row}>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
              <Text style={{ ...mono, flex: 1, minWidth: 0 }} numberOfLines={2}>{f.key}</Text>
              {isAdmin ? <Button small label={tr('settings.sharing.block')} disabled={busy} onClick={() => block(f.key)} /> : null}
            </View>
            <Text style={dim} numberOfLines={2}>{tr('monitor.net.remoteFailure', { user: f.user, count: f.count, minutes: agoMin(f.last) })}</Text>
          </View>
        ))}
        {fails && !fails.length ? <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.net.remoteNoFailures')}</Text> : null}
      </Section>
      <Section id="remoteBlocked" title={tr('settings.sharing.blocked')}>
        {temp.map((b) => (
          <View key={`a${b.ip}`} style={row}>
            <Text style={mono} numberOfLines={2}>{b.ip}</Text>
            <Text style={dim}>{tr('settings.sharing.blockedAuto', { minutes: remoteMinutesLeft(b.until) })}</Text>
            {isAdmin ? (
              <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6 }}>
                <Button small label={tr('settings.sharing.unblock')} disabled={busy} onClick={() => run(remoteAccess.unblock(b.ip))} />
                <Button small label={tr('settings.sharing.blockForever')} disabled={busy}
                  onClick={() => run(remoteAccess.unblock(b.ip)).then((ok) => ok && block(b.ip))} />
              </View>
            ) : null}
          </View>
        ))}
        {!temp.length ? <Text style={{ fontSize: 12, color: t.textDim }}>{tr('settings.sharing.blockedNone')}</Text> : null}
      </Section>
      <Section id="remoteLists" title={tr('monitor.net.remoteLists')}>
        <KV k={tr('settings.sharing.netFrom')} v={tr({ lan: 'settings.sharing.netLan', list: 'settings.sharing.netList', any: 'settings.sharing.netAny' }[st.net] || 'settings.sharing.netLan')} />
        <Text style={{ fontSize: 11, fontWeight: 600, color: t.textDim, marginTop: 2 }}>{tr('settings.sharing.blocklist')}</Text>
        {manual.map((b) => (
          <View key={`m${b.ip}`} style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
            <Text style={{ ...mono, flex: 1, minWidth: 0 }} numberOfLines={2}>{b.ip}</Text>
            {isAdmin ? <Button small label={tr('settings.sharing.unblock')} disabled={busy} onClick={() => run(remoteAccess.unblock(b.ip))} /> : null}
          </View>
        ))}
        {!manual.length ? <Text style={dim}>{tr('settings.sharing.listEmpty')}</Text> : null}
        <Text style={{ fontSize: 11, fontWeight: 600, color: t.textDim, marginTop: 2 }}>{tr('settings.sharing.allowlist')}</Text>
        {st.allowed.map((c) => <Text key={c} style={mono}>{c}</Text>)}
        {!st.allowed.length ? <Text style={dim}>{tr('settings.sharing.listEmpty')}</Text> : null}
        {isAdmin ? (
          <View style={{ gap: 4, marginTop: 4 }}>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
              <TextInput value={text} placeholder={tr('settings.sharing.blockPlaceholder')} style={{ flex: 1, direction: 'ltr' }} size={12}
                onChange={(v) => { setText(v.slice(0, 60)); setBad(false); }} onSubmit={typed} />
              <Button small label={tr('settings.sharing.block')} disabled={busy || !text.trim()} onClick={typed} />
            </View>
            {bad ? <Text style={{ fontSize: 10, color: '#ff453a', lineHeight: 1.35 }}>{tr('settings.sharing.badAddress')}</Text> : null}
          </View>
        ) : null}
        <KV k={tr('monitor.net.remoteBlockedTotal')} v={formatNumber(st.blocked.length, 0)} />
      </Section>
    </>
  );
}
