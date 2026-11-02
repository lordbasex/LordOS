// network.jsx — Files → Network: what is on the local network, found by itself. lord.lan (sdk/lan.js)
// does one bounded pass while this page is open; a share is mounted by the closed share-mount
// action and then browsed like any disk. See docs/FILES.md, "Network".
import { View, FormFields, Text, Button, ScrollView, TextInput, Toggle, useTheme, lan, spacing, typeScale, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { FolderGlyph } from './glyphs.jsx';

const SHARE_FS = ['cifs', 'smb3', 'nfs', 'nfs4', 'fuse.rclone'];
export const isShareFs = (fs) => SHARE_FS.includes(fs);
// A mounted share's folder keeps its fixed name on disk (share_leaf, lan_connect); people read it in their language.
export const shareTitle = (name) => { const m = /^(.+) on ([^ ]+)$/.exec(name); return m ? tr('files.net.shareOn', { name: m[1], host: m[2] }) : name; };

// the folder share-mount makes under /media/<user> (share_leaf in server/src/lan/lanaddr.c)
const leafOf = (kind, addr, share) => {
  const base = kind === 'nfs' ? (share.split('/').pop() || 'export') : share;
  return `${base.replace(/^\./, '_')} on ${addr}`;
};

export { hostName };

export function NetGlyph({ color, size = 16 }) {
  const dot = size * 0.26;
  const place = (left, top) => <View style={{ position: 'absolute', left, top, width: dot, height: dot, borderRadius: dot / 2, backgroundColor: color }} />;
  return (
    <View style={{ width: size, height: size }}>
      <View style={{ position: 'absolute', left: size * 0.47, top: size * 0.2, width: 1, height: size * 0.5, backgroundColor: color }} />
      <View style={{ position: 'absolute', left: size * 0.13, top: size * 0.76, width: size * 0.74, height: 1, backgroundColor: color }} />
      {place(size * 0.37, size * 0.04)}{place(0, size * 0.66)}{place(size * 0.37, size * 0.66)}{place(size * 0.74, size * 0.66)}
    </View>
  );
}

// what the interface calls a machine: the name it announces, else the one it gave, else its address
function hostName(h) {
  const v = h.services.find((s) => s.name && (s.kind === 'computer' || s.kind === 'smb')) || h.services.find((s) => s.name && s.kind !== 'printer');
  return v?.name || h.name || h.addr;
}

function kinds(h) {
  const k = [];
  if (h.smb) k.push(tr('files.net.kind.smb'));
  if (h.nfs) k.push(tr('files.net.kind.nfs'));
  for (const s of h.services) { const l = tr(`files.net.kind.${s.kind}`); if (s.kind !== 'smb' && s.kind !== 'nfs' && !k.includes(l)) k.push(l); }
  return k.length ? k.join(', ') : tr(h.name ? 'files.net.computer' : 'files.net.device');
}

// Connecting: who connects, then the folders that server shows them, so nobody has to know a share's name. It's only
// asked for when the server can't list them. The password goes to axia-lan for the list, to the action for the mount
// and, only if asked, to the keyring.
export function ConnectSheet({ ask, onClose, onMounted }) {
  const t = useTheme();
  const [share, setShare] = useState(ask.share || '');
  const [user, setUser] = useState(ask.user || '');
  const [password, setPassword] = useState('');
  const [remember, setRemember] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [list, setList] = useState(null);                 // the server's folders, once signed in
  const [typed, setTyped] = useState(false);              // it couldn't list them: the name is typed
  const [guest, setGuest] = useState(false);
  const nfs = ask.kind === 'nfs';
  const needName = nfs ? !ask.share : !ask.share && typed;
  useEffect(() => {
    if (nfs) return;
    lan.saved(ask.kind, ask.addr, ask.share || '').then((s) => s || (ask.share ? lan.saved(ask.kind, ask.addr, '') : null)).then((s) => { if (s) { setUser(s.user); setPassword(s.password); setRemember(true); } });
  }, []);
  // DOMAIN\user, as people write it
  const who = (g) => {
    if (g || nfs) return { user: '', domain: '', password: '' };
    const m = /^([^\\]+)\\(.+)$/.exec(user.trim());
    return { user: m ? m[2] : user.trim(), domain: m ? m[1] : '', password };
  };
  const mount = async (name, g) => {
    if (!name || busy) return;
    setBusy(true); setError('');
    const w = who(g);
    try {
      const at = await lan.mount({ kind: ask.kind, addr: ask.addr, share: name, ...w });
      let note = null;
      if (!g && !nfs && w.user) {
        const key = ask.share || '';
        if (remember) { try { await lan.remember(ask.kind, ask.addr, key, user.trim(), password, tr('files.net.keyringLabel', { name: name, addr: ask.addr })); } catch (e) { note = tr('files.net.notRemembered'); } }
        else lan.forget(ask.kind, ask.addr, key, user.trim());
      }
      onMounted(at, note, name);
    } catch (e) { setError(e.message); setBusy(false); }
  };
  const signIn = async (g) => {
    if (busy) return;
    setGuest(g);
    if (ask.share || nfs || typed) { mount((ask.share || share).trim(), g); return; }
    setBusy(true); setError('');
    const r = await lan.listShares({ addr: ask.addr, ...who(g) });
    setBusy(false);
    if (r.state === 'done' && r.shares.length) { setList(r.shares); return; }
    if (r.state === 'denied') { setError(tr(g ? 'files.net.noGuest' : 'files.net.badLogin')); return; }
    if (r.state === 'done') { setError(tr('files.net.noShares')); return; }
    if (r.state === 'off-link') { setTyped(true); setError(tr('files.net.offLink')); return; }   // lan.c only speaks to its own link
    setTyped(true);                                       // an old server, another subnet, a refused dialect
    setError(tr('files.net.cantList'));
  };
  const field = (label, input) => (
    <View style={{ gap: 3 }}>
      <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{label}</Text>
      {input}
    </View>
  );
  const ready = nfs ? (ask.share || share.trim()) : user.trim() && (!needName || share.trim());
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.3)', alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: 380, padding: spacing.xl, gap: spacing.s, borderRadius: 12, backgroundColor: t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 14px 36px rgba(0,0,0,0.45)' }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('files.net.connectTo', { name: ask.host })}</Text>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{`${tr(`files.net.kind.${ask.kind}`)} · ${ask.addr}`}</Text>
        {list ? (
          <>
            <Text style={{ fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{tr('files.net.pickShare')}</Text>
            <ScrollView style={{ maxHeight: 220 }}>
              {list.map((s) => (
                <View key={s.name} onClick={() => mount(s.name, guest)} style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, minHeight: 34, paddingHorizontal: spacing.s, borderRadius: 6 }}>
                  <FolderGlyph size={18} />
                  <View style={{ flex: 1, minWidth: 0 }}>
                    <Text style={{ fontSize: typeScale.body, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{s.name}</Text>
                    {s.comment ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{s.comment}</Text> : null}
                  </View>
                </View>
              ))}
            </ScrollView>
            {error ? <Text style={{ fontSize: typeScale.callout, color: '#ff453a', textAlign: 'start' }}>{error}</Text> : null}
            <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s, marginTop: spacing.m }}>
              <Button label={tr('files.cancel')} onClick={onClose} disabled={busy} />
            </View>
          </>
        ) : (
          <FormFields>
            {ask.share ? <Text style={{ fontSize: typeScale.body, color: t.text, textAlign: 'start' }}>{ask.share}</Text> : null}
            {needName ? field(tr(nfs ? 'files.net.exportPath' : 'files.net.shareName'), <TextInput value={share} onChange={setShare} autoFocus placeholder={nfs ? '/srv/data' : ''} onSubmit={() => signIn(false)} onCancel={onClose} />) : null}
            {!nfs && field(tr('files.net.user'), <TextInput value={user} onChange={setUser} autoFocus={!needName} onSubmit={() => signIn(false)} onCancel={onClose} />)}
            {!nfs && field(tr('files.net.password'), <TextInput secure value={password} onChange={setPassword} onSubmit={() => signIn(false)} onCancel={onClose} />)}
            {!nfs && (
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, marginTop: 4 }}>
                <Toggle small on={remember} onChange={setRemember} />
                <Text style={{ flex: 1, fontSize: typeScale.callout, color: t.text, textAlign: 'start' }}>{tr('files.net.remember')}</Text>
              </View>
            )}
            {error ? <Text style={{ fontSize: typeScale.callout, color: '#ff453a', textAlign: 'start' }}>{error}</Text> : null}
            <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s, marginTop: spacing.m }}>
              <Button label={tr('files.cancel')} onClick={onClose} disabled={busy} />
              {!nfs && <Button label={tr('files.net.asGuest')} onClick={() => signIn(true)} disabled={busy || (needName && !share.trim())} />}
              <Button primary label={busy ? tr('files.net.connecting') : tr('files.net.connect')} onClick={() => signIn(false)} disabled={busy || !ready} />
            </View>
          </FormFields>
        )}
      </View>
    </View>
  );
}

