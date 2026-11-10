// The Connect tab (the approved mocks Main and ConnectCloud): the favorites and their groups, Quick Connect and the
// recent connections in the sidebar; the selected favorite's form (Standard, Socket, SSH, Cloud), its color, TLS and
// the production / read-only switches; Test Connection and Connect. The password is asked in a sheet when Passwords
// doesn't keep it or the server refuses it; a certificate that doesn't check asks to be trusted by its sha256. The first
// run (and File > Import Connections) offers Sequel Ace's and Sequel Pro's favorites.
import { View, Text, ScrollView, TextInput, FilePicker, ConfirmDialog, AutofillButton, ContextMenu, useStore, sqlAvailable, sqlPasswords, t as tr } from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { usePal, Icons, Dot, Btn, IconBtn, Check, Popup, Field, Box, Note, Sheet, MONO, MenuGlyph } from './ui.jsx';
import { store, favRev, readFavorites, writeFavorites, openSession, testFavorite, prefs, setPrefs } from './conn.js';
import { FAV_COLORS, colorOf, groupColor, blankFavorite, normalizeFavorite, validateFavorite, cleanFavorite, newFavoriteId, newGroupId, groupTree } from './model.js';
import { errText, cloudOptions, ago, favLine, serverName, favProblems } from './texts.js';
import { findSequelFiles, previewImports } from './importer.js';
import { mergeImport } from './plist.js';

// asks from the window's menu (File > Import Connections…)
export const connectAsks = { importNow: null };

const QUICK = '__quick';

