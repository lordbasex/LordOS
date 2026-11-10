// FilesMode.jsx — the Terminal's Files mode (docs/TERMINAL.md "Files mode"): this computer and the server side by
// side, in the terminal's own look, over the tab's ssh connection (sftp.js → lord.sftp). It covers the panes; the
// shell stays alive behind it. A toolbar (up, new folder, refresh, hidden, filter, as root), right-click menus, the
// transfers queue with its speed graph (uploads orange, downloads blue) and the sheets: new folder, rename,
// permissions and owner, information, delete, the sudo password.
import { View, Text, TextInput, Button, ScrollView, ContextMenu, Hairline, fs, clipboard, term, useStore, t as tr } from '@lordos/sdk';
import { useEffect, useRef, useState } from 'react';
import { meta } from './state.js';
import {
  request, transfer, cancel, clearFinished, transfers, closedConns, samplesOf, speedSeries,
  permString, permBoxes, boxesPerm, octal, parseOctal, parentOf, joinPath, baseName, goodName, sortEntries, visible, size, rate, freeName, fromLocal, errCode,
} from './sftp.js';

const UP = '#ff9f0a', DOWN = '#0a84ff';
const MONO = 'monospace';
const homeDir = () => { try { return lord.fs.home(); } catch (e) { return '/'; } };
const shQuote = (p) => `'${String(p).replace(/'/g, "'\\''")}'`;
const when = (m) => {
  if (!m) return '';
  const d = new Date(m > 1e12 ? m : m * 1000);
  const now = new Date();
  return d.toDateString() === now.toDateString() ? d.toTimeString().slice(0, 5) : d.toISOString().slice(0, 10);
};
const errText = (e) => tr(`terminal.files.err.${errCode(e)}`);

// a local entry as the remote ones look (lord.fs gives the mode as octal text, "0755")

