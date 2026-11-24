// Processes.jsx — the Processes tab: the table, and the actions on a process (docs/MONITOR.md,
// "Process actions"): Stop (Quit = SIGTERM, Force Quit = SIGKILL) and Get Info (memory,
// statistics, open files and ports), from the toolbar, a right-click menu, a double click and the
// keyboard (⌘/Ctrl+I, Delete).
//
// Grouped (docs/MONITOR.md, "Grouped view"): one row per app (its scope), per LordOS, the kernel or a process tree
// (procgroupmodel.js), opened with its arrow (→ ← or Enter), with the totals and a hover summary; Quit All ends the
// processes the group had when it was chosen (PID and start time), and offers Force Quit for the ones still open.
// A steady list (docs/MONITOR.md, "A steady list"): while the pointer is over the table, a menu or a sheet is open, and
// for two seconds after the pointer leaves, the rows keep their order (the values still change), new processes go at
// the end and exited ones stay greyed; the selection is the process (PID and start time), never a row number.
//
// The user's own processes are signalled by lord-server itself (lord.monitor.signal); another
// user's or root's go through the root helper's proc-signal / proc-info (administrators only).
// Both check the process's start time, so a PID reused meanwhile is never hit, and both refuse
// the protected ones (PID 1, kernel threads, the session, LordOS's services and helpers,
// PipeWire), which the table marks with `prot`. Quit All uses the same calls, once per process.
import {
  View, Text, ScrollView, Segmented, SearchField, ToolbarButton, Button, GroupBox, FormRow, Disclosure, typeScale, spacing, useTheme, useLocale,
  useStore, useSettings, useKeyboard, createStore, monitor, monitorProcesses, monitorDetail, firewall, firewallState, formatNumber,
  formatDate, formatTime, addListener, appName, Table, useNarrow, direction, Tooltip, Dropdown, Image, webAppEntry, refreshWebApps, webAppsModel,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { fmt, AppGlyph, isolatedAppOf, componentName } from './index.jsx';
import { sealPixels, SEAL_COLORS } from './seal.js';
import { statsStore, WINDOWS } from './procstatsmodel.js';
import {
  HOLD_MS, procKey, procText, groupRowKey, rowKeyOf, isPaused, buildGroups, filterGroups, sortGroups, sortProcs, holdOrder, holdGroups,
  flattenRows, groupSummary, snapshotOf, planStop, stillRunning, groupKeys, chainCache, originOf, groupOriginOf, passesFilter, programChoices,
  ORIGINS, exePath,
} from './procgroupmodel.js';

const RED = '#ff453a';
const TIP_MS = 700;                             // the hover summary's delay
const WAIT_MS = 3000;                           // Quit All: how long before Force Quit is offered

// What is open over the window: the selected row ({ pid, st } or { group: key }), the context menu, the stop sheet
// (one process), the group's stop sheet, Get Info and the group's hover summary.
const ui = createStore({ sel: null, menu: null, stop: null, gstop: null, info: null, tip: null });
const put = (p) => ui.set((s) => ({ ...s, ...p }));
const keyOf = procKey;
const layerRoot = { current: null };            // the window-sized box the layers are placed in
// the groups that are open (kept while switching tabs) and the groups last shown (the menu and the summary find theirs)
const openGroups = createStore(new Set());
const toggleGroup = (key, on) => openGroups.set((s) => {
  const n = new Set(s);
  if (on ?? !n.has(key)) n.add(key); else n.delete(key);
  return n;
});
const shownGroups = { list: [] };

let adminCache = null;
const isAdmin = () => {
  if (adminCache == null) { try { adminCache = !!lord.system.info().admin; } catch (e) { adminCache = false; } }
  return adminCache;
};

const myUid = (procs) => procs?.list.find((p) => p.self)?.uid;
const procLabel = (tr, p) => (p.app === 'lordos' ? tr('monitor.lordos') : isolatedAppOf(p) ? appName(isolatedAppOf(p)) : p.name);
// a machine's name (Machines' list), else "Machines"
const machineOf = (app) => /^desktop:lordos-vm-(?:tor-)?(.+)$/.exec(app || '')?.[1] || null;
const machineName = (id) => (id ? monitor.machineNames([id])[id] || null : null);
const groupLabel = (tr, g) => {
  if (g.kind === 'kernel') return tr('monitor.group.kernel');
  if (g.kind === 'lordos') return tr('monitor.lordos');
  if (isolatedAppOf(g.item)) return appName(isolatedAppOf(g.item));
  if (g.kind !== 'app') return procLabel(tr, g.lead);
  const vm = machineOf(g.item.app);
  if (vm) return machineName(vm) || tr('monitor.comp.machines');
  const web = /^desktop:lordos-web-(.+)$/.exec(g.item.app || '')?.[1];
  if (web && webAppEntry(web)?.name) return webAppEntry(web).name;
  return componentName(tr, g.item.app) || g.item.appName || g.item.name;
};
// the facts the origin badges stand on (procgroupmodel.js originOf)
const facts = {
  owner: (exe) => monitor.ownerOf(exe),
  origin: (pkg) => monitor.packageOrigin(pkg),
  signer: (id) => monitor.appSigner(id),
  machine: machineName,
  webapp: (app) => { const e = webAppEntry(app); return e ? webAppsModel.hostOf(e.origin) || e.name || null : null; },
};
const chainOfProc = chainCache();
const STAT_KEYS = ['cpuAvg', 'cpuPeak', 'memAvg', 'memPeak'];
const SORT_KEYS = ['name', 'cpu', 'rss', 'threads', 'count', 'pid', 'user', ...STAT_KEYS];

// The rolling statistics (procstatsmodel.js): every process (PID and start time) and group, fed from each new process
// list (once a second while Monitor shows Processes or Memory); a series not seen for 15 minutes is dropped.
const stats = statsStore();
let lastSweep = 0;
monitorProcesses.subscribe(() => {
  const p = monitorProcesses.get();
  if (!p) return;
  const t = Date.now();
  for (const x of p.list) stats.feed(procKey(x), t, x.cpu || 0, x.rss || 0);
  for (const g of buildGroups(p.list)) stats.feed(groupRowKey(g.key), t, g.cpu, g.mem);
  if (t - lastSweep > 30000) { lastSweep = t; stats.sweep(t); }
});
// a row's statistics over w minutes, as sortable fields (the current values while there is no series yet)
const withStats = (x, key, w, cur, mem) => {
  const s = stats.get(key);
  return s ? { ...x, cpuAvg: s.cpu[w].avg, cpuPeak: s.cpu[w].peak, memAvg: s.mem[w].avg, memPeak: s.mem[w].peak }
    : { ...x, cpuAvg: cur, cpuPeak: cur, memAvg: mem, memPeak: mem };
};
const groupGlyph = (g) => (g.kind === 'lordos' ? { app: 'lordos' } : g.item);
const findProc = (procs, sel) => (sel ? procs?.list.find((p) => p.pid === sel.pid && p.st === sel.st) || null : null);
const selKeyOf = (sel) => (!sel ? '' : sel.group ? groupRowKey(sel.group) : `${sel.pid}:${sel.st}`);
const selOf = (r) => (r.group ? { group: r.group.key } : { pid: r.proc.pid, st: r.proc.st });

// ---- icons (drawn: a circle with a cross, a circled "i", two bars) ----

function StopGlyph({ color, size = 16 }) {
  return (
    <View style={{ width: size, height: size, borderRadius: size / 2, border: `1.5px solid ${color}`, alignItems: 'center', justifyContent: 'center' }}>
      <Text style={{ fontSize: Math.round(size * 0.56), fontWeight: 700, color, marginTop: -1 }}>✕</Text>
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

function PauseGlyph({ color }) {
  return (
    <View style={{ flexDirection: 'row', gap: 2, width: 8, height: 9, direction: 'ltr' }}>
      <View style={{ width: 3, height: 9, borderRadius: 1, backgroundColor: color }} />
      <View style={{ width: 3, height: 9, borderRadius: 1, backgroundColor: color }} />
    </View>
  );
}

// ---- actions ----

const openStop = (p, force = false) => { if (p && !p.gone) put({ stop: { pid: p.pid, st: p.st, force }, menu: null, tip: null }); };
// keep: leave the selection as it is (a group's Get Info shows its main process)
const openInfo = (p, keep = false) => {
  if (!p || p.gone) return;
  put({ info: { pid: p.pid, st: p.st }, ...(keep ? null : { sel: { pid: p.pid, st: p.st } }), menu: null, tip: null });
};
// the group's processes as they are now (PID and start time): what its sheet lists and acts on
const openGroupStop = (tr, g, force = false) => {
  if (!g || g.gone) return;
  put({ gstop: { key: g.key, kind: g.kind, name: groupLabel(tr, g), item: groupGlyph(g), uid: g.uid, user: g.user, snap: snapshotOf(g.procs), force },
    menu: null, tip: null });
};

// "Grouped": a switch drawn like Mine | All's buttons, which works with either of them. A Tab stop: Space and Enter
// press it.
function GroupedToggle({ on, onChange, label }) {
  return (
    <View focusable onClick={() => onChange(!on)} accessibilityLabel={label} style={{ borderRadius: 7 }}>
      <Segmented focusable={false} value={on ? 'on' : ''} onChange={() => {}} options={[['on', label]]} />
    </View>
  );
}

// a small button inside the selected row (Quit All on a group, ✕ on a process)
function RowButton({ label, color, onClick }) {
  const t = useTheme();
  return (
    <View onMouseDown={(e) => e?.stopPropagation?.()} onClick={(e) => { e?.stopPropagation?.(); onClick(); }}
      style={{ height: 18, minWidth: 18, paddingHorizontal: label ? 7 : 0, borderRadius: 9, alignItems: 'center', justifyContent: 'center', flexShrink: 0,
        backgroundColor: 'rgba(255,255,255,0.16)', border: `1px solid ${color === '#fff' ? 'rgba(255,255,255,0.5)' : t.separator}` }}>
      {label ? <Text style={{ fontSize: 11, fontWeight: 600, color, whiteSpace: 'nowrap' }}>{label}</Text> : <StopGlyph color={color} size={12} />}
    </View>
  );
}

// ---- origin badges (procgroupmodel.js originOf) ----

// LordOS's own get a verified seal (seal.js): blue for LordOS itself, pink for a LordOS app; the other origins a small
// labelled pill. Each says why in its tooltip; updated: a pill saying it needs a restart.
function badgeTip(tr, o) {
  switch (o.kind) {
    case 'system': return tr('monitor.badge.systemTip', { exe: o.exe, pkg: o.pkg });
    case 'lordos': return o.signer?.signer ? tr('monitor.badge.lordosTip', { id: o.id, signer: o.signer.signer })
      : o.signer?.trust === 'lordos' ? tr('monitor.badge.lordosTipLordos', { id: o.id }) : tr('monitor.badge.lordosTipUnsigned', { id: o.id });
    case 'flatpak': return tr('monitor.badge.flatpakTip', { id: o.id });
    case 'deb': return o.origin ? tr('monitor.badge.debTip', { pkg: o.pkg, origin: o.origin }) : tr('monitor.badge.debTipFile', { pkg: o.pkg });
    case 'webapp': return o.site ? tr('monitor.badge.webappTip', { site: o.site }) : tr('monitor.badge.webappTipId', { id: o.id });
    case 'machine': return tr('monitor.badge.machineTip', { name: o.name || o.id });
    default: return tr('monitor.badge.unpackagedTip', { exe: o.exe });
  }
}
const PILL = { flatpak: '#4a90e2', deb: '#ff6961', webapp: '#0a84ff', machine: '#5e5ce6', unpackaged: '#ff9f0a', updated: '#ff9f0a' };

function Seal({ kind, size = 13 }) {
  return <Image pixels={sealPixels(size, 2, SEAL_COLORS[kind])} style={{ width: size, height: size, flexShrink: 0 }} />;
}

function Pill({ color, label, selected }) {
  return (
    <View style={{ height: 15, paddingHorizontal: 5, borderRadius: 4, justifyContent: 'center', flexShrink: 0,
      backgroundColor: selected ? 'rgba(255,255,255,0.22)' : `${color}26`, border: `1px solid ${selected ? 'rgba(255,255,255,0.45)' : `${color}66`}` }}>
      <Text style={{ fontSize: 9.5, fontWeight: 600, color: selected ? '#fff' : color, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
}

function pillLabel(tr, o) {
  if (o.kind === 'deb') return o.origin && o.origin.length <= 18 ? o.origin : tr('monitor.badge.deb');
  return tr(`monitor.badge.${o.kind}`);
}

export function OriginBadge({ o, selected }) {
  const { t: tr } = useLocale();
  if (!o) return null;
  const tip = badgeTip(tr, o);
  const mark = o.kind === 'system' || o.kind === 'lordos'
    ? <Seal kind={o.kind} />
    : <Pill color={PILL[o.kind]} label={pillLabel(tr, o)} selected={selected} />;
  return (
    <>
      <Tooltip text={tip}><View accessibilityLabel={tip} style={{ flexDirection: 'row', alignItems: 'center' }}>{mark}</View></Tooltip>
      {o.updated ? <Tooltip text={tr('monitor.badge.updatedTip')}><Pill color={PILL.updated} label={tr('monitor.badge.updated')} selected={selected} /></Tooltip> : null}
    </>
  );
}

// A filter chip: an origin, on or off (a Tab stop: Space and Enter press it)
function FilterChip({ kind, label, on, onChange }) {
  const t = useTheme();
  return (
    <View focusable onClick={() => onChange(!on)} accessibilityLabel={label}
      style={{ height: 22, paddingHorizontal: 9, borderRadius: 11, flexDirection: 'row', alignItems: 'center', gap: 5,
        backgroundColor: on ? t.accent : t.control, border: `1px solid ${on ? t.accent : t.separator}` }}>
      {kind === 'system' || kind === 'lordos' ? <Seal kind={kind} size={11} />
        : <View style={{ width: 7, height: 7, borderRadius: 4, backgroundColor: PILL[kind] }} />}
      <Text style={{ fontSize: 11, fontWeight: 500, color: on ? '#fff' : t.text, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
}

// ---- who started it ----

const linkLabel = (tr, chain, i) => (chain[i].self ? componentName(tr, chain[i + 1]?.app) || tr('monitor.lordos') : chain[i].name);

// A child row's chain of ancestors (chain: the whole chain, the process itself last, which isn't shown), the nearest
// last ("Terminal › bash › claude › sh"); a link selects that process.
// Left to right in every direction, as a path of program names reads.
function ChainLine({ chain, color, onPick }) {
  const { t: tr } = useLocale();
  const shown = chain.length - 1;                       // the ancestors
  if (shown < 1) return null;
  const from = Math.max(0, shown - 4);
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 3, minWidth: 0, flexShrink: 1, overflow: 'hidden', marginInlineStart: 4, direction: 'ltr' }}>
      {from ? <Text style={{ fontSize: 10.5, color, whiteSpace: 'nowrap' }}>{'… ›'}</Text> : null}
      {chain.slice(from, shown).map((l, k) => (
        <View key={`${l.pid}:${l.st}`} style={{ flexDirection: 'row', alignItems: 'center', gap: 3 }}>
          {k ? <Text style={{ fontSize: 10.5, color, whiteSpace: 'nowrap' }}>›</Text> : null}
          <Text onMouseDown={(e) => e?.stopPropagation?.()} onClick={(e) => { e?.stopPropagation?.(); onPick(l); }}
            style={{ fontSize: 10.5, color, whiteSpace: 'nowrap' }}>{linkLabel(tr, chain, from + k)}</Text>
        </View>
      ))}
    </View>
  );
}

// ---- the tab ----

export function ProcessesTab() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const procs = useStore(monitorProcesses);
  const state = useStore(ui);
  const open = useStore(openGroups);
  const [settings, setSettings] = useSettings();
  // remembered (settings.monitor): Grouped, the order and the filters
  const prefs = settings.monitor || {};
  const setPref = (patch) => setSettings({ monitor: { ...(settings.monitor || {}), ...patch } });
  const grouped = prefs.procGrouped === true;
  const setGrouped = (on) => setPref({ procGrouped: !!on });
  const statsOn = WINDOWS.includes(prefs.procStats);
  const sort = SORT_KEYS.includes(prefs.procSort?.key) && (prefs.procSort.dir === 1 || prefs.procSort.dir === -1) && (grouped || prefs.procSort.key !== 'count')
    && (statsOn || !STAT_KEYS.includes(prefs.procSort.key)) ? prefs.procSort : { key: 'cpu', dir: -1 };
  const setSort = (fn) => setPref({ procSort: fn(sort) });
  const filter = {
    origins: Array.isArray(prefs.procFilter?.origins) ? prefs.procFilter.origins.filter((o) => ORIGINS.includes(o)) : [],
    program: typeof prefs.procFilter?.program === 'string' ? prefs.procFilter.program : null,
  };
  const setFilter = (patch) => setPref({ procFilter: { ...filter, ...patch } });
  // the statistics' columns: off (0) or over 1, 5 or 15 minutes
  const win = WINDOWS.includes(prefs.procStats) ? prefs.procStats : 0;
  const [q, setQ] = useState('');
  const [mine, setMine] = useState('mine');
  const [hover, setHover] = useState(false);
  const [leftAt, setLeftAt] = useState(0);
  const held = useRef({ reset: '', flat: null, groups: null });
  const tipTimer = useRef(0), tipKey = useRef(null);
  const me = myUid(procs);
  const narrow = useNarrow(760);           // a small window: fewer columns, the header on two lines
  const cols = [
    { key: 'name', label: tr('monitor.process'), flex: 1, sort: true },
    { key: 'cpu', label: tr('monitor.cpuPct'), width: 80, num: true, sort: true },
    { key: 'rss', label: tr('monitor.memory'), width: 96, num: true, sort: true },
    ...(narrow ? [] : [{ key: 'threads', label: tr('monitor.threads'), width: 64, num: true, sort: true, dim: true }]),
    ...(win ? [
      { key: 'cpuAvg', label: tr('monitor.stats.cpuAvg'), width: 76, num: true, sort: true },
      { key: 'cpuPeak', label: tr('monitor.stats.cpuPeak'), width: 76, num: true, sort: true },
      { key: 'memAvg', label: tr('monitor.stats.memAvg'), width: 88, num: true, sort: true },
      { key: 'memPeak', label: tr('monitor.stats.memPeak'), width: 88, num: true, sort: true },
    ] : []),
    ...(grouped ? [{ key: 'count', label: tr('monitor.processes'), width: 80, num: true, sort: true, dim: true }] : []),
    { key: 'pid', label: tr('monitor.pid'), width: 72, num: true, sort: true, dim: true },
    ...(narrow ? [] : [{ key: 'user', label: tr('monitor.user'), width: 96, sort: true, dim: true }]),
  ];
  const needle = q.trim().toLowerCase();
  const overlay = !!(state.stop || state.gstop || state.info || state.menu);
  const paused = isPaused({ hover, overlay, leftAt });
  // the order is held for HOLD_MS after the pointer leaves the table
  useEffect(() => {
    if (hover || !leftAt) return undefined;
    const id = setTimeout(() => setLeftAt(0), Math.max(0, HOLD_MS - (Date.now() - leftAt)) + 20);
    return () => clearTimeout(id);
  }, [hover, leftAt]);
  useEffect(() => { refreshWebApps(); return () => clearTimeout(tipTimer.current); }, []);
  // choosing another order, filter or search shows it at once, held or not
  const reset = `${grouped}|${mine}|${needle}|${sort.key}|${sort.dir}|${filter.origins.join(',')}|${filter.program}|${win}`;
  const view = useMemo(() => {
    if (!procs) return { list: [], groups: [], programs: [], byPid: new Map(), keys: new Map() };
    const hold = paused && held.current.reset === reset;
    const uid = mine === 'all' ? null : me;
    const byPid = new Map(procs.list.map((p) => [p.pid, p]));
    const all = buildGroups(procs.list);
    const label = (g) => groupLabel(tr, g);
    const programs = programChoices(all.filter((g) => uid == null || g.uid === uid), label);
    if (grouped) {
      const kept = all.filter((g) => passesFilter(groupOriginOf(g, facts), g.key, filter));
      const fresh = sortGroups(filterGroups(kept, { uid, needle, label }).map((g) => (win ? withStats(g, groupRowKey(g.key), win, g.cpu, g.mem) : g)), sort, label)
        .map((g) => ({ ...g, shown: sortProcs(win ? g.shown.map((p) => withStats(p, procKey(p), win, p.cpu, p.rss)) : g.shown, sort, (p) => p.name) }));
      const groups = holdGroups(hold ? held.current.groups : null, fresh, hold);
      held.current = { reset, flat: null, groups };
      return { list: [], groups, programs, byPid, keys: new Map() };
    }
    const keys = groupKeys(procs.list);
    const kept = procs.list.filter((p) => (uid == null || p.uid === uid) && (!needle || procText(p).includes(needle))
      && passesFilter(originOf(p, facts), keys.get(p.pid)?.key, filter));
    const fresh = sortProcs(win ? kept.map((p) => withStats(p, procKey(p), win, p.cpu, p.rss)) : kept, sort, (p) => procLabel(tr, p));
    const list = holdOrder(hold ? held.current.flat : null, fresh, procKey, hold);
    held.current = { reset, flat: list, groups: null };
    return { list, groups: [], programs, byPid, keys };
  }, [procs, reset, paused, me, tr]);
  shownGroups.list = view.groups;
  // the origin badges: ask lord-server who owns the executables on screen (answered by its worker thread)
  useEffect(() => {
    if (!procs) return;
    const shownProcs = grouped ? view.groups.flatMap((g) => [g.lead, ...g.rows]) : view.list.slice(0, 150);
    monitor.askOwners(shownProcs.map((p) => exePath(p?.exe)).filter(Boolean));
  }, [procs, grouped]);
  const rows = grouped ? flattenRows(view.groups, open).slice(0, 400) : view.list.slice(0, 150).map((p) => ({ proc: p }));
  // a link of a chain (who started it) selects that process, opening its group
  const selectProc = (l) => {
    const p = view.byPid.get(l.pid);
    if (!p || p.st !== l.st) return;
    if (grouped) { const g = view.groups.find((x) => x.procs.some((y) => y.pid === p.pid)); if (g) toggleGroup(g.key, true); }
    put({ sel: { pid: p.pid, st: p.st } });
  };
  // the selection only counts while its row is visible (filters, search and closed groups hide it)
  const selKey = selKeyOf(state.sel);
  const selRow = selKey ? rows.find((r) => rowKeyOf(r) === selKey) || null : null;
  const selItem = selRow ? selRow.group || selRow.proc : null;
  const canAct = !!selItem && !selItem.gone && (!selRow.group || selRow.group.stoppable);

  const infoOf = (r) => { if (r) { if (r.group) openInfo(r.group.lead, true); else openInfo(r.proc); } };
  const stopOf = (r, force = false) => { if (r) { if (r.group) openGroupStop(tr, r.group, force); else openStop(r.proc, force); } };
  const activate = (r) => (r.group ? toggleGroup(r.group.key) : openInfo(r.proc));
  const hideTip = () => { clearTimeout(tipTimer.current); tipKey.current = null; if (ui.get().tip) put({ tip: null }); };

  useKeyboard((e) => {
    if (state.menu) { if (e.key === 'Escape') put({ menu: null }); return; }
    if (state.gstop) { if (e.key === 'Escape') put({ gstop: null }); return; }
    if (state.stop) { if (e.key === 'Escape') put({ stop: null }); return; }
    if (state.info) { if (e.key === 'Escape') put({ info: null }); return; }
    const k = String(e.base || e.key || '').toLowerCase();
    if ((e.meta || e.ctrl) && k === 'i') { infoOf(selRow); return; }
    if (e.key === 'Delete' || ((e.meta || e.ctrl) && e.key === 'Backspace')) { stopOf(selRow, !!e.shift); return; }
    if (e.key === 'Enter' && selRow) { activate(selRow); return; }
    if ((e.key === 'ArrowRight' || e.key === 'ArrowLeft') && selRow) {
      const opens = (e.key === 'ArrowRight') !== (direction() === 'rtl');      // → opens a group (← in right to left)
      if (selRow.group) toggleGroup(selRow.group.key, opens);
      else if (selRow.parent && !opens) put({ sel: { group: selRow.parent } });
      return;
    }
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      const i = rows.findIndex((r) => rowKeyOf(r) === selKey);
      const j = Math.max(0, Math.min(rows.length - 1, i < 0 ? 0 : i + (e.key === 'ArrowDown' ? 1 : -1)));
      if (rows[j]) put({ sel: selOf(rows[j]) });
    }
  });

  if (!procs) return <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.loading')}</Text>;
  const totalCpu = procs.list.reduce((a, p) => a + p.cpu, 0);
  const onContext = (r, ev) => {
    const at = layerRoot.current && ev?.local ? ev.local(layerRoot.current) : { x: ev?.x || 0, y: ev?.y || 0 };
    hideTip();
    put({ sel: selOf(r), menu: { ...(r.group ? { group: r.group.key } : { pid: r.proc.pid, st: r.proc.st }), x: at.x, y: at.y } });
  };
  // a group row's hover summary: what it says is taken when it opens, and stays until the pointer leaves the row
  const onRowEnter = (r, e) => {
    clearTimeout(tipTimer.current);
    if (!r.group || r.group.gone) { hideTip(); return; }
    const key = r.group.key;
    tipKey.current = key;
    if (ui.get().tip) put({ tip: null });
    const at = layerRoot.current && e?.local ? e.local(layerRoot.current) : { x: e?.x || 0, y: e?.y || 0 };
    tipTimer.current = setTimeout(() => {
      const s = ui.get();
      if (tipKey.current !== key || s.menu || s.stop || s.gstop || s.info) return;
      const g = shownGroups.list.find((x) => x.key === key);
      if (g && !g.gone) put({ tip: { key, x: at.x, y: at.y, name: groupLabel(tr, g), sum: groupSummary(g), stats: stats.get(groupRowKey(key)) } });
    }, TIP_MS);
  };
  const onRowLeave = (r) => { if (r.group && tipKey.current === r.group.key) hideTip(); };
  const ended = (dim) => <Text style={{ fontSize: 11, color: dim, whiteSpace: 'nowrap' }}>{tr('monitor.proc.ended')}</Text>;
  const cpuText = (v, color) => <Text style={{ fontSize: 12, color, fontWeight: v > 0.5 ? 600 : 400, whiteSpace: 'nowrap', textAlign: 'end' }}>{formatNumber(v * 100, 1)}</Text>;
  const groupCell = (g, c, { color, dim, selected: on }) => {
    switch (c.key) {
      case 'name': return (
        <>
          <Disclosure state={g.match === 'procs' || open.has(g.key) ? 'open' : 'closed'} color={dim} onToggle={() => toggleGroup(g.key)} />
          <AppGlyph item={groupGlyph(g)} />
          <Text style={{ fontSize: 12, fontWeight: 600, color, whiteSpace: 'nowrap' }}>{groupLabel(tr, g)}</Text>
          <OriginBadge o={groupOriginOf(g, facts)} selected={on} />
          {g.gone ? ended(dim) : null}
          {on && g.stoppable && !g.gone ? <><View style={{ flex: 1 }} /><RowButton label={tr('monitor.group.quitAll')} color={color} onClick={() => openGroupStop(tr, g)} /></> : null}
        </>
      );
      case 'cpu': return g.gone ? '—' : cpuText(g.cpu, color);
      case 'rss': return fmt.mem(g.mem);
      case 'threads': return formatNumber(g.threads, 0);
      case 'count': return g.gone ? '' : formatNumber(g.count, 0);
      case 'cpuAvg': case 'cpuPeak': return g.gone ? '—' : formatNumber((g[c.key] || 0) * 100, 1);
      case 'memAvg': case 'memPeak': return fmt.mem(g[c.key]);
      case 'pid': return '';
      default: return g.user;
    }
  };
  const procCell = (p, child, c, { color, dim, selected: on }) => {
    switch (c.key) {
      case 'name': return (
        <>
          {child ? <View style={{ width: 22, flexShrink: 0 }} /> : null}
          <AppGlyph item={p} />
          <Text style={{ fontSize: 12, color, whiteSpace: 'nowrap' }}>{child ? p.name : procLabel(tr, p)}</Text>
          <OriginBadge o={originOf(p, facts)} selected={on} />
          {!child && p.appName && p.appName !== p.name ? <Text style={{ fontSize: 11, color: dim, whiteSpace: 'nowrap' }}>{p.appName}</Text> : null}
          {p.gone ? ended(dim) : null}
          {child && !p.gone ? <ChainLine chain={chainOfProc(p, view.byPid)} color={dim} onPick={selectProc} /> : null}
          {child && on && !p.gone && !p.prot ? <><View style={{ flex: 1 }} /><RowButton color={color} onClick={() => openStop(p)} /></> : null}
        </>
      );
      case 'cpu': return p.gone ? '—' : cpuText(p.cpu, color);
      case 'rss': return fmt.mem(p.rss);
      case 'threads': return formatNumber(p.threads, 0);
      case 'count': return '';
      case 'cpuAvg': case 'cpuPeak': return p.gone ? '—' : formatNumber((p[c.key] || 0) * 100, 1);
      case 'memAvg': case 'memPeak': return fmt.mem(p[c.key]);
      case 'pid': return String(p.pid);
      default: return p.user;
    }
  };
  const liveProcs = grouped ? view.groups.reduce((s, g) => s + g.rows.filter((p) => !p.gone).length, 0) : view.list.filter((p) => !p.gone).length;
  return (
    <>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, zIndex: 10, flexWrap: narrow ? 'wrap' : 'nowrap' }}>
        <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text }}>{tr('monitor.processes')}</Text>
        <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.procCount', { count: procs.total })}</Text>
        <View style={{ flex: 1 }} />
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.xs }}>
          <ToolbarButton icon={<StopGlyph color={canAct ? t.text : t.textDim} />} title={tr(selRow?.group ? 'monitor.group.stopTip' : 'monitor.proc.stopTip')}
            disabled={!canAct || overlay} onClick={() => stopOf(selRow)} />
          <ToolbarButton icon={<InfoGlyph color={selItem && !selItem.gone ? t.text : t.textDim} />} title={tr('monitor.proc.infoTip')}
            disabled={!selItem || !!selItem.gone || overlay} onClick={() => infoOf(selRow)} />
        </View>
        <Segmented value={mine} onChange={setMine} options={[['mine', tr('monitor.mine')], ['all', tr('monitor.all')]]} />
        <GroupedToggle on={grouped} onChange={setGrouped} label={tr('monitor.group.toggle')} />
        <SearchField value={q} onChange={setQ} placeholder={tr('monitor.search')} width={narrow ? 150 : 200} />
      </View>
      {/* the filters: by origin badge (none chosen: all) and one program, with Mine | All, Grouped and the search */}
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6, flexWrap: 'wrap', zIndex: 9 }}>
        {ORIGINS.map((o) => (
          <FilterChip key={o} kind={o} label={tr(`monitor.filter.${o}`)} on={filter.origins.includes(o)}
            onChange={(on) => setFilter({ origins: on ? [...filter.origins, o] : filter.origins.filter((x) => x !== o) })} />
        ))}
        <View style={{ flex: 1 }} />
        <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.stats.label')}</Text>
        <Dropdown small width={96} value={String(win)} onChange={(v) => setPref({ procStats: Number(v) || 0 })}
          options={[['0', tr('monitor.stats.off')], ...WINDOWS.map((w) => [String(w), tr('monitor.stats.window', { n: w })])]} />
        <Text style={{ fontSize: 12, color: t.textDim }}>{tr('monitor.filter.program')}</Text>
        <Dropdown small width={narrow ? 150 : 200} value={filter.program || ''} onChange={(v) => setFilter({ program: v || null })}
          options={[['', tr('monitor.filter.allPrograms')], ...view.programs, ...(filter.program && !view.programs.some(([k]) => k === filter.program) ? [[filter.program, filter.program.replace(/^[a-z]+:\d+:/, '')]] : [])]} />
      </View>
      {/* the SDK's table (0.8.8.8): a press or the keys choose a row (↑ ↓, Home, End, Page Up and Page Down, typing a
          name), Enter and a double click open Get Info (on a group: open or close it); → and ← open and close a group */}
      <View onMouseEnter={() => setHover(true)} onMouseLeave={() => { setHover(false); setLeftAt(Date.now()); }} onMouseDown={hideTip}>
        <Table columns={cols} rows={rows} rowKey={rowKeyOf} sort={sort}
          onSort={(key) => setSort((s0) => ({ key, dir: s0.key === key ? -s0.dir : key === 'name' || key === 'user' || key === 'pid' ? 1 : -1 }))}
          selection={selRow ? [selKey] : []} textOf={(r) => (r.group ? groupLabel(tr, r.group) : procLabel(tr, r.proc))} onActivate={activate} onContextMenu={onContext}
          onRowEnter={onRowEnter} onRowLeave={onRowLeave} rowStyle={(r) => ((r.group || r.proc).gone ? { opacity: 0.55 } : null)}
          onSelectionChange={(keys) => { const r = rows.find((x) => rowKeyOf(x) === keys[0]); put({ sel: r ? selOf(r) : null }); }}
          renderCell={(r, c, o) => (r.group ? groupCell(r.group, c, o) : procCell(r.proc, !!r.parent, c, o))} />
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
        <Text style={{ fontSize: 11, color: t.textDim, textAlign: 'start' }}>
          {grouped
            ? tr('monitor.group.foot', { groups: formatNumber(view.groups.filter((g) => !g.gone).length, 0), count: formatNumber(liveProcs, 0), cpu: formatNumber(totalCpu * 100, 0) })
            : tr('monitor.procFoot', { shown: rows.filter((r) => !r.proc.gone).length, count: liveProcs, cpu: formatNumber(totalCpu * 100, 0) })}
        </Text>
        <View style={{ flex: 1 }} />
        {paused ? (
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 5 }}>
            <PauseGlyph color={t.textDim} />
            <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap' }}>{tr('monitor.proc.paused')}</Text>
          </View>
        ) : null}
      </View>
    </>
  );
}

