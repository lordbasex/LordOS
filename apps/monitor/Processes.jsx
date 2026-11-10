// Processes.jsx — the Processes tab: the table, and the actions on a process (docs/MONITOR.md,
// "Process actions"): Stop (Quit = SIGTERM, Force Quit = SIGKILL) and Get Info (memory,
// statistics, open files and ports), from the toolbar, a right-click menu, a double click and the
// keyboard (⌘/Ctrl+I, Delete).
//
// The user's own processes are signalled by lord-server itself (lord.monitor.signal); another
// user's or root's go through the root helper's proc-signal / proc-info (administrators only).
// Both check the process's start time, so a PID reused meanwhile is never hit, and both refuse
// the protected ones (PID 1, kernel threads, the session, LordOS's services and helpers,
// PipeWire), which the table marks with `prot`.
import {
  View, Text, ScrollView, Segmented, SearchField, ToolbarButton, Button, GroupBox, FormRow, typeScale, spacing, useTheme, useLocale,
  useStore, useSettings, useKeyboard, createStore, monitor, monitorProcesses, monitorDetail, firewall, firewallState, formatNumber,
  formatDate, formatTime, addListener, appName, Table,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { fmt, AppGlyph, isolatedAppOf } from './index.jsx';

const RED = '#ff453a';

// What is open over the window: the selected row, the context menu, the stop sheet and Get Info.
const ui = createStore({ sel: null, menu: null, stop: null, info: null });
const put = (p) => ui.set((s) => ({ ...s, ...p }));
const keyOf = (p) => (p ? `${p.pid}:${p.st}` : '');
const layerRoot = { current: null };            // the window-sized box the layers are placed in

let adminCache = null;
const isAdmin = () => {
  if (adminCache == null) { try { adminCache = !!lord.system.info().admin; } catch (e) { adminCache = false; } }
  return adminCache;
};

const myUid = (procs) => procs?.list.find((p) => p.self)?.uid;
const procLabel = (tr, p) => (p.app === 'lordos' ? tr('monitor.lordos') : isolatedAppOf(p) ? appName(isolatedAppOf(p)) : p.name);
const findProc = (procs, sel) => (sel ? procs?.list.find((p) => p.pid === sel.pid && p.st === sel.st) || null : null);

// ---- icons (drawn: a circle with a cross, and a circled "i") ----

function StopGlyph({ color }) {
  return (
    <View style={{ width: 16, height: 16, borderRadius: 8, border: `1.5px solid ${color}`, alignItems: 'center', justifyContent: 'center' }}>
      <Text style={{ fontSize: 9, fontWeight: 700, color, marginTop: -1 }}>✕</Text>
    </View>
  );
}

function InfoGlyph({ color }) {
  return (
    <View style={{ width: 16, height: 16, borderRadius: 8, border: `1.5px solid ${color}`, alignItems: 'center', justifyContent: 'center' }}>
      <Text style={{ fontSize: 10, fontWeight: 700, color, marginTop: -1 }}>i</Text>
    </View>
  );
}

// ---- actions ----

const openStop = (p) => { if (p) put({ stop: { pid: p.pid, st: p.st }, menu: null }); };
const openInfo = (p) => { if (p) put({ info: { pid: p.pid, st: p.st }, sel: { pid: p.pid, st: p.st }, menu: null }); };

// ---- the tab ----

export function ProcessesTab() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const procs = useStore(monitorProcesses);
  const state = useStore(ui);
  const [q, setQ] = useState('');
  const [mine, setMine] = useState('mine');
  const [sort, setSort] = useState({ key: 'cpu', dir: -1 });
  const me = myUid(procs);
  const cols = [
    { key: 'name', label: tr('monitor.process'), flex: 1, sort: true },
    { key: 'cpu', label: tr('monitor.cpuPct'), width: 80, num: true, sort: true },
    { key: 'rss', label: tr('monitor.memory'), width: 96, num: true, sort: true },
    { key: 'threads', label: tr('monitor.threads'), width: 64, num: true, sort: true, dim: true },
    { key: 'pid', label: tr('monitor.pid'), width: 72, num: true, sort: true, dim: true },
    { key: 'user', label: tr('monitor.user'), width: 96, sort: true, dim: true },
  ];
  const needle = q.trim().toLowerCase();
  const list = useMemo(() => {
    if (!procs) return [];
    const k = sort.key;
    const label = (p) => procLabel(tr, p);
    return procs.list.filter((p) => (mine === 'all' || p.uid === me) && (!needle || `${p.name} ${p.cmd} ${p.appName || ''} ${p.pid}`.toLowerCase().includes(needle)))
      .sort((a, b) => {
        const va = k === 'name' ? label(a).toLowerCase() : a[k], vb = k === 'name' ? label(b).toLowerCase() : b[k];
        return (va < vb ? -1 : va > vb ? 1 : a.pid - b.pid) * sort.dir;
      });
  }, [procs, mine, me, needle, sort]);
  // the selection only counts while its row is visible (filters and search hide it)
  const selected = state.sel ? list.slice(0, 150).find((p) => p.pid === state.sel.pid && p.st === state.sel.st) || null : null;
  const overlay = !!(state.stop || state.info || state.menu);

  useKeyboard((e) => {
    if (state.menu) { if (e.key === 'Escape') put({ menu: null }); return; }
    if (state.stop) { if (e.key === 'Escape') put({ stop: null }); return; }
    if (state.info) { if (e.key === 'Escape') put({ info: null }); return; }
    const k = String(e.base || e.key || '').toLowerCase();
    if ((e.meta || e.ctrl) && k === 'i') { openInfo(selected); return; }
    if (e.key === 'Delete' || ((e.meta || e.ctrl) && e.key === 'Backspace')) { openStop(selected); return; }
    if (e.key === 'Enter' && selected) { openInfo(selected); return; }
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      const shown = list.slice(0, 150);
      const i = shown.findIndex((p) => keyOf(p) === keyOf(selected));
      const j = Math.max(0, Math.min(shown.length - 1, i < 0 ? 0 : i + (e.key === 'ArrowDown' ? 1 : -1)));
      if (shown[j]) put({ sel: { pid: shown[j].pid, st: shown[j].st } });
    }
  });

  if (!procs) return <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.loading')}</Text>;
  const shown = list.slice(0, 150);
  const totalCpu = procs.list.reduce((a, p) => a + p.cpu, 0);
  const onContext = (p, ev) => {
    const at = layerRoot.current && ev?.local ? ev.local(layerRoot.current) : { x: ev?.x || 0, y: ev?.y || 0 };
    put({ sel: { pid: p.pid, st: p.st }, menu: { pid: p.pid, st: p.st, x: at.x, y: at.y } });
  };
  return (
    <>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, zIndex: 10 }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text }}>{tr('monitor.processes')}</Text>
        <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.procCount', { count: procs.total })}</Text>
        <View style={{ flex: 1 }} />
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.xs }}>
          <ToolbarButton icon={<StopGlyph color={selected ? t.text : t.textDim} />} title={tr('monitor.proc.stopTip')} disabled={!selected || overlay}
            onClick={() => openStop(selected)} />
          <ToolbarButton icon={<InfoGlyph color={selected ? t.text : t.textDim} />} title={tr('monitor.proc.infoTip')} disabled={!selected || overlay}
            onClick={() => openInfo(selected)} />
        </View>
        <Segmented value={mine} onChange={setMine} options={[['mine', tr('monitor.mine')], ['all', tr('monitor.all')]]} />
        <SearchField value={q} onChange={setQ} placeholder={tr('monitor.search')} width={200} />
      </View>
      {/* the SDK's table (0.8.8.8): a press or the keys choose a row (↑ ↓, Home, End, Page Up and Page Down, typing a
          name), Enter and a double click open Get Info */}
      <Table columns={cols} rows={shown} rowKey={keyOf} sort={sort}
        onSort={(key) => setSort((s0) => ({ key, dir: s0.key === key ? -s0.dir : key === 'name' || key === 'user' || key === 'pid' ? 1 : -1 }))}
        selection={selected ? [keyOf(selected)] : []} textOf={(p) => procLabel(tr, p)} onActivate={openInfo} onContextMenu={onContext}
        onSelectionChange={(keys) => { const p = shown.find((x) => keyOf(x) === keys[0]); put({ sel: p ? { pid: p.pid, st: p.st } : null }); }}
        renderCell={(p, c, { color, dim }) => {
          switch (c.key) {
            case 'name': return (
              <>
                <AppGlyph item={p} />
                <Text style={{ fontSize: 12, color, whiteSpace: 'nowrap' }}>{procLabel(tr, p)}</Text>
                {p.appName && p.appName !== p.name ? <Text style={{ fontSize: 11, color: dim, whiteSpace: 'nowrap' }}>{p.appName}</Text> : null}
              </>
            );
            case 'cpu': return <Text style={{ fontSize: 12, color, fontWeight: p.cpu > 0.5 ? 600 : 400, whiteSpace: 'nowrap', textAlign: 'end' }}>{formatNumber(p.cpu * 100, 1)}</Text>;
            case 'rss': return fmt.mem(p.rss);
            case 'threads': return formatNumber(p.threads, 0);
            case 'pid': return String(p.pid);
            default: return p.user;
          }
        }} />
      <Text style={{ fontSize: 11, color: t.textDim }}>{tr('monitor.procFoot', { shown: shown.length, count: list.length, cpu: formatNumber(totalCpu * 100, 0) })}</Text>
    </>
  );
}

