// Backups — scheduled snapshots of the home folder to an external disk or a folder, like Time
// Machine (see docs/SYSTEM.md). The engine lives in lord-server (lord.backup): a lowest-priority
// thread that copies only what changed and hard-links the rest. This app picks the destination,
// schedule and exclusions, shows progress, and lets you browse backups by date and restore a
// file, a folder or everything.
import {
  View, Text, ScrollView, Segmented, Toggle, Button, TextInput, useTheme, useBackup, backup, fs, wm, t as tr, errorText,
  formatBytes, formatDate, formatTime, formatNumber, direction,
  Toolbar, ToolbarButton, ToolbarSpacer, MenuButton, SidebarItem, SectionTitle, EmptyState, GroupBox, FormRow, Hairline, spacing, typeScale,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';

// ---- icon: a clock with a returning arrow (drawn with View) ----
export function Icon({ size }) {
  const s = size, ring = s * 0.62, bw = Math.max(1.5, s * 0.06);
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, alignItems: 'center', justifyContent: 'center',
      background: 'linear-gradient(180deg, #2e8b57 0%, #1d5c3a 100%)', direction: 'ltr' }}>
      <View style={{ width: ring, height: ring, borderRadius: ring / 2, border: `${bw}px solid #fff`, alignItems: 'center', justifyContent: 'center' }}>
        <View style={{ position: 'absolute', left: ring / 2 - bw / 2 - bw, top: ring * 0.16, width: bw, height: ring * 0.36, backgroundColor: '#fff', borderRadius: bw / 2 }} />
        <View style={{ position: 'absolute', left: ring / 2 - bw, top: ring / 2 - bw * 1.5, width: ring * 0.3, height: bw, backgroundColor: '#fff', borderRadius: bw / 2 }} />
      </View>
      {/* the arrow's gap (top left) and its head */}
      <View style={{ position: 'absolute', left: s * 0.13, top: s * 0.17, width: s * 0.2, height: s * 0.2, backgroundColor: '#25744a', borderRadius: s * 0.04 }} />
      <View style={{ position: 'absolute', left: s * 0.16, top: s * 0.3, width: s * 0.17, height: bw * 1.4, backgroundColor: '#fff', borderRadius: bw }} />
      <View style={{ position: 'absolute', left: s * 0.26, top: s * 0.2, width: bw * 1.4, height: s * 0.13, backgroundColor: '#fff', borderRadius: bw }} />
      {/* disk at the bottom */}
      <View style={{ position: 'absolute', left: s * 0.25, bottom: s * 0.08, width: s * 0.5, height: s * 0.07, borderRadius: s * 0.03, backgroundColor: 'rgba(255,255,255,0.55)' }} />
    </View>
  );
}

// ---- helpers ----
const when = (sec) => {
  if (!sec) return '';
  const d = new Date(sec * 1000), now = new Date();
  const same = d.toDateString() === now.toDateString();
  return same ? tr('backup.todayAt', { time: formatTime(d) }) : `${formatDate(d, 'short')} ${formatTime(d)}`;
};
// code lines from C (backup.c, errcode.h) → translated text; anything else is shown as is
const errText = (e) => (e ? errorText(e) : '');
const pct = (a, b) => (b > 0 ? Math.min(1, a / b) : 0);
const join = (a, b) => (a ? `${a}/${b}` : b);

function Bar({ value }) {
  const th = useTheme();
  return (
    <View style={{ height: 6, borderRadius: 3, backgroundColor: th.control, overflow: 'hidden', direction: 'ltr' }}>
      <View style={{ width: `${Math.round(value * 1000) / 10}%`, height: 6, borderRadius: 3, backgroundColor: th.accent }} />
    </View>
  );
}

function Row({ children, style }) {
  return <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, ...style }}>{children}</View>;
}

function Label({ children, dim, size = 13, weight = 400, style }) {
  const th = useTheme();
  return <Text style={{ fontSize: size, fontWeight: weight, color: dim ? th.textDim : th.text, textAlign: 'start', ...style }}>{children}</Text>;
}

// Width of a box after layout (re-measured on every render).
function useWidth(ref, initial) {
  const [w, setW] = useState(initial);
  useEffect(() => {
    const id = setTimeout(() => {
      if (!ref.current) return;
      const lw = Math.round(lord.getLayout(ref.current.id)[2]);
      if (lw > 0 && Math.abs(lw - w) > 2) setW(lw);
    }, 20);
    return () => clearTimeout(id);
  });
  return w;
}