// sftp, ftp, ftps and WebDAV: who connects, then a folder in Files. A missing program goes to the Store.
export function RemoteSheet({ ask, onClose, onMounted, onStore, onTerminal }) {
  const t = useTheme();
  const [user, setUser] = useState(ask.user || '');
  const [password, setPassword] = useState('');
  const [remember, setRemember] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [pkg, setPkg] = useState('');
  const [confirm, setConfirm] = useState(null);           // { print, res }: a key or certificate seen for the first time
  const answer = (yes) => { confirm.res(yes); setConfirm(null); };
  const key = `${ask.host}:${ask.port || 0}${ask.path || ''}`;
  useEffect(() => { lan.saved(ask.kind, key, '').then((s) => { if (s) { setUser(s.user); setPassword(s.password); setRemember(true); } }); }, []);
  const go = async () => {
    if (busy) return;
    setBusy(true); setError(''); setPkg('');
    const r = await lan.connect({ kind: ask.kind, host: ask.host, port: ask.port, user: user.trim(), password, path: ask.path }, (print) => new Promise((res) => setConfirm({ print, res })));
    setBusy(false);
    if (r.state === 'changed') { setError(tr('files.connect.err.changed')); return; }
    if (r.state === 'refused') return;
    if (r.state === 'tlsname') { setError(tr('files.connect.err.tlsName', { name: ask.host })); return; }
    if (r.state === 'missing') { setPkg(r.package); setError(tr('files.connect.err.package', { name: r.package })); return; }
    if (r.state !== 'done') { setError(tr('files.connect.err.failed')); return; }
    if (user.trim()) { if (remember) lan.remember(ask.kind, key, '', user.trim(), password, tr('files.net.keyringLabel', { name: ask.kind, addr: ask.host })).catch(() => {}); else lan.forget(ask.kind, key, '', user.trim()); }
    onMounted(r.path);
  };
  const field = (label, input) => <View style={{ gap: 3 }}><Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{label}</Text>{input}</View>;
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.3)', alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: 380, padding: spacing.xl, gap: spacing.s, borderRadius: 12, backgroundColor: t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 14px 36px rgba(0,0,0,0.45)' }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start' }}>{tr('files.net.connectTo', { name: ask.host })}</Text>
        <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{ask.kind}</Text>
        <FormFields>
        {field(tr('files.net.user'), <TextInput value={user} onChange={setUser} autoFocus onSubmit={go} onCancel={onClose} />)}
        {field(tr('files.net.password'), <TextInput secure value={password} onChange={setPassword} onSubmit={go} onCancel={onClose} />)}
        </FormFields>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, marginTop: 4 }}>
          <Toggle small on={remember} onChange={setRemember} />
          <Text style={{ flex: 1, fontSize: typeScale.callout, color: t.text, textAlign: 'start' }}>{tr('files.net.remember')}</Text>
        </View>
        {ask.kind === 'ftp' || ask.kind === 'webdav' ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr('files.connect.plain')}</Text> : null}
        {confirm ? (
          <View style={{ gap: spacing.xs }}>
            <Text style={{ fontSize: typeScale.callout, color: t.text, textAlign: 'start' }}>{tr('files.connect.firstKey', { print: confirm.print })}</Text>
            <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s }}>
              <Button label={tr('files.cancel')} onClick={() => answer(false)} />
              <Button primary label={tr('files.connect.trust')} onClick={() => answer(true)} />
            </View>
          </View>
        ) : null}
        {error ? <Text style={{ fontSize: typeScale.callout, color: '#ff453a', textAlign: 'start' }}>{error}</Text> : null}
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: spacing.s, marginTop: spacing.m }}>
          {onTerminal ? <Button label={tr('files.menu.openTerminal')} onClick={onTerminal} disabled={busy} /> : null}
          {pkg ? <Button label={tr('files.connect.openStore')} onClick={() => onStore(pkg)} /> : null}
          <Button label={tr('files.cancel')} onClick={onClose} disabled={busy} />
          <Button primary label={busy ? tr('files.net.connecting') : tr('files.net.connect')} onClick={go} disabled={busy} />
        </View>
      </View>
    </View>
  );
}