// ---- layers over the window: the context menu, the stop sheet and Get Info ----

export function ProcessLayers({ active }) {
  const state = useStore(ui);
  const procs = useStore(monitorProcesses);
  const root = useRef(null);
  layerRoot.current = root.current;
  useEffect(() => { layerRoot.current = root.current; });
  // leaving the tab closes everything (and stops following the process)
  useEffect(() => { if (!active) put({ menu: null, stop: null, info: null }); }, [active]);
  const menuP = state.menu ? findProc(procs, state.menu) : null;
  return (
    <>
      <View ref={root} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, pointerEvents: 'none' }} />
      {active && state.info ? <InfoSheet key={keyOf(state.info)} want={state.info} /> : null}
      {active && state.stop ? <StopSheet want={state.stop} /> : null}
      {active && menuP ? <RowMenu p={menuP} x={state.menu.x} y={state.menu.y} /> : null}
    </>
  );
}

function bounds() {
  try { const l = lord.getLayout(layerRoot.current.id); return { w: l[2], h: l[3] }; } catch (e) { return { w: 900, h: 580 }; }
}

function RowMenu({ p, x, y }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const ref = useRef(null);
  const [hover, setHover] = useState(-1);
  useEffect(() => addListener('pointerdown', (e) => {
    const n = ref.current;
    if (!n) return;
    const [bx, by, bw, bh] = lord.getLayout(n.id);
    if (!(e.x >= bx && e.x <= bx + bw && e.y >= by && e.y <= by + bh)) put({ menu: null });
  }), []);
  const items = [
    { label: tr('monitor.proc.getInfo'), hint: '❖I', onClick: () => openInfo(p) },
    '-',
    { label: tr('monitor.proc.quitMenu'), hint: tr('monitor.proc.deleteKey'), danger: true, onClick: () => openStop(p) },
  ];
  const width = 220, h = items.reduce((s, it) => s + (it === '-' ? 9 : 24), 10);
  const b = bounds();
  const left = Math.max(4, Math.min(x, b.w - width - 4)), top = Math.max(4, Math.min(y, b.h - h - 4));
  return (
    <View ref={ref} style={{ position: 'absolute', left, top, width, paddingVertical: 5, borderRadius: 8, zIndex: 300,
      backgroundColor: t.mode === 'dark' ? '#2c2c31' : '#ffffff', border: `1px solid ${t.separator}`, boxShadow: '0 10px 30px rgba(0,0,0,0.35)' }}>
      {items.map((it, i) => {
        if (it === '-') return <View key={i} style={{ height: 1, marginVertical: 4, marginHorizontal: 8, backgroundColor: t.separator }} />;
        const on = hover === i;
        return (
          <View key={i} onMouseEnter={() => setHover(i)} onMouseLeave={() => setHover(-1)} onClick={() => { put({ menu: null }); it.onClick(); }}
            style={{ height: 24, marginHorizontal: 5, paddingHorizontal: 10, borderRadius: 5, flexDirection: 'row', alignItems: 'center', gap: 8, backgroundColor: on ? t.accent : 'transparent' }}>
            <Text style={{ flex: 1, fontSize: 13, color: on ? '#fff' : it.danger ? RED : t.text, whiteSpace: 'nowrap', textAlign: 'start' }}>{it.label}</Text>
            <Text style={{ fontSize: 11, color: on ? '#fff' : t.textDim, direction: 'ltr' }}>{it.hint}</Text>
          </View>
        );
      })}
    </View>
  );
}