// Small drawn glyphs (no icon font): a folder, a document and an external drive.
function FolderGlyph({ color = '#5ea7ff' }) {
  return (
    <View style={{ width: 16, height: 13, justifyContent: 'flex-end' }}>
      <View style={{ position: 'absolute', top: 0, left: 0, width: 7, height: 4, borderRadius: 1.5, backgroundColor: color }} />
      <View style={{ height: 11, borderRadius: 2.5, backgroundColor: color }} />
    </View>
  );
}
function FileGlyph({ color }) {
  return <View style={{ width: 12, height: 15, marginHorizontal: 2, borderRadius: 2.5, border: `1.5px solid ${color}` }} />;
}
function DriveGlyph({ color }) {
  return (
    <View style={{ width: 26, height: 18, borderRadius: 4, backgroundColor: color, justifyContent: 'center', direction: 'ltr' }}>
      <View style={{ position: 'absolute', right: 5, width: 4, height: 4, borderRadius: 2, backgroundColor: '#30d158' }} />
    </View>
  );
}

function Confirm({ title, body, ok, onOk, onCancel, danger }) {
  const th = useTheme();
  return (
    <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.35)', alignItems: 'center', justifyContent: 'center' }}>
      <View style={{ width: 420, padding: spacing.xl, gap: spacing.m, borderRadius: 12, backgroundColor: th.windowBg, border: `1px solid ${th.separator}`, boxShadow: '0 12px 40px rgba(0,0,0,0.45)' }}>
        <Label size={typeScale.headline} weight={600}>{title}</Label>
        <Label dim style={{ lineHeight: 1.4 }}>{body}</Label>
        <Row style={{ justifyContent: 'flex-end', gap: spacing.s, marginTop: spacing.xs }}>
          <Button label={tr('backup.cancel')} onClick={onCancel} />
          <Button label={ok} primary onClick={onOk} style={danger ? { backgroundColor: '#d9443b' } : null} />
        </Row>
      </View>
    </View>
  );
}

// ---- header: status and buttons ----
function Header({ st }) {
  const th = useTheme();
  const [err, setErr] = useState('');
  const running = st && (st.state === 'running' || st.state === 'paused');
  const p = st?.progress || {};
  let line;
  if (!st) line = tr('backup.unavailable');
  else if (!st.dest) line = tr('backup.noDest');
  else if (st.state === 'running') line = tr('backup.running', { files: formatNumber(p.files || 0, 0), total: formatNumber(p.totalFiles || 0, 0) });
  else if (st.state === 'paused') line = st.battery ? tr('backup.pausedBattery') : tr('backup.paused');
  else if (st.state === 'restoring') line = tr('backup.restoring');
  else if (st.state === 'deleting') line = tr('backup.deleting');
  else if (st.state === 'locked') line = tr('backup.lockedLine');
  else if (st.state === 'unlocking') line = tr('backup.encWorking');
  else if (st.state === 'nodest' || !st.destAvailable) line = tr('backup.destMissing');
  else line = st.lastOk ? tr('backup.lastOk', { when: when(st.lastOk) }) : tr('backup.never');
  const next = st && st.next && !running && st.state !== 'restoring' ? tr('backup.next', { when: when(st.next) }) : '';
  const act = (fn) => { setErr(''); try { fn(); } catch (e) { setErr(String(e.message || e)); } };
  return (
    <View style={{ paddingHorizontal: spacing.xl, paddingTop: spacing.l, paddingBottom: spacing.m, gap: spacing.m }}>
      <Row style={{ gap: spacing.l }}>
        <Icon size={56} />
        <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
          <Label size={typeScale.title} weight={600}>{tr('backup.title')}</Label>
          <Label dim size={typeScale.callout}>{line}</Label>
          {next ? <Label dim size={typeScale.callout}>{next}</Label> : null}
          {st && st.last && st.last.ok === 0 && st.last.error && !running ? <Label size={typeScale.callout} style={{ color: '#e5534b' }}>{tr('backup.lastError', { error: errText(st.last.error) })}</Label> : null}
          {err ? <Label size={typeScale.callout} style={{ color: '#e5534b' }}>{err}</Label> : null}
        </View>
        {running ? (
          <Row style={{ gap: spacing.s }}>
            <Button label={st.userPause ? tr('backup.resume') : tr('backup.pause')} onClick={() => act(() => backup.pause(!st.userPause))} />
            <Button label={tr('backup.stop')} onClick={() => act(() => backup.cancel())} />
          </Row>
        ) : (
          <Button label={tr('backup.now')} primary onClick={() => act(() => backup.runNow())} />
        )}
      </Row>
      {running ? (
        <View style={{ gap: spacing.xs }}>
          <Bar value={pct(p.bytes || 0, p.totalBytes || 0)} />
          <Row>
            <Label dim size={typeScale.caption} style={{ flex: 1, whiteSpace: 'nowrap', overflow: 'hidden' }}>{p.file || ''}</Label>
            <Label dim size={typeScale.caption}>{tr('backup.bytes', { done: formatBytes(p.bytes || 0), total: formatBytes(p.totalBytes || 0) })}</Label>
          </Row>
        </View>
      ) : null}
    </View>
  );
}

