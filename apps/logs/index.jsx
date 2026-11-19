// Logs — a log viewer (see docs/SYSTEM.md, «Logs»).
//
// Sources (sidebar):
//   - LordOS: the session's logs (compositor, shell, Xwayland, Cloud; each previous session's too), each app's
//     and each web app's, in the user's log folder (/var/log/lordos/users/<user>, or ~/.cache/lordos without
//     lord-sessiond; server/src/lordlog.h), read through lord.logs; lines parsed by sdk/logfile.js;
//   - My logs: the user's journal (user-<uid>.journal), read by lord-server as the user with our
//     own reader (server/src/logs/journal.c);
//   - System and Kernel: the system journal. If the user is in systemd-journal or adm it is read
//     directly; otherwise an administrator asks for it via the helper's closed logs-system action.
// Cloud's log (cloud.log) shows which account each line is about where lord-server knows it (#183, cloudAccounts).
// Filters by level (that level only), time (the journal and the files, #194), process or unit, and text; live with
// inotify (no polling); exports the filtered view to Documents, each row as the list shows it (rowText, #194). The list
// draws only the visible rows. The sidebar scrolls, and each group folds with its triangle like Mail's folders; which
// ones are folded is kept in the user's settings ("logs": { collapsed }, #199).
import {
  View, Text, TextInput, Segmented, SidebarItem, ScrollView, Disclosure, useTheme, useLocale, useSettings, settings, direction,
  logs, fs, formatDate, formatNumber, addListener, parseLordLog, sortFiles, fileGroup, fileLabel, isPrevious, isOlder,
  levelMatch, LEVEL_MAX, cloudAccounts, inRange, rowText, exportLine, toggleGroup, useCloud, cloudModel,
  Toolbar, ToolbarButton, ToolbarSpacer, SearchField, EmptyState, Hairline, spacing, typeScale, useAppWindow,
  useSidebar, SidebarToggle, SidebarOverlay,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';

const ROW = 22;
const COL_TIME = 92, COL_PROC = 140;     // list columns (px)
const MAX_ROWS = 10000;              // live: anything older than this is dropped
const LEVEL_COLOR = { error: '#ff453a', warning: '#ff9f0a', info: null, debug: null };

// ---- icon: a dark sheet with log lines (red, yellow, grey) ----
export function Icon({ size: s = 32 }) {
  const lines = [['#ff453a', 0.42], ['#d0d0d6', 0.62], ['#ffd60a', 0.5], ['#d0d0d6', 0.68], ['#30d158', 0.36]];
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, backgroundColor: '#2b2d33', padding: s * 0.16, gap: s * 0.075,
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, direction: 'ltr' }}>
      <View style={{ flexDirection: 'row', gap: s * 0.05, marginBottom: s * 0.02 }}>
        {['#ff5f57', '#febc2e', '#28c840'].map((c) => <View key={c} style={{ width: s * 0.08, height: s * 0.08, borderRadius: s * 0.04, backgroundColor: c }} />)}
      </View>
      {lines.map(([c, w], i) => (
        <View key={i} style={{ flexDirection: 'row', gap: s * 0.05, alignItems: 'center' }}>
          <View style={{ width: s * 0.06, height: s * 0.06, borderRadius: s * 0.03, backgroundColor: c }} />
          <View style={{ width: s * w, height: Math.max(1, s * 0.045), borderRadius: s * 0.02, backgroundColor: i % 2 ? 'rgba(255,255,255,0.45)' : 'rgba(255,255,255,0.7)' }} />
        </View>
      ))}
    </View>
  );
}