export function Connect({ onConnected }) {
  const p = usePal();
  const rev = useStore(favRev);
  const st = useStore(store);
  const file = useMemo(() => readFavorites(), [rev]);
  const avail = sqlAvailable();
  const [sel, setSel] = useState(() => file.favorites.find((f) => !f.quick)?.id || QUICK);
  const [draft, setDraft] = useState(null);           // the form: a copy of the selected favorite
  const [dirty, setDirty] = useState(false);
  const [password, setPassword] = useState('');
  const [status, setStatus] = useState(null);         // { kind, text }
  const [busy, setBusy] = useState('');               // 'test' | 'connect'
  const [sheet, setSheet] = useState(null);
  const [groupMenu, setGroupMenu] = useState(null);   // { x, y, group }: a folder's own menu (right click), empty ones too
  const rootRef = useRef(null);           // { kind: 'password' | 'cert' | 'import' | 'group' | 'delete' | 'pick', … }
  const [banner, setBanner] = useState(null);         // first run: what the import would bring
  const [collapsed, setCollapsed] = useState({});
  const nextStatus = useRef(null);                    // a message that survives the selection changing to what it's about

  // the form follows the selection
  useEffect(() => {
    const f = sel === QUICK ? null : file.favorites.find((x) => x.id === sel);
    setDraft(f ? normalizeFavorite(f) : blankFavorite({ quick: true, host: '127.0.0.1' }));
    setDirty(false); setPassword(''); setStatus(nextStatus.current); nextStatus.current = null;
  }, [sel]);
  // the file changed (saved here, an import): the form shows what was saved, unless it has edits of its own
  useEffect(() => {
    if (sel === QUICK || dirty) return;
    const f = file.favorites.find((x) => x.id === sel);
    if (f) setDraft(normalizeFavorite(f));
    else setSel(file.favorites.find((x) => !x.quick)?.id || QUICK);
  }, [rev]);

  // first run: look for Sequel Ace / Sequel Pro once
  useEffect(() => {
    connectAsks.importNow = () => scan(true);
    if (!file.favorites.some((f) => !f.quick) && !prefs().importOffered) scan(false);
    return () => { connectAsks.importNow = null; };
  }, []);
  async function scan(fromMenu) {
    const files = previewImports(await findSequelFiles(), readFavorites());
    const useful = files.filter((f) => f.imp && f.imp.favorites.length);
    if (fromMenu) setSheet({ kind: 'import', files });
    else if (useful.length) setBanner(useful);
  }
  function doImport(entries) {
    let cur = readFavorites();
    let added = 0;
    for (const e of entries) {
      const fresh = previewImports([e], cur)[0];
      if (!fresh.imp) continue;
      cur = mergeImport(cur, fresh.imp);
      added += fresh.imp.favorites.length;
    }
    try { writeFavorites(cur); } catch (err) { setStatus({ kind: 'bad', text: errText({ code: 'save', message: err.message }) }); return; }
    setPrefs({ importOffered: true });
    setBanner(null); setSheet(null);
    const done = { kind: 'ok', text: tr('databases.import.done', { count: added }) };
    const first = cur.favorites.find((f) => !f.quick);
    if (first && sel === QUICK) { nextStatus.current = done; setSel(first.id); } else setStatus(done);
  }

  // ---- favorites file edits ----
  const save = (next) => { try { writeFavorites(next); return true; } catch (e) { setStatus({ kind: 'bad', text: errText({ code: 'save', message: e.message }) }); return false; } };
  const set = (patch) => { setDraft((d) => ({ ...d, ...patch })); setDirty(true); setStatus(null); };
  // a color is applied at once: the dot in the list changes without Save Changes (the rest of the draft stays a draft)
  function setColor(i) {
    const color = draft.color === i ? null : i;
    setDraft((d) => ({ ...d, color }));
    if (draft.quick) return;
    const cur = readFavorites();
    if (cur.favorites.some((x) => x.id === draft.id)) save({ ...cur, favorites: cur.favorites.map((x) => (x.id === draft.id ? { ...x, color } : x)) });
  }
  function setGroupColor(g, color) {
    const cur = readFavorites();
    save({ ...cur, groups: cur.groups.map((x) => (x.id === g.id ? { ...x, color } : x)) });
  }
  const setIn = (k, patch) => { setDraft((d) => ({ ...d, [k]: { ...d[k], ...patch } })); setDirty(true); setStatus(null); };

  // the draft as a favorite in the file → its saved form, or null (with the problem shown)
  function commit({ quick = draft.quick, asNew = false } = {}) {
    const cur = readFavorites();
    const others = cur.favorites.filter((f) => f.id !== draft.id || asNew);
    let f = { ...draft, quick };
    if (quick && !f.name) f.name = `${f.user || ''}@${f.type === 'socket' ? f.socket.split('/').pop() : f.host}`;
    if (quick) {
      // one quick favorite per server and user: reuse it
      const same = cur.favorites.find((x) => x.quick && x.type === f.type && x.host === f.host && Number(x.port) === Number(f.port) && x.user === f.user && x.socket === f.socket);
      if (same) f.id = same.id;
    }
    if (!f.id || asNew) f.id = newFavoriteId(f.name || f.host, others.map((x) => x.id));
    const problems = validateFavorite(f, others);
    if (problems.length) { setStatus({ kind: 'bad', text: favProblems(problems) }); return null; }
    const clean = cleanFavorite(f);
    const list = cur.favorites.some((x) => x.id === clean.id) ? cur.favorites.map((x) => (x.id === clean.id ? clean : x)) : [...cur.favorites, clean];
    if (!save({ ...cur, favorites: list })) return null;
    setDirty(false);
    return clean;
  }
  function saveChanges() {
    const quick = draft.quick;
    const f = commit({ quick: false, asNew: quick });
    if (!f) return;
    const done = { kind: 'ok', text: tr('databases.fav.saved') };
    if (f.id !== sel) { nextStatus.current = done; setSel(f.id); } else setStatus(done);
  }
  function newFavorite(group = null) {
    const cur = readFavorites();
    const name = tr('databases.fav.newName');
    const f = cleanFavorite(blankFavorite({ id: newFavoriteId(name, cur.favorites.map((x) => x.id)), name, host: '127.0.0.1', group }));
    if (save({ ...cur, favorites: [...cur.favorites, f] })) setSel(f.id);
  }
  function duplicate() {
    if (!draft || draft.quick) return;
    const cur = readFavorites();
    const name = tr('databases.fav.copyName', { name: draft.name });
    const f = cleanFavorite({ ...draft, id: newFavoriteId(name, cur.favorites.map((x) => x.id)), name, imported: undefined, trust: '' });
    if (save({ ...cur, favorites: [...cur.favorites, f] })) setSel(f.id);
  }
  function remove(fav) {
    const cur = readFavorites();
    if (save({ ...cur, favorites: cur.favorites.filter((x) => x.id !== fav.id) })) {
      sqlPasswords.remove(fav);
      setSel(cur.favorites.find((x) => !x.quick && x.id !== fav.id)?.id || QUICK);
    }
    setSheet(null);
  }
  function moveTo(group) {
    const cur = readFavorites();
    save({ ...cur, favorites: cur.favorites.map((x) => (x.id === draft.id ? { ...x, group } : x)) });
  }
  function saveGroup(name, g, color = null) {
    const cur = readFavorites();
    const groups = g ? cur.groups.map((x) => (x.id === g.id ? { ...x, name, color } : x)) : [...cur.groups, { id: newGroupId(cur.groups.map((x) => x.id)), name, parent: null, expanded: true, color }];
    if (save({ ...cur, groups })) setSheet(null);
  }
  function removeGroup(g) {
    const cur = readFavorites();
    // what was in it moves up to the group's parent
    save({ ...cur, groups: cur.groups.filter((x) => x.id !== g.id).map((x) => (x.parent === g.id ? { ...x, parent: g.parent || null } : x)),
      favorites: cur.favorites.map((x) => (x.group === g.id ? { ...x, group: g.parent || null } : x)) });
  }

  // ---- connecting ----
  async function connect(fav, pw, keep) {
    setBusy('connect'); setStatus(null);
    try {
      const s = await openSession(fav, pw);
      if (keep && pw) sqlPasswords.set(fav, pw);
      setPrefs({ servers: { ...(prefs().servers || {}), [fav.id]: serverName(s.server) } });
      setSheet(null);
      onConnected?.(s);
    } catch (e) {
      if (e.code === 'auth') setSheet({ kind: 'password', fav, error: errText(e), keep: keep !== false });
      else if (e.code === 'cert') {
        const ev = e.data?.sha256 ? e.data : store.get().cert;
        setSheet(ev && ev.sha256 ? { kind: 'cert', fav, ev, pw, keep } : null);
        if (!ev?.sha256) setStatus({ kind: 'bad', text: errText(e) });
      } else { setSheet(null); setStatus({ kind: 'bad', text: errText(e) }); }
    } finally { setBusy(''); }
  }
  async function go() {
    if (!avail) { setStatus({ kind: 'bad', text: errText({ code: 'unavailable' }) }); return; }
    const fav = dirty || draft.quick ? commit() : cleanFavorite(draft);
    if (!fav) return;
    if (fav.type === 'ssh') { setStatus({ kind: 'warn', text: tr('databases.ssh.later') }); return; }
    if (password) { connect(fav, password, false); return; }
    const kept = await sqlPasswords.get(fav);
    if (kept !== null && kept !== undefined) connect(fav, kept, false);
    else setSheet({ kind: 'password', fav, error: '', keep: true });
  }
  async function test() {
    if (!avail) { setStatus({ kind: 'bad', text: errText({ code: 'unavailable' }) }); return; }
    const fav = dirty || draft.quick ? commit() : cleanFavorite(draft);
    if (!fav) return;
    setBusy('test'); setStatus(null);
    try {
      const pw = password || (await sqlPasswords.get(fav)) || '';
      const r = await testFavorite(fav, pw);
      const tls = r.tls ? tr('databases.test.tls', { version: r.tls.version || '' }) : tr('databases.test.noTls');
      setStatus({ kind: 'ok', text: tr('databases.test.ok', { tls, server: r.server?.version || '', ms: r.ms }) });
    } catch (e) {
      if (e.code === 'cert' && (e.data?.sha256 || store.get().cert?.sha256)) setSheet({ kind: 'cert', fav, ev: e.data?.sha256 ? e.data : store.get().cert, test: true });
      else setStatus({ kind: 'bad', text: errText(e) });
    } finally { setBusy(''); }
  }
  async function keepPassword() {
    if (!password) { setStatus({ kind: 'warn', text: tr('databases.pw.typeFirst') }); return; }
    const fav = dirty || draft.quick ? commit() : cleanFavorite(draft);
    if (!fav) return;
    const okKept = await sqlPasswords.set(fav, password);
    setStatus(okKept ? { kind: 'ok', text: tr('databases.pw.kept') } : { kind: 'bad', text: tr('databases.pw.notKept') });
  }
  function trust(sh) {
    const cur = readFavorites();
    const list = cur.favorites.map((x) => (x.id === sh.fav.id ? { ...x, trust: sh.ev.sha256 } : x));
    if (!save({ ...cur, favorites: list })) return;
    const fav = cleanFavorite({ ...sh.fav, trust: sh.ev.sha256 });
    setSheet(null);
    if (sh.test) test(); else if (sh.pw !== undefined && sh.pw !== null) connect(fav, sh.pw, sh.keep); else go();
  }

  if (!draft) return null;
  const visible = file.favorites.filter((f) => !f.quick);
  const recent = (prefs().recent || []).map((r) => ({ r, f: file.favorites.find((x) => x.id === r.id) })).filter((x) => x.f).slice(0, 5);
  const tree = groupTree(file.groups);
  const servers = prefs().servers || {};
  const lastAt = (id) => (prefs().recent || []).find((r) => r.id === id)?.at;
  const curGroup = (id) => file.groups.find((g) => g.id === id);

  const favRow = (f, depth = 0) => (
    <SideFav key={f.id} name={f.name} sub={favLine(f, servers[f.id])} color={colorOf(f)} on={sel === f.id} depth={depth} onClick={() => setSel(f.id)} />
  );
  const actions = [
    { label: tr('databases.fav.duplicate'), disabled: draft.quick, onClick: duplicate },
    { label: tr('databases.fav.delete'), disabled: draft.quick, destructive: true, onClick: () => setSheet({ kind: 'delete', fav: draft }) },
    '-',
    { label: tr('databases.fav.noGroup'), checked: !draft.group, disabled: draft.quick, onClick: () => moveTo(null) },
    ...tree.map(({ group: g, depth }) => ({ label: `${'  '.repeat(depth)}${g.name || tr('databases.group.untitled')}`, checked: draft.group === g.id, disabled: draft.quick, onClick: () => moveTo(g.id) })),
    '-',
    ...(draft.group ? [
      { label: tr('databases.group.rename'), onClick: () => setSheet({ kind: 'group', group: curGroup(draft.group), name: curGroup(draft.group)?.name || '' }) },
      { label: tr('databases.group.delete'), onClick: () => removeGroup(curGroup(draft.group)) },
      '-',
    ] : []),
    { label: tr('databases.import.menu'), onClick: () => scan(true) },
  ];

  const kind = draft.type;
  const srv = !draft.quick ? servers[draft.id] : '';
  const at = !draft.quick ? lastAt(draft.id) : null;
  const tlsOn = draft.tls.mode === 'required' || draft.tls.mode === 'verify';
  const pwField = (
    <View style={{ flexDirection: 'row', gap: 8 }}>
      <TextInput secure value={password} onChange={setPassword} placeholder={tr('databases.pw.placeholder')} style={{ flex: 1 }} onSubmit={go} />
      <PasswordsFill fav={draft} onFill={({ username, password: pw }) => { setPassword(pw || ''); if (username && !draft.user) set({ user: username }); }} />
      <Btn label={tr('databases.pw.keep')} icon={(c) => Icons.lock(c, 12)} onClick={keepPassword} />
    </View>
  );

  return (
    <View ref={rootRef} style={{ flex: 1, flexDirection: 'row', minHeight: 0 }}>
      {/* favorites */}
      <View style={{ width: 260, backgroundColor: p.side }}>
        <View style={{ position: 'absolute', right: 0, top: 0, bottom: 0, width: 1, backgroundColor: p.line }} />
        <ScrollView style={{ flex: 1 }} contentStyle={{ paddingBottom: 8 }}>
          <View style={{ paddingHorizontal: 8, paddingTop: 10 }}>
            <SideFav name={tr('databases.quick')} icon={Icons.bolt('#ffd60a', 18)} on={sel === QUICK} onClick={() => setSel(QUICK)} />
          </View>
          <Text style={{ paddingHorizontal: 18, paddingTop: 14, paddingBottom: 6, fontSize: 11, fontWeight: 600, color: p.dim, letterSpacing: 0.5, textAlign: 'start' }}>{tr('databases.favorites')}</Text>
          {visible.filter((f) => !f.group || !curGroup(f.group)).map((f) => favRow(f))}
          {tree.map(({ group: g, depth }) => {
            const hidden = (() => { for (let x = curGroup(g.parent); x; x = curGroup(x.parent)) if (collapsed[x.id] ?? !x.expanded) return true; return false; })();
            if (hidden) return null;
            const closed = collapsed[g.id] ?? !g.expanded;
            return (
              <View key={g.id}>
                <View onClick={() => setCollapsed((c) => ({ ...c, [g.id]: !closed }))}
                  onContextMenu={(e) => { const pt = rootRef.current ? e.local(rootRef.current) : { x: e.x, y: e.y }; setGroupMenu({ x: pt.x, y: pt.y, group: g }); }} style={{ flexDirection: 'row', alignItems: 'center', gap: 6, height: 26, marginHorizontal: 8, paddingLeft: 10 + depth * 14, paddingRight: 10 }}>
                  <Text style={{ width: 10, fontSize: 9, color: p.dim }}>{closed ? '▶' : '▼'}</Text>
                  {Icons.folder(FAV_COLORS[groupColor(g)] || p.dim, 13)}
                  <Text style={{ flex: 1, fontSize: 12.5, fontWeight: 600, color: p.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{g.name || tr('databases.group.untitled')}</Text>
                </View>
                {closed ? null : visible.filter((f) => f.group === g.id).map((f) => favRow(f, depth + 1))}
              </View>
            );
          })}
          {!visible.length ? <Text style={{ paddingHorizontal: 18, paddingVertical: 6, fontSize: 12, color: p.dim, textAlign: 'start' }}>{tr('databases.fav.none')}</Text> : null}
          {recent.length ? <Text style={{ paddingHorizontal: 18, paddingTop: 14, paddingBottom: 6, fontSize: 11, fontWeight: 600, color: p.dim, letterSpacing: 0.5, textAlign: 'start' }}>{tr('databases.recent')}</Text> : null}
          {recent.map(({ r, f }) => (
            <SideFav key={`r${f.id}`} name={f.quick ? f.name : f.name} sub={ago(r.at)} icon={Icons.clock(p.dim, 14)} on={false}
              onClick={() => setSel(f.id)} />
          ))}
        </ScrollView>
        <View style={{ height: 38, flexDirection: 'row', alignItems: 'center', gap: 2, paddingHorizontal: 8 }}>
          <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 1, backgroundColor: p.line }} />
          <IconBtn glyph="+" size={17} title={tr('databases.fav.new')} onClick={() => newFavorite(draft.quick ? null : draft.group)} />
          <IconBtn icon={(c) => Icons.folder(c, 15)} title={tr('databases.group.new')} onClick={() => setSheet({ kind: 'group', group: null, name: '' })} />
          <View style={{ flex: 1 }} />
          <MenuGlyph title={tr('databases.fav.actions')} items={actions} />
        </View>
      </View>

      {/* the form */}
      <ScrollView style={{ flex: 1 }} contentStyle={{ alignItems: 'center', paddingHorizontal: 40, paddingVertical: 24 }}>
        <View style={{ width: 600, maxWidth: '100%', gap: 16 }}>
          {!avail ? <Note kind="warn">{tr('databases.unavailable')}</Note> : null}
          {banner ? (
            <Note kind="info" action={(
              <View style={{ flexDirection: 'row', gap: 6 }}>
                <Btn small primary label={tr('databases.import.go')} onClick={() => doImport(banner)} />
                <Btn small label={tr('databases.import.notNow')} onClick={() => { setBanner(null); setPrefs({ importOffered: true }); }} />
              </View>
            )}>{bannerText(banner)}</Note>
          ) : null}
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 14 }}>
            <View style={{ width: 44, height: 44, borderRadius: 11, alignItems: 'center', justifyContent: 'center', background: 'linear-gradient(180deg, #2f7cf6, #1858c9)' }}>{Icons.db('#fff', 26)}</View>
            <View style={{ flex: 1, gap: 2 }}>
              <Text style={{ fontSize: 18, fontWeight: 700, color: p.text, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden' }}>{draft.quick ? tr('databases.quick') : draft.name || tr('databases.fav.untitled')}</Text>
              <Text style={{ fontSize: 12, color: p.dim, textAlign: 'start' }}>
                {draft.quick ? tr('databases.quickHint') : [srv, at ? tr('databases.lastConnected', { when: ago(at) }) : tr('databases.neverConnected')].filter(Boolean).join(' · ')}
              </Text>
            </View>
            <ColorDots value={draft.color} onPick={setColor} />
          </View>

          <View style={{ alignItems: 'center' }}>
            <Seg value={kind} onChange={(v) => set({ type: v, ...(v === 'cloud' && draft.tls.mode !== 'verify' ? { tls: { ...draft.tls, mode: 'verify' } } : null) })}
              options={[['tcp', tr('databases.type.tcp')], ['socket', tr('databases.type.socket')], ['ssh', tr('databases.type.ssh')], ['cloud', tr('databases.type.cloud')]]} />
          </View>

          {kind === 'cloud' ? (
            <>
              <Box title={tr('databases.box.cloud')}>
                {!draft.quick ? <Field label={tr('databases.f.name')}><TextInput value={draft.name} onChange={(v) => set({ name: v })} /></Field> : null}
                <Field label={tr('databases.f.service')}><Popup width={400} value={draft.cloud.service || 'rds'} options={cloudOptions()} onChange={(v) => setIn('cloud', { service: v })} /></Field>
                <Field label={tr('databases.f.endpoint')}><TextInput value={draft.host} onChange={(v) => set({ host: v.trim() })} placeholder={tr('databases.f.endpointHint')} /></Field>
                <Field label={tr('databases.f.port')}><TextInput value={String(draft.port ?? '')} onChange={(v) => set({ port: v.replace(/\D/g, '').slice(0, 5) })} style={{ width: 100 }} /></Field>
                <Field label={tr('databases.f.database')}><TextInput value={draft.database} onChange={(v) => set({ database: v })} placeholder={tr('databases.f.optional')} /></Field>
              </Box>
              <Box title={tr('databases.box.signIn')}>
                <Field label={tr('databases.f.method')}>
                  <View style={{ flexDirection: 'row', gap: 18 }}>
                    <RadioLabel on label={tr('databases.f.methodPassword')} />
                    <RadioLabel on={false} disabled label={tr('databases.f.methodIam')} />
                  </View>
                </Field>
                <Field label={tr('databases.f.user')}><TextInput value={draft.user} onChange={(v) => set({ user: v })} /></Field>
                <Field label={tr('databases.f.password')}>{pwField}</Field>
                <Field label=""><Text style={{ fontSize: 12, color: p.dim, lineHeight: 1.4, textAlign: 'start' }}>{tr('databases.iamLater')}</Text></Field>
              </Box>
              <Box title={tr('databases.box.encryption')}>
                <Field label={tr('databases.f.tls')}><Check on label={tr('databases.tls.cloudRequired')} disabled onChange={() => {}} /></Field>
                <Field label={tr('databases.f.certificate')}>
                  <Popup width={400} value={draft.tls.ca ? 'file' : 'system'} options={[['system', tr('databases.tls.systemCa')], ['file', draft.tls.ca ? draft.tls.ca.split('/').pop() : tr('databases.tls.caFile')]]}
                    onChange={(v) => (v === 'file' ? setSheet({ kind: 'pick', key: 'ca' }) : setIn('tls', { ca: '' }))} />
                </Field>
                <Field label={tr('databases.f.identity')}><Check on={draft.tls.mode === 'verify'} onChange={(on) => setIn('tls', { mode: on ? 'verify' : 'required' })} label={tr('databases.tls.identity')} /></Field>
              </Box>
            </>
          ) : (
            <>
              <Box title={tr('databases.box.server')}>
                {!draft.quick ? <Field label={tr('databases.f.name')}><TextInput value={draft.name} onChange={(v) => set({ name: v })} /></Field> : null}
                {kind === 'socket'
                  ? <Field label={tr('databases.f.socket')}><TextInput value={draft.socket} onChange={(v) => set({ socket: v.trim() })} placeholder="/run/mysqld/mysqld.sock" /></Field>
                  : <Field label={tr('databases.f.host')}><TextInput value={draft.host} onChange={(v) => set({ host: v.trim() })} placeholder="127.0.0.1" /></Field>}
                <Field label={tr('databases.f.user')}><TextInput value={draft.user} onChange={(v) => set({ user: v })} /></Field>
                <Field label={tr('databases.f.password')}>{pwField}</Field>
                <Field label={tr('databases.f.database')}><TextInput value={draft.database} onChange={(v) => set({ database: v })} placeholder={tr('databases.f.optional')} /></Field>
                {kind !== 'socket' ? <Field label={tr('databases.f.port')}><TextInput value={String(draft.port ?? '')} onChange={(v) => set({ port: v.replace(/\D/g, '').slice(0, 5) })} style={{ width: 100 }} /></Field> : null}
              </Box>
              {kind === 'ssh' ? (
                <Box title={tr('databases.box.ssh')}>
                  <Field label={tr('databases.f.sshHost')}><TextInput value={draft.ssh.host} onChange={(v) => setIn('ssh', { host: v.trim() })} /></Field>
                  <Field label={tr('databases.f.sshUser')}><TextInput value={draft.ssh.user} onChange={(v) => setIn('ssh', { user: v })} /></Field>
                  <Field label={tr('databases.f.sshKey')}>
                    <View style={{ flexDirection: 'row', gap: 8 }}>
                      <TextInput value={draft.ssh.key} onChange={(v) => setIn('ssh', { key: v })} placeholder="~/.ssh/id_ed25519" style={{ flex: 1 }} />
                      <Btn label={tr('databases.choose')} onClick={() => setSheet({ kind: 'pick', key: 'ssh' })} />
                    </View>
                  </Field>
                  <Field label={tr('databases.f.sshPort')}><TextInput value={String(draft.ssh.port ?? '')} onChange={(v) => setIn('ssh', { port: v.replace(/\D/g, '').slice(0, 5) })} style={{ width: 100 }} /></Field>
                  <Field label=""><Text style={{ fontSize: 12, color: p.warn, textAlign: 'start' }}>{tr('databases.ssh.later')}</Text></Field>
                </Box>
              ) : null}
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: 18, paddingHorizontal: 4, flexWrap: 'wrap' }}>
                <Check on={tlsOn} onChange={(on) => setIn('tls', { mode: on ? 'required' : 'preferred' })} label={tr('databases.tls.require')} />
                <Check on={draft.tls.mode === 'verify'} onChange={(on) => setIn('tls', { mode: on ? 'verify' : tlsOn ? 'required' : 'preferred' })} label={tr('databases.tls.verify')} />
                <Check on={!!(draft.tls.cert || draft.tls.key)} onChange={(on) => (on ? setSheet({ kind: 'pick', key: 'cert' }) : setIn('tls', { cert: '', key: '' }))} label={tr('databases.tls.client')} />
              </View>
              {draft.tls.cert || draft.tls.key ? (
                <Box>
                  <Field label={tr('databases.f.cert')}><FileRow path={draft.tls.cert} onPick={() => setSheet({ kind: 'pick', key: 'cert' })} /></Field>
                  <Field label={tr('databases.f.key')}><FileRow path={draft.tls.key} onPick={() => setSheet({ kind: 'pick', key: 'key' })} /></Field>
                  <Field label={tr('databases.f.ca')}><FileRow path={draft.tls.ca} onPick={() => setSheet({ kind: 'pick', key: 'ca' })} onClear={draft.tls.ca ? () => setIn('tls', { ca: '' }) : null} /></Field>
                </Box>
              ) : null}
            </>
          )}

          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 18, paddingHorizontal: 4, flexWrap: 'wrap' }}>
            <Check on={!!draft.production} onChange={(on) => set({ production: on })} label={tr('databases.f.production')} />
            <Check on={!!draft.readOnly} onChange={(on) => set({ readOnly: on })} label={tr('databases.f.readOnly')} />
            {draft.trust ? <Text style={{ fontSize: 12, color: p.dim }}>{tr('databases.cert.trusted', { sha: `${draft.trust.slice(0, 8)}…` })}</Text> : null}
            {draft.trust ? <Btn small link label={tr('databases.cert.forget')} onClick={() => set({ trust: '' })} /> : null}
          </View>

          {status ? <Note kind={status.kind}>{status.text}</Note> : null}

          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, marginTop: 4, paddingHorizontal: 4 }}>
            {draft.quick
              ? <Btn link label={tr('databases.addToFavorites')} onClick={saveChanges} />
              : <Btn link label={tr('databases.saveChanges')} disabled={!dirty} onClick={saveChanges} />}
            <View style={{ flex: 1 }} />
            <Btn label={busy === 'test' ? tr('databases.testing') : tr('databases.test')} disabled={!!busy} onClick={test} />
            <Btn primary label={busy === 'connect' ? tr('databases.connecting') : tr('databases.connect')} disabled={!!busy} onClick={go} />
          </View>
        </View>
      </ScrollView>

      {sheet?.kind === 'password' ? <PasswordSheet sheet={sheet} busy={busy === 'connect'} onCancel={() => setSheet(null)} onGo={(pw, keep) => connect(sheet.fav, pw, keep)} /> : null}
      {sheet?.kind === 'cert' ? <CertSheet sheet={sheet} onCancel={() => setSheet(null)} onTrust={() => trust(sheet)} /> : null}
      {sheet?.kind === 'import' ? <ImportSheet files={sheet.files} onCancel={() => setSheet(null)} onImport={doImport} /> : null}
      {groupMenu ? <ContextMenu x={groupMenu.x} y={groupMenu.y} onClose={() => setGroupMenu(null)} items={[
        { label: tr('databases.fav.new'), onClick: () => newFavorite(groupMenu.group.id) },
        { label: tr('databases.group.rename'), onClick: () => setSheet({ kind: 'group', group: groupMenu.group, name: groupMenu.group.name || '' }) },
        { label: tr('databases.group.color'), submenu: [
          ...FAV_COLORS.map((c, i) => ({ label: tr(`databases.color.${i}`), checked: groupColor(groupMenu.group) === i, onClick: () => setGroupColor(groupMenu.group, i) })),
          '-',
          { label: tr('databases.color.none'), checked: groupColor(groupMenu.group) === null, onClick: () => setGroupColor(groupMenu.group, null) },
        ] },
        '-',
        { label: tr('databases.group.delete'), destructive: true, onClick: () => removeGroup(groupMenu.group) },
      ]} /> : null}
      {sheet?.kind === 'group' ? <GroupSheet sheet={sheet} onCancel={() => setSheet(null)} onSave={(name, color) => saveGroup(name, sheet.group, color)} /> : null}
      {sheet?.kind === 'delete' ? (
        <ConfirmDialog title={tr('databases.fav.deleteTitle', { name: sheet.fav.name })} detail={tr('databases.fav.deleteDetail')} destructive
          okLabel={tr('databases.fav.delete')} cancelLabel={tr('databases.cancel')} onOk={() => remove(sheet.fav)} onCancel={() => setSheet(null)} />
      ) : null}
      {sheet?.kind === 'pick' ? (
        <FilePicker title={tr(`databases.pick.${sheet.key}`)} onClose={() => setSheet(null)} onPick={(path) => {
          if (typeof path !== 'string') return;
          if (sheet.key === 'ssh') setIn('ssh', { key: path });
          else if (sheet.key === 'cert') { setIn('tls', { cert: path }); setSheet(draft.tls.key ? null : { kind: 'pick', key: 'key' }); return; }
          else setIn('tls', { [sheet.key]: path });
          setSheet(null);
        }} />
      ) : null}
    </View>
  );
}