// ---- Settings ----
function Settings({ st }) {
  const th = useTheme();
  const [cfg, setCfg] = useState(() => backup.config() || { dest: '', interval: 'hourly', pauseOnBattery: true, exclude: [] });
  const [vols, setVols] = useState(() => { try { return fs.volumes(); } catch (e) { return []; } });
  const [folder, setFolder] = useState('');
  const [ex, setEx] = useState('');
  const [err, setErr] = useState('');
  const home = useMemo(() => { try { return fs.home(); } catch (e) { return ''; } }, []);
  const save = (c) => {
    setErr('');
    try { backup.configure(c); setCfg(backup.config() || c); } catch (e) { setErr(String(e.message || e)); }
  };
  const expand = (p) => (p.startsWith('~') ? home + p.slice(1) : p);
  const short = (p) => (home && p.startsWith(home) ? `~${p.slice(home.length)}` : p);
  useEffect(() => { const id = setInterval(() => { try { setVols(fs.volumes()); } catch (e) { /* no disks */ } }, 5000); return () => clearInterval(id); }, []);
  const removable = vols.filter((v) => v.path !== home && v.path !== '/');
  const addExclude = () => { const v = ex.trim().replace(/^~\/?/, '').replace(/\/+$/, ''); if (v) { save({ ...cfg, exclude: [...(cfg.exclude || []), v] }); setEx(''); } };
  const useFolder = () => folder && save({ ...cfg, dest: expand(folder.trim()) });
  return (
    <ScrollView style={{ flex: 1 }}>
      <View style={{ alignItems: 'center', paddingHorizontal: spacing.xl, paddingTop: spacing.l, paddingBottom: spacing.xxl }}>
        <View style={{ width: '100%', maxWidth: 640, gap: spacing.xxl }}>
          <GroupBox title={tr('backup.dest')} footer={tr('backup.destHelp')}>
            {removable.map((v) => {
              const on = cfg.dest === v.path;
              return (
                <View key={v.path} onClick={() => save({ ...cfg, dest: v.path })}
                  style={{ minHeight: 48, flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingVertical: spacing.s }}>
                  <DriveGlyph color={th.mode === 'dark' ? '#8e8e93' : '#aeaeb2'} />
                  <View style={{ flex: 1, minWidth: 0, gap: 1 }}>
                    <Label weight={500} style={{ whiteSpace: 'nowrap', overflow: 'hidden' }}>{v.name || v.path}</Label>
                    <Label dim size={typeScale.caption}>{tr('backup.volFree', { free: formatBytes(v.free || 0), total: formatBytes(v.total || 0), fs: v.fs || '' })}</Label>
                    {['vfat', 'exfat', 'msdos', 'ntfs', 'ntfs3', 'fuseblk'].includes(v.fs) ? <Label size={typeScale.caption} style={{ color: '#d29922' }}>{tr('backup.noLinks')}</Label> : null}
                  </View>
                  <Text style={{ width: 18, fontSize: 15, fontWeight: 600, color: th.accent, textAlign: 'center' }}>{on ? '✓' : ''}</Text>
                </View>
              );
            })}
            {!removable.length ? <View style={{ minHeight: 36, justifyContent: 'center' }}><Label dim size={typeScale.callout}>{tr('backup.noDisks')}</Label></View> : null}
            <Row style={{ minHeight: 44, gap: spacing.s }}>
              <TextInput value={folder} onChange={setFolder} placeholder={tr('backup.folderPlaceholder')} style={{ flex: 1 }} onSubmit={useFolder} />
              <Button label={tr('backup.useFolder')} onClick={useFolder} />
            </Row>
            {cfg.dest ? (
              <Row style={{ minHeight: 44, gap: spacing.s }}>
                <Label size={typeScale.callout} style={{ flex: 1, minWidth: 0 }}>{tr('backup.current', { path: short(cfg.dest) })}{st && st.destTotal > 0 ? ` · ${tr('backup.free', { free: formatBytes(st.destFree), total: formatBytes(st.destTotal) })}` : ''}</Label>
                <Button small label={tr('backup.stopUsing')} onClick={() => save({ ...cfg, dest: '' })} />
              </Row>
            ) : null}
          </GroupBox>
          <GroupBox title={tr('backup.schedule')} footer={tr('backup.retention')}>
            <Row style={{ minHeight: 44 }}>
              <Segmented appearance="neutral" fill style={{ flex: 1 }} value={cfg.interval} onChange={(v) => save({ ...cfg, interval: v })}
                options={[['hourly', tr('backup.hourly')], ['daily', tr('backup.daily')], ['weekly', tr('backup.weekly')], ['manual', tr('backup.manual')]]} />
            </Row>
            <Row style={{ minHeight: 48, gap: spacing.m, paddingVertical: spacing.s }}>
              <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
                <Label>{tr('backup.battery')}</Label>
                <Label dim size={typeScale.caption} style={{ lineHeight: 1.35 }}>{tr('backup.priority')}</Label>
              </View>
              <Toggle small on={cfg.pauseOnBattery !== false} onChange={(v) => save({ ...cfg, pauseOnBattery: v })} />
            </Row>
          </GroupBox>
          <GroupBox title={tr('backup.exclude')} footer={tr('backup.excludeHelp')}>
            {(cfg.exclude || []).map((x) => (
              <Row key={x} style={{ minHeight: 36 }}>
                <FolderGlyph color={th.textDim} />
                <Label style={{ flex: 1, minWidth: 0, whiteSpace: 'nowrap', overflow: 'hidden' }}>~/{x}</Label>
                <Button small plain label={tr('backup.remove')} onClick={() => save({ ...cfg, exclude: cfg.exclude.filter((y) => y !== x) })} />
              </Row>
            ))}
            <Row style={{ minHeight: 44, gap: spacing.s }}>
              <TextInput value={ex} onChange={setEx} placeholder={tr('backup.excludePlaceholder')} style={{ flex: 1 }} onSubmit={addExclude} />
              <Button label={tr('backup.add')} onClick={addExclude} />
            </Row>
          </GroupBox>
          <Encryption st={st} cfg={cfg} save={save} />
          {err ? <Label size={typeScale.callout} style={{ color: '#e5534b' }}>{err}</Label> : null}
        </View>
      </View>
    </ScrollView>
  );
}