// ---- utilities ----
const pad = (n, w = 2) => String(n).padStart(w, '0');
export function levelOf(p) { return p == null || p < 0 ? 'info' : p <= 3 ? 'error' : p === 4 ? 'warning' : p === 7 ? 'debug' : 'info'; }
function clock(ms) { const d = new Date(ms); return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}.${pad(d.getMilliseconds(), 3)}`; }

// The user's Documents (xdg-user-dirs; otherwise whichever localized folder name exists)
function documentsDir() {
  const home = fs.home();
  try {
    const m = /^XDG_DOCUMENTS_DIR="\$HOME\/([^"]+)"/m.exec(fs.read('.config/user-dirs.dirs'));
    if (m) return `${home}/${m[1]}`;
  } catch (e) { /* no xdg-user-dirs */ }
  for (const d of ['Documentos', 'Documents']) { try { if (fs.stat(`${home}/${d}`)) return `${home}/${d}`; } catch (e) { /* not there */ } }
  return home;
}

// Size of a box after layout (re-measured on every render).
function useSize(ref, initial) {
  const [size, setSize] = useState(initial);
  useEffect(() => {
    const id = setTimeout(() => {
      if (!ref.current) return;
      const l = lord.getLayout(ref.current.id);
      const w = Math.round(l[2]), h = Math.round(l[3]);
      if (w > 0 && h > 0 && (Math.abs(w - size[0]) > 2 || Math.abs(h - size[1]) > 2)) setSize([w, h]);
    }, 20);
    return () => clearTimeout(id);
  });
  return size;
}

// ---- virtual list: draws only what is visible ----
function VirtualList({ items, selected, onSelect, stick, onStick, renderRow }) {
  const t = useTheme();
  const box = useRef(null);
  const [h, setH] = useState(400);
  const [off, setOff] = useState(0);
  const total = items.length * ROW, max = Math.max(0, total - h);
  const y = stick ? max : Math.min(off, max);
  useEffect(() => {
    const id = setTimeout(() => { if (box.current) { const v = lord.getLayout(box.current.id)[3]; if (v > 0 && Math.abs(v - h) > 1) setH(v); } }, 30);
    return () => clearTimeout(id);
  });
  const scrollTo = (v) => {
    const c = Math.max(0, Math.min(max, v));
    setOff(c);
    onStick(c >= max - 2);
  };
  // dragging the scrollbar
  const drag = useRef(null);
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => { if (drag.current) scrollTo(drag.current.y0 + ((e.y - drag.current.py) * total) / Math.max(1, h)); });
    const offUp = addListener('pointerup', () => { drag.current = null; });
    return () => { offMove(); offUp(); };
  });
  const first = Math.max(0, Math.floor(y / ROW) - 2), last = Math.min(items.length, Math.ceil((y + h) / ROW) + 2);
  const rows = [];
  for (let i = first; i < last; i++) rows.push(renderRow(items[i], i, i === selected, () => onSelect(i)));
  const bar = total > h ? Math.max(28, (h * h) / total) : 0;
  return (
    <View ref={box} onWheel={(e) => scrollTo(y + e.deltaY)} style={{ flex: 1, overflow: 'hidden' }}>
      <View style={{ position: 'absolute', left: 0, right: 0, top: first * ROW - y }}>{rows}</View>
      {bar > 0 && (
        <View onMouseDown={(e) => { drag.current = { py: e.y, y0: y }; }}
          style={{ position: 'absolute', right: 2, width: 7, borderRadius: 4, height: bar, top: (h - bar) * (y / (max || 1)),
            backgroundColor: t.mode === 'dark' ? 'rgba(255,255,255,0.3)' : 'rgba(0,0,0,0.28)' }} />
      )}
    </View>
  );
}

function Dot({ p }) {
  const c = LEVEL_COLOR[levelOf(p)];
  return <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: c || 'transparent', border: c ? 'none' : '1px solid rgba(128,128,128,0.5)' }} />;
}

// A Cloud account a line of cloud.log is about (#183): "Provider · Name" on screen; withName false gives
// "Provider (c1a2b3c4d)" for the exported file, which may end up attached to a public report (an account's name is
// often an email address). Both go in the same place on the line (#194). An account removed since: its id.
function accountLabel(id, accounts, withName = true) {
  const a = (accounts || []).find((x) => x.id === id);
  if (!a) return id;
  const p = cloudModel.providerName(a);
  return withName ? `${p} · ${a.name}` : `${p} (${id})`;
}

// ---- entry details ----
function Detail({ e, narrow, accounts }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  if (!e) return <EmptyState title={null} hint={tr('logs.pick')} style={{ padding: spacing.l }} />;
  const acct = e.acct ? accountLabel(e.acct, accounts) : '';
  const rows = [
    [tr('logs.field.time'), e.t ? `${formatDate(new Date(e.t / 1000), 'long')} ${clock(e.t / 1000)}` : tr('logs.line', { n: e.line })],
    [tr('logs.field.level'), tr(`logs.level.${levelOf(e.p)}`) + (e.p >= 0 ? ` (${e.p})` : '')],
    ...(acct ? [[tr('logs.field.account'), acct]] : []),
    [tr('logs.field.process'), e.id || '—'],
    [tr('logs.field.pid'), e.pid >= 0 ? String(e.pid) : '—'],
    [tr('logs.field.unit'), e.u || '—'],
    [tr('logs.field.uid'), e.uid >= 0 ? String(e.uid) : '—'],
    [tr('logs.field.boot'), e.b || '—'],
  ];
  return (
    <View style={{ flexDirection: 'row', gap: spacing.l, paddingHorizontal: spacing.l, paddingVertical: spacing.m, flex: 1 }}>
      <View style={{ flex: 1, minWidth: 0, gap: spacing.xs }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
          <Dot p={e.p} />
          <Text style={{ fontSize: typeScale.body, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{acct || e.id || '—'}</Text>
        </View>
        <Text style={{ fontSize: typeScale.callout, color: t.text, fontFamily: 'monospace', lineHeight: 1.4, textAlign: 'start' }}>{e.m}</Text>
      </View>
      <View style={{ width: narrow ? 210 : 260, gap: 3 }}>
        {rows.map(([k, v]) => (
          <View key={k} style={{ flexDirection: 'row', gap: spacing.s }}>
            <Text style={{ width: 76, fontSize: typeScale.caption, color: t.textDim, textAlign: 'end' }}>{k}</Text>
            <Text style={{ flex: 1, fontSize: typeScale.caption, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{v}</Text>
          </View>
        ))}
      </View>
    </View>
  );
}

const RANGES = { '15m': 15 * 60e6, hour: 3600e6, day: 86400e6, all: 0 };

export default function Registros() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const { args } = useAppWindow() || {};
  // opened on one LordOS log (Settings → Cloud → Show Log: { file: 'cloud.log' })
  const [src, setSrc] = useState(() => (/^((apps|web)\/)?[A-Za-z0-9._+-]+\.log$/.test(args?.file || '') ? { kind: 'lordos', file: args.file } : { kind: 'user' }));        // { kind: 'user'|'system'|'kernel'|'lordos', file }
  const [items, setItems] = useState([]);
  const [level, setLevel] = useState('all');
  const [range, setRange] = useState('hour');
  const [search, setSearch] = useState('');
  const [proc, setProc] = useState('');
  const [live, setLive] = useState(true);
  const [stick, setStick] = useState(true);
  const [sel, setSel] = useState(-1);
  const [state, setState] = useState({ loading: false, error: '', ms: 0 });
  const [notice, setNotice] = useState('');
  const accounts = useCloud().accounts || [];
  const [prefs] = useSettings();
  const collapsed = Array.isArray(prefs.logs?.collapsed) ? prefs.logs.collapsed : [];
  const toggle = (g) => settings.set({ logs: { ...(settings.get().logs || {}), collapsed: toggleGroup(collapsed, g) } });
  const sources = useMemo(() => { try { return logs.sources(); } catch (e) { return { user: 0, system: false }; } }, []);
  const files = useMemo(() => { try { return sortFiles(logs.files()); } catch (e) { return []; } }, [src.kind]);
  const logDir = useMemo(() => { try { return (logs.dir() || {}).dir || ''; } catch (e) { return ''; } }, []);
  const query = useRef({ seq: 0 });
  const box = useRef(null);
  const [width, height] = useSize(box, [820, 600]);
  // a small window (a tile of Lord Workspaces): the sources hide behind a toolbar button and open over the list
  const bar = useSidebar(640, 'logs');
  const tight = width < 560;           // narrower fields and process column, the filters may wrap
  const colProc = tight ? 100 : COL_PROC;
  const newest = useRef(0);

  const isJournal = src.kind !== 'lordos';
  const procValid = !proc || logs.validMatch(proc);

  const buildQuery = (since) => {
    const q = { limit: 3000 };
    if (LEVEL_MAX[level] != null) q.prio = LEVEL_MAX[level];      // that level and more urgent: levelMatch keeps the level
    if (RANGES[range]) q.since = Date.now() * 1000 - RANGES[range];
    if (since) q.since = since;
    if (proc && procValid) q.match = proc;
    if (search) q.grep = search;
    if (src.kind === 'kernel') q.kernel = true;
    return q;
  };

  const runJournal = (q) => {
    const only = (rows) => (Array.isArray(rows) ? rows.filter((r) => levelMatch(r.p, level)) : rows);
    if (src.kind === 'user') return logs.query({ ...q, scope: 'user' }).then(only);
    if (sources.system) return logs.query({ ...q, scope: 'system' }).then(only);
    return logs.system(q).then(only);         // administrators, through the helper
  };

  // one of LordOS's files, parsed and filtered; cloud.log's rows get their account (#183); the time range too (#194)
  const readFile = () => {
    let rows = parseLordLog(src.file ? logs.tail(src.file, 512 * 1024) || '' : '', src.file || '');
    if (/^cloud\.log(\.prev|\.old)?$/.test(src.file || '')) rows = cloudAccounts(rows);
    rows = inRange(rows, RANGES[range] ? Date.now() * 1000 - RANGES[range] : 0);
    const s = search.toLowerCase();
    return rows.filter((r) => levelMatch(r.p, level) && (!s || r.m.toLowerCase().includes(s)) && (!proc || r.id === proc));
  };

  // full reload when the source or the filters change (text, after a short pause)
  useEffect(() => {
    const seq = ++query.current.seq;
    const id = setTimeout(async () => {
      setState({ loading: true, error: '', ms: 0 });
      const t0 = Date.now();
      try {
        let rows;
        if (isJournal) rows = await runJournal(buildQuery(0));
        else rows = readFile();
        if (seq !== query.current.seq) return;
        newest.current = rows.length ? rows[rows.length - 1].t : Date.now() * 1000;
        setItems(rows);
        setSel(-1);
        setStick(true);
        setState({ loading: false, error: '', ms: Date.now() - t0 });
      } catch (e) {
        if (seq !== query.current.seq) return;
        setItems([]);
        const msg = String(e.message || e);
        setState({ loading: false, error: /administrador/.test(msg) ? tr('logs.needAdmin') : msg, ms: 0 });
      }
    }, search || proc ? 300 : 0);
    return () => clearTimeout(id);
  }, [src.kind, src.file, level, range, search, proc]);

  // live: inotify in lord-server notifies; only new entries are requested (at most ~1 per second)
  useEffect(() => {
    if (!live) return undefined;
    let timer = null, busy = false;
    try { logs.follow(true); } catch (e) { return undefined; }
    const pull = async () => {
      timer = null;
      if (busy) { timer = setTimeout(pull, 600); return; }
      busy = true;
      const seq = query.current.seq;
      try {
        let more;
        if (isJournal) more = await runJournal(buildQuery(newest.current));
        else {
          setItems(readFile());
          more = null;
        }
        if (more && more.length && seq === query.current.seq) {
          newest.current = more[more.length - 1].t;
          setItems((old) => { const n = old.concat(more); return n.length > MAX_ROWS ? n.slice(n.length - MAX_ROWS) : n; });
        }
      } catch (e) { /* retried on the next notification */ }
      busy = false;
    };
    const off = logs.onChange((kind, name) => {
      if (isJournal ? kind !== 'journal' : !(kind === 'file' && name === src.file)) return;
      if (!timer) timer = setTimeout(pull, 700);
    });
    return () => { off(); if (timer) clearTimeout(timer); try { logs.follow(false); } catch (e) { /* already gone */ } };
  }, [live, src.kind, src.file, level, range, search, proc]);

  const exportView = () => {
    const lines = items.map((e) => {
      const when = e.t ? `${formatDate(new Date(e.t / 1000), 'short')} ${clock(e.t / 1000)}` : `#${e.line}`;
      return exportLine(e, when, tr(`logs.level.${levelOf(e.p)}`), e.acct ? accountLabel(e.acct, accounts, false) : '');
    });
    const d = new Date();
    const name = `${tr('logs.exportName')}-${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}-${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}.txt`;
    const path = `${documentsDir()}/${name}`;
    try { fs.write(path, lines.join('\n') + '\n'); setNotice(tr('logs.exported', { path: path.replace(fs.home(), '~') })); }
    catch (e) { setNotice(tr('logs.exportFailed', { error: String(e.message || e) })); }
    setTimeout(() => setNotice(''), 5000);
  };

  const counts = useMemo(() => {
    let err = 0, warn = 0;
    for (const e of items) { const l = levelOf(e.p); if (l === 'error') err++; else if (l === 'warning') warn++; }
    return { err, warn };
  }, [items]);

  const renderRow = (e, i, on, pick) => {
    const lv = levelOf(e.p);
    const fg = on ? '#fff' : lv === 'debug' ? t.textDim : t.text;
    return (
      <View key={`${e.t}-${e.s ?? e.line}-${i}`} onClick={pick} style={{ height: ROW, flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingHorizontal: spacing.m,
        backgroundColor: on ? t.accent : i % 2 ? (t.mode === 'dark' ? 'rgba(255,255,255,0.03)' : 'rgba(0,0,0,0.03)') : 'transparent' }}>
        <View style={{ width: 10, alignItems: 'center' }}><Dot p={e.p} /></View>
        <Text style={{ width: COL_TIME, fontSize: typeScale.caption, color: on ? '#fff' : t.textDim, fontFamily: 'monospace', whiteSpace: 'nowrap', direction: 'ltr', textAlign: 'start' }}>{e.t ? clock(e.t / 1000) : `#${e.line}`}</Text>
        <Text style={{ width: colProc, fontSize: typeScale.callout, color: fg, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{e.id || '—'}</Text>
        <Text style={{ flex: 1, fontSize: typeScale.callout, color: fg, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>
          {rowText(e, e.acct ? accountLabel(e.acct, accounts) : '')}</Text>
      </View>
    );
  };

  const side = (kind, label, icon, extra) => (
    <SidebarItem key={kind + (extra?.file || '')} label={label} icon={icon} selected={src.kind === kind && (!extra || src.file === extra.file)}
      onClick={() => { setSrc({ kind, ...(extra || {}) }); if (bar.narrow) bar.close(); }} />
  );
  const narrow = width < 640;
  const caption = { fontSize: typeScale.caption, fontWeight: 600, color: t.textDim, whiteSpace: 'nowrap', textAlign: 'start' };

  // a group's header: its triangle and name; a click on either folds it (#199). Folded with its selected source inside,
  // the header stands in for the selected row, as Mail's account headers do.
  const group = (g, title, rows, selectedIn) => {
    const folded = collapsed.includes(g);
    const here = folded && selectedIn;
    const dim = here ? 'rgba(255,255,255,0.85)' : t.textDim;
    return [
      <View key={`h-${g}`} onClick={() => toggle(g)} style={{ flexDirection: 'row', alignItems: 'center', height: 22, gap: 6, marginTop: spacing.s,
        marginBottom: folded ? 0 : 2, [direction() === 'rtl' ? 'paddingLeft' : 'paddingRight']: 8, borderRadius: 6, backgroundColor: here ? t.accent : 'transparent' }}>
        <Disclosure state={folded ? 'closed' : 'open'} color={dim} onToggle={() => toggle(g)} style={{ [direction() === 'rtl' ? 'marginLeft' : 'marginRight']: -6 }} />
        <Text style={{ flex: 1, fontSize: 11, fontWeight: 600, color: here ? '#fff' : t.textDim, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{title}</Text>
      </View>,
      ...(folded ? [] : rows),
    ];
  };
  const fileRow = (f) => side('lordos', isPrevious(f.name) ? tr('logs.previous', { name: fileLabel(f.name) })
    : isOlder(f.name) ? tr('logs.older', { name: fileLabel(f.name) }) : fileLabel(f.name), '≡', { file: f.name });
  const fileOn = (list) => src.kind === 'lordos' && list.some((f) => f.name === src.file);
  const session = files.filter((f) => fileGroup(f.name) === 'session');

  const sourceList = (
    <View style={{ width: 210, ...(bar.narrow ? { flex: 1 } : null), backgroundColor: t.sidebarBg }}>
      {/* #199: many apps' logs made the last groups unreachable: the list scrolls, the hint stays at the bottom */}
      <ScrollView style={{ flex: 1, minHeight: 0 }} contentStyle={{ paddingHorizontal: 10, paddingTop: spacing.xs, paddingBottom: spacing.s, gap: 2 }}>
        {group('journal', tr('logs.section.journal'), [
          side('user', tr('logs.src.user'), '☺'),
          side('system', tr('logs.src.system'), '▣'),
          side('kernel', tr('logs.src.kernel'), '◐'),
        ], src.kind !== 'lordos')}
        {group('session', tr('logs.section.lordos'), files.length ? session.map(fileRow)
          : [<Text key="none" style={{ fontSize: typeScale.caption, color: t.textDim, paddingHorizontal: 10, textAlign: 'start' }}>{tr('logs.noFiles')}</Text>], fileOn(session))}
        {['apps', 'web', 'old'].map((g) => {
          const list = files.filter((f) => fileGroup(f.name) === g);
          return list.length ? group(g, tr(`logs.section.${g}`), list.map(fileRow), fileOn(list)) : null;
        })}
      </ScrollView>
      <Text style={{ fontSize: typeScale.caption, color: t.textDim, lineHeight: 1.4, paddingHorizontal: spacing.m, paddingTop: spacing.s, paddingBottom: spacing.m, textAlign: 'start' }}>
        {src.kind === 'user' ? tr('logs.hint.user') : src.kind === 'lordos' ? tr('logs.hint.lordos', { dir: logDir.replace(fs.home(), '~') }) : sources.system ? tr('logs.hint.systemDirect') : tr('logs.hint.systemAdmin')}
      </Text>
      <Hairline edge="end" />
    </View>
  );

  return (
    <View style={{ flex: 1, flexDirection: 'row' }}>
      {bar.narrow ? null : sourceList}
      <View ref={box} style={{ flex: 1, minWidth: 0 }}>
        <Toolbar>
          {bar.narrow ? <SidebarToggle shown={bar.shown} onClick={bar.toggle} /> : null}
          <ToolbarButton icon="●" iconSize={9} label={tr('logs.live')} active={live} color={t.textDim} onClick={() => setLive(!live)} />
          <ToolbarButton label={tr('logs.export')} onClick={exportView} />
          <ToolbarSpacer />
          <TextInput value={proc} onChange={setProc} placeholder={tr('logs.process')}
            style={{ width: tight ? 96 : narrow ? 130 : 150, ...(procValid ? {} : { border: '1px solid #ff453a' }) }} />
          <SearchField value={search} onChange={setSearch} placeholder={tr('logs.search')} width={tight ? 130 : narrow ? 190 : 220} />
        </Toolbar>
        <View style={{ ...(tight ? { minHeight: 40, flexWrap: 'wrap', paddingVertical: 6 } : { height: 40 }), flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingHorizontal: spacing.m }}>
          <Segmented small appearance="neutral" value={level} onChange={setLevel}
            options={[['all', tr('logs.lv.all')], ['info', tr('logs.lv.info')], ['warning', tr('logs.lv.warning')], ['error', tr('logs.lv.error')]]} />
          <Segmented small appearance="neutral" value={range} onChange={setRange}
            options={[['15m', tr('logs.range.15m')], ['hour', tr('logs.range.hour')], ['day', tr('logs.range.day')], ['all', tr('logs.range.all')]]} />
          <Hairline edge="bottom" />
        </View>
        <View style={{ height: 26, flexDirection: 'row', alignItems: 'center', gap: spacing.s, paddingHorizontal: spacing.m }}>
          <View style={{ width: 10 }} />
          <Text style={[caption, { width: COL_TIME }]}>{tr('logs.field.time')}</Text>
          <Text style={[caption, { width: colProc }]}>{tr('logs.field.process')}</Text>
          <Text style={[caption, { flex: 1 }]}>{tr('logs.field.message')}</Text>
          <Hairline edge="bottom" />
        </View>
        <View style={{ flex: 1, minHeight: 0 }}>
          {state.error ? <EmptyState mascot="confused" title={tr('logs.errorTitle')} hint={state.error} />
            : !items.length ? <EmptyState title={null} hint={state.loading ? tr('logs.loading') : tr('logs.empty')} />
            : <VirtualList items={items} selected={sel} onSelect={(i) => { setSel(i); setStick(false); }} stick={stick && sel < 0} onStick={setStick} renderRow={renderRow} />}
        </View>
        <View style={{ height: height < 560 ? 112 : 156 }}>
          <Hairline edge="top" />
          <Detail e={sel >= 0 ? items[sel] : null} narrow={narrow} accounts={accounts} />
        </View>
        <View style={{ height: 26, flexDirection: 'row', alignItems: 'center', gap: spacing.m, paddingHorizontal: spacing.m }}>
          <Hairline edge="top" />
          <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{tr('logs.count', { count: items.length, n: formatNumber(items.length, 0) })}</Text>
          {counts.err ? <Text style={{ fontSize: typeScale.caption, color: '#ff453a' }}>{tr('logs.errors', { count: counts.err, n: formatNumber(counts.err, 0) })}</Text> : null}
          {counts.warn ? <Text style={{ fontSize: typeScale.caption, color: '#ff9f0a' }}>{tr('logs.warnings', { count: counts.warn, n: formatNumber(counts.warn, 0) })}</Text> : null}
          {state.ms && !narrow ? <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{tr('logs.took', { ms: formatNumber(state.ms, 0) })}</Text> : null}
          <View style={{ flex: 1 }} />
          {notice ? <Text style={{ fontSize: typeScale.caption, color: t.accent, whiteSpace: 'nowrap', overflow: 'hidden' }}>{notice}</Text> : null}
          {live && !notice ? (
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
              <View style={{ width: 7, height: 7, borderRadius: 4, backgroundColor: '#30d158' }} />
              <Text style={{ fontSize: typeScale.caption, color: t.textDim }}>{tr('logs.following')}</Text>
            </View>
          ) : null}
        </View>
      </View>
      {bar.narrow && bar.shown ? <SidebarOverlay width={210} onClose={bar.close}>{sourceList}</SidebarOverlay> : null}
    </View>
  );
}