// A sheet centered over the window (docs/DESIGN.md "Dialogs and sheets").
function Sheet({ width, height, children, z = 200 }) {
  const t = useTheme();
  const b = bounds();
  return (
    <View onMouseDown={(e) => e.stopPropagation?.()} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, zIndex: z, alignItems: 'center', justifyContent: 'center',
      backgroundColor: 'rgba(0,0,0,0.28)' }}>
      <View style={{ width: Math.min(width, b.w - 32), height: height ? Math.min(height, b.h - 32) : undefined, padding: spacing.xl, gap: spacing.m, borderRadius: 12,
        backgroundColor: t.mode === 'dark' ? '#2a2a2f' : '#ffffff', border: `1px solid ${t.separator}`, boxShadow: '0 18px 50px rgba(0,0,0,0.35)' }}>
        {children}
      </View>
    </View>
  );
}

function StopSheet({ want }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const procs = useStore(monitorProcesses);
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState(null);
  const [p0] = useState(() => findProc(procs, want));       // keep the name if it exits meanwhile
  const p = findProc(procs, want) || p0;
  if (!p) return null;
  const own = p.uid === myUid(procs);
  const close = () => put({ stop: null });
  const name = procLabel(tr, p);
  const run = async (kind) => {
    setBusy(true);
    setErr(null);
    try { await monitor.stopProcess(p, kind, { own }); close(); } catch (e) { setErr(e?.message || String(e)); setBusy(false); }
  };
  let title, body, actions;
  if (p.prot || (!own && !isAdmin())) {
    title = tr('monitor.proc.cantStop', { name });
    body = p.prot ? tr(`monitor.proc.why.${p.prot}`) : tr('monitor.proc.needAdmin', { user: p.user });
    actions = <><View style={{ flex: 1 }} /><Button primary label={tr('monitor.proc.ok')} onClick={close} /></>;
  } else {
    title = tr('monitor.proc.confirm', { name });
    body = own ? tr('monitor.proc.confirmHint') : tr('monitor.proc.confirmOther', { user: p.user });
    actions = (
      <>
        <Button label={tr('monitor.proc.cancel')} disabled={busy} onClick={close} />
        <View style={{ flex: 1 }} />
        <Button destructive label={tr('monitor.proc.forceQuit')} disabled={busy} onClick={() => run('KILL')} />
        <Button primary label={tr('monitor.proc.quit')} disabled={busy} onClick={() => run('TERM')} />
      </>
    );
  }
  return (
    <Sheet width={440} z={250}>
      <View style={{ flexDirection: 'row', gap: spacing.m, alignItems: 'flex-start' }}>
        <AppGlyph item={p} size={32} />
        <View style={{ flex: 1, gap: 6, minWidth: 0 }}>
          <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start', lineHeight: 1.3 }}>{title}</Text>
          <Text style={{ fontSize: typeScale.body, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{body}</Text>
          <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>{tr('monitor.proc.pidUser', { pid: p.pid, user: p.user })}</Text>
          {err ? <Text style={{ fontSize: typeScale.caption, color: RED, textAlign: 'start' }}>{err}</Text> : null}
        </View>
      </View>
      <View style={{ flexDirection: 'row', gap: spacing.s, marginTop: spacing.xs }}>{actions}</View>
    </Sheet>
  );
}

// ---- Get Info ----

const na = (tr) => tr('monitor.proc.na');

function cpuTime(tr, sec) {
  if (sec < 60) return tr('monitor.proc.seconds', { v: formatNumber(sec, 2) });
  const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = Math.floor(sec % 60);
  const pad = (n) => String(n).padStart(2, '0');
  return h ? `${h}:${pad(m)}:${pad(s)}` : `${m}:${pad(s)}`;
}

function Row({ label, children }) {
  return <FormRow label={label} labelWidth={170} minHeight={30}>{children}</FormRow>;
}

function Val({ children, dim, mono }) {
  const t = useTheme();
  return <Text style={{ fontSize: typeScale.body, color: dim ? t.textDim : t.text, textAlign: 'start', fontFamily: mono ? 'monospace' : undefined }}>{children}</Text>;
}

function MemoryTab({ d }) {
  const { t: tr } = useLocale();
  const m = (v) => (v == null ? na(tr) : fmt.mem(v));
  const shared = d.shared != null ? d.shared : d.rssFile != null && d.rssShmem != null ? d.rssFile + d.rssShmem : null;
  return (
    <View style={{ gap: spacing.l }}>
      <GroupBox>
        <Row label={tr('monitor.proc.mem.real')}><Val>{m(d.rss)}</Val></Row>
        <Row label={tr('monitor.proc.mem.virtual')}><Val>{m(d.vsize)}</Val></Row>
        <Row label={tr('monitor.proc.mem.shared')}><Val>{m(shared)}</Val></Row>
        <Row label={tr('monitor.proc.mem.private')}><Val dim={d.private == null}>{m(d.private)}</Val></Row>
        <Row label={tr('monitor.proc.mem.pss')}><Val dim={d.pss == null}>{m(d.pss)}</Val></Row>
        <Row label={tr('monitor.proc.mem.swap')}><Val>{m(d.swap)}</Val></Row>
      </GroupBox>
      <GroupBox title={tr('monitor.proc.mem.residentParts')}>
        <Row label={tr('monitor.proc.mem.anon')}><Val>{m(d.rssAnon)}</Val></Row>
        <Row label={tr('monitor.proc.mem.file')}><Val>{m(d.rssFile)}</Val></Row>
        <Row label={tr('monitor.proc.mem.shmem')}><Val>{m(d.rssShmem)}</Val></Row>
      </GroupBox>
    </View>
  );
}

function restrictionsOf(tr, d, p) {
  const out = [];
  if (d.isolated) out.push(tr('monitor.proc.r.isolated'));
  if (d.seccomp === 2) out.push(tr('monitor.proc.r.seccomp'));
  else if (d.seccomp === 1) out.push(tr('monitor.proc.r.seccompStrict'));
  if (d.nnp === 1) out.push(tr('monitor.proc.r.nnp'));
  const app = p?.app;
  if (app && firewall.available() && firewall.enforceable(app)) {
    if (firewall.appRule(app) === 'deny') out.push(tr('monitor.proc.r.netBlocked'));
    const flags = firewall.state()?.restricted?.[app] || [];
    for (const f of flags) out.push(tr(`firewall.r.${f}`));
  }
  return out;
}

function StatsTab({ d, p, parent }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const rev = useStore(firewallState, (s) => s.rev);
  const restr = useMemo(() => restrictionsOf(tr, d, p), [d.isolated, d.seccomp, d.nnp, p?.app, rev]);
  const scope = (d.cgroup || '').split('/').filter(Boolean).pop() || '/';
  const start = d.startMs ? new Date(d.startMs) : null;
  return (
    <GroupBox>
      <Row label={tr('monitor.proc.st.user')}><Val>{p ? p.user : String(d.uid)}{d.euid !== d.uid ? ` (${tr('monitor.proc.st.effective', { uid: d.euid })})` : ''}</Val></Row>
      <Row label={tr('monitor.proc.st.parent')}>
        {parent ? (
          <Text onClick={() => openInfo(parent)} style={{ fontSize: typeScale.body, color: t.accent, textAlign: 'start' }}>{`${procLabel(tr, parent)} (${parent.pid})`}</Text>
        ) : <Val>{d.ppid ? String(d.ppid) : '—'}</Val>}
      </Row>
      <Row label={tr('monitor.proc.st.group')}><Val>{`${d.pgid} · ${tr('monitor.proc.st.session', { sid: d.sid })}`}</Val></Row>
      <Row label={tr('monitor.proc.st.cpu')}><Val>{p ? fmt.pct(tr, p.cpu) : '—'}</Val></Row>
      <Row label={tr('monitor.proc.st.cpuTime')}><Val>{cpuTime(tr, (d.utime + d.stime) / (d.hz || 100))}</Val></Row>
      <Row label={tr('monitor.threads')}><Val>{formatNumber(d.threads, 0)}</Val></Row>
      <Row label={tr('monitor.proc.st.ctx')}>
        <Val>{d.vctx == null ? na(tr) : tr('monitor.proc.st.ctxValue', { v: formatNumber(d.vctx, 0), n: formatNumber(d.nvctx || 0, 0) })}</Val>
      </Row>
      <Row label={tr('monitor.proc.st.faults')}><Val>{tr('monitor.proc.st.faultsValue', { minor: formatNumber(d.minflt, 0), major: formatNumber(d.majflt, 0) })}</Val></Row>
      <Row label={tr('monitor.proc.st.started')}><Val>{start ? `${formatDate(start, 'long')} ${formatTime(start, { seconds: true })}` : '—'}</Val></Row>
      <Row label={tr('monitor.proc.st.nice')}><Val>{tr('monitor.proc.st.niceValue', { nice: d.nice, prio: d.prio })}</Val></Row>
      <Row label={tr('monitor.proc.st.scope')}>
        <View style={{ gap: 2 }}>
          <Val>{scope}</Val>
          <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start', direction: 'ltr' }}>{d.cgroup || '/'}</Text>
        </View>
      </Row>
      <Row label={tr('monitor.proc.st.restrictions')}>
        <View style={{ gap: 2 }}>{restr.length ? restr.map((r) => <Val key={r}>{r}</Val>) : <Val dim>{tr('monitor.proc.r.none')}</Val>}</View>
      </Row>
    </GroupBox>
  );
}

const KINDS = ['file', 'dev', 'socket', 'pipe', 'anon', 'other'];

function FilesTab({ d, own }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [settings] = useSettings();
  const hideAddr = settings.monitor?.connections === false;
  if (!d.fds) {
    const hint = own ? tr('monitor.proc.files.naOwn') : isAdmin() ? tr('monitor.proc.files.naAdmin') : tr('monitor.proc.files.needAdmin');
    return <Text style={{ fontSize: typeScale.body, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{hint}</Text>;
  }
  const text = (f) => {
    if (f.k === 'socket' && f.proto && f.proto !== 'UNIX') {
      const remote = f.remote ? (hideAddr ? tr('monitor.proc.files.hidden') : f.remote) : '';
      return `${f.local || '*'}${remote ? ` → ${remote}` : ''}${f.state ? ` · ${f.state}` : ''}`;
    }
    if (f.k === 'socket' && f.proto === 'UNIX') return f.p || tr('monitor.proc.files.unnamed');
    return f.p || '';
  };
  const kind = (f) => (f.k === 'socket' && f.proto ? f.proto : tr(`monitor.proc.kind.${KINDS.includes(f.k) ? f.k : 'other'}`));
  return (
    <View style={{ gap: spacing.s }}>
      <Text style={{ fontSize: typeScale.caption, color: t.textDim, textAlign: 'start' }}>
        {d.fdTotal > d.fds.length ? tr('monitor.proc.files.countCut', { shown: d.fds.length, count: d.fdTotal }) : tr('monitor.proc.files.count', { count: d.fdTotal })}
      </Text>
      <View style={{ borderRadius: 8, border: `1px solid ${t.separator}`, overflow: 'hidden' }}>
        {d.fds.map((f, i) => (
          <View key={f.fd} style={{ flexDirection: 'row', alignItems: 'center', minHeight: 24, paddingHorizontal: 10, paddingVertical: 3, gap: spacing.s, backgroundColor: i % 2 ? t.hover : 'transparent' }}>
            <Text style={{ width: 36, fontSize: 11, color: t.textDim, fontFamily: 'monospace', textAlign: 'end' }}>{String(f.fd)}</Text>
            <Text style={{ width: 64, fontSize: 11, fontWeight: 600, color: f.k === 'socket' ? t.accent : t.textDim, whiteSpace: 'nowrap' }}>{kind(f)}</Text>
            <Text style={{ flex: 1, minWidth: 0, fontSize: 11, color: t.text, fontFamily: 'monospace', direction: 'ltr', textAlign: 'left' }} numberOfLines={2}>{text(f)}</Text>
          </View>
        ))}
      </View>
    </View>
  );
}

function InfoSheet({ want }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const procs = useStore(monitorProcesses);
  const d = useStore(monitorDetail);
  const [tab, setTab] = useState('memory');
  const [p0] = useState(() => findProc(procs, want));
  const live = findProc(procs, want);
  const p = live || p0;
  const own = p ? p.uid === myUid(procs) : false;
  useEffect(() => (p ? monitor.watchProcess(p, { asAdmin: !own && isAdmin() }) : undefined), []);
  const close = () => put({ info: null });
  const parent = d && d.ppid ? procs?.list.find((x) => x.pid === d.ppid) : null;
  const ready = d && d.pid === want.pid;
  const gone = (ready && d.gone) || (procs && !live);
  const cmd = ready && d.cmdline ? d.cmdline.map((a) => (/[\s"']/.test(a) || !a ? JSON.stringify(a) : a)).join(' ') : '';
  return (
    <Sheet width={620} height={620}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.m }}>
        {p ? <AppGlyph item={p} size={32} /> : null}
        <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
          <Text style={{ fontSize: typeScale.title, fontWeight: 600, color: t.text, textAlign: 'start', whiteSpace: 'nowrap', textOverflow: 'ellipsis' }}>{p ? procLabel(tr, p) : String(want.pid)}</Text>
          <Text style={{ fontSize: typeScale.callout, color: gone ? RED : t.textDim, textAlign: 'start' }}>
            {gone ? tr('monitor.proc.gone') : tr('monitor.proc.pidUser', { pid: want.pid, user: p?.user || '' })}
          </Text>
        </View>
      </View>
      {ready && !d.gone ? (
        <View style={{ gap: 4 }}>
          <View style={{ flexDirection: 'row', gap: spacing.s }}>
            <Text style={{ width: 110, fontSize: typeScale.caption, fontWeight: 600, color: t.textDim, textAlign: 'start' }}>{tr('monitor.proc.exe')}</Text>
            <Text style={{ flex: 1, minWidth: 0, fontSize: 11, color: d.exe ? t.text : t.textDim, fontFamily: 'monospace', direction: 'ltr', textAlign: 'left' }} numberOfLines={2}>
              {d.exe || (own ? na(tr) : tr('monitor.proc.files.needAdminShort'))}</Text>
          </View>
          <View style={{ flexDirection: 'row', gap: spacing.s }}>
            <Text style={{ width: 110, fontSize: typeScale.caption, fontWeight: 600, color: t.textDim, textAlign: 'start' }}>{tr('monitor.proc.cmdline')}</Text>
            <Text style={{ flex: 1, minWidth: 0, fontSize: 11, color: cmd ? t.text : t.textDim, fontFamily: 'monospace', direction: 'ltr', textAlign: 'left', lineHeight: 1.35 }} numberOfLines={4}>
              {cmd ? `${cmd}${d.cmdCut ? ' …' : ''}` : tr('monitor.proc.noCmdline')}</Text>
          </View>
        </View>
      ) : null}
      <View style={{ alignItems: 'center' }}>
        <Segmented appearance="neutral" value={tab} onChange={setTab}
          options={[['memory', tr('monitor.memory')], ['stats', tr('monitor.proc.tab.stats')], ['files', tr('monitor.proc.tab.files')]]} />
      </View>
      <View style={{ flex: 1, minHeight: 0 }}>
        {!ready ? <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.loading')}</Text>
          : d.gone ? <Text style={{ fontSize: 12, color: t.textDim, textAlign: 'start' }}>{tr('monitor.proc.goneHint')}</Text>
            : (
              <ScrollView style={{ flex: 1, marginInlineEnd: -12 }} contentStyle={{ paddingInlineEnd: 12 }}>
                <View style={{ paddingBottom: 4 }}>
                  {tab === 'memory' && <MemoryTab d={d} />}
                  {tab === 'stats' && <StatsTab d={d} p={live} parent={parent} />}
                  {tab === 'files' && <FilesTab d={d} own={own} />}
                </View>
              </ScrollView>
            )}
      </View>
      <View style={{ flexDirection: 'row', gap: spacing.s }}>
        <Button label={tr('monitor.proc.quitDots')} disabled={!live || gone} onClick={() => openStop(live)} />
        <View style={{ flex: 1 }} />
        <Button primary label={tr('monitor.proc.close')} onClick={close} />
      </View>
    </Sheet>
  );
}