// The local network as a place like any folder: one entry per computer, in the same views (index.jsx). lord.lan runs while
// the place is open and stops when it closes.
export function useLan(active, setMsg) {
  const [st, setSt] = useState(() => (lan.available() ? lan.poll() : { hosts: [] }));
  const timer = useRef(null);
  const watch = () => {
    if (timer.current) return;
    timer.current = setInterval(() => {
      const s = lan.poll();
      setSt(s);
      if (!s.running) { clearInterval(timer.current); timer.current = null; }
    }, 700);
  };
  useEffect(() => {
    if (!active || !lan.available()) return undefined;
    lan.start();
    setSt(lan.poll());
    watch();
    return () => { if (timer.current) clearInterval(timer.current); timer.current = null; lan.stop(); };
  }, [active]);
  const search = () => { const started = lan.start(true); setSt(lan.poll()); if (started || lan.poll().running) watch(); else setMsg(tr('files.net.tooSoon')); };
  return [st, search];
}

export function lanStatus(st, count) {
  const status = { 'no-lan': 'files.net.noLan', 'no-connector': 'files.net.noConnector', 'no-helper': 'files.net.noHelper', denied: 'files.net.denied' }[st.state];
  if (status) return tr(status);
  if (st.running) return st.total ? tr('files.net.scanning', { done: st.probed, total: st.total }) : tr('files.net.listening');
  return tr('files.net.found', { count });
}