// ---- Encryption (Monocypher: chunked XChaCha20-Poly1305, key from Argon2id) ----
function Encryption({ st, cfg, save }) {
  const th = useTheme();
  const [p1, setP1] = useState('');
  const [p2, setP2] = useState('');
  const [err, setErr] = useState('');
  const [wantOn, setWantOn] = useState(false);
  const destEnc = !!(st && st.destEncrypted);
  const locked = !!(st && st.locked);
  const busy = st && st.state === 'unlocking';
  const u = st?.unlock;
  useEffect(() => { if (u && u.done && u.ok) { setP1(''); setP2(''); setWantOn(false); } }, [u && u.done, u && u.ok]);
  const run = (fn) => { setErr(''); try { fn(); } catch (e) { setErr(String(e.message || e)); } };
  const turnOn = () => {
    if (p1.length < 8) { setErr(tr('backup.encShort')); return; }
    if (p1 !== p2) { setErr(tr('backup.encMismatch')); return; }
    run(() => { if (!cfg.encrypt) save({ ...cfg, encrypt: true }); backup.unlock(p1, true); });
  };
  const setup = !destEnc && (wantOn || cfg.encrypt);
  const msg = busy ? tr('backup.encWorking') : u && u.done && !u.ok ? errText(u.error) : err;
  return (
    <GroupBox title={tr('backup.encryption')}>
      {!destEnc && !setup ? (
        <Row style={{ minHeight: 48, gap: spacing.m, paddingVertical: spacing.s }}>
          <Label dim size={typeScale.callout} style={{ flex: 1, lineHeight: 1.35 }}>{tr('backup.encOffHelp')}</Label>
          <Toggle small on={false} onChange={() => (cfg.dest ? setWantOn(true) : setErr(tr('backup.noDest')))} />
        </Row>
      ) : null}
      {setup ? (
        <View style={{ gap: spacing.s, paddingVertical: spacing.m }}>
          <Label dim size={typeScale.callout} style={{ lineHeight: 1.35 }}>{tr('backup.encWarn')}</Label>
          <TextInput secure value={p1} onChange={setP1} placeholder={tr('backup.encPass')} />
          <TextInput secure value={p2} onChange={setP2} placeholder={tr('backup.encPass2')} onSubmit={turnOn} />
          <Row style={{ justifyContent: 'flex-end', gap: spacing.s, marginTop: spacing.xs }}>
            <Button label={tr('backup.encNo')} onClick={() => { setWantOn(false); setP1(''); setP2(''); if (cfg.encrypt) save({ ...cfg, encrypt: false }); }} />
            <Button label={tr('backup.encTurnOn')} primary onClick={turnOn} />
          </Row>
        </View>
      ) : null}
      {destEnc && locked ? (
        <View style={{ gap: spacing.s, paddingVertical: spacing.m }}>
          <Label dim size={typeScale.callout} style={{ lineHeight: 1.35 }}>{tr('backup.encLocked')}</Label>
          <Row style={{ gap: spacing.s }}>
            <TextInput secure value={p1} onChange={setP1} placeholder={tr('backup.encPass')} style={{ flex: 1 }} onSubmit={() => run(() => backup.unlock(p1, false))} />
            <Button label={tr('backup.encUnlock')} primary onClick={() => run(() => backup.unlock(p1, false))} />
          </Row>
        </View>
      ) : null}
      {destEnc && !locked ? (
        <Row style={{ minHeight: 44 }}>
          <Label style={{ flex: 1 }}>{tr('backup.encOn')}</Label>
          <Button small label={tr('backup.encLock')} onClick={() => run(() => backup.lock())} />
        </Row>
      ) : null}
      {msg ? <View style={{ minHeight: 32, justifyContent: 'center' }}><Label size={typeScale.callout} style={{ color: busy ? th.textDim : '#e5534b' }}>{msg}</Label></View> : null}
    </GroupBox>
  );
}