// ---- layers over the window: the context menu, the stop sheets, Get Info and a group's summary ----

export function ProcessLayers({ active }) {
  const state = useStore(ui);
  const procs = useStore(monitorProcesses);
  const root = useRef(null);
  layerRoot.current = root.current;
  useEffect(() => { layerRoot.current = root.current; });
  // leaving the tab closes everything (and stops following the process)
  useEffect(() => { if (!active) put({ menu: null, stop: null, gstop: null, info: null, tip: null }); }, [active]);
  const menuP = state.menu && !state.menu.group ? findProc(procs, state.menu) : null;
  const menuG = state.menu?.group ? shownGroups.list.find((g) => g.key === state.menu.group && !g.gone) || null : null;
  return (
    <>
      <View ref={root} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, pointerEvents: 'none' }} />
      {active && state.info ? <InfoSheet key={keyOf(state.info)} want={state.info} /> : null}
      {active && state.stop ? <StopSheet want={state.stop} /> : null}
      {active && state.gstop ? <GroupStopSheet key={state.gstop.key} want={state.gstop} /> : null}
      {active && (menuP || menuG) ? <RowMenu p={menuP} g={menuG} x={state.menu.x} y={state.menu.y} /> : null}
      {active && state.tip && !state.menu ? <GroupTip tip={state.tip} /> : null}
    </>
  );
}

