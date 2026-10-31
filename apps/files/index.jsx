// Files — a file browser: tabs, four views (icons, list, columns and gallery), a sidebar
// with Favorites, Locations and Tags, the freedesktop Trash, Quick Preview, Get Info,
// copy/cut/paste, drag and drop, and search. Locations also has This computer (the whole tree,
// read-only outside the home folder and the user's disks) and every disk, mounted or not.
// Disk operations go through lord.fs (server/src/fsops.c); see docs/FILES.md.
import {
  View, Text, useTheme, useAppWindow, useKeyboard, useStore, useLauncherApps, addListener, wm, fs, clipboard, archive, warmDocs,
  formatBytes, t as tr, ScrollView, Toolbar, ToolbarButton, ToolbarGroup, SearchField, Hairline, direction, spacing, typeScale, registerDockActions,
  useDisks, admin, systemInfo, lan,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { FolderGlyph, DocGlyph, TAG_COLORS, TrashGlyph, DriveGlyph } from './glyphs.jsx';
import { IconView, ListView, ColumnView, GalleryView } from './views.jsx';
import { ContextMenu, QuickLook, InfoPanel, Confirm } from './panels.jsx';
import { loadEntries, sortEntries, groupEntries, sameLoc, userDirs } from './locations.js';
import {
  fsVersion, bump, fileClipboard, copyFiles, cutFiles, paste, transfer, freeName, jobState, jobErrors, tagStore, toggleTag, moveTags,
  openersFor, openEntry,
} from './ops.js';
import { archiveVersion, archiveState, extractEntry, extractHere, compress, isArchiveFile } from './archive.js';
import { NetworkView, NetGlyph, isShareFs } from './network.jsx';

export function Icon({ size: s }) {
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(180deg, #74c7ff, #1f6fe8)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, overflow: 'hidden' }}>
      <View style={{ position: 'absolute', left: s * 0.2, top: s * 0.27, width: s * 0.27, height: s * 0.12,
        borderRadius: s * 0.03, backgroundColor: 'rgba(255,255,255,0.75)' }} />
      <View style={{ position: 'absolute', left: s * 0.2, top: s * 0.33, width: s * 0.6, height: s * 0.42,
        borderRadius: s * 0.06, backgroundColor: 'rgba(255,255,255,0.95)' }} />
    </View>
  );
}

const VIEWS = ['icons', 'list', 'columns', 'gallery'];
const VIEW_GLYPH = { icons: '▦', list: '≡', columns: '◫', gallery: '▣' };
const SIDEBAR_W = 200;
let tabSeq = 1;
const newTab = (loc, from) => ({ id: tabSeq++, loc, back: [], fwd: [], view: from?.view || 'icons', sort: from?.sort || { by: 'name', desc: false }, group: from?.group || 'none', chain: [] });
// State of each window: it survives the interface being remounted (a language change).
const winStates = new Map();

const homeLoc = () => ({ type: 'dir', path: fs.home() });
const computerLoc = () => ({ type: 'dir', path: '/' });
// This computer is named after the machine
const computerName = () => { try { return systemInfo().hostname || tr('files.place.computer'); } catch (e) { return tr('files.place.computer'); } };
// Partitions (or whole disks without a table) that hold a file system and aren't mounted, off the
// disk the system runs from: the sidebar offers them, mounted with the Disks app's disk-mount.
const NO_MOUNT = ['', 'swap', 'crypto_LUKS', 'LVM2_member', 'linux_raid_member'];
function unmountedDisks(list) {
  const out = [];
  for (const d of list || []) {
    if (d.system) continue;
    for (const p of d.partitions?.length ? d.partitions : [d]) {
      if (!NO_MOUNT.includes(p.fstype || '') && !p.mountpoint) out.push({ name: p.name, label: p.label, size: p.size, removable: d.removable });
    }
  }
  return out;
}
// What ⏏ does for a mounted volume: a removable disk is ejected whole (disk-eject), another disk's
// partition is unmounted (disk-unmount); nothing on the disk the system runs from.
function volumeRelease(list, path, fstype) {
  if (isShareFs(fstype)) return { action: 'share-unmount', name: path };
  for (const d of list || []) {
    if (d.system) continue;
    const p = (d.partitions?.length ? d.partitions : [d]).find((x) => x.mountpoint === path);
    if (p) return d.removable ? { action: 'disk-eject', name: d.name } : { action: 'disk-unmount', name: p.name };
  }
  return null;
}
function argLoc(args) {
  if (args?.place === 'trash') return { type: 'trash' };
  if (args?.place === 'network') return { type: 'network' };
  if (!args?.path) return null;
  const home = fs.home();
  const p = String(args.path).replace(/^~(?=\/|$)/, home);
  const path = p.startsWith('/') ? p.replace(/\/+$/, '') || '/' : `${home}/${p}`;
  // an archive opens browsed, like double-clicking it
  if (archive.isArchive(fs.basename(path))) { try { if (!fs.stat(path).dir) return { type: 'archive', path, inner: '' }; } catch (e) { /* not there */ } }
  return { type: 'dir', path };
}
// The Dock's quick actions: Home, Downloads, Documents and the Trash (sdk/dock.js).
registerDockActions('files', () => {
  const dirs = userDirs();
  const dir = (label) => dirs.find((d) => d.label === label);
  return [
    { id: 'home', label: 'files.place.home', args: { path: '~' } },
    ...['downloads', 'documents'].filter(dir).map((k) => ({ id: k, label: `files.place.${k}`, args: { path: dir(k).path } })),
    { id: 'trash', label: 'files.place.trash', args: { place: 'trash' } },
  ];
});

// args.view ('icons' | 'list' | 'columns' | 'gallery') picks the view of the opened folder
const argView = (args) => (VIEWS.includes(args?.view) ? args.view : null);

function locTitle(loc, dirs) {
  switch (loc.type) {
    case 'recent': return tr('files.place.recent');
    case 'apps': return tr('files.place.apps');
    case 'trash': return tr('files.place.trash');
    case 'network': return tr('files.place.network');
    case 'tag': return tr(`files.tag.${loc.color}`);
    case 'search': return tr('files.searchTitle', { q: loc.query });
    case 'archive': return loc.inner ? fs.basename(loc.inner) : fs.basename(loc.path);
    default: {
      if (loc.path === fs.home()) return tr('files.place.home');
      if (loc.path === '/') return computerName();
      const d = dirs.find((x) => x.path === loc.path);
      return d ? tr(`files.place.${d.label}`) : fs.basename(loc.path);
    }
  }
}

// ---- window pieces ----

// Source-list row: 28 px, accent-colored icon, a soft highlight when selected and the
// accent when something is dragged over it.
function SideItem({ label, icon, selected, onClick, t, drop, hot, dim, trail }) {
  const [hover, setHover] = useState(false);
  return (
    <View onClick={onClick} onMouseEnter={() => setHover(true)} onMouseLeave={() => setHover(false)} {...drop}
      style={{ height: 28, flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 8, borderRadius: 6,
        backgroundColor: hot ? t.accent : selected ? (t.mode === 'dark' ? 'rgba(255,255,255,0.12)' : 'rgba(0,0,0,0.08)') : hover ? t.hover : 'transparent' }}>
      <View style={{ width: 18, alignItems: 'center', justifyContent: 'center' }}>{icon}</View>
      <Text style={{ flex: 1, fontSize: typeScale.body, color: hot ? '#fff' : dim ? t.textDim : t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{label}</Text>
      {trail && (hover || selected) ? (
        <View onClick={(e) => { e.stopPropagation?.(); trail.onClick(); }} style={{ width: 18, alignItems: 'center' }}>
          <Text style={{ fontSize: 12, color: t.textDim }}>{trail.glyph}</Text>
        </View>
      ) : null}
    </View>
  );
}

const SideTitle = ({ children, t, first }) => (
  <Text style={{ fontSize: typeScale.caption, fontWeight: 600, color: t.textDim, marginHorizontal: 8, marginTop: first ? 6 : 14, marginBottom: 2 }}>{children}</Text>
);
const Dot = ({ color }) => <View style={{ width: 10, height: 10, borderRadius: 5, backgroundColor: color }} />;
const Glyph = ({ ch, t }) => <Text style={{ fontSize: 14, color: t.accent, textAlign: 'center' }}>{ch}</Text>;

// Width of a box after layout, re-measured while the window is resized.
function useWidth(ref, initial) {
  const [w, setW] = useState(initial);
  useEffect(() => {
    const measure = () => { if (!ref.current) return; const lw = Math.round(lord.getLayout(ref.current.id)[2]); if (lw > 0) setW((x) => (Math.abs(x - lw) > 2 ? lw : x)); };
    measure();
    const id = setInterval(measure, 300);
    return () => clearInterval(id);
  }, []);
  return w;
}

// Search scope chip ("Search: Home | “Folder”"), as in a search scope bar.
function ScopeChip({ label, on, onClick, t }) {
  return (
    <View onClick={onClick} style={{ paddingHorizontal: 10, height: 22, borderRadius: 6, justifyContent: 'center',
      backgroundColor: on ? (t.mode === 'dark' ? 'rgba(255,255,255,0.14)' : 'rgba(0,0,0,0.08)') : 'transparent' }}>
      <Text style={{ fontSize: typeScale.callout, fontWeight: on ? 600 : 400, color: on ? t.text : t.textDim, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
}

// ---- the window ----

export default function Files() {
  const t = useTheme();
  const { id: wid, args } = useAppWindow();
  const launcherApps = useLauncherApps();
  const version = useStore(fsVersion);
  const tags = useStore(tagStore);
  const arcVer = useStore(archiveVersion);
  const clip = useStore(fileClipboard);
  const jobs = useStore(jobState);
  const errs = useStore(jobErrors);
  const saved = winStates.get(wid);
  const [tabs, setTabs] = useState(() => saved?.tabs || [{ ...newTab(argLoc(args) || homeLoc()), view: argView(args) || 'icons' }]);
  const [active, setActive] = useState(() => saved?.active || 0);
  const tab = tabs[Math.min(active, tabs.length - 1)];
  useEffect(() => { winStates.set(wid, { tabs, active }); }, [tabs, active]);
  useEffect(() => () => { if (!wm.get(wid)) winStates.delete(wid); }, []);
  const patchTab = (p) => setTabs((ts) => ts.map((x, i) => (i === active ? { ...x, ...(typeof p === 'function' ? p(x) : p) } : x)));

  const [selected, setSelected] = useState(() => new Set());
  const anchor = useRef(null);
  const [renaming, setRenaming] = useState(null);         // { path, value }
  const [menu, setMenu] = useState(null);                 // { x, y, items }
  const [quick, setQuick] = useState(false);
  const [info, setInfo] = useState(false);
  const [confirm, setConfirm] = useState(null);
  const [query, setQuery] = useState('');
  const [scope, setScope] = useState('here');
  const [debounced, setDebounced] = useState('');
  const [hidden, setHidden] = useState(false);
  const [expanded, setExpanded] = useState(() => new Set());
  const [drag, setDrag] = useState(null);                 // { paths, x, y, alt }
  const [dropTarget, setDropTarget] = useState(null);
  const [volumes, setVolumes] = useState(() => fs.volumes());
  const [diskList] = useDisks();
  useEffect(() => { setTimeout(warmDocs, 800); }, []);             // PDF Quick Preview without waiting for the helper
  const [msg, setMsg] = useState(null);
  const [netModal, setNetModal] = useState(false);        // Network's connect sheet has the keyboard
  const root = useRef(null);
  const press = useRef(null);
  const typed = useRef({ text: '', at: 0 });

  const dirs = useMemo(() => userDirs(), []);
  const baseLoc = tab.loc;
  const loc = debounced ? { type: 'search', root: scope === 'home' || baseLoc.type !== 'dir' ? fs.home() : baseLoc.path, query: debounced } : baseLoc;
  const [entries, error] = useMemo(() => {
    try { return [loadEntries(loc, { hidden, launcherApps }), null]; } catch (e) { return [[], e.message]; }
  }, [loc.type, loc.path, loc.color, loc.query, loc.root, loc.inner, hidden, version, tags, launcherApps, arcVer]);
  // Recents and the Trash go by date (newest first), unless another order is chosen
  const sortBy = (loc.type === 'recent' || loc.type === 'trash') && !tab.sortChosen ? { by: 'date', desc: true } : tab.sort;
  const sorted = useMemo(() => sortEntries(entries, sortBy), [entries, sortBy.by, sortBy.desc]);
  const groups = useMemo(() => groupEntries(sorted, tab.group, tr), [sorted, tab.group]);
  const flat = useMemo(() => groups.flatMap((g) => g.items), [groups]);
  const byPath = useMemo(() => new Map(flat.map((e) => [e.path, e])), [flat]);
  const curDir = baseLoc.type === 'dir' ? baseLoc.path : null;
  // outside the home folder and the user's disks a folder is only browsed (lord.fs reads there but
  // doesn't write): the actions that change it are left out
  const readOnly = useMemo(() => { if (!curDir) return false; try { return !fs.stat(curDir).writable; } catch (e) { return true; } }, [curDir, version]);
  const wDir = readOnly ? null : curDir;
  const title = locTitle(loc, dirs);

  function navigate(next, push = true) {
    if (sameLoc(next, tab.loc)) return;
    setSelected(new Set()); setRenaming(null); setQuery(''); setDebounced(''); setExpanded(new Set());
    patchTab((x) => ({ loc: next, back: push ? [...x.back, x.loc].slice(-50) : x.back, fwd: push ? [] : x.fwd, chain: [] }));
  }
  const goBack = () => { if (!tab.back.length) return; const prev = tab.back[tab.back.length - 1]; setSelected(new Set()); patchTab((x) => ({ loc: prev, back: x.back.slice(0, -1), fwd: [x.loc, ...x.fwd], chain: [] })); };
  const goForward = () => { if (!tab.fwd.length) return; const nxt = tab.fwd[0]; setSelected(new Set()); patchTab((x) => ({ loc: nxt, back: [...x.back, x.loc], fwd: x.fwd.slice(1), chain: [] })); };
  const goUp = () => {
    if (baseLoc.type === 'archive') { navigate(baseLoc.inner ? { type: 'archive', path: baseLoc.path, inner: baseLoc.inner.split('/').slice(0, -1).join('/') } : { type: 'dir', path: fs.parent(baseLoc.path) || '/' }); return; }
    if (curDir && curDir !== '/' && curDir !== fs.home()) navigate({ type: 'dir', path: fs.parent(curDir) || '/' });
  };

  // other apps open a folder: wm.open('files', { args: { path } })
  useEffect(() => { const l = argLoc(args); if (l && args?.nonce && saved) navigate(l); if (args?.nonce && saved && argView(args)) patchTab({ view: argView(args) }); }, [args?.nonce]);
  useEffect(() => { const id = setInterval(() => setVolumes(fs.volumes()), 4000); return () => clearInterval(id); }, []);
  useEffect(() => { const id = setTimeout(() => setDebounced(query.trim()), 280); return () => clearTimeout(id); }, [query]);
  useEffect(() => { if (!msg) return undefined; const id = setTimeout(() => setMsg(null), 4000); return () => clearTimeout(id); }, [msg]);
  useEffect(() => { if (errs.length) { setMsg(errs[errs.length - 1]); jobErrors.set([]); } }, [errs]);
  useEffect(() => { wm.setTitle(wid, title); }, [title]);

  const sel = flat.filter((e) => selected.has(e.path));
  const selPaths = sel.map((e) => e.path);
  const first = sel[0];
  const local = (ev) => (root.current ? ev.local(root.current) : { x: ev.x, y: ev.y });
  const bounds = () => { const l = root.current ? lord.getLayout(root.current.id) : [0, 0, 800, 600]; return { w: l[2], h: l[3] }; };

  // ---- actions ----
  const report = (e) => { console.log(`files: ${e?.message || e}`); setMsg(String(e?.message || e)); };
  const newFolder = (withPaths) => {
    if (!wDir) return;
    const name = freeName(wDir, tr('files.newFolder'));
    try {
      fs.mkdir(`${wDir}/${name}`);
      if (withPaths && withPaths.length) transfer(withPaths, `${wDir}/${name}`, true);
      bump();
      setSelected(new Set([`${wDir}/${name}`]));
      setRenaming({ path: `${wDir}/${name}`, value: name });
    } catch (e) { report(e); }
  };
  const startRename = (e = first) => { if (e && !e.app && !e.trash && wDir !== null) { setSelected(new Set([e.path])); setRenaming({ path: e.path, value: e.name }); } };
  const commitRename = () => {
    const r = renaming;
    setRenaming(null);
    if (!r) return;
    const name = r.value.trim();
    if (!name || name === fs.basename(r.path) || name.includes('/')) return;
    const to = `${fs.parent(r.path)}/${name}`;
    try { fs.rename(r.path, to); moveTags(r.path, to); bump(); setSelected(new Set([to])); } catch (e) { report(e); }
  };
  // actions receive the paths to use (the context menu fixes them when it opens: the selection
  // at that moment, or the right-clicked item if it was not selected)
  const askDelete = (paths = selPaths) => setConfirm({ text: tr('files.confirm.delete', { count: paths.length }), detail: tr('files.confirm.cantUndo'), ok: tr('files.menu.deleteNow'),
    run: () => { for (const p of paths) { try { fs.removeFromTrash(p); } catch (err) { report(err); } } bump(); } });
  const askEmpty = () => setConfirm({ text: tr('files.confirm.empty'), detail: tr('files.confirm.cantUndo'), ok: tr('files.menu.emptyTrash'),
    run: () => { fs.emptyTrash(); bump(); } });
  const toTrash = (paths = selPaths) => {
    if (!paths.length || readOnly) return;
    if (loc.type === 'trash') { askDelete(paths); return; }
    let n = 0;
    for (const p of paths) { try { fs.trash(p); n++; } catch (e) { report(e); } }
    if (n) { setMsg(tr('files.trashed', { count: n })); bump(); }
  };
  const restore = (paths = selPaths) => { let n = 0; for (const p of paths) { try { fs.restore(p); n++; } catch (err) { report(err); } } if (n) { setMsg(tr('files.restored', { count: n })); bump(); } };
  // disks: the same closed actions as the Disks app (an internal disk asks for an administrator)
  const mount = (p) => {
    setMsg(tr('files.disk.mounting', { name: p.label || p.name }));
    admin.run('disk-mount', p.name, null, { quiet: true }).then(
      (path) => { setVolumes(fs.volumes()); setMsg(null); const at = String(path || '').trim(); if (at.startsWith('/')) navigate(dirLoc(at)); },
      (e) => setMsg(e.message));
  };
  const release = (r, path) => {
    admin.run(r.action, r.name, null, { quiet: true }).then(
      () => { setVolumes(fs.volumes()); setMsg(tr(r.action === 'disk-eject' ? 'disks.ejected' : 'disks.unmounted')); if (curDir && (curDir === path || curDir.startsWith(`${path}/`))) navigate(homeLoc()); },
      (e) => setMsg(e.message));
  };
  const duplicate = (paths = selPaths) => { if (wDir && paths.length) transfer(paths, wDir, false); };
  const doPaste = () => { if (wDir) paste(wDir); };
  const ctx = { navigate, linuxApps: launcherApps, t: tr, quickLook: (e) => { setSelected(new Set([e.path])); setQuick(true); },
    openArc: (e) => { setMsg(tr('docs.opening', { name: e.name })); extractEntry(e).then((real) => openEntry({ name: e.name, path: real, size: e.size, mtime: e.mtime }, ctx)).catch((err) => setMsg(tr('docs.extractError', { name: e.name, error: err.message }))); } };
  const arcReport = (m) => setMsg(m);
  const open = (e) => { if (e.trash) { setSelected(new Set([e.path])); setQuick(true); return; } openEntry(e, ctx); };
  const openSel = (list = sel) => { for (const e of list.slice(0, 8)) open(e); };
  const newTabAt = (l) => { setTabs((ts) => [...ts, newTab(l || tab.loc, tab)]); setActive(tabs.length); setSelected(new Set()); };
  const closeTab = (i) => {
    if (tabs.length === 1) { wm.close(wid); return; }
    setTabs((ts) => ts.filter((_, j) => j !== i));
    setActive((a) => (a >= i && a > 0 ? a - 1 : a));
  };
  const setView = (view) => patchTab({ view });
  const setSort = (by) => patchTab((x) => ({ sortChosen: true, sort: { by, desc: x.sort.by === by ? !x.sort.desc : by === 'date' || by === 'size' } }));

  // ---- selection ----
  const onItemDown = (e, ev) => {
    if (renaming && renaming.path !== e.path) commitRename();
    if (ev.button === 2) return;
    let next;
    if (ev.meta || ev.ctrl) { next = new Set(selected); if (next.has(e.path)) next.delete(e.path); else next.add(e.path); anchor.current = e.path; }
    else if (ev.shift && anchor.current) {
      const a = flat.findIndex((x) => x.path === anchor.current), b = flat.findIndex((x) => x.path === e.path);
      next = new Set(flat.slice(Math.min(a, b), Math.max(a, b) + 1).map((x) => x.path));
    } else { next = selected.has(e.path) ? selected : new Set([e.path]); anchor.current = e.path; }
    setSelected(next);
    if (!e.app && loc.type !== 'trash') press.current = { paths: next.has(e.path) ? [...next] : [e.path], x: ev.x, y: ev.y };
  };

  // ---- drag and drop (⌥ copies; otherwise it moves) ----
  const dropOf = (inst) => { for (let n = inst; n; n = n.parent) { if (n.props?.dropPath) return n.props.dropPath; if (n.props?.dropTrash) return 'trash:'; } return null; };
  useEffect(() => {
    const mv = addListener('pointermove', (e) => {
      const p = press.current;
      if (!p) return;
      if (!p.moved && Math.hypot(e.x - p.x, e.y - p.y) < 6) return;
      p.moved = true;
      const target = dropOf(e.target);
      setDrag({ paths: p.paths, x: e.x, y: e.y, alt: e.alt });
      setDropTarget(target && !p.paths.includes(target) ? target : null);
    });
    const up = addListener('pointerup', (e) => {
      const p = press.current;
      press.current = null;
      if (!p || !p.moved) return;
      const target = dropOf(e.target);
      setDrag(null); setDropTarget(null);
      if (!target || p.paths.includes(target)) return;
      if (target === 'trash:') { let n = 0; for (const x of p.paths) { try { fs.trash(x); n++; } catch (err) { setMsg(err.message); } } if (n) bump(); return; }
      transfer(p.paths, target, !e.alt);
    });
    return () => { mv(); up(); };
  }, []);

  // ---- context menus ----
  const tagSwatches = (paths) => ({ swatches: Object.entries(TAG_COLORS).map(([color, hex]) => ({ color, hex, on: paths.every((p) => (tags[p] || []).includes(color)), onClick: () => toggleTag(paths, color) })) });
  function itemMenu(e) {
    const paths = selected.has(e.path) ? selPaths : [e.path];
    const list = selected.has(e.path) ? sel : [e];
    if (loc.type === 'trash') {
      return [{ label: tr('files.menu.restore'), onClick: () => restore(paths) }, { label: tr('files.menu.deleteNow'), danger: true, onClick: () => askDelete(paths) }, '-',
        { label: tr('files.menu.quickLook'), hint: '␣', onClick: () => setQuick(true) }, { label: tr('files.menu.getInfo'), hint: '❖I', onClick: () => setInfo(true) }];
    }
    if (e.app) return [{ label: tr('files.menu.open'), onClick: () => open(e) }];
    if (e.arc) {
      // inside an archive: open, view and extract (it cannot be modified)
      return [{ label: tr('files.menu.open'), onClick: () => open(e) }, { label: tr('files.menu.quickLook'), hint: '␣', onClick: () => setQuick(true) }, '-',
        { label: tr('docs.extractSelected', { count: list.length }), onClick: () => { const dir = fs.parent(e.arc.file); const name = freeName(dir, archive.stem(fs.basename(e.arc.file)));
          setMsg(tr('docs.extracting', { name: fs.basename(e.arc.file) }));
          archive.extract(e.arc.file, `${dir}/${name}`, { only: list.map((x) => x.arc.inner) }).then((r) => { bump(); setMsg(tr('docs.extracted', { name, count: r.files })); }).catch((err) => setMsg(tr('docs.extractError', { name, error: err.message }))); } }];
    }
    const openers = openersFor(e, launcherApps, tr);
    return [
      { label: tr('files.menu.open'), hint: '❖↓', onClick: () => openSel(list) },
      ...(e.dir ? [{ label: tr('files.menu.openTab'), onClick: () => newTabAt({ type: 'dir', path: e.path }) }] : []),
      ...(!e.dir && openers.length ? [{ label: tr('files.menu.openWith'), submenu: openers.map((o) => ({ label: o.label, onClick: o.run })) }] : []),
      '-',
      { label: tr('files.menu.trash'), hint: '❖⌫', onClick: () => toTrash(paths), disabled: readOnly },
      '-',
      // compress and extract (M9, lord-doc)
      ...(wDir && list.length === 1 && isArchiveFile(e) ? [{ label: tr('docs.extractHere'), onClick: () => extractHere(e, arcReport, tr) }] : []),
      ...(wDir ? [{ label: list.length === 1 ? tr('docs.compressOne', { name: e.name }) : tr('docs.compressMany', { count: list.length }), onClick: () => compress(paths, wDir, arcReport, tr) }] : []),
      '-',
      { label: tr('files.menu.getInfo'), hint: '❖I', onClick: () => setInfo(true) },
      { label: tr('files.menu.rename'), hint: '↵', onClick: () => startRename(e), disabled: paths.length > 1 || !wDir },
      { label: tr('files.menu.duplicate'), hint: '❖D', onClick: () => duplicate(paths), disabled: !wDir },
      { label: tr('files.menu.quickLook'), hint: '␣', onClick: () => setQuick(true) },
      '-',
      { label: tr('files.menu.copy'), hint: '❖C', onClick: () => copyFiles(paths) },
      { label: tr('files.menu.cut'), hint: '❖X', onClick: () => cutFiles(paths), disabled: readOnly },
      { label: tr('files.menu.copyPath'), onClick: () => clipboard.set(paths.join('\n')) },
      ...(wDir ? [{ label: tr('files.menu.newFolderWith', { count: paths.length }), onClick: () => newFolder(paths) }] : []),
      '-',
      { header: tr('files.menu.tags') }, tagSwatches(paths),
    ];
  }
  function bgMenu() {
    const viewItems = VIEWS.map((v, i) => ({ label: tr(`files.view.${v}`), hint: `❖${i + 1}`, checked: tab.view === v, onClick: () => setView(v) }));
    const sortItems = ['name', 'kind', 'date', 'size'].map((k) => ({ label: tr(`files.sort.${k}`), checked: tab.sort.by === k, onClick: () => setSort(k) }));
    const groupItems = ['none', 'kind', 'date', 'size'].map((k) => ({ label: tr(`files.groupBy.${k}`), checked: tab.group === k, onClick: () => patchTab({ group: k }) }));
    if (loc.type === 'trash') return [{ label: tr('files.menu.emptyTrash'), danger: true, onClick: askEmpty, disabled: !entries.length }, '-', { label: tr('files.menu.view'), submenu: viewItems }];
    return [
      ...(wDir ? [{ label: tr('files.menu.newFolder'), hint: '❖⇧N', onClick: () => newFolder(null) }, { label: tr('files.menu.paste'), hint: 'Ctrl+V', onClick: doPaste, disabled: !clip }, '-'] : []),
      { label: tr('files.menu.view'), submenu: viewItems },
      { label: tr('files.menu.sortBy'), submenu: sortItems },
      { label: tr('files.menu.groupBy'), submenu: groupItems },
      '-',
      { label: tr('files.menu.showHidden'), hint: '❖⇧.', checked: hidden, onClick: () => setHidden((h) => !h) },
      ...(curDir ? [{ label: tr('files.menu.getInfo'), onClick: () => { setSelected(new Set()); setInfo(true); } }] : []),
    ];
  }
  const onContext = (e, ev) => {
    if (!selected.has(e.path)) setSelected(new Set([e.path]));
    const p = local(ev);
    setMenu({ x: p.x, y: p.y, items: itemMenu(e) });
  };
  const onBgContext = (ev) => { setSelected(new Set()); const p = local(ev); setMenu({ x: p.x, y: p.y, items: bgMenu() }); };

  // ---- keyboard ----
  const cols = () => { const w = root.current ? lord.getLayout(root.current.id)[2] - SIDEBAR_W : 600; return Math.max(1, Math.floor((w - 24 + 8) / 108)); };
  const moveSel = (d, ev) => {
    if (!flat.length) return;
    const i = first ? flat.findIndex((x) => x.path === (anchor.current && selected.has(anchor.current) ? anchor.current : first.path)) : -1;
    const j = Math.max(0, Math.min(flat.length - 1, i < 0 ? 0 : i + d));
    const p = flat[j].path;
    if (ev?.shift && first) setSelected(new Set([...selected, p])); else setSelected(new Set([p]));
    anchor.current = p;
  };
  useKeyboard((e) => {
    if (renaming || confirm || menu || netModal) return;
    const mod = e.meta || e.ctrl;
    const b = (e.base || e.char || '').toLowerCase();
    const space = e.char === ' ' || e.key === ' ' || b === ' ';
    if (quick) {
      if (e.key === 'Escape' || space) { setQuick(false); return; }
      if (e.key === 'ArrowRight' || e.key === 'ArrowDown') { moveSel(1); return; }
      if (e.key === 'ArrowLeft' || e.key === 'ArrowUp') { moveSel(-1); return; }
      return;
    }
    if (mod && e.key === 'Tab') { setActive((a) => (a + (e.shift ? tabs.length - 1 : 1)) % tabs.length); return; }
    if (mod && e.shift && b === 'n') { newFolder(null); return; }
    if (mod && b === 'n') { wm.open('files', { new: true, args: { path: curDir || fs.home() } }); return; }
    if (mod && b === 't') { newTabAt(); return; }
    if (mod && b === 'w') { closeTab(active); return; }
    if (mod && e.shift && (b === '.' || b === '>' || b === ':')) { setHidden((h) => !h); return; }
    if (mod && ['1', '2', '3', '4'].includes(b)) { setView(VIEWS[Number(b) - 1]); return; }
    if (mod && b === 'i') { setInfo((v) => !v); return; }
    if (mod && b === 'd') { duplicate(); return; }
    if (mod && b === 'c') { if (selPaths.length) copyFiles(selPaths); return; }
    if (mod && b === 'x') { if (selPaths.length && !readOnly) cutFiles(selPaths); return; }
    if (e.ctrl && b === 'v') { doPaste(); return; }
    if (mod && b === 'a') { setSelected(new Set(flat.map((x) => x.path))); return; }
    if (mod && b === '[') { goBack(); return; }
    if (mod && b === ']') { goForward(); return; }
    if (mod && e.key === 'ArrowUp') { goUp(); return; }
    if (mod && e.key === 'ArrowDown') { openSel(); return; }
    if (mod && (e.key === 'Backspace' || e.key === 'Delete')) { toTrash(); return; }
    if (e.key === 'Delete') { toTrash(); return; }
    if (e.key === 'Enter') { startRename(); return; }
    if (e.key === 'Escape') { setSelected(new Set()); setInfo(false); return; }
    if (space) { if (sel.length) setQuick(true); return; }
    const grid = tab.view === 'icons' ? cols() : 1;
    if (e.key === 'ArrowRight') { if (tab.view === 'list') { if (first?.dir) setExpanded((x) => new Set([...x, first.path])); } else moveSel(1, e); return; }
    if (e.key === 'ArrowLeft') { if (tab.view === 'list') { if (first?.dir) setExpanded((x) => { const n = new Set(x); n.delete(first.path); return n; }); } else moveSel(-1, e); return; }
    if (e.key === 'ArrowDown') { moveSel(grid, e); return; }
    if (e.key === 'ArrowUp') { moveSel(-grid, e); return; }
    if (e.key === 'Home') { if (flat[0]) setSelected(new Set([flat[0].path])); return; }
    if (e.key === 'End') { if (flat.length) setSelected(new Set([flat[flat.length - 1].path])); return; }
    // typing the start of a name selects it
    if (e.char && !mod && e.char.trim()) {
      const now = Date.now();
      const txt = (now - typed.current.at < 900 ? typed.current.text : '') + e.char.toLowerCase();
      typed.current = { text: txt, at: now };
      const hit = flat.find((x) => x.name.toLowerCase().startsWith(txt));
      if (hit) { setSelected(new Set([hit.path])); anchor.current = hit.path; }
    }
  });

  // ---- app menus (the theme's menu bar) ----
  useEffect(() => {
    wm.setMenus(wid, [
      { label: tr('files.menu.file'), items: [
        { label: tr('files.menu.newWindow'), hint: '❖N', onClick: () => wm.open('files', { new: true, args: { path: curDir || fs.home() } }) },
        { label: tr('files.menu.newTab'), hint: '❖T', onClick: () => newTabAt() },
        { label: tr('files.menu.newFolder'), hint: '❖⇧N', onClick: () => newFolder(null), disabled: !wDir }, '-',
        { label: tr('files.menu.open'), hint: '❖↓', onClick: () => openSel(), disabled: !sel.length },
        { label: tr('files.menu.getInfo'), hint: '❖I', onClick: () => setInfo(true) },
        { label: tr('files.menu.rename'), hint: '↵', onClick: () => startRename(), disabled: sel.length !== 1 || !wDir },
        { label: tr('files.menu.duplicate'), hint: '❖D', onClick: () => duplicate(), disabled: !sel.length || !wDir },
        { label: tr('files.menu.trash'), hint: '❖⌫', onClick: () => toTrash(), disabled: !sel.length || readOnly }, '-',
        { label: tr('files.menu.closeTab'), hint: '❖W', onClick: () => closeTab(active) },
      ] },
      { label: tr('files.menu.edit'), items: [
        { label: tr('files.menu.copy'), hint: '❖C', onClick: () => copyFiles(selPaths), disabled: !sel.length },
        { label: tr('files.menu.cut'), hint: '❖X', onClick: () => cutFiles(selPaths), disabled: !sel.length || readOnly },
        { label: tr('files.menu.paste'), hint: 'Ctrl+V', onClick: doPaste, disabled: !clip || !wDir },
        { label: tr('files.menu.selectAll'), hint: '❖A', onClick: () => setSelected(new Set(flat.map((x) => x.path))) },
      ] },
      { label: tr('files.menu.view'), items: [
        ...VIEWS.map((v, i) => ({ label: tr(`files.view.${v}`), hint: `❖${i + 1}`, checked: tab.view === v, onClick: () => setView(v) })), '-',
        { label: tr('files.menu.showHidden'), hint: '❖⇧.', checked: hidden, onClick: () => setHidden((h) => !h) },
      ] },
      { label: tr('files.menu.go'), items: [
        { label: tr('files.menu.back'), hint: '❖[', onClick: goBack, disabled: !tab.back.length },
        { label: tr('files.menu.forward'), hint: '❖]', onClick: goForward, disabled: !tab.fwd.length },
        { label: tr('files.menu.up'), hint: '❖↑', onClick: goUp, disabled: !curDir || curDir === fs.home() }, '-',
        { label: tr('files.place.recent'), onClick: () => navigate({ type: 'recent' }) },
        { label: tr('files.place.home'), onClick: () => navigate(homeLoc()) },
        { label: computerName(), onClick: () => navigate(computerLoc()) },
        ...dirs.map((d) => ({ label: tr(`files.place.${d.label}`), onClick: () => navigate({ type: 'dir', path: d.path }) })),
        ...(lan.available() ? [{ label: tr('files.place.network'), onClick: () => navigate({ type: 'network' }) }] : []),
        { label: tr('files.place.apps'), onClick: () => navigate({ type: 'apps' }) },
        { label: tr('files.place.trash'), onClick: () => navigate({ type: 'trash' }) },
      ] },
    ]);
  }, [tab.view, tab.back.length, tab.fwd.length, tab.loc, hidden, sel.length, clip, curDir, readOnly, active, tabs.length]);

  // ---- view ----
  const cut = new Set(clip?.mode === 'cut' ? clip.paths : []);
  const viewCtx = {
    selected, cut, renaming, dropTarget, hidden, expanded, showFolder: loc.type === 'search' || loc.type === 'recent' || loc.type === 'tag',
    emptyText: (error === 'archive-invalid' ? tr('docs.err.invalid') : error) || (loc.type === 'archive' && archiveState(loc.path)?.state === 'loading' ? tr('docs.loading') : null) || (loc.type === 'trash' ? tr('files.trashEmpty') : loc.type === 'search' ? tr('files.noResults') : tr('files.empty')),
    onItemDown, onOpen: open, onContext,
    onRenameChange: (v) => setRenaming((r) => (r ? { ...r, value: v } : r)), onRenameCommit: commitRename, onRenameCancel: () => setRenaming(null),
    onToggleExpand: (p) => setExpanded((x) => { const n = new Set(x); if (n.has(p)) n.delete(p); else n.add(p); return n; }),
    isDir: (p) => { const e = byPath.get(p); if (e) return e.dir; try { return lord.fs.stat(p).dir; } catch (err) { return false; } },
    entryAt: (p) => byPath.get(p) || (() => { try { const s = lord.fs.stat(p); return { name: s.name, path: s.path, dir: s.dir, size: s.size, mtime: s.mtime }; } catch (err) { return null; } })(),
  };
  // columns only in folders; elsewhere, the list
  const view = tab.view === 'columns' && loc.type !== 'dir' ? 'list' : tab.view;
  const entriesOf = (dir) => { try { return lord.fs.list(dir).filter((x) => hidden || !x.name.startsWith('.')).map((x) => ({ ...x, path: `${dir}/${x.name}` })); } catch (e) { return []; } };
  const freeSpace = useMemo(() => { try { return curDir ? fs.free(curDir) : null; } catch (e) { return null; } }, [curDir, version]);
  const quickEntry = quick ? (first || flat[0]) : null;
  const infoEntry = info ? (first || (curDir ? { name: fs.basename(curDir), path: curDir, dir: true, size: 0 } : null)) : null;
  const job = jobs[0];
  const crumbs = (() => {
    if (loc.type !== 'dir') return [{ label: title, loc }];
    const home = fs.home();
    const base = loc.path === home || loc.path.startsWith(`${home}/`) ? home
      : loc.path.startsWith('/run/media/') ? loc.path.split('/').slice(0, 5).join('/') : loc.path.startsWith('/media/') ? loc.path.split('/').slice(0, 4).join('/') : '/';
    const rest = loc.path.slice(base.length).split('/').filter(Boolean);
    const out = [{ label: base === home ? tr('files.place.home') : fs.basename(base), loc: { type: 'dir', path: base } }];
    let acc = base;
    for (const r of rest) { acc = `${acc}/${r}`; out.push({ label: r, loc: { type: 'dir', path: acc } }); }
    return out;
  })();
  const sideSel = (l) => sameLoc(l, baseLoc) && !debounced;
  const dirLoc = (p) => ({ type: 'dir', path: p });
  const dragLocal = drag && root.current ? (() => { const [lx, ly] = lord.getLayout(root.current.id); return { x: drag.x - lx, y: drag.y - ly }; })() : null;
  const dragEntry = drag ? byPath.get(drag.paths[0]) : null;

  // toolbar menus open under their button, aligned to its trailing edge
  const menuUnder = (ref, items) => {
    if (!ref.current || !root.current) return;
    const [bx, by, bw, bh] = lord.getLayout(ref.current.id);
    const [rx, ry] = lord.getLayout(root.current.id);
    const rtl = direction() === 'rtl';
    setMenu({ x: rtl ? bx - rx : bx - rx + bw - 220, y: by - ry + bh + 4, items });
  };
  const groupSortItems = () => [
    { header: tr('files.menu.groupBy') }, ...['none', 'kind', 'date', 'size'].map((k) => ({ label: tr(`files.groupBy.${k}`), checked: tab.group === k, onClick: () => patchTab({ group: k }) })), '-',
    { header: tr('files.menu.sortBy') }, ...['name', 'kind', 'date', 'size'].map((k) => ({ label: tr(`files.sort.${k}`), checked: tab.sort.by === k, onClick: () => setSort(k) })),
    { label: tab.sort.desc ? tr('files.sort.desc') : tr('files.sort.asc'), onClick: () => patchTab((x) => ({ sort: { ...x.sort, desc: !x.sort.desc } })) },
  ];
  const viewMenuItems = () => VIEWS.map((v, i) => ({ label: tr(`files.view.${v}`), hint: `❖${i + 1}`, checked: tab.view === v, onClick: () => setView(v) }));
  const tagItems = () => (sel.length ? [{ header: tr('files.menu.tags') }, tagSwatches(selPaths)] : [{ header: tr('files.menu.tags') }, { label: tr('files.noSelection'), disabled: true }]);
  const actionItems = () => (sel.length && first ? itemMenu(first) : bgMenu());
  // narrow windows: one view button with a menu, tags inside "⋯", a shorter search field
  const width = useWidth(root, 960);
  const content = width - SIDEBAR_W;
  const narrow = content < 600;
  const tight = content < 470;
  const viewBtn = useRef(null), sortBtn = useRef(null), tagBtn = useRef(null), moreBtn = useRef(null);
  const sideScroll = (
    <ScrollView style={{ flex: 1 }}>
      <View style={{ paddingHorizontal: 10, paddingBottom: 12 }}>
        <SideTitle t={t} first>{tr('files.favorites')}</SideTitle>
        <SideItem t={t} label={tr('files.place.recent')} icon={<Glyph ch="◷" t={t} />} selected={sideSel({ type: 'recent' })} onClick={() => navigate({ type: 'recent' })} />
        <SideItem t={t} label={tr('files.place.apps')} icon={<Glyph ch="⌘" t={t} />} selected={sideSel({ type: 'apps' })} onClick={() => navigate({ type: 'apps' })} />
        {dirs.map((d) => (
          <SideItem key={d.path} t={t} label={tr(`files.place.${d.label}`)} icon={<FolderGlyph size={16} />} selected={sideSel(dirLoc(d.path))}
            onClick={() => navigate(dirLoc(d.path))} drop={{ dropPath: d.path }} hot={dropTarget === d.path} />
        ))}
        <SideTitle t={t}>{tr('files.locations')}</SideTitle>
        <SideItem t={t} label={tr('files.place.home')} icon={<Glyph ch="⌂" t={t} />} selected={sideSel(homeLoc())} onClick={() => navigate(homeLoc())}
          drop={{ dropPath: fs.home() }} hot={dropTarget === fs.home()} />
        <SideItem t={t} label={computerName()} icon={<DriveGlyph color={t.accent} />} selected={sideSel(computerLoc())} onClick={() => navigate(computerLoc())} />
        {lan.available() && <SideItem t={t} label={tr('files.place.network')} icon={<NetGlyph color={t.accent} />} selected={sideSel({ type: 'network' })} onClick={() => navigate({ type: 'network' })} />}
        {volumes.map((v) => {
          const rel = volumeRelease(diskList, v.path, v.fs);
          return (
            <SideItem key={v.path} t={t} label={v.name} icon={<DriveGlyph color={t.accent} />} selected={sideSel(dirLoc(v.path))} onClick={() => navigate(dirLoc(v.path))}
              drop={{ dropPath: v.path }} hot={dropTarget === v.path} trail={rel ? { glyph: '⏏', onClick: () => release(rel, v.path) } : null} />
          );
        })}
        {unmountedDisks(diskList).map((p) => (
          <SideItem key={p.name} t={t} dim label={p.label || tr('files.disk.unnamed', { size: formatBytes(p.size) })} icon={<DriveGlyph color={t.textDim} />}
            onClick={() => mount(p)} />
        ))}
        <SideItem t={t} label={tr('files.place.trash')} icon={<TrashGlyph color={t.accent} />} selected={sideSel({ type: 'trash' })} onClick={() => navigate({ type: 'trash' })}
          drop={{ dropTrash: true }} hot={dropTarget === 'trash:'} />
        <SideTitle t={t}>{tr('files.tags')}</SideTitle>
        {Object.entries(TAG_COLORS).map(([color, hex]) => (
          <SideItem key={color} t={t} label={tr(`files.tag.${color}`)} icon={<Dot color={hex} />} selected={sideSel({ type: 'tag', color })} onClick={() => navigate({ type: 'tag', color })} />
        ))}
      </View>
    </ScrollView>
  );

  return (
    <View ref={root} style={{ flex: 1, flexDirection: 'row' }}>
      {/* sidebar (source list) */}
      <View style={{ width: SIDEBAR_W, backgroundColor: t.sidebarBg }}>
        <Hairline edge="end" />
        {sideScroll}
      </View>

      <View style={{ flex: 1 }}>
        {/* toolbar */}
        <Toolbar>
          <ToolbarGroup>
            <ToolbarButton icon={direction() === 'rtl' ? '›' : '‹'} iconSize={20} small title={tr('files.menu.back')} onClick={goBack} disabled={!tab.back.length} />
            <ToolbarButton icon={direction() === 'rtl' ? '‹' : '›'} iconSize={20} small title={tr('files.menu.forward')} onClick={goForward} disabled={!tab.fwd.length} />
          </ToolbarGroup>
          <View style={{ flex: 1, minWidth: 0 }}>
            <Text style={{ fontSize: typeScale.headline, fontWeight: 600, color: t.text, whiteSpace: 'nowrap', overflow: 'hidden', textAlign: 'start' }}>{tight ? '' : title}</Text>
          </View>
          {narrow ? (
            <View ref={viewBtn}><ToolbarButton icon={VIEW_GLYPH[tab.view]} title={tr('files.menu.view')} onClick={() => menuUnder(viewBtn, viewMenuItems())} /></View>
          ) : (
            <ToolbarGroup style={{ direction: 'ltr' }}>
              {VIEWS.map((v) => <ToolbarButton key={v} small icon={VIEW_GLYPH[v]} iconSize={14} title={tr(`files.view.${v}`)} active={tab.view === v} onClick={() => setView(v)} />)}
            </ToolbarGroup>
          )}
          <View ref={sortBtn}><ToolbarButton icon="⇅" title={tr('files.groupSort')} onClick={() => menuUnder(sortBtn, groupSortItems())} /></View>
          {!narrow && <View ref={tagBtn}><ToolbarButton icon="●" iconSize={11} color={t.textDim} title={tr('files.menu.tags')} disabled={!sel.length} onClick={() => menuUnder(tagBtn, tagItems())} /></View>}
          <View ref={moreBtn}><ToolbarButton icon="⋯" title={tr('files.actions')} onClick={() => menuUnder(moreBtn, actionItems())} /></View>
          <SearchField value={query} onChange={setQuery} placeholder={tr('files.search')} width={tight ? 120 : narrow ? 150 : 180} />
        </Toolbar>
        {query.trim() !== '' && (
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.xs, paddingHorizontal: spacing.m, height: 32 }}>
            <Hairline edge="bottom" />
            <Text style={{ fontSize: typeScale.callout, color: t.textDim, marginHorizontal: spacing.xs }}>{tr('files.searchIn')}</Text>
            {(baseLoc.type === 'dir' ? ['home', 'here'] : ['home']).map((sc) => (
              <ScopeChip key={sc} t={t} on={scope === sc || baseLoc.type !== 'dir'} onClick={() => setScope(sc)}
                label={sc === 'here' ? `“${locTitle(baseLoc, dirs)}”` : tr('files.place.home')} />
            ))}
          </View>
        )}
        {/* tabs */}
        {tabs.length > 1 && (
          <View style={{ flexDirection: 'row', height: 28, backgroundColor: t.mode === 'dark' ? 'rgba(0,0,0,0.18)' : 'rgba(0,0,0,0.045)' }}>
            <Hairline edge="bottom" />
            {tabs.map((x, i) => (
              <View key={x.id} onClick={() => { setActive(i); setSelected(new Set()); }} {...(x.loc.type === 'dir' ? { dropPath: x.loc.path } : {})}
                style={{ flex: 1, flexDirection: 'row', alignItems: 'center', paddingHorizontal: spacing.s, gap: 6,
                  backgroundColor: dropTarget && x.loc.path === dropTarget && i !== active ? t.accent : i === active ? t.windowBg : 'transparent' }}>
                {i > 0 && <Hairline edge="start" />}
                <View onClick={(ev) => { ev.stopPropagation(); closeTab(i); }} style={{ width: 16, height: 16, borderRadius: 4, alignItems: 'center', justifyContent: 'center' }}>
                  <Text style={{ fontSize: 9, color: t.textDim }}>✕</Text>
                </View>
                <Text style={{ flex: 1, fontSize: typeScale.callout, fontWeight: i === active ? 600 : 400, color: i === active ? t.text : t.textDim, textAlign: 'center', whiteSpace: 'nowrap', overflow: 'hidden' }}>{locTitle(x.loc, dirs)}</Text>
                <View style={{ width: 16 }} />
              </View>
            ))}
            <View onClick={() => newTabAt()} style={{ width: 32, alignItems: 'center', justifyContent: 'center' }}>
              <Hairline edge="start" />
              <Text style={{ fontSize: 15, color: t.textDim }}>+</Text>
            </View>
          </View>
        )}
        {/* content */}
        <View style={{ flex: 1 }} onMouseDown={() => { if (renaming) commitRename(); setSelected(new Set()); }} onContextMenu={onBgContext}
          {...(curDir ? { dropPath: curDir } : {})}>
          {loc.type === 'network' ? <NetworkView volumes={volumes} refreshVolumes={() => setVolumes(fs.volumes())} openFolder={(p) => navigate(dirLoc(p))} setMsg={setMsg} setModal={setNetModal} /> : <>
            {view === 'icons' && <IconView groups={groups} ctx={viewCtx} />}
            {view === 'list' && <ListView groups={groups} ctx={viewCtx} sort={sortBy} onSort={setSort} />}
            {view === 'columns' && <ColumnView root={curDir || fs.home()} chain={tab.chain} setChain={(chain) => patchTab({ chain })} entriesOf={entriesOf} ctx={viewCtx} sort={tab.sort} />}
            {view === 'gallery' && <GalleryView entries={flat} ctx={viewCtx} />}
          </>}
        </View>
        {/* path bar and status bar */}
        <View style={{ height: 26, flexDirection: 'row', alignItems: 'center', gap: spacing.xs, paddingHorizontal: spacing.m, overflow: 'hidden' }}>
          <Hairline edge="top" />
          {crumbs.map((c, i) => (
            <View key={i} style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.xs }}>
              {i > 0 && <Text style={{ fontSize: 11, color: t.textDim }}>{direction() === 'rtl' ? '‹' : '›'}</Text>}
              <View onClick={() => navigate(c.loc)} {...(c.loc.type === 'dir' ? { dropPath: c.loc.path } : {})}
                style={{ flexDirection: 'row', alignItems: 'center', gap: 5, height: 20, paddingHorizontal: 4, borderRadius: 4 }}>
                {c.loc.type === 'dir' ? <FolderGlyph size={13} /> : null}
                <Text style={{ fontSize: typeScale.caption, color: i === crumbs.length - 1 ? t.text : t.textDim, whiteSpace: 'nowrap' }}>{c.label}</Text>
              </View>
            </View>
          ))}
        </View>
        <View style={{ height: 24, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 10, paddingHorizontal: spacing.m,
          backgroundColor: t.mode === 'dark' ? 'rgba(0,0,0,0.12)' : 'rgba(0,0,0,0.025)' }}>
          <Hairline edge="top" />
          {job ? (
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.s }}>
              <Text style={{ fontSize: typeScale.caption, color: t.textDim, whiteSpace: 'nowrap' }}>{tr(job.move ? 'files.job.moving' : 'files.job.copying', { name: fs.basename(job.src) })}</Text>
              <View style={{ width: 120, height: 5, borderRadius: 3, backgroundColor: t.control, direction: 'ltr' }}>
                <View style={{ width: Math.round(120 * (job.total ? Math.min(1, job.bytes / job.total) : 0)), height: 5, borderRadius: 3, backgroundColor: t.accent }} />
              </View>
              <View onClick={() => fs.cancelJob(job.id)} style={{ width: 16, height: 16, alignItems: 'center', justifyContent: 'center' }}><Text style={{ fontSize: 10, color: t.textDim }}>✕</Text></View>
            </View>
          ) : (
            <Text style={{ fontSize: typeScale.caption, color: msg ? t.accent : t.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>
              {msg || (loc.type === 'network' ? '' : [sel.length ? tr('files.status.selected', { count: sel.length, total: flat.length }) : tr('files.items', { count: flat.length }),
                freeSpace ? tr('files.status.free', { free: formatBytes(freeSpace.free) }) : ''].filter(Boolean).join(', '))}
            </Text>
          )}
        </View>
      </View>

      {/* what is being dragged */}
      {dragLocal && (
        <View style={{ position: 'absolute', left: dragLocal.x - 20, top: dragLocal.y - 20, pointerEvents: 'none', opacity: 0.85 }}>
          {dragEntry?.dir ? <FolderGlyph size={40} /> : <DocGlyph size={40} kind="other" ext="" dark={t.mode === 'dark'} />}
          {drag.paths.length > 1 && (
            <View style={{ position: 'absolute', right: -6, top: -6, minWidth: 18, height: 18, borderRadius: 9, paddingHorizontal: 4, backgroundColor: '#ff3b30', alignItems: 'center', justifyContent: 'center' }}>
              <Text style={{ fontSize: 10, fontWeight: 700, color: '#fff' }}>{drag.paths.length}</Text>
            </View>
          )}
          {drag.alt && <Text style={{ position: 'absolute', left: 32, top: 26, fontSize: 15, fontWeight: 700, color: '#30d158' }}>+</Text>}
        </View>
      )}
      {quickEntry && <QuickLook e={quickEntry} index={flat.indexOf(quickEntry)} count={flat.length} onClose={() => setQuick(false)} />}
      {infoEntry && <InfoPanel e={infoEntry} onClose={() => setInfo(false)} openers={infoEntry.dir || infoEntry.app ? [] : openersFor(infoEntry, launcherApps, tr)} />}
      {confirm && <Confirm text={confirm.text} detail={confirm.detail} okLabel={confirm.ok} onCancel={() => setConfirm(null)} onOk={() => { const c = confirm; setConfirm(null); c.run(); }} />}
      {menu && <ContextMenu x={menu.x} y={menu.y} items={menu.items} onClose={() => setMenu(null)} bounds={bounds()} />}
    </View>
  );
}