function bannerText(list) {
  const total = list.reduce((n, f) => n + f.imp.favorites.length, 0);
  const first = list[0];
  const app = tr(`databases.import.${first.source}`);
  return first.disk ? tr('databases.import.foundDisk', { count: total, app, disk: first.disk }) : tr('databases.import.foundHome', { count: total, app });
}

function SideFav({ name, sub, color, icon, on, onClick, depth = 0 }) {
  const p = usePal();
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)}
      style={{ flexDirection: 'row', alignItems: 'center', gap: 10, marginHorizontal: 8, marginVertical: 1, paddingLeft: 10 + depth * 14, paddingRight: 10, paddingVertical: sub ? 6 : 7, borderRadius: 7,
        backgroundColor: on ? p.accent : hover ? p.hover : 'transparent' }}>
      {icon || <Dot color={color} size={10} />}
      <View style={{ flex: 1, minWidth: 0 }}>
        <Text style={{ fontSize: 13, color: on ? '#fff' : p.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{name}</Text>
        {sub ? <Text style={{ fontSize: 11, color: on ? 'rgba(255,255,255,0.85)' : p.dim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{sub}</Text> : null}
      </View>
    </View>
  );
}

// the mocks' segmented control (Standard / Socket / SSH / Cloud)
function Seg({ value, options, onChange }) {
  const p = usePal();
  return (
    <View style={{ flexDirection: 'row', backgroundColor: p.input, borderRadius: 8, padding: 2, gap: 2 }}>
      {options.map(([v, label]) => (
        <View key={v} onClick={() => onChange(v)} style={{ paddingHorizontal: 18, height: 28, justifyContent: 'center', borderRadius: 6, backgroundColor: value === v ? (p.dark ? '#4a4a52' : '#ffffff') : 'transparent' }}>
          <Text style={{ fontSize: 12.5, fontWeight: value === v ? 600 : 400, color: value === v ? p.text : p.dim }}>{label}</Text>
        </View>
      ))}
    </View>
  );
}

function RadioLabel({ on, label, disabled }) {
  const p = usePal();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, opacity: disabled ? 0.5 : 1 }}>
      <View style={{ width: 15, height: 15, borderRadius: 8, backgroundColor: on ? '#fff' : 'transparent', border: on ? `5px solid ${p.accent}` : `1.5px solid ${p.dim}` }} />
      <Text style={{ fontSize: 13, color: p.text }}>{label}</Text>
    </View>
  );
}