function bounds() {
  try { const l = lord.getLayout(layerRoot.current.id); return { w: l[2], h: l[3] }; } catch (e) { return { w: 900, h: 580 }; }
}

// A sparkline: the last minute's samples as thin bars, the tallest at full height (left to right in every direction).
export function Spark({ values, color, width = 240, height = 22 }) {
  const t = useTheme();
  const n = SPARK_N;
  const v = (values || []).slice(-n);
  const top = Math.max(...v, 0) || 1;
  const bw = Math.max(1, Math.floor(width / n) - 1);
  return (
    <View style={{ width, height, flexDirection: 'row', alignItems: 'flex-end', justifyContent: 'flex-end', gap: 1, direction: 'ltr', borderBottomWidth: 1, borderColor: t.separator }}>
      {v.map((x, i) => <View key={i} style={{ width: bw, height: Math.max(1, Math.round((x / top) * (height - 1))), backgroundColor: color, opacity: 0.85 }} />)}
    </View>
  );
}
const SPARK_N = 60;

// A group's hover summary: information only, it takes no clicks and holds no buttons.
function GroupTip({ tip }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const { sum } = tip;
  const width = tip.stats ? 380 : 300;
  const b = bounds();
  const tall = tip.stats ? 190 : 100;
  const left = Math.max(4, Math.min(tip.x + 12, b.w - width - 4)), top = Math.max(4, Math.min(tip.y + 18, b.h - tall));
  const line = (s, bold) => (
    <Text style={{ fontSize: 11, color: bold ? t.text : t.textDim, fontWeight: bold ? 600 : 400, textAlign: 'start', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{s}</Text>
  );
  return (
    <View style={{ position: 'absolute', left, top, width, paddingVertical: 7, paddingHorizontal: 10, gap: 3, borderRadius: 6, zIndex: 280, pointerEvents: 'none',
      backgroundColor: t.mode === 'dark' ? '#3a3a40' : '#fbfbfd', border: `1px solid ${t.separator}`, boxShadow: '0 6px 18px rgba(0,0,0,0.3)' }}>
      {line(tip.name, true)}
      {line(tr('monitor.procCount', { count: sum.count }))}
      {sum.cpu ? line(tr('monitor.group.tipCpu', { name: sum.cpu.name, pid: sum.cpu.pid, cpu: formatNumber(sum.cpu.cpu * 100, 1) })) : null}
      {sum.mem ? line(tr('monitor.group.tipMem', { name: sum.mem.name, pid: sum.mem.pid, mem: fmt.mem(sum.mem.rss) })) : null}
      {tip.stats ? (
        <>
          {line(tr('monitor.stats.tipCpu', { avg: formatNumber(tip.stats.cpu[1].avg * 100, 1), peak: formatNumber(tip.stats.cpu[1].peak * 100, 1), p95: formatNumber(tip.stats.cpu[1].p95 * 100, 1) }))}
          <Spark values={tip.stats.spark.cpu} color="#0a84ff" width={358} />
          {line(tr('monitor.stats.tipMem', { avg: fmt.mem(tip.stats.mem[1].avg), peak: fmt.mem(tip.stats.mem[1].peak) }))}
          <Spark values={tip.stats.spark.mem} color="#30d158" width={358} />
        </>
      ) : null}
    </View>
  );
}

function RowMenu({ p, g, x, y }) {
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
  // a group: Quit All and Force Quit All (on the processes it has when chosen), and its main process's Get Info
  const items = g ? [
    ...(g.stoppable ? [
      { label: tr('monitor.group.quitAllMenu'), hint: tr('monitor.proc.deleteKey'), danger: true, onClick: () => openGroupStop(tr, g) },
      { label: tr('monitor.group.forceAllMenu'), hint: '', danger: true, onClick: () => openGroupStop(tr, g, true) },
      '-',
    ] : []),
    { label: tr('monitor.proc.getInfo'), hint: '❖I', onClick: () => openInfo(g.lead, true) },
  ] : [
    { label: tr('monitor.proc.getInfo'), hint: '❖I', onClick: () => openInfo(p) },
    '-',
    { label: tr('monitor.proc.quitMenu'), hint: tr('monitor.proc.deleteKey'), danger: true, onClick: () => openStop(p) },
    { label: tr('monitor.proc.forceMenu'), hint: '', danger: true, onClick: () => openStop(p, true) },
  ];
  const width = 240, h = items.reduce((s, it) => s + (it === '-' ? 9 : 24), 10);
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

// One process. want.force (Force Quit… in the menu): only Force Quit is offered.
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
  } else if (want.force) {
    title = tr('monitor.proc.forceConfirm', { name });
    body = own ? tr('monitor.proc.forceHint') : tr('monitor.proc.forceOther', { user: p.user });
    actions = (
      <>
        <Button label={tr('monitor.proc.cancel')} disabled={busy} onClick={close} />
        <View style={{ flex: 1 }} />
        <Button destructive label={tr('monitor.proc.forceQuit')} disabled={busy} onClick={() => run('KILL')} />
      </>
    );
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

// A group: Quit All (or Force Quit All) on the processes it had when it was chosen (want.snap: PID and start time).
// Running it signals the ones that are still those processes (a PID now used by another one is skipped and counted),
// never the protected ones; then it waits: when all have exited it closes, and after WAIT_MS it offers Force Quit for
// those still open. Another user's group: administrators only (the root helper's proc-signal, once per process).
function GroupStopSheet({ want }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const procs = useStore(monitorProcesses);
  const [phase, setPhase] = useState('ask');            // ask → wait → (stuck) → done
  const [sent, setSent] = useState([]);                 // the processes signalled
  const [notes, setNotes] = useState(null);             // { reused, gone, failed, prot }
  const [busy, setBusy] = useState(false);
  const own = want.uid === myUid(procs);
  const live = procs?.list || [];
  const close = () => put({ gstop: null });
  const running = phase === 'wait' || phase === 'stuck' ? stillRunning(sent, live) : [];
  const run = async (kind, snap) => {
    setBusy(true);
    const plan = planStop(snap, monitorProcesses.get()?.list);
    let gone = plan.gone.length, failed = 0;
    for (const s of plan.send) {
      try { await monitor.stopProcess(s, kind, { own }); } catch (e) { if (e?.code === 'proc.gone') gone++; else failed++; }
    }
    setNotes((n) => ({ reused: plan.reused.length + (n?.reused || 0), gone: gone + (n?.gone || 0), failed: failed + (n?.failed || 0), prot: plan.prot.length + (n?.prot || 0) }));
    setSent(plan.send);
    setBusy(false);
    setPhase('wait');
  };
  // waiting: closed when every signalled process has exited (unless something needs saying); after WAIT_MS, stuck
  useEffect(() => {
    if (phase !== 'wait') return undefined;
    const id = setTimeout(() => setPhase('stuck'), WAIT_MS);
    return () => clearTimeout(id);
  }, [phase, sent]);
  useEffect(() => {
    if ((phase === 'wait' || phase === 'stuck') && !running.length) {
      if (notes && (notes.reused || notes.failed || notes.prot)) setPhase('done'); else close();
    }
  });
  const protectedAll = want.snap.length > 0 && want.snap.every((s) => s.prot);
  const cant = want.kind === 'lordos' || want.kind === 'kernel' || protectedAll || (!own && !isAdmin());
  let title, body, actions;
  if (cant) {
    title = tr('monitor.group.cantStop', { name: want.name });
    body = want.kind === 'lordos' || want.kind === 'kernel' ? tr(`monitor.group.why.${want.kind}`)
      : protectedAll ? tr('monitor.group.why.protected') : tr('monitor.group.needAdmin', { user: want.user });
    actions = <><View style={{ flex: 1 }} /><Button primary label={tr('monitor.proc.ok')} onClick={close} /></>;
  } else if (phase === 'ask') {
    title = tr(want.force ? 'monitor.group.forceConfirm' : 'monitor.group.confirm', { name: want.name });
    body = `${tr(want.force ? 'monitor.group.forceHint' : 'monitor.group.confirmHint')}${own ? '' : ` ${tr('monitor.group.otherUser', { user: want.user })}`}`;
    actions = (
      <>
        <Button label={tr('monitor.proc.cancel')} disabled={busy} onClick={close} />
        <View style={{ flex: 1 }} />
        {want.force ? null : <Button destructive label={tr('monitor.proc.forceQuit')} disabled={busy} onClick={() => run('KILL', want.snap)} />}
        {want.force ? <Button destructive label={tr('monitor.proc.forceQuit')} disabled={busy} onClick={() => run('KILL', want.snap)} />
          : <Button primary label={tr('monitor.group.quitAll')} disabled={busy} onClick={() => run('TERM', want.snap)} />}
      </>
    );
  } else if (phase === 'done') {
    title = tr('monitor.group.doneTitle', { name: want.name });
    body = '';
    actions = <><View style={{ flex: 1 }} /><Button primary label={tr('monitor.proc.close')} onClick={close} /></>;
  } else {
    title = tr('monitor.group.waitTitle', { name: want.name });
    body = phase === 'stuck' ? tr('monitor.group.stuck', { n: formatNumber(running.length, 0) }) : tr('monitor.group.waiting', { n: formatNumber(running.length, 0) });
    actions = (
      <>
        <Button label={tr('monitor.proc.close')} disabled={busy} onClick={close} />
        <View style={{ flex: 1 }} />
        {phase === 'stuck' ? <Button destructive primary label={tr('monitor.proc.forceQuit')} disabled={busy} onClick={() => run('KILL', running)} /> : null}
      </>
    );
  }
  const list = phase === 'ask' ? want.snap : running;
  const note = (k, v) => (v ? <Text key={k} style={{ fontSize: typeScale.caption, color: k === 'failed' ? RED : t.textDim, textAlign: 'start' }}>{tr(`monitor.group.note.${k}`, { n: formatNumber(v, 0) })}</Text> : null);
  return (
    <Sheet width={480} z={250}>
      <View style={{ flexDirection: 'row', gap: spacing.m, alignItems: 'flex-start' }}>
        <AppGlyph item={want.item} size={32} />
        <View style={{ flex: 1, gap: 6, minWidth: 0 }}>
          <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, textAlign: 'start', lineHeight: 1.3 }}>{title}</Text>
          {body ? <Text style={{ fontSize: typeScale.body, color: t.textDim, lineHeight: 1.4, textAlign: 'start' }}>{body}</Text> : null}
          {!cant && list.length ? (
            <View style={{ gap: 4 }}>
              <Text style={{ fontSize: typeScale.caption, fontWeight: 600, color: t.textDim, textAlign: 'start' }}>{tr('monitor.procCount', { count: list.length })}</Text>
              <ScrollView style={{ maxHeight: 150, borderRadius: 6, border: `1px solid ${t.separator}` }}>
                <View style={{ paddingVertical: 3 }}>
                  {list.map((s) => (
                    <View key={keyOf(s)} style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s, height: 20, paddingHorizontal: 8 }}>
                      <Text style={{ flex: 1, minWidth: 0, fontSize: 11, color: s.prot ? t.textDim : t.text, whiteSpace: 'nowrap', textAlign: 'start' }}>{s.name}</Text>
                      {s.prot ? <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap' }}>{tr('monitor.group.protectedTag')}</Text> : null}
                      <Text style={{ fontSize: 11, color: t.textDim, whiteSpace: 'nowrap' }}>{tr('monitor.proc.pidUser', { pid: s.pid, user: s.user })}</Text>
                    </View>
                  ))}
                </View>
              </ScrollView>
            </View>
          ) : null}
          {notes && phase !== 'ask' ? <View style={{ gap: 2 }}>{['reused', 'gone', 'prot', 'failed'].map((k) => note(k, notes[k]))}</View> : null}
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
  // who started it: the chain from the same process list, by identity (PID and start time)
  const procs = useStore(monitorProcesses);
  const byPid = useMemo(() => new Map((procs?.list || []).map((x) => [x.pid, x])), [procs]);
  const chain = p ? chainOfProc(p, byPid) : [];
  const pst = p ? stats.get(procKey(p)) : null;          // its rolling statistics (redrawn with each process list)
  return (
    <GroupBox>
      <Row label={tr('monitor.proc.st.user')}><Val>{p ? p.user : String(d.uid)}{d.euid !== d.uid ? ` (${tr('monitor.proc.st.effective', { uid: d.euid })})` : ''}</Val></Row>
      {chain.length > 1 ? (
        <Row label={tr('monitor.chain.label')}>
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 4, direction: 'ltr' }}>
            {chain.map((l, i) => {
              const q = byPid.get(l.pid);
              const last = i === chain.length - 1;
              return (
                <View key={`${l.pid}:${l.st}`} style={{ flexDirection: 'row', alignItems: 'center', gap: 4 }}>
                  {i ? <Text style={{ fontSize: typeScale.body, color: t.textDim }}>›</Text> : null}
                  {q ? <AppGlyph item={q} size={14} /> : null}
                  <Text onClick={last || !q ? undefined : () => openInfo(q)}
                    style={{ fontSize: typeScale.body, color: last || !q ? t.text : t.accent, whiteSpace: 'nowrap' }}>{linkLabel(tr, chain, i)}</Text>
                </View>
              );
            })}
          </View>
        </Row>
      ) : null}
      <Row label={tr('monitor.proc.st.parent')}>
        {parent ? (
          <Text onClick={() => openInfo(parent)} style={{ fontSize: typeScale.body, color: t.accent, textAlign: 'start' }}>{`${procLabel(tr, parent)} (${parent.pid})`}</Text>
        ) : <Val>{d.ppid ? String(d.ppid) : '—'}</Val>}
      </Row>
      <Row label={tr('monitor.proc.st.group')}><Val>{`${d.pgid} · ${tr('monitor.proc.st.session', { sid: d.sid })}`}</Val></Row>
      <Row label={tr('monitor.proc.st.cpu')}><Val>{p ? fmt.pct(tr, p.cpu) : '—'}</Val></Row>
      {pst ? (
        <>
          <Row label={tr('monitor.stats.cpuAvgRow')}><Val>{WINDOWS.map((w) => `${formatNumber(pst.cpu[w].avg * 100, 1)}%`).join(' · ')}</Val></Row>
          <Row label={tr('monitor.stats.cpuPeakRow')}><Val>{WINDOWS.map((w) => `${formatNumber(pst.cpu[w].peak * 100, 1)}%`).join(' · ')}</Val></Row>
          <Row label={tr('monitor.stats.p95Row')}><Val>{`${formatNumber(pst.cpu[1].p95 * 100, 1)}%`}</Val></Row>
          <Row label={tr('monitor.stats.memAvgRow')}><Val>{WINDOWS.map((w) => fmt.mem(pst.mem[w].avg)).join(' · ')}</Val></Row>
          <Row label={tr('monitor.stats.memPeakRow')}><Val>{WINDOWS.map((w) => fmt.mem(pst.mem[w].peak)).join(' · ')}</Val></Row>
          <Row label={tr('monitor.stats.spark')}>
            <View style={{ gap: 4, paddingVertical: 4 }}>
              <Spark values={pst.spark.cpu} color="#0a84ff" width={300} />
              <Spark values={pst.spark.mem} color="#30d158" width={300} />
            </View>
          </Row>
        </>
      ) : null}
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
