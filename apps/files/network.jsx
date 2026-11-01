// network.jsx — Files → Network: what is on the local network, found by itself. lord.lan (sdk/lan.js)
// does one bounded pass while this page is open; a share is mounted by the closed share-mount
// action and then browsed like any disk. See docs/FILES.md, "Network".
import { View, Text, Button, ScrollView, TextInput, Toggle, Hairline, useTheme, clipboard, lan, spacing, typeScale, t as tr } from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { DriveGlyph, FolderGlyph } from './glyphs.jsx';

const SHARE_FS = ['cifs', 'smb3', 'nfs', 'nfs4', 'fuse.rclone'];
export const isShareFs = (fs) => SHARE_FS.includes(fs);

// the folder share-mount makes under /media/<user> (share_leaf in server/src/lan/lanaddr.c)
const leafOf = (kind, addr, share) => {
  const base = kind === 'nfs' ? (share.split('/').pop() || 'export') : share;
  return `${base.replace(/^\./, '_')} on ${addr}`;
};

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
const isComputer = (h) => h.smb || h.nfs || h.name || h.services.length > 0;

// a service that isn't mounted here: its address, to copy
function serviceUrl(addr, s) {
  const scheme = { webdav: 'http', webdavs: 'https', sftp: 'sftp', ssh: 'ssh', ftp: 'ftp', http: 'http', https: 'https', printer: 'ipp', afp: 'afp' }[s.kind];
  const std = { http: 80, https: 443, ftp: 21, ssh: 22, sftp: 22, ipp: 631, afp: 548 }[scheme];
  if (!scheme) return `${addr}:${s.port}`;
  return `${scheme}://${addr}${s.port === std ? '' : `:${s.port}`}${s.path && s.path.startsWith('/') ? s.path : ''}`;
}

function kinds(h) {
  const k = [];
  if (h.smb) k.push(tr('files.net.kind.smb'));
  if (h.nfs) k.push(tr('files.net.kind.nfs'));
  for (const s of h.services) { const l = tr(`files.net.kind.${s.kind}`); if (s.kind !== 'smb' && s.kind !== 'nfs' && !k.includes(l)) k.push(l); }
  return k.length ? k.join(', ') : tr(h.name ? 'files.net.computer' : 'files.net.device');
}