function FileRow({ path, onPick, onClear }) {
  const p = usePal();
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
      <Text style={{ flex: 1, fontSize: 12, fontFamily: MONO, color: path ? p.text : p.dim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{path || tr('databases.f.none')}</Text>
      {onClear ? <Btn small label={tr('databases.clear')} onClick={onClear} /> : null}
      <Btn small label={tr('databases.choose')} onClick={onPick} />
    </View>
  );
}

// 🔑 next to a password field: what Databases kept for this favorite, and the Passwords entries whose host
// field or site is exactly the favorite's host (none for a socket), while the vault is unlocked
function PasswordsFill({ fav, onFill }) {
  return <AutofillButton app="databases" service={sqlPasswords.service(fav)} account={fav.user || ''} host={fav.type === 'socket' ? '' : fav.host || ''} onFill={onFill} />;
}

function PasswordSheet({ sheet, busy, onCancel, onGo }) {
  const p = usePal();
  const [pw, setPw] = useState('');
  const [keep, setKeep] = useState(sheet.keep !== false);
  const f = sheet.fav;
  return (
    <Sheet title={tr('databases.pw.title', { name: f.name })} onCancel={onCancel} width={440}
      actions={[<Btn key="c" label={tr('databases.cancel')} onClick={onCancel} />, <Btn key="g" primary disabled={busy} label={busy ? tr('databases.connecting') : tr('databases.connect')} onClick={() => onGo(pw, keep)} />]}>
      <Text style={{ fontSize: 12.5, color: p.dim, lineHeight: 1.4, textAlign: 'start' }}>
        {tr('databases.pw.detail', { user: f.user || '—', server: f.type === 'socket' ? f.socket : `${f.host}:${f.port}` })}
      </Text>
      {f.imported?.from ? <Text style={{ fontSize: 12, color: p.dim, lineHeight: 1.4, textAlign: 'start' }}>{tr('databases.pw.imported')}</Text> : null}
      <View style={{ flexDirection: 'row', gap: 8 }}>
        <TextInput secure autoFocus value={pw} onChange={setPw} onSubmit={() => onGo(pw, keep)} placeholder={tr('databases.f.password')} style={{ flex: 1 }} />
        <PasswordsFill fav={f} onFill={({ password }) => setPw(password || '')} />
      </View>
      <Check on={keep} onChange={setKeep} label={tr('databases.pw.keepIn')} />
      {sheet.error ? <Note kind="bad">{sheet.error}</Note> : null}
    </Sheet>
  );
}

function CertSheet({ sheet, onCancel, onTrust }) {
  const p = usePal();
  const ev = sheet.ev;
  const sha = String(ev.sha256 || '').toUpperCase().replace(/[^0-9A-F]/g, '');
  const groups = sha.match(/.{1,4}/g) || [];
  const lines = [];
  for (let i = 0; i < groups.length; i += 8) lines.push(groups.slice(i, i + 8).join(' '));
  return (
    <Sheet title={tr('databases.cert.title')} onCancel={onCancel} width={540}
      actions={[<Btn key="c" label={tr('databases.cancel')} onClick={onCancel} />, <Btn key="t" primary danger label={tr('databases.cert.trust')} onClick={onTrust} />]}>
      <Text style={{ fontSize: 12.5, color: p.text, lineHeight: 1.45, textAlign: 'start' }}>{tr('databases.cert.detail', { name: sheet.fav.name })}</Text>
      {ev.problem ? <Note kind="warn">{tr('databases.cert.problem', { problem: ev.problem })}</Note> : null}
      <Box>
        <Field label={tr('databases.cert.subject')} labelWidth={90}><Text style={{ fontSize: 12, color: p.text, textAlign: 'start' }}>{ev.subject || '—'}</Text></Field>
        <Field label={tr('databases.cert.issuer')} labelWidth={90}><Text style={{ fontSize: 12, color: p.text, textAlign: 'start' }}>{ev.issuer || '—'}</Text></Field>
        <Field label="SHA-256" labelWidth={90} top>
          <View style={{ direction: 'ltr' }}>{lines.map((l, i) => <Text key={i} style={{ fontSize: 12, fontFamily: MONO, color: p.text }}>{l}</Text>)}</View>
        </Field>
      </Box>
      <Text style={{ fontSize: 12, color: p.dim, lineHeight: 1.4, textAlign: 'start' }}>{tr('databases.cert.hint')}</Text>
    </Sheet>
  );
}

function ImportSheet({ files, onCancel, onImport }) {
  const p = usePal();
  const useful = files.filter((f) => f.imp && f.imp.favorites.length);
  return (
    <Sheet title={tr('databases.import.title')} onCancel={onCancel} width={560}
      actions={[<Btn key="c" label={useful.length ? tr('databases.cancel') : tr('databases.close')} onClick={onCancel} />,
        ...(useful.length ? [<Btn key="i" primary label={tr('databases.import.go')} onClick={() => onImport(useful)} />] : [])]}>
      {!files.length ? <Text style={{ fontSize: 12.5, color: p.dim, lineHeight: 1.4, textAlign: 'start' }}>{tr('databases.import.none')}</Text> : null}
      {files.map((f) => (
        <View key={f.path} style={{ flexDirection: 'row', alignItems: 'center', gap: 10, paddingVertical: 4 }}>
          <View style={{ flex: 1, minWidth: 0 }}>
            <Text style={{ fontSize: 13, color: p.text, textAlign: 'start' }}>{tr(`databases.import.${f.source}`)}{f.disk ? ` · ${f.disk}` : ` · ${tr('databases.import.home')}`}</Text>
            <Text style={{ fontSize: 11, color: p.dim, fontFamily: MONO, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{f.path}</Text>
          </View>
          <Text style={{ fontSize: 12, color: f.error ? p.bad : p.dim, whiteSpace: 'nowrap' }}>
            {f.error ? tr(`databases.import.err.${f.error === 'binary' ? 'binary' : 'syntax'}`)
              : tr('databases.import.counts', { count: f.imp.favorites.length, skipped: f.imp.skipped })}
          </Text>
        </View>
      ))}
      {useful.length ? <Text style={{ fontSize: 12, color: p.dim, lineHeight: 1.4, textAlign: 'start' }}>{tr('databases.import.passwords')}</Text> : null}
    </Sheet>
  );
}

function GroupSheet({ sheet, onCancel, onSave }) {
  const p = usePal();
  const [name, setName] = useState(sheet.name || '');
  const [color, setColor] = useState(groupColor(sheet.group));
  const ok = name.trim().length > 0 && name.length <= 120;
  const done = () => ok && onSave(name.trim(), color);
  return (
    <Sheet title={sheet.group ? tr('databases.group.rename') : tr('databases.group.new')} onCancel={onCancel} width={400}
      actions={[<Btn key="c" label={tr('databases.cancel')} onClick={onCancel} />, <Btn key="s" primary disabled={!ok} label={tr('databases.save')} onClick={done} />]}>
      <TextInput autoFocus value={name} onChange={setName} onSubmit={done} placeholder={tr('databases.group.name')} />
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, marginTop: 12 }}>
        <Text style={{ fontSize: 12.5, color: p.dim }}>{tr('databases.group.color')}</Text>
        <ColorDots value={color} onPick={(i) => setColor((c) => (c === i ? null : i))} />
      </View>
    </Sheet>
  );
}

// the seven colors: the chosen one has a ring (a border: the engine draws one shadow, without spread); a second click
// on it takes it away
function ColorDots({ value, onPick }) {
  return (
    <View style={{ flexDirection: 'row', gap: 4, direction: 'ltr', alignItems: 'center' }}>
      {FAV_COLORS.map((c, i) => (
        <View key={c} onClick={() => onPick(i)} title={tr(`databases.color.${i}`)}
          style={{ width: 20, height: 20, borderRadius: 10, alignItems: 'center', justifyContent: 'center', border: `2px solid ${value === i ? c : 'transparent'}` }}>
          <View style={{ width: 12, height: 12, borderRadius: 6, backgroundColor: c }} />
        </View>
      ))}
    </View>
  );
}