// Every way into a computer, best first. What mounts comes before a remote screen.
const ORDER = { smb: 0, nfs: 1, sftp: 2, ssh: 3, webdavs: 4, webdav: 5, ftps: 6, ftp: 7, screen: 8, rdp: 9 };
export function hostWays(h, volumes) {
  const ways = [];
  const on = (kind, share) => volumes.find((v) => isShareFs(v.fs) && v.name === leafOf(kind, h.addr, share));
  for (const s of h.shares) ways.push({ id: `smb:${s.name}`, order: 0, label: s.name, sub: tr('files.net.sharedFolder'), mode: 'share', kind: 'smb', share: s.name, volume: on('smb', s.name) });
  if (h.smb && !h.shares.length) ways.push({ id: 'smb', order: 0, label: tr('files.net.kind.smb'), mode: 'share', kind: 'smb', share: '' });
  for (const e of h.exports) ways.push({ id: `nfs:${e.path}`, order: 1, label: e.path, sub: tr('files.net.sharedFolder'), mode: 'share', kind: 'nfs', share: e.path, volume: on('nfs', e.path) });
  if (h.nfs && !h.exports.length) ways.push({ id: 'nfs', order: 1, label: tr('files.net.kind.nfs'), mode: 'share', kind: 'nfs', share: '' });
  const FOLDER = { ssh: 'sftp', sftp: 'sftp', ftp: 'ftp', ftps: 'ftps', webdav: 'webdav', webdavs: 'davs' };
  const seen = new Set();
  for (const s of h.services) {
    if (ORDER[s.kind] === undefined || s.kind === 'smb' || s.kind === 'nfs') continue;
    const label = tr(`files.net.kind.${s.kind}`);
    if (FOLDER[s.kind]) {
      const kind = FOLDER[s.kind];
      if (seen.has(kind)) continue;
      seen.add(kind);
      const std = { sftp: 22, ftp: 21, ftps: 21, webdav: 80, davs: 443 }[kind];
      const path = kind === 'webdav' || kind === 'davs' ? (s.path && s.path !== '/' ? s.path.replace(/\/+$/, '') : '') : '';
      ways.push({ id: `${kind}`, order: ORDER[s.kind], label, mode: 'remote', ask: { kind, host: h.addr, port: s.port === std ? 0 : s.port, user: '', path } });
    } else ways.push({ id: `${s.kind}${s.port}`, order: ORDER[s.kind], label, mode: 'screen', app: s.port === 3389 || s.kind === 'rdp' ? 'rdp' : 'vnc', port: s.port });
  }
  return ways.sort((a, b) => a.order - b.order);
}

export function hostEntries(st, volumes) {
  return [...st.hosts].filter((h) => h.addr).map((h) => {
    const ways = hostWays(h, volumes);
    return { name: hostName(h), path: `net:${h.addr}`, dir: ways.length > 1 || h.smb, netHost: h, ways, size: 0, mtime: 0, sub: kinds(h) };
  });
}