function Row({ t, depth = 0, icon, name, sub, kind, children, onClick, chevron }) {
  return (
    <View onClick={onClick} style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, minHeight: 34, paddingVertical: 4, paddingHorizontal: spacing.m, paddingStart: spacing.m + depth * 24 }}>
      <Text style={{ width: 12, fontSize: 11, color: t.textDim }}>{chevron || ''}</Text>
      <View style={{ width: 20, alignItems: 'center' }}>{icon}</View>
      <View style={{ flex: 2, minWidth: 0 }}>
        <Text style={{ fontSize: typeScale.body, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{name}</Text>
        {sub ? <Text style={{ fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{sub}</Text> : null}
      </View>
      {kind ? <Text style={{ flex: 2, minWidth: 0, fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{kind}</Text> : null}
      <View style={{ flexDirection: 'row', gap: spacing.xs }}>{children}</View>
    </View>
  );
}

// Connecting: who connects, then the folders that server shows them, so nobody has to know a share's name. It's only
// asked for when the server can't list them. The password goes to lord-lan for the list, to the action for the mount
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
    lan.saved(ask.kind, ask.addr, ask.share || '').then((s) => { if (s) { setUser(s.user); setPassword(s.password); setRemember(true); } });
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
      onMounted(at, note);
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
          <>
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
          </>
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
  const key = `${ask.host}:${ask.port || 0}${ask.path || ''}`;
  useEffect(() => { lan.saved(ask.kind, key, '').then((s) => { if (s) { setUser(s.user); setPassword(s.password); setRemember(true); } }); }, []);
  const go = async () => {
    if (busy) return;
    setBusy(true); setError(''); setPkg('');
    const r = await lan.connect({ kind: ask.kind, host: ask.host, port: ask.port, user: user.trim(), password, path: ask.path });
    setBusy(false);
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
        {field(tr('files.net.user'), <TextInput value={user} onChange={setUser} autoFocus onSubmit={go} onCancel={onClose} />)}
        {field(tr('files.net.password'), <TextInput secure value={password} onChange={setPassword} onSubmit={go} onCancel={onClose} />)}
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, marginTop: 4 }}>
          <Toggle small on={remember} onChange={setRemember} />
          <Text style={{ flex: 1, fontSize: typeScale.callout, color: t.text, textAlign: 'start' }}>{tr('files.net.remember')}</Text>
        </View>
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

export function NetworkView({ volumes, refreshVolumes, openFolder, setMsg, setModal }) {
  const t = useTheme();
  const [st, setSt] = useState(() => lan.poll());
  const [open, setOpen] = useState(() => new Set());
  const [ask, setAsk] = useState(null);
  const timer = useRef(null);
  useEffect(() => { setModal(!!ask); }, [ask]);
  useEffect(() => () => setModal(false), []);
  const watch = () => {
    if (timer.current) return;
    timer.current = setInterval(() => {
      const s = lan.poll();
      setSt(s);
      if (!s.running) { clearInterval(timer.current); timer.current = null; }
    }, 700);
  };
  const search = () => { const started = lan.start(true); setSt(lan.poll()); if (started || lan.poll().running) watch(); else setMsg(tr('files.net.tooSoon')); };
  // the page being there is the only thing that starts it, and leaving it stops it
  useEffect(() => {
    lan.start();
    setSt(lan.poll());
    watch();
    return () => { if (timer.current) clearInterval(timer.current); timer.current = null; lan.stop(); };
  }, []);

  // machines with a name first
  const hosts = useMemo(() => [...st.hosts].sort((a, b) => (hostName(a) === a.addr) - (hostName(b) === b.addr) || hostName(a).localeCompare(hostName(b), undefined, { numeric: true, sensitivity: 'base' })), [st]);
  const computers = hosts.filter(isComputer), devices = hosts.filter((h) => !isComputer(h));
  const toggle = (a) => setOpen((o) => { const n = new Set(o); if (n.has(a)) n.delete(a); else n.add(a); return n; });
  const mountedAt = (kind, addr, share) => volumes.find((v) => isShareFs(v.fs) && v.name === leafOf(kind, addr, share));
  const copy = (text) => { clipboard.set(text); setMsg(tr('files.net.copied', { text })); };
  const disconnect = (v) => lan.unmount(v.path).then(() => { refreshVolumes(); setMsg(tr('disks.unmounted')); }, (e) => setMsg(e.message));
  const connect = (h, kind, share) => {
    // NFS has no sign-in: a listed export goes straight to it
    if (kind === 'nfs' && share) {
      setMsg(tr('files.net.connecting'));
      lan.mount({ kind, addr: h.addr, share }).then((at) => { refreshVolumes(); setMsg(null); openFolder(at); }, (e) => setMsg(e.message));
      return;
    }
    setAsk({ kind, addr: h.addr, share, host: hostName(h) });
  };
  const shareActions = (h, kind, share) => {
    const v = mountedAt(kind, h.addr, share);
    return v ? <><Button small label={tr('files.net.open')} onClick={() => openFolder(v.path)} /><Button small label={tr('files.net.disconnect')} onClick={() => disconnect(v)} /></>
      : <Button small label={tr('files.net.connect')} onClick={() => connect(h, kind, share)} />;
  };

  const status = {
    'no-lan': 'files.net.noLan', 'no-connector': 'files.net.noConnector', 'no-helper': 'files.net.noHelper', denied: 'files.net.denied',
  }[st.state];
  const scanning = st.running;
  const line = status ? tr(status)
    : scanning ? (st.total ? tr('files.net.scanning', { done: st.probed, total: st.total }) : tr('files.net.listening'))
      : tr('files.net.found', { count: hosts.length });

  const body = (h) => (
    <View>
      {h.domain && h.domain !== h.name ? <Row t={t} depth={1} name={tr('files.net.workgroup', { name: h.domain })} /> : null}
      {h.shares.map((s) => (
        <Row key={`s${s.name}`} t={t} depth={1} icon={<FolderGlyph size={16} />} name={s.name} sub={s.comment} kind={tr('files.net.kind.smb')}>{shareActions(h, 'smb', s.name)}</Row>
      ))}
      {h.smb && !h.shares.length ? (
        <Row t={t} depth={1} icon={<FolderGlyph size={16} />} name={tr(h.listed ? 'files.net.noShares' : 'files.net.signInToList')} kind={tr('files.net.kind.smb')}>
          <Button small label={tr('files.net.connect')} onClick={() => connect(h, 'smb', '')} />
        </Row>
      ) : null}
      {h.exports.map((e) => (
        <Row key={`e${e.path}`} t={t} depth={1} icon={<FolderGlyph size={16} />} name={e.path} sub={e.open ? '' : tr('files.net.restricted')} kind={tr('files.net.kind.nfs')}>{shareActions(h, 'nfs', e.path)}</Row>
      ))}
      {h.nfs && !h.exports.length ? (
        <Row t={t} depth={1} icon={<FolderGlyph size={16} />} name={tr('files.net.nfsUnlisted')} kind={tr('files.net.kind.nfs')}>
          <Button small label={tr('files.net.connect')} onClick={() => connect(h, 'nfs', '')} />
        </Row>
      ) : null}
      {h.services.filter((s) => s.kind !== 'smb' && s.kind !== 'nfs' && s.kind !== 'computer').map((s) => (
        <Row key={`v${s.kind}${s.port}`} t={t} depth={1} name={s.model || s.name || tr(`files.net.kind.${s.kind}`)} sub={serviceUrl(h.addr, s)} kind={tr(`files.net.kind.${s.kind}`)}>
          <Button small label={tr('files.net.copyAddress')} onClick={() => copy(serviceUrl(h.addr, s))} />
        </Row>
      ))}
    </View>
  );
  const hostRow = (h) => {
    const has = h.smb || h.nfs || h.services.length > 0 || h.domain;
    const on = open.has(h.addr);
    return (
      <View key={h.addr}>
        <Row t={t} icon={<DriveGlyph color={t.accent} />} name={hostName(h)} sub={hostName(h) === h.addr ? '' : h.addr} kind={kinds(h)} chevron={has ? (on ? '⌄' : '›') : ''} onClick={has ? () => toggle(h.addr) : undefined}>
          <Button small label={tr('files.net.copyAddress')} onClick={() => copy(h.addr)} />
        </Row>
        {on ? body(h) : null}
      </View>
    );
  };
  const heading = (k) => <Text style={{ fontSize: typeScale.caption, fontWeight: 600, color: t.textDim, marginHorizontal: spacing.m, marginTop: spacing.m, marginBottom: 2, textAlign: 'start' }}>{k}</Text>;
  return (
    <View style={{ flex: 1 }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, height: 38, paddingHorizontal: spacing.m }}>
        <Hairline edge="bottom" />
        <Text style={{ flex: 1, fontSize: typeScale.callout, color: t.textDim, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{line}</Text>
        <Button small label={tr('files.net.search')} onClick={search} disabled={scanning} />
      </View>
      <ScrollView style={{ flex: 1 }}>
        <View style={{ paddingBottom: spacing.m }}>
          {computers.length ? heading(tr('files.net.computers')) : null}
          {computers.map(hostRow)}
          {devices.length ? heading(tr('files.net.devices')) : null}
          {devices.map(hostRow)}
          {!hosts.length && !scanning && !status ? <Text style={{ margin: spacing.xl, fontSize: typeScale.body, color: t.textDim, textAlign: 'center' }}>{tr('files.net.empty')}</Text> : null}
        </View>
      </ScrollView>
      {ask && <ConnectSheet ask={ask} onClose={() => setAsk(null)} onMounted={(at, note) => { setAsk(null); refreshVolumes(); setMsg(note); if (at.startsWith('/')) openFolder(at); }} />}
    </View>
  );
}