// ---- Backups: timeline and browser ----
function Browser({ st }) {
  const th = useTheme();
  const [snaps, setSnaps] = useState(() => backup.list());
  const [sel, setSel] = useState(null);
  const [rel, setRel] = useState('');
  const [items, setItems] = useState([]);
  const [pick, setPick] = useState(null);
  const [replace, setReplace] = useState(false);
  const [ask, setAsk] = useState(null);       // 'all' | 'delete'
  const [err, setErr] = useState('');
  const lastWhen = st?.last?.when, stateNow = st?.state;
  useEffect(() => { setSnaps(backup.list()); }, [lastWhen, stateNow]);
  const list = useMemo(() => [...snaps].reverse(), [snaps]);
  useEffect(() => { if (!sel && list.length) setSel(list[0].id); if (sel && !snaps.find((s) => s.id === sel)) setSel(list[0]?.id || null); }, [list]);
  useEffect(() => {
    setPick(null);
    if (!sel) { setItems([]); return; }
    try { setItems(backup.ls(sel, rel)); setErr(''); } catch (e) { setItems([]); setErr(String(e.message || e)); }
  }, [sel, rel]);
  const busy = st && st.state !== 'idle' && st.state !== 'nodest';
  const restore = (r) => { setErr(''); try { backup.restore(sel, r, { replace }); } catch (e) { setErr(String(e.message || e)); } };
  // grouped by day
  const groups = [];
  for (const s of list) {
    const d = new Date(s.time * 1000);
    const key = d.toDateString();
    if (!groups.length || groups[groups.length - 1].key !== key) groups.push({ key, date: d, items: [] });
    groups[groups.length - 1].items.push(s);
  }
  const crumbs = rel ? rel.split('/') : [];
  const rr = st?.restore;
  const box = useRef(null);
  const width = useWidth(box, 760);
  const narrow = width < 700;
  if (!list.length) {
    return <EmptyState icon={<Icon size={56} />} title={tr('backup.emptyTitle')} hint={st?.dest ? tr('backup.emptyBody') : tr('backup.emptyNoDest')} />;
  }
  const whenSel = () => when(snaps.find((s) => s.id === sel)?.time);
  const caption = { fontSize: typeScale.caption, fontWeight: 600, color: th.textDim, whiteSpace: 'nowrap' };
  const restoreLabel = pick ? tr('backup.restoreItem', { name: pick }) : rel ? tr('backup.restoreFolder') : tr('backup.restorePick');
  return (
    <View style={{ flex: 1, flexDirection: 'row' }}>
      <View style={{ width: 220, backgroundColor: th.sidebarBg }}>
        <ScrollView style={{ flex: 1 }}>
          <View style={{ paddingHorizontal: 10, paddingBottom: spacing.m, gap: 2 }}>
            {groups.map((g) => (
              <View key={g.key} style={{ gap: 2 }}>
                <SectionTitle>{formatDate(g.date, 'dayMonth')}</SectionTitle>
                {g.items.map((s) => (
                  <SidebarItem key={s.id} label={formatTime(new Date(s.time * 1000))} selected={sel === s.id} onClick={() => { setSel(s.id); setRel(''); }} />
                ))}
              </View>
            ))}
          </View>
        </ScrollView>
        <View style={{ height: 30, justifyContent: 'center', paddingHorizontal: spacing.l }}>
          <Hairline edge="top" />
          <Label dim size={typeScale.caption}>{tr('backup.count', { count: list.length })}</Label>
        </View>
        <Hairline edge="end" />
      </View>
      <View ref={box} style={{ flex: 1, minWidth: 0 }}>
        <Toolbar>
          <View style={{ flex: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', gap: spacing.xs, overflow: 'hidden' }}>
            <View onClick={() => setRel('')}><Text style={{ fontSize: typeScale.body, fontWeight: 600, color: rel ? th.accent : th.text, whiteSpace: 'nowrap' }}>{tr('backup.home')}</Text></View>
            {crumbs.map((c, i) => (
              <Row key={i} style={{ gap: spacing.xs }}>
                <Text style={{ fontSize: typeScale.body, color: th.textDim }}>{direction() === 'rtl' ? '‹' : '›'}</Text>
                <View onClick={() => setRel(crumbs.slice(0, i + 1).join('/'))}>
                  <Text style={{ fontSize: typeScale.body, color: i === crumbs.length - 1 ? th.text : th.accent, whiteSpace: 'nowrap' }}>{c}</Text>
                </View>
              </Row>
            ))}
          </View>
          <ToolbarButton label={tr('backup.openFiles')} onClick={() => { const p = backup.path(sel); if (p) wm.open('files', { new: true, args: { path: rel ? `${p}/${rel}` : p } }); }} />
        </Toolbar>
        <View style={{ height: 26, flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: spacing.l }}>
          <View style={{ width: 18 }} />
          <Text style={[caption, { flex: 1, textAlign: 'start' }]}>{tr('backup.col.name')}</Text>
          <Text style={[caption, { width: 90, textAlign: 'end' }]}>{tr('backup.col.size')}</Text>
          {!narrow ? <Text style={[caption, { width: 150, textAlign: 'end' }]}>{tr('backup.col.modified')}</Text> : null}
          <Hairline edge="bottom" />
        </View>
        <ScrollView resetKey={`${sel}|${rel}`} style={{ flex: 1 }}>
          <View style={{ paddingVertical: spacing.xs, paddingHorizontal: spacing.s }}>
            {items.map((it, i) => {
              const on = pick === it.name;
              const dimFg = on ? 'rgba(255,255,255,0.85)' : th.textDim;
              return (
                <View key={it.name} onClick={() => setPick(it.name)} onDoubleClick={() => it.dir && setRel(join(rel, it.name))}
                  style={{ flexDirection: 'row', alignItems: 'center', gap: 10, height: 28, paddingHorizontal: spacing.s, borderRadius: 6,
                    backgroundColor: on ? th.accent : i % 2 ? (th.mode === 'dark' ? 'rgba(255,255,255,0.03)' : 'rgba(0,0,0,0.03)') : 'transparent' }}>
                  <View style={{ width: 18, alignItems: 'center' }}>
                    {it.dir ? <FolderGlyph color={on ? '#fff' : '#5ea7ff'} /> : it.link ? <Text style={{ fontSize: 13, color: dimFg }}>↪</Text> : <FileGlyph color={dimFg} />}
                  </View>
                  <Text style={{ flex: 1, fontSize: typeScale.body, color: on ? '#fff' : th.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{it.name}</Text>
                  <Text style={{ width: 90, fontSize: typeScale.callout, color: dimFg, whiteSpace: 'nowrap', textAlign: 'end' }}>{it.dir ? '—' : formatBytes(it.size)}</Text>
                  {!narrow ? <Text style={{ width: 150, fontSize: typeScale.callout, color: dimFg, whiteSpace: 'nowrap', textAlign: 'end' }}>{when(it.mtime)}</Text> : null}
                </View>
              );
            })}
            {!items.length ? <EmptyState title={null} hint={err || tr('backup.emptyFolder')} /> : null}
          </View>
        </ScrollView>
        <View style={{ paddingHorizontal: spacing.l, paddingVertical: spacing.m, gap: spacing.s }}>
          <Hairline edge="top" />
          {rr && rr.done && rr.id === sel ? (
            <Label size={typeScale.callout} dim>{rr.ok ? tr('backup.restoredInfo', { count: rr.files, both: rr.keptBoth, replaced: rr.replaced })
              : tr('backup.restoreFailed', { error: errText(rr.error) || tr('backup.restoreErrors', { count: rr.errors }) })}</Label>
          ) : null}
          {err && items.length ? <Label size={typeScale.callout} style={{ color: '#e5534b' }}>{err}</Label> : null}
          <Row style={{ gap: spacing.s }}>
            <Toggle small on={replace} onChange={setReplace} />
            <Label size={typeScale.callout} style={{ whiteSpace: 'nowrap' }}>{tr('backup.replace')}</Label>
            <ToolbarSpacer />
            {narrow ? (
              <MenuButton title={tr('backup.more')} items={[
                { label: tr('backup.restoreAll'), disabled: busy, onClick: () => setAsk('all') },
                '-',
                { label: tr('backup.delete'), destructive: true, disabled: busy, onClick: () => setAsk('delete') },
              ]} />
            ) : [
              <Button key="del" label={tr('backup.delete')} disabled={busy} onClick={() => setAsk('delete')} />,
              <Button key="all" label={tr('backup.restoreAll')} disabled={busy} onClick={() => setAsk('all')} />,
            ]}
            <Button label={restoreLabel} primary disabled={busy || (!pick && !rel)} style={{ maxWidth: narrow ? 200 : 260, overflow: 'hidden' }}
              onClick={() => (pick ? restore(join(rel, pick)) : rel ? restore(rel) : null)} />
          </Row>
        </View>
      </View>
      {ask === 'all' ? (
        <Confirm title={tr('backup.restoreAllTitle')} ok={tr('backup.restoreAll')}
          body={replace ? tr('backup.restoreAllReplace', { when: whenSel() }) : tr('backup.restoreAllKeep', { when: whenSel() })}
          onCancel={() => setAsk(null)} onOk={() => { setAsk(null); restore(''); }} />
      ) : null}
      {ask === 'delete' ? (
        <Confirm danger title={tr('backup.deleteTitle')} ok={tr('backup.delete')} body={tr('backup.deleteBody', { when: whenSel() })}
          onCancel={() => setAsk(null)} onOk={() => { setAsk(null); try { backup.remove(sel); } catch (e) { setErr(String(e.message || e)); } }} />
      ) : null}
    </View>
  );
}

export default function Backup() {
  const st = useBackup();
  const [tab, setTab] = useState(() => (st && st.dest ? 'snapshots' : 'settings'));
  return (
    <View style={{ flex: 1 }}>
      <Header st={st} />
      <View style={{ height: 44, alignItems: 'center', justifyContent: 'center' }}>
        <Segmented appearance="neutral" value={tab} onChange={setTab} options={[['snapshots', tr('backup.tabSnapshots')], ['settings', tr('backup.tabSettings')]]} />
        <Hairline edge="bottom" />
      </View>
      {tab === 'snapshots' ? <Browser st={st} /> : <Settings st={st} />}
    </View>
  );
}