export function FilesMode({ t, wid, sid, startLocal }) {
  const [conn, setConn] = useState(null);
  const [state, setState] = useState('opening');      // opening | ready | error
  const [err, setErr] = useState(null);
  const [root, setRoot] = useState(false);
  const [rpath, setRpath] = useState('.');
  const [rlist, setRlist] = useState([]);
  const [lpath, setLpath] = useState(startLocal || homeDir());
  const [llist, setLlist] = useState([]);
  const [hidden, setHidden] = useState(false);
  const [filter, setFilter] = useState('');
  const [sel, setSel] = useState({ side: null, names: [] });
  const [menu, setMenu] = useState(null);
  const [sheet, setSheet] = useState(null);
  const [msg, setMsg] = useState(null);
  const box = useRef(null);
  const closedAt = useStore(closedConns);
  const jobs = useStore(transfers);

  const open = async (asRoot = false, password) => {
    setState('opening');
    try {
      const r = await request('open', { sid, ...(asRoot ? { root: true } : {}), ...(password ? { password } : {}) });
      if (conn) request('close', { conn }).catch(() => {});
      setConn(r.conn);
      setRoot(!!r.root);
      setState('ready');
      return r.conn;
    } catch (e) {
      if (asRoot && e.code === 'root' && !password) { setSheet({ kind: 'password' }); setState(conn ? 'ready' : 'error'); return null; }
      setErr(e);
      setState(conn && asRoot ? 'ready' : 'error');
      if (asRoot) setMsg(errText(e));
      return null;
    }
  };
  useEffect(() => { open(); }, []);
  useEffect(() => () => { if (conn) request('close', { conn }).catch(() => {}); }, [conn]);
  useEffect(() => { if (closedAt && conn) { setState('error'); setErr({ code: 'closed' }); } }, [closedAt]);

  const loadRemote = async (p = rpath, c = conn) => {
    if (!c) return;
    try {
      const r = await request('list', { conn: c, path: p });
      setRpath(r.path);
      setRlist(r.entries || []);
    } catch (e) { if (e.code === 'busy') return; setMsg(errText(e)); }
  };
  const loadLocal = async (p = lpath) => {
    try { setLlist((await fs.listAsync(p)).map(fromLocal)); setLpath(p); } catch (e) { setMsg(tr('terminal.files.err.denied')); }
  };
  // the first listing: where the remote shell is (its OSC 7, meta.remoteCwd), else the remote home; a new connection
  // (As Root) stays in the folder shown
  useEffect(() => {
    if (!conn) return;
    const start = rpath !== '.' ? rpath : meta.get()[sid]?.remoteCwd || '.';
    request('list', { conn, path: start }).then((r) => { setRpath(r.path); setRlist(r.entries || []); }, () => loadRemote('.', conn));
  }, [conn]);
  useEffect(() => { loadLocal(lpath); }, []);
  // refreshed by itself: the server every 5 s while shown (and after our own operations), this computer every 3 s
  useEffect(() => {
    if (state !== 'ready') return undefined;
    const a = setInterval(() => { if (!sheet && !menu) loadRemote(); }, 5000);
    const b = setInterval(() => { if (!sheet && !menu) loadLocal(); }, 3000);
    return () => { clearInterval(a); clearInterval(b); };
  }, [state, rpath, lpath, sheet, menu, conn]);
  // a finished upload shows on the server's side, a finished download here
  const finished = jobs.filter((j) => j.state === 'done').length;
  useEffect(() => { if (finished) { loadRemote(); loadLocal(); } }, [finished]);

  const pick = (side, name, e) => {
    if (e && (e.ctrl || e.meta) && sel.side === side) setSel({ side, names: sel.names.includes(name) ? sel.names.filter((n) => n !== name) : [...sel.names, name] });
    else setSel({ side, names: [name] });
  };
  const chosen = (side) => (sel.side === side ? sel.names : []);

  // ---- transfers (folders recursively, at most 5000 files) ----
  const download = async (names) => {
    let taken = new Set(llist.map((e) => e.name));
    const walk = async (rdir, ldir, list, depth) => {
      for (const e of list) {
        if (e.kind === 'd') {
          if (depth > 20) continue;
          const dest = joinPath(ldir, ldir === lpath ? freeName(e.name, taken) : e.name);
          try { await fs.mkdirAsync(dest); } catch (x) { /* there already */ }
          const sub = await request('list', { conn, path: joinPath(rdir, e.name) });
          await walk(sub.path, dest, sub.entries || [], depth + 1);
        } else if (e.kind === '-') {
          const name = ldir === lpath ? freeName(e.name, taken) : e.name;
          taken = new Set([...taken, name]);
          try { await transfer('down', conn, { path: joinPath(rdir, e.name), dest: joinPath(ldir, name) }, e.name); } catch (x) { setMsg(`${e.name}: ${errText(x)}`); }
        }
      }
    };
    try { await walk(rpath, lpath, rlist.filter((e) => names.includes(e.name)), 0); } catch (x) { setMsg(errText(x)); }
  };
  // An upload never replaces a file on the server unasked: names already there are asked about first (Replace, Keep
  // Both, Skip), and that choice applies inside folders that already exist too.
  const upload = async (names, policy = null, ldir = lpath, rdir = rpath, list = llist) => {
    const top = list.filter((e) => names.includes(e.name));
    if (!policy && rdir === rpath) {
      const clash = top.filter((e) => e.kind === '-' && rlist.some((r) => r.name === e.name));
      if (clash.length) { setSheet({ kind: 'conflict', names, count: clash.length, first: clash[0].name }); return; }
    }
    const walk = async (ld, rd, entries, depth, there) => {
      let taken = new Set(there);
      for (const e of entries) {
        if (e.link) continue;                               // a link is never followed: it could lead out of what was chosen
        if (e.kind === 'd') {
          if (depth > 20) continue;
          let existed = false;
          try { await request('mkdir', { conn, path: joinPath(rd, e.name) }); } catch (x) { existed = true; }
          let inside = [];
          if (existed) { try { inside = ((await request('list', { conn, path: joinPath(rd, e.name) })).entries || []).map((r) => r.name); } catch (x) { inside = []; } }
          const sub = (await fs.listAsync(joinPath(ld, e.name))).map(fromLocal);
          await walk(joinPath(ld, e.name), joinPath(rd, e.name), sub, depth + 1, inside);
        } else if (e.kind === '-') {
          let name = e.name;
          if (taken.has(name)) {
            if (policy === 'skip') continue;
            if (policy === 'keep') name = freeName(name, taken);
            else if (policy !== 'replace') continue;          // never replaced without a choice
          }
          taken = new Set([...taken, name]);
          try { await transfer('up', conn, { src: joinPath(ld, e.name), path: joinPath(rd, name), perm: e.perm & 0o777 }, name); } catch (x) { setMsg(`${e.name}: ${errText(x)}`); }
        }
      }
    };
    try { await walk(ldir, rdir, top, 0, rdir === rpath ? rlist.map((r) => r.name) : []); } catch (x) { setMsg(errText(x)); }
  };

  // ---- operations ----
  const run = async (fn) => { try { await fn(); } catch (e) { setMsg(errText(e)); } loadRemote(); loadLocal(); };
  const remoteMenu = (e, name) => {
    const it = rlist.find((x) => x.name === name);
    const names = chosen('remote').includes(name) ? chosen('remote') : [name];
    if (!chosen('remote').includes(name)) setSel({ side: 'remote', names });
    const path = joinPath(rpath, name);
    setMenu({ x: e.x, y: e.y, items: [
      { label: tr('terminal.files.download'), onClick: () => download(names) },
      '-',
      { label: tr('terminal.files.rename'), disabled: names.length !== 1, onClick: () => setSheet({ kind: 'rename', side: 'remote', name, value: name }) },
      { label: tr('terminal.files.newFolder'), onClick: () => setSheet({ kind: 'mkdir', side: 'remote', value: '' }) },
      '-',
      { label: tr('terminal.files.permissions'), disabled: names.length !== 1, onClick: () => setSheet({ kind: 'perm', path, entry: it, boxes: permBoxes(it.perm), octal: octal(it.perm) }) },
      { label: tr('terminal.files.owner'), disabled: names.length !== 1 || it.uid == null, onClick: () => setSheet({ kind: 'owner', path, entry: it, uid: String(it.uid), gid: String(it.gid) }) },
      { label: tr('terminal.files.info'), disabled: names.length !== 1, onClick: () => showInfo(path, it) },   // (file/sha256 refuse a ' or \ in the path)
      '-',
      { label: tr('terminal.files.copyPath'), onClick: () => clipboard.set(path) },
      // pasted into the tab's shell: only while the ssh is up (never into the local shell), and never a path with a quote or a
      // backslash, which shells quote differently (fish takes \' inside single quotes as a quote)
      { label: tr('terminal.files.cdHere'), disabled: it.kind !== 'd' || state !== 'ready' || /['\\\x00-\x1f\x7f]/.test(path), onClick: () => { term.paste(sid, `cd -- ${shQuote(path)}\n`); } },
      '-',
      { label: tr('terminal.files.delete'), destructive: true, onClick: () => setSheet({ kind: 'delete', side: 'remote', names }) },
    ] });
  };
  const localMenu = (e, name) => {
    const names = chosen('local').includes(name) ? chosen('local') : [name];
    if (!chosen('local').includes(name)) setSel({ side: 'local', names });
    setMenu({ x: e.x, y: e.y, items: [
      { label: tr('terminal.files.upload'), onClick: () => upload(names) },
      '-',
      { label: tr('terminal.files.rename'), disabled: names.length !== 1, onClick: () => setSheet({ kind: 'rename', side: 'local', name, value: name }) },
      { label: tr('terminal.files.newFolder'), onClick: () => setSheet({ kind: 'mkdir', side: 'local', value: '' }) },
      { label: tr('terminal.files.copyPath'), onClick: () => clipboard.set(joinPath(lpath, name)) },
      '-',
      { label: tr('terminal.files.toTrash'), destructive: true, onClick: () => run(async () => { for (const n of names) await fs.trashAsync(joinPath(lpath, n)); }) },
    ] });
  };
  const showInfo = async (path, it) => {
    setSheet({ kind: 'info', path, entry: it, file: '…', sha: null });
    try { const r = await request('info', { conn, path, what: 'file' }); setSheet((s) => s && s.kind === 'info' ? { ...s, file: r.text.trim() } : s); }
    catch (e) { setSheet((s) => s && s.kind === 'info' ? { ...s, file: errText(e) } : s); }
  };
  const sha = async (path) => {
    setSheet((s) => ({ ...s, sha: '…' }));
    try { const r = await request('info', { conn, path, what: 'sha256' }); setSheet((s) => s && s.kind === 'info' ? { ...s, sha: (r.text.match(/^[0-9a-f]{64}/) || [errText({})])[0] } : s); }
    catch (e) { setSheet((s) => s && s.kind === 'info' ? { ...s, sha: errText(e) } : s); }
  };

  const applySheet = () => {
    const s = sheet;
    setSheet(null);
    if (s.kind === 'mkdir' && goodName(s.value)) {
      run(() => (s.side === 'remote' ? request('mkdir', { conn, path: joinPath(rpath, s.value) }) : fs.mkdirAsync(joinPath(lpath, s.value))));
    } else if (s.kind === 'rename' && goodName(s.value) && s.value !== s.name) {
      run(() => (s.side === 'remote' ? request('rename', { conn, from: joinPath(rpath, s.name), to: joinPath(rpath, s.value) })
        : fs.renameAsync(joinPath(lpath, s.name), joinPath(lpath, s.value))));
    } else if (s.kind === 'perm') {
      const p = parseOctal(s.octal);
      if (p != null) run(() => request('chmod', { conn, path: s.path, perm: p }));
    } else if (s.kind === 'owner' && /^\d+$/.test(s.uid) && /^\d+$/.test(s.gid)) {
      run(() => request('chown', { conn, path: s.path, uid: +s.uid, gid: +s.gid }));
    } else if (s.kind === 'delete') {
      run(async () => { for (const n of s.names) { const e = rlist.find((x) => x.name === n); await request('remove', { conn, path: joinPath(rpath, n), dir: e && e.kind === 'd' }); } });
    } else if (s.kind === 'conflict') {
      upload(s.names, s.choice || 'keep');
    } else if (s.kind === 'password' && s.value) {
      open(true, s.value);
    }
  };
  const toggleRoot = () => { if (root) open(false); else open(true); };

  // ---- drawing ----
  const pane = (side) => {
    const remote = side === 'remote';
    const list = sortEntries(visible(remote ? rlist : llist, hidden, filter));
    const path = remote ? rpath : lpath;
    const go = (p) => (remote ? loadRemote(p) : loadLocal(p));
    return (
      <View style={{ flex: 1, minWidth: 0 }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, height: 30, paddingHorizontal: 10 }}>
          <Hairline edge="bottom" />
          <Text style={{ fontSize: 10.5, fontWeight: 700, color: t.textDim }}>{tr(remote ? (root ? 'terminal.files.serverRoot' : 'terminal.files.server') : 'terminal.files.local').toUpperCase()}</Text>
          <Text onClick={() => go(parentOf(path))} style={{ fontSize: 13, color: t.accent, paddingHorizontal: 4 }}>↑</Text>
          <Text style={{ flex: 1, minWidth: 0, fontFamily: MONO, fontSize: 12, color: remote ? '#30d158' : t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', direction: 'ltr', textAlign: 'start' }}>{path}</Text>
        </View>
        <View style={{ flexDirection: 'row', height: 22, alignItems: 'center', paddingHorizontal: 12, direction: 'ltr' }}>
          <Text style={{ flex: 1, fontSize: 10.5, color: t.textDim }}>{tr('terminal.files.name')}</Text>
          <Text style={{ width: 70, fontSize: 10.5, color: t.textDim, textAlign: 'end' }}>{tr('terminal.files.size')}</Text>
          <Text style={{ width: 92, fontSize: 10.5, color: t.textDim, textAlign: 'end' }}>{tr('terminal.files.modified')}</Text>
          <Text style={{ width: 92, fontSize: 10.5, color: t.textDim, textAlign: 'end' }}>{tr('terminal.files.perms')}</Text>
        </View>
        <ScrollView style={{ flex: 1 }}>
          <View style={{ paddingHorizontal: 4, paddingBottom: 6 }}>
            {list.map((e) => {
              const on = sel.side === side && sel.names.includes(e.name);
              return (
                <View key={e.name} onClick={(ev) => pick(side, e.name, ev)}
                  onDoubleClick={() => (e.kind === 'd' ? go(joinPath(path, e.name)) : remote ? download([e.name]) : upload([e.name]))}
                  onContextMenu={(ev) => (remote ? remoteMenu(ev, e.name) : localMenu(ev, e.name))}
                  style={{ flexDirection: 'row', alignItems: 'center', height: 24, paddingHorizontal: 8, borderRadius: 5, backgroundColor: on ? t.accent : 'transparent', direction: 'ltr' }}>
                  <Text style={{ width: 16, fontSize: 11, color: on ? '#fff' : e.kind === 'd' ? '#64b5ff' : t.textDim }}>{e.kind === 'd' ? '▸' : e.kind === 'l' ? '↪' : '·'}</Text>
                  <Text style={{ flex: 1, minWidth: 0, fontFamily: MONO, fontSize: 12.5, color: on ? '#fff' : t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{e.name}</Text>
                  <Text style={{ width: 70, fontFamily: MONO, fontSize: 11.5, color: on ? '#fff' : t.textDim, textAlign: 'end' }}>{e.kind === 'd' ? '' : size(e.size)}</Text>
                  <Text style={{ width: 92, fontFamily: MONO, fontSize: 11.5, color: on ? '#fff' : t.textDim, textAlign: 'end' }}>{when(e.mtime)}</Text>
                  <Text style={{ width: 92, fontFamily: MONO, fontSize: 11.5, color: on ? '#fff' : t.textDim, textAlign: 'end' }}>{permString(e.perm, e.kind)}</Text>
                </View>
              );
            })}
            {!list.length && <Text style={{ padding: 12, fontSize: 12, color: t.textDim, textAlign: 'start' }}>{tr('terminal.files.empty')}</Text>}
          </View>
        </ScrollView>
      </View>
    );
  };

  const btn = (label, onClick, opts = {}) => (
    <View onClick={opts.disabled ? undefined : onClick} style={{ height: 24, paddingHorizontal: 9, borderRadius: 6, justifyContent: 'center',
      backgroundColor: opts.on ? (opts.danger ? 'rgba(255,69,58,0.25)' : t.accent) : t.control, opacity: opts.disabled ? 0.45 : 1 }}>
      <Text style={{ fontSize: 12, color: opts.on && !opts.danger ? '#fff' : opts.danger && opts.on ? '#ffb4ad' : t.text, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );

  const running = jobs.filter((j) => j.state === 'run');
  const series = speedSeries(samplesOf(), 60);
  const top = Math.max(1, ...series.map((s) => s.up + s.down));
  const nowUp = series[59].up, nowDown = series[59].down;

  if (state !== 'ready' && !conn) {
    return (
      <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: t.windowBg, alignItems: 'center', justifyContent: 'center', gap: 10, padding: 24 }}>
        <Text style={{ fontSize: 14, color: t.text, textAlign: 'center' }}>{state === 'opening' ? tr('terminal.files.opening') : errText(err)}</Text>
        {state === 'error' && <Text style={{ maxWidth: 520, fontSize: 12, color: t.textDim, textAlign: 'center', lineHeight: 1.4 }}>{tr(err?.code === 'nossh' ? 'terminal.files.noSshHint' : 'terminal.files.retryHint')}</Text>}
        {state === 'error' && <View style={{ flexDirection: 'row', gap: 8 }}><Button label={tr('terminal.files.retry')} onClick={() => open()} /></View>}
      </View>
    );
  }

  return (
    <View ref={box} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: t.windowBg }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, height: 36, paddingHorizontal: 10 }}>
        <Hairline edge="bottom" />
        {btn(tr('terminal.files.newFolder'), () => setSheet({ kind: 'mkdir', side: sel.side || 'remote', value: '' }))}
        {btn(tr('terminal.files.refresh'), () => { loadRemote(); loadLocal(); })}
        {btn(tr('terminal.files.hidden'), () => setHidden((h) => !h), { on: hidden })}
        <Text style={{ flex: 1, minWidth: 0, fontSize: 11, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{msg || tr('terminal.files.autoRefresh')}</Text>
        {btn(tr('terminal.files.asRoot'), toggleRoot, { on: root, danger: true })}
        <TextInput value={filter} onChange={setFilter} placeholder={tr('terminal.files.filter')} style={{ width: 130 }} />
      </View>
      <View style={{ flex: 1, flexDirection: 'row', minHeight: 0 }}>
        {pane('local')}
        <View style={{ width: 48, alignItems: 'center', justifyContent: 'center', gap: 10 }}>
          <Hairline edge="start" />
          <Hairline edge="end" />
          <View onClick={() => sel.side === 'local' && upload(chosen('local'))} style={{ width: 34, height: 30, borderRadius: 7, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(255,159,10,0.18)', opacity: sel.side === 'local' ? 1 : 0.4 }}>
            <Text style={{ fontSize: 15, color: UP }}>→</Text>
          </View>
          <View onClick={() => sel.side === 'remote' && download(chosen('remote'))} style={{ width: 34, height: 30, borderRadius: 7, alignItems: 'center', justifyContent: 'center', backgroundColor: 'rgba(10,132,255,0.18)', opacity: sel.side === 'remote' ? 1 : 0.4 }}>
            <Text style={{ fontSize: 15, color: '#64b5ff' }}>←</Text>
          </View>
        </View>
        {pane('remote')}
      </View>
      {/* transfers */}
      <View style={{ height: jobs.length ? 168 : 0, flexDirection: 'row', overflow: 'hidden' }}>
        <Hairline edge="top" />
        <View style={{ flex: 1, minWidth: 0, padding: 10, gap: 6 }}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
            <Text style={{ fontSize: 12, fontWeight: 600, color: t.text }}>{tr('terminal.files.transfers')}</Text>
            <Text style={{ flex: 1, fontSize: 11.5, color: t.textDim, textAlign: 'start' }}>{tr('terminal.files.active', { count: running.length })}</Text>
            {btn(tr('terminal.files.clearFinished'), clearFinished, { disabled: running.length === jobs.length })}
          </View>
          <ScrollView style={{ flex: 1 }}>
            {[...jobs].reverse().map((j) => {
              const pct = j.total ? Math.min(100, Math.round((j.done / j.total) * 100)) : j.state === 'done' ? 100 : 0;
              const col = j.kind === 'up' ? UP : DOWN;
              return (
                <View key={j.job} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, height: 22, direction: 'ltr' }}>
                  <Text style={{ width: 14, fontSize: 12, fontWeight: 700, color: col }}>{j.kind === 'up' ? '↑' : '↓'}</Text>
                  <Text style={{ width: 200, fontFamily: MONO, fontSize: 11.5, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', textAlign: 'start' }}>{j.name}</Text>
                  <View style={{ flex: 1, height: 7, borderRadius: 4, backgroundColor: t.control, overflow: 'hidden' }}>
                    <View style={{ width: `${pct}%`, height: 7, borderRadius: 4, backgroundColor: j.state === 'error' ? '#ff453a' : col }} />
                  </View>
                  <Text style={{ width: 120, fontSize: 11, color: j.state === 'error' ? '#ff6961' : t.textDim, textAlign: 'end', whiteSpace: 'nowrap' }}>
                    {j.state === 'run' ? `${size(j.done)} / ${size(j.total)}` : j.state === 'done' ? tr('terminal.files.done') : j.state === 'cancelled' ? tr('terminal.files.cancelled') : errText({ code: j.error })}
                  </Text>
                  <Text onClick={j.state === 'run' ? () => cancel(j.job) : undefined} style={{ width: 16, fontSize: 12, color: j.state === 'run' ? t.textDim : 'transparent' }}>✕</Text>
                </View>
              );
            })}
          </ScrollView>
        </View>
        <View style={{ width: 280, padding: 10, gap: 6 }}>
          <Hairline edge="start" />
          <View style={{ flexDirection: 'row', gap: 12 }}>
            <Text style={{ fontSize: 11.5, color: UP }}>{`↑ ${rate(nowUp)}`}</Text>
            <Text style={{ fontSize: 11.5, color: '#64b5ff' }}>{`↓ ${rate(nowDown)}`}</Text>
          </View>
          <View style={{ flex: 1, flexDirection: 'row', alignItems: 'flex-end', gap: 1 }}>
            {series.map((s, i) => (
              <View key={i} style={{ flex: 1, height: '100%', justifyContent: 'flex-end' }}>
                <View style={{ height: `${(s.up / top) * 100}%`, backgroundColor: UP }} />
                <View style={{ height: `${(s.down / top) * 100}%`, backgroundColor: DOWN }} />
              </View>
            ))}
          </View>
          <Text style={{ fontSize: 10.5, color: t.textDim }}>{tr('terminal.files.lastMinute')}</Text>
        </View>
      </View>
      {menu && <ContextMenu x={menu.x - (box.current ? lord.getLayout(box.current.id)[0] : 0)} y={menu.y - (box.current ? lord.getLayout(box.current.id)[1] : 0)} items={menu.items} onClose={() => setMenu(null)} />}
      {sheet && <Sheet t={t} sheet={sheet} setSheet={setSheet} onApply={applySheet} onSha={sha} />}
    </View>
  );
}

function Sheet({ t, sheet, setSheet, onApply, onSha }) {
  const s = sheet;
  const set = (patch) => setSheet({ ...s, ...patch });
  const title = tr(`terminal.files.sheet.${s.kind}`, { count: s.names ? s.names.length : 1, name: s.names ? s.names[0] : s.name || '' });
  const row = (label, el) => (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
      <Text style={{ width: 96, fontSize: 12, color: t.textDim, textAlign: 'end' }}>{label}</Text>
      <View style={{ flex: 1, minWidth: 0 }}>{el}</View>
    </View>
  );
  const mono = (v) => <Text style={{ fontFamily: MONO, fontSize: 12, color: t.text, textAlign: 'start', direction: 'ltr' }}>{v}</Text>;
  let body = null;
  if (s.kind === 'mkdir' || s.kind === 'rename') body = <TextInput autoFocus value={s.value} onChange={(v) => set({ value: v })} onSubmit={onApply} />;
  else if (s.kind === 'password') body = (
    <View style={{ gap: 8 }}>
      <Text style={{ fontSize: 12, color: t.textDim, textAlign: 'start', lineHeight: 1.4 }}>{tr('terminal.files.passwordHint')}</Text>
      <TextInput autoFocus secure value={s.value || ''} onChange={(v) => set({ value: v })} onSubmit={onApply} />
    </View>
  );
  else if (s.kind === 'conflict') body = (
    <View style={{ gap: 8 }}>
      <Text style={{ fontSize: 12.5, color: t.text, textAlign: 'start', lineHeight: 1.4 }}>{tr('terminal.files.conflictHint', { count: s.count, name: s.first })}</Text>
      <View style={{ flexDirection: 'row', gap: 8 }}>
        {['replace', 'keep', 'skip'].map((c) => (
          <Button key={c} small primary={(s.choice || 'keep') === c} label={tr(`terminal.files.conflict.${c}`)} onClick={() => set({ choice: c })} />
        ))}
      </View>
    </View>
  );
  else if (s.kind === 'delete') body = <Text style={{ fontSize: 12.5, color: t.text, textAlign: 'start', lineHeight: 1.4 }}>{tr('terminal.files.deleteHint')}</Text>;
  else if (s.kind === 'perm') body = (
    <View style={{ gap: 8 }}>
      {['owner', 'group', 'others'].map((who, wi) => row(tr(`terminal.files.who.${who}`), (
        <View style={{ flexDirection: 'row', gap: 14 }}>
          {['read', 'write', 'exec'].map((what, ki) => {
            const on = s.boxes[wi][ki];
            return (
              <View key={what} onClick={() => { const b = s.boxes.map((r) => [...r]); b[wi][ki] = !on; set({ boxes: b, octal: octal(boxesPerm(b) | ((parseOctal(s.octal) || 0) & 0o7000)) }); }}
                style={{ flexDirection: 'row', alignItems: 'center', gap: 5 }}>
                <View style={{ width: 14, height: 14, borderRadius: 3, alignItems: 'center', justifyContent: 'center', backgroundColor: on ? t.accent : t.control }}>
                  {on ? <Text style={{ fontSize: 10, color: '#fff', fontWeight: 700 }}>✓</Text> : null}
                </View>
                <Text style={{ fontSize: 12, color: t.text }}>{tr(`terminal.files.what.${what}`)}</Text>
              </View>
            );
          })}
        </View>
      )))}
      {row(tr('terminal.files.octal'), <TextInput value={s.octal} onChange={(v) => { const p = parseOctal(v); set(p == null ? { octal: v } : { octal: v, boxes: permBoxes(p) }); }} style={{ width: 90 }} />)}
    </View>
  );
  else if (s.kind === 'owner') body = (
    <View style={{ gap: 8 }}>
      {row(tr('terminal.files.uid'), <TextInput value={s.uid} onChange={(v) => set({ uid: v })} style={{ width: 110 }} />)}
      {row(tr('terminal.files.gid'), <TextInput value={s.gid} onChange={(v) => set({ gid: v })} style={{ width: 110 }} />)}
      <Text style={{ fontSize: 11.5, color: t.textDim, textAlign: 'start' }}>{tr('terminal.files.ownerHint', { owner: s.entry.owner || s.entry.uid, group: s.entry.group || s.entry.gid })}</Text>
    </View>
  );
  else if (s.kind === 'info') body = (
    <View style={{ gap: 7 }}>
      {row(tr('terminal.files.type'), mono(s.file))}
      {row(tr('terminal.files.size'), mono(`${size(s.entry.size)} (${s.entry.size || 0})`))}
      {row(tr('terminal.files.owner'), mono(`${s.entry.owner || s.entry.uid} : ${s.entry.group || s.entry.gid}`))}
      {row(tr('terminal.files.perms'), mono(`${permString(s.entry.perm, s.entry.kind)}  ${octal(s.entry.perm)}`))}
      {row('SHA-256', s.sha ? mono(s.sha) : <Button small label={tr('terminal.files.computeSha')} onClick={() => onSha(s.path)} />)}
    </View>
  );
  const plain = s.kind === 'info';
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.35)', alignItems: 'center', paddingTop: 70 }}>
      <View style={{ width: 460, padding: 16, gap: 12, borderRadius: 12, backgroundColor: t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 18px 44px rgba(0,0,0,0.5)' }}>
        <Text style={{ fontSize: 13.5, fontWeight: 600, color: t.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{title}</Text>
        {s.path && !plain && <Text style={{ fontFamily: MONO, fontSize: 11.5, color: t.textDim, textAlign: 'start', direction: 'ltr' }}>{s.path}</Text>}
        {plain && <Text style={{ fontFamily: MONO, fontSize: 11.5, color: t.textDim, textAlign: 'start', direction: 'ltr' }}>{s.path}</Text>}
        {body}
        <View style={{ flexDirection: 'row', justifyContent: 'flex-end', gap: 8 }}>
          {plain ? <Button primary label={tr('terminal.files.close')} onClick={() => setSheet(null)} /> : (
            <>
              <Button label={tr('terminal.files.cancel')} onClick={() => setSheet(null)} />
              <Button primary label={tr(s.kind === 'delete' ? 'terminal.files.delete' : 'terminal.files.apply')} onClick={onApply} />
            </>
          )}
        </View>
      </View>
    </View>
  );
}

export { baseName };
