// Files — a file browser: tabs, four views (icons, list, columns and gallery), a sidebar
// with Favorites, Locations and Tags, the freedesktop Trash, Quick Preview, Get Info,
// copy/cut/paste, drag and drop, and search. Locations also has This computer (the whole tree,
// read-only outside the home folder and the user's disks) and every disk, mounted or not.
// Disk operations go through lord.fs (server/src/fsops.c); see docs/FILES.md.
import {
  needsDiskFormats, openDiskFormats, View, Text, useTheme, useAppWindow, useKeyboard, useStore, useLauncherApps, addListener, wm, fs, clipboard, archive, warmDocs,
  formatBytes, t as tr, ScrollView, Toolbar, ToolbarButton, ToolbarGroup, SearchField, Hairline, direction, spacing, typeScale, registerDockActions,
  useDisks, admin, systemInfo, lan, shortcutTaken, useSettings, cloudModel, cloud, errorText, useCloud, formatNumber, Button, openUrl, Layer, shellLayers,
  linuxAppFor, shortAppName,
} from '@lordos/sdk';
import { useEffect, useMemo, useRef, useState } from 'react';
import { FolderGlyph, DocGlyph, TAG_COLORS, TrashGlyph, DriveGlyph } from './glyphs.jsx';
import { IconView, ListView, ColumnView, GalleryView } from './views.jsx';
import { ContextMenu, QuickLook, InfoPanel, Confirm } from './panels.jsx';
import { loadEntries, sortEntries, groupEntries, sameLoc, userDirs, LIST_CAP } from './locations.js';
import {
  fsVersion, bump, fileClipboard, copyFiles, cutFiles, paste, pastePlan, canPaste, transfer, freeName, jobState, jobErrors, tagStore, toggleTag, moveTags,
  openersFor, openEntry,
} from './ops.js';
import { archiveVersion, archiveState, extractEntry, extractHere, compress, isArchiveFile } from './archive.js';
import { NetGlyph, isShareFs, shareTitle, rememberHostName, ConnectSheet, RemoteSheet, useLan, hostEntries, lanStatus, netStatus, hostName, placeEntry } from './network.jsx';
import { connectedEntries, netGroups } from './netplaces.js';
import { GoToFolder, ConnectServer, FilesSettings } from './dialogs.jsx';
import { parseAddress, sshLine, hostAddress, looksDav, davOf } from './connect.js';
import { filesPrefs, setPrefs, useFilesPrefs, columnsOf, noteFolder, noteServer, noteOrigin, originOf, togglePin, addFavoriteFolder, removeFavoriteFolder, viewFor, viewKey, folderViewsWith, clampSide } from './prefs.js';
import { ZOOM_MIN, ZOOM_MAX, ZOOM_VIEWS, zoomOf, zoomWith, iconColumns } from './viewprefs.js';
import { ALL_COLUMNS, toggleColumn } from './columns.js';
import { SideItem, SideTitle, PlaceGlyph, PathCrumbs, useWidth, ZoomControl } from '../../sdk/fileparts.jsx';
import { pathCrumbs } from '../../sdk/filepickermodel.js';
import { pasteAsk } from './clipfiles.js';
import { useCloudFiles, CloudSection, TransfersPill, footerLine, cloudMenuItems, ShareSheet, CloudInfo, OfflineDialog } from './cloud.jsx';
import { createRemoteDirs } from './remotedirs.js';

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

// A menu never shows a key the theme takes: Lord Classic has ❖N, ❖D, ❖I, ❖A and ❖1…4, where Files answers to Ctrl as well.
const ARROW_KEY = { '↑': 'ArrowUp', '↓': 'ArrowDown' };
function localKey(hint) {
  const m = /^❖([A-Z0-9,↑↓])$/.exec(hint || '');
  if (!m) return hint;
  const key = ARROW_KEY[m[1]] || m[1];
  if (!shortcutTaken(`Meta+${key}`)) return hint;
  return shortcutTaken(`Ctrl+${key}`) ? undefined : `Ctrl+${m[1]}`;
}
const localKeys = (items) => items.map((i) => (typeof i === 'string' ? i : { ...i, hint: localKey(i.hint), ...(i.items ? { items: localKeys(i.items) } : null), ...(Array.isArray(i.submenu) ? { submenu: localKeys(i.submenu) } : null) }));

const VIEWS = ['icons', 'list', 'columns', 'gallery'];
const VIEW_GLYPH = { icons: '▦', list: '≡', columns: '◫', gallery: '▣' };
// the toolbar's New Folder: a small folder with a plus on it
const NewFolderGlyph = () => (
  <View style={{ width: 18, height: 18, justifyContent: 'center' }}>
    <FolderGlyph size={18} />
    <Text style={{ position: 'absolute', left: 0, right: 0, top: 2, fontSize: 12, fontWeight: 700, color: '#ffffff', textAlign: 'center' }}>+</Text>
  </View>
);
let tabSeq = 1;
const newTab = (loc, from) => ({ id: tabSeq++, loc, back: [], fwd: [], view: from?.view || 'icons', sort: from?.sort || { by: 'name', desc: false }, zoom: from?.zoom || {}, group: from?.group || 'none', chain: [] });
// State of each window: it survives the interface being remounted (a language change).
const winStates = new Map();

const homeLoc = () => ({ type: 'dir', path: fs.home() });
const computerLoc = () => ({ type: 'dir', path: '/' });
// Settings → General: what a new window shows
function startLoc(k) {
  if (k === 'recent') return { type: 'recent' };
  if (k === 'computer') return computerLoc();
  const d = userDirs().find((x) => x.label === k);
  return d ? { type: 'dir', path: d.path } : homeLoc();
}
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
      if (!NO_MOUNT.includes(p.fstype || '') && !p.mountpoint) out.push({ name: p.name, label: p.label, size: p.size, removable: d.removable, fstype: p.fstype });
    }
  }
  return out;
}
// What ⏏ does for a mounted volume: a removable disk is ejected whole (disk-eject), another disk's
// partition is unmounted (disk-unmount); nothing on the disk the system runs from.
function volumeRelease(list, path, fstype) {
  if (fstype === 'fuse.rclone') return { remote: true, name: path };
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

const tagName = (color) => filesPrefs().tagNames[color]?.trim() || tr(`files.tag.${color}`);
function locTitle(loc, dirs) {
  switch (loc.type) {
    case 'recent': return tr('files.place.recent');
    case 'apps': return tr('files.place.apps');
    case 'trash': return tr('files.place.trash');
    case 'network': return tr('files.place.network');
    case 'tag': return filesPrefs().tagNames[loc.color]?.trim() || tr(`files.tag.${loc.color}`);
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

const Dot = ({ color }) => <View style={{ width: 10, height: 10, borderRadius: 5, backgroundColor: color }} />;
// Search scope chip ("Search: Home | “Folder”"), as in a search scope bar.
function ScopeChip({ label, on, onClick, t }) {
  return (
    <View onClick={onClick} style={{ paddingHorizontal: 10, height: 22, borderRadius: 6, justifyContent: 'center',
      backgroundColor: on ? (t.mode === 'dark' ? 'rgba(255,255,255,0.14)' : 'rgba(0,0,0,0.08)') : 'transparent' }}>
      <Text style={{ fontSize: typeScale.callout, fontWeight: on ? 600 : 400, color: on ? t.text : t.textDim, whiteSpace: 'nowrap' }}>{label}</Text>
    </View>
  );
}

// The status bar's zoom (issue #33) is the SDK's ZoomControl (sdk/fileparts.jsx), shared with the file dialog's
// thumbnails. Narrow windows keep only the glyphs.
const ZOOM_AT = { position: 'absolute', top: 0, bottom: 0, insetInlineEnd: spacing.m };

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
  const prefs = useFilesPrefs();
  const [tabs, setTabs] = useState(() => {
    if (saved?.tabs) return saved.tabs;
    const l = argLoc(args) || startLoc(prefs.newWindow), v = viewFor(prefs, l);
    return [{ ...newTab(l), ...v, sortChosen: !!viewKey(l) && !!prefs.folderViews[viewKey(l)], view: argView(args) || v.view }];
  });
  const [active, setActive] = useState(() => saved?.active || 0);
  const tab = tabs[Math.min(active, tabs.length - 1)];
  useEffect(() => { winStates.set(wid, { tabs, active }); }, [tabs, active]);
  useEffect(() => () => { if (!wm.get(wid)) winStates.delete(wid); }, []);
  // the sidebar's width, dragged from its edge and kept
  const [dragW, setDragW] = useState(null);
  const sideW = clampSide(dragW ?? prefs.sideWidth);
  const startSide = (e) => {
    const x0 = e.x, w0 = sideW;
    const off = [addListener('pointermove', (m) => setDragW(clampSide(w0 + (m.x - x0)))),
      addListener('pointerup', (m) => { off.forEach((f) => f()); setDragW(null); setPrefs({ sideWidth: clampSide(w0 + (m.x - x0)) }); })];
  };
  const patchTab = (p) => setTabs((ts) => ts.map((x, i) => (i === active ? { ...x, ...(typeof p === 'function' ? p(x) : p) } : x)));

  const [selected, setSelected] = useState(() => new Set());
  const anchor = useRef(null);
  const [renaming, setRenaming] = useState(null);         // { path, value }
  const [menu, setMenu] = useState(null);                 // { x, y, items }
  const [quick, setQuick] = useState(false);
  const [info, setInfo] = useState(false);
  const [confirm, setConfirm] = useState(null);
  const [query, setQuery] = useState('');
  const [scope, setScope] = useState(() => (prefs.searchScope === 'last' ? prefs.lastScope : prefs.searchScope));
  const [sheet, setSheet] = useState(null);               // 'goto' | 'connect' | 'settings'
  const [connAsk, setConnAsk] = useState(null);           // Connect to Server's sign-in
  const [remoteAsk, setRemoteAsk] = useState(null);       // the same for sftp, ftp and WebDAV
  const [debounced, setDebounced] = useState('');
  const [hidden, setHidden] = useState(false);
  const [expanded, setExpanded] = useState(() => new Set());
  const netExpanded = useRef(new Set());
  const [drag, setDrag] = useState(null);                 // { paths, x, y, alt }
  const [dropTarget, setDropTarget] = useState(null);
  const [volumes, setVolumes] = useState(() => fs.volumes());
  const [diskList] = useDisks();
  useEffect(() => { setTimeout(warmDocs, 800); }, []);             // PDF Quick Preview without waiting for the helper
  const [msg, setMsg] = useState(null);
  const [sharing, setSharing] = useState(null);           // Cloud: the Share Link sheet's entry
  const [offlineAsk, setOfflineAsk] = useState(null);     // Cloud: a file that isn't here, with the cloud out of reach
  const [userSettings] = useSettings();
  // folders on a Cloud account or a network share: listed, created, renamed and sent to the Trash off lord-server's
  // thread (fs.*Async, server/src/fsasync.c), so a provider that takes seconds never freezes the desktop (0.8.8.7)
  const [remoteVer, setRemoteVer] = useState(0);
  const rdirs = useMemo(() => createRemoteDirs({ list: (dir) => fs.listAsync(dir), onChange: () => setRemoteVer((n) => n + 1) }), []);
  // the names in a slow folder, as last listed (copies and new folders pick free names from them); undefined elsewhere
  const knownNames = (dir) => (fs.isRemote(dir) ? (rdirs.get(dir).entries || []).map((e) => e.name) : undefined);
  const root = useRef(null);
  const rootWidthRef = useRef(960);
  const press = useRef(null);
  const typed = useRef({ text: '', at: 0 });

  const dirs = useMemo(() => userDirs(), []);
  const baseLoc = tab.loc;
  const loc = debounced ? { type: 'search', root: scope === 'home' || baseLoc.type !== 'dir' ? fs.home() : baseLoc.path, query: debounced } : baseLoc;
  const [netState, searchLan] = useLan(baseLoc.type === 'network', (m) => setMsg(m));
  // a search inside a cloud account goes to cloud.search, off the interface's thread (docs/CLOUD.md 17.2 item 31)
  const cloudSt = useCloud();
  const searchAcc = loc.type === 'search' ? cloudModel.accountOf(loc.root, cloudSt && cloudSt.accounts) : null;
  const [cs, setCs] = useState(null);            // { key, busy, hits, complete, error }
  const csKey = useRef('');
  const searchKey = searchAcc && loc.query ? `${loc.root}|${loc.query}` : '';
  useEffect(() => {
    if (!searchKey) { csKey.current = ''; setCs(null); return; }
    if (csKey.current === searchKey) return;          // this search already ran or runs: only its own answer counts
    csKey.current = searchKey;
    setCs({ key: searchKey, busy: true, hits: [] });
    cloud.search(loc.root, loc.query).then((r) => {
      if (csKey.current === searchKey) setCs({ key: searchKey, busy: false, hits: (r && r.hits) || [], complete: !r || r.complete !== false, error: r && r.error });
    });
  }, [searchKey]);
  // a folder of a cloud account (docs/CLOUD.md 7.3): listed from rclone's listing, which the cloud side keeps current in
  // the background (cloud.browse), and listed again here when that one changed (the account's `listed`) or once a minute,
  // not on the 4-second volumes tick that local folders get: every listing of a mount is a call to rclone per file, and
  // one that falls on an expired folder waits for the provider
  const listAcc = loc.type === 'dir' ? cloudModel.accountOf(loc.path, cloudSt && cloudSt.accounts) : null;
  const [cloudTick, setCloudTick] = useState(0);
  useEffect(() => {
    if (!listAcc) return undefined;
    cloud.browse(loc.path);
    const id = setInterval(() => { cloud.browse(loc.path); setCloudTick((n) => n + 1); }, 60000);
    return () => clearInterval(id);
  }, [listAcc ? loc.path : null]);
  // a folder whose file system may be slow (fs.isRemote: rclone's Cloud mounts, Connect to Server, SMB/NFS): its listing
  // comes from rdirs, which asks lord-server's workers and shows the last listing meanwhile
  const remoteDir = loc.type === 'dir' && fs.isRemote(loc.path);
  const remoteSeen = useRef(null);
  useEffect(() => {
    if (!remoteDir) { remoteSeen.current = null; return; }
    const first = remoteSeen.current !== loc.path;
    remoteSeen.current = loc.path;
    // the first look at a folder lists it (rdirs.get); one seen before (going back) is listed again behind its last
    // listing, and so is the shown one when something changed
    if (!first || rdirs.has(loc.path)) rdirs.refresh(loc.path);
  }, [remoteDir, loc.path, version, listAcc ? listAcc.listed : 0, listAcc ? cloudTick : 0, listAcc ? null : volumes]);
  const [entries, error] = useMemo(() => {
    if (loc.type === 'network') {
      // the places this person is connected to first, then what was found (netplaces.js, issue #59)
      const mine = connectedEntries({ volumes, pins: prefs.pins, accounts: cloudSt && cloudSt.accounts, hiddenAccounts: userSettings.cloudHidden || [], home: fs.home() }).map(placeEntry);
      const found = hostEntries(netState, volumes, !!prefs.netAllDevices);
      const all = [...mine, ...found];
      all.foundCount = found.length;
      all.hiddenDevices = found.hiddenDevices;
      return [all, null];
    }
    if (searchAcc) return [cloudModel.searchEntries(cs && cs.hits), cs && cs.error ? errorText(cs.error) : null];
    if (remoteDir) {
      const r = rdirs.get(loc.path);
      const raw = r.entries || [];
      const out = raw.filter((e) => hidden || !e.name.startsWith('.')).map((e) => ({ ...e, path: `${loc.path === '/' ? '' : loc.path}/${e.name}` }));
      out.capped = raw.length === LIST_CAP;
      out.loading = !r.entries;
      out.busy = r.busy;
      return [out, r.error && !raw.length ? r.error : null];
    }
    try { return [loadEntries(loc, { hidden, launcherApps }), null]; } catch (e) { return [[], e.message]; }
  }, [netState, listAcc ? null : volumes, listAcc ? listAcc.listed : 0, listAcc ? cloudTick : 0, loc.type, loc.path, loc.color, loc.query, loc.root, loc.inner, hidden, version, tags, launcherApps, arcVer, cs, !!searchAcc, remoteDir, remoteVer,
    loc.type === 'network' ? prefs.pins : null, loc.type === 'network' ? !!prefs.netAllDevices : null, loc.type === 'network' ? cloudSt : null, loc.type === 'network' ? userSettings.cloudHidden : null]);
  // a slow folder still being listed: the status bar says so once it takes longer than a blink (no flicker on a fast one)
  const listBusy = !!entries.busy;
  const [waitShown, setWaitShown] = useState(false);
  useEffect(() => {
    if (!listBusy) { setWaitShown(false); return undefined; }
    const id = setTimeout(() => setWaitShown(true), 400);
    return () => clearTimeout(id);
  }, [listBusy, loc.path]);
  // Recents and the Trash go by date (newest first), unless another order is chosen
  const sortBy = (loc.type === 'recent' || loc.type === 'trash') && !tab.sortChosen ? { by: 'date', desc: true } : tab.sort;
  // what the list's extra columns need, asked once per file until something changes
  const stats = useRef({ v: -1, m: new Map() });
  const statOf = (e) => {
    if (stats.current.v !== version) stats.current = { v: version, m: new Map() };
    const m = stats.current.m;
    if (!m.has(e.path)) {
      let st = null;
      // a slow folder's listing already brings what the columns need (fs.listAsync): its files aren't asked one by one
      if (e.mode != null && e.user != null) st = e;
      else if (!e.trash && !e.app && !e.arc && !e.netHost && !e.netWay && !fs.isRemote(e.path)) { try { st = lord.fs.stat(e.path); } catch (err) { /* gone */ } }
      m.set(e.path, st);
    }
    return m.get(e.path);
  };
  const sortOpts = { statOf, foldersFirst: prefs.foldersFirst };
  const sorted = useMemo(() => sortEntries(entries, sortBy, sortOpts), [entries, sortBy.by, sortBy.desc, prefs.foldersFirst]);
  const groups = useMemo(() => (loc.type === 'network' ? netGroups(sorted, tr) : groupEntries(sorted, tab.group, tr)), [sorted, tab.group, loc.type]);
  const flat = useMemo(() => groups.flatMap((g) => g.items), [groups]);
  const byPath = useMemo(() => new Map(flat.map((e) => [e.path, e])), [flat]);
  // Cloud (docs/CLOUD.md): the statuses of what is shown, the account of this folder
  const cf = useCloudFiles(useMemo(() => flat.map((e) => e.path), [flat]));
  const curDir = baseLoc.type === 'dir' ? baseLoc.path : null;
  const curAcc = curDir ? cf.accountOf(curDir) : null;
  // outside the home folder and the user's disks a folder is only browsed (lord.fs reads there but
  // doesn't write): the actions that change it are left out
  // on a slow folder that is asked off the thread too, with the space left (rdirs' folders: fs.isRemote)
  const curRemote = !!curDir && fs.isRemote(curDir);
  const [remoteInfo, setRemoteInfo] = useState(null);     // { path, writable, free }
  useEffect(() => {
    if (!curRemote) return undefined;
    let live = true;
    const dir = curDir;
    const put = (p) => { if (live) setRemoteInfo((x) => ({ ...(x && x.path === dir ? x : { path: dir }), ...p })); };
    fs.statAsync(dir).then((s) => put({ writable: !!(s && s.writable) }), () => put({ writable: false }));
    // not in a cloud account: its status bar tells its own space, and statfs there waits for the provider
    if (!curAcc && !dir.includes('/.local/share/lordos/remote/')) fs.freeAsync(dir).then((f) => put({ free: f }), () => {});
    return () => { live = false; };
  }, [curRemote ? curDir : null, version, !!curAcc]);
  const readOnly = useMemo(() => {
    if (!curDir) return false;
    if (curRemote) return !!remoteInfo && remoteInfo.path === curDir && remoteInfo.writable === false;
    try { return !fs.stat(curDir).writable; } catch (e) { return true; }
  }, [curDir, version, curRemote, remoteInfo]);
  const wDir = readOnly ? null : curDir;
  const title = (() => { const v = loc.type === 'dir' ? volumes.find((x) => x.path === loc.path && isShareFs(x.fs)) : null; return v ? shareTitle(v.name) : locTitle(loc, dirs); })();

  const viewOf = (l) => { const k = viewKey(l); return { ...viewFor(filesPrefs(), l), sortChosen: !!k && !!filesPrefs().folderViews[k] }; };
  // what this place shows is kept for it, across windows and restarts (in settings.json, never as a file in the folder)
  const keepView = (view, sort, zoom = tab.zoom) => { const k = viewKey(tab.loc); if (k) setPrefs({ folderViews: folderViewsWith(filesPrefs().folderViews, k, { view, sort, zoom }) }); };
  function navigate(next, push = true) {
    if (sameLoc(next, tab.loc)) return;
    if (tab.loc.type === 'network') netExpanded.current = expanded;      // back in Network, what was unfolded stays
    setSelected(new Set()); setRenaming(null); setQuery(''); setDebounced(''); setExpanded(next.type === 'network' ? netExpanded.current : new Set());
    if (next.type === 'dir') noteFolder(next.path);
    patchTab((x) => ({ loc: next, back: push ? [...x.back, x.loc].slice(-50) : x.back, fwd: push ? [] : x.fwd, chain: [], ...viewOf(next) }));
  }
  const goBack = () => { if (!tab.back.length) return; const prev = tab.back[tab.back.length - 1]; setSelected(new Set()); patchTab((x) => ({ ...viewOf(prev), loc: prev, back: x.back.slice(0, -1), fwd: [x.loc, ...x.fwd], chain: [] })); };
  const goForward = () => { if (!tab.fwd.length) return; const nxt = tab.fwd[0]; setSelected(new Set()); patchTab((x) => ({ ...viewOf(nxt), loc: nxt, back: [...x.back, x.loc], fwd: x.fwd.slice(1), chain: [] })); };
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
    if (fs.isRemote(wDir)) {
      // a slow folder: the names it shows, and the folder made off the thread (the field opens when it exists)
      const dir = wDir;
      const name = freeName(dir, tr('files.newFolder'), new Set((rdirs.get(dir).entries || []).map((e) => e.name)));
      fs.mkdirAsync(`${dir}/${name}`).then(() => {
        if (withPaths && withPaths.length) transfer(withPaths, `${dir}/${name}`, true, new Set());
        rdirs.refresh(dir);
        bump();
        setSelected(new Set([`${dir}/${name}`]));
        setRenaming({ path: `${dir}/${name}`, value: name });
      }, report);
      return;
    }
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
    const go = () => {
      if (fs.isRemote(r.path)) {
        // on a slow folder the server renames it (rclone moves it at the provider): off the thread, shown when done
        fs.renameAsync(r.path, to).then(() => { moveTags(r.path, to); rdirs.settle([r.path]); bump(); setSelected(new Set([to])); }, report);
        return;
      }
      try { fs.rename(r.path, to); moveTags(r.path, to); bump(); setSelected(new Set([to])); } catch (e) { report(e); }
    };
    const ext = (n) => { const i = n.lastIndexOf('.'); return i > 0 ? n.slice(i + 1).toLowerCase() : ''; };
    const entry = byPath.get(r.path);
    if (prefs.warnExtension && entry && !entry.dir && ext(name) !== ext(entry.name)) {
      setConfirm({ text: tr('files.confirm.extension', { from: `.${ext(entry.name)}`, to: ext(name) ? `.${ext(name)}` : tr('files.confirm.noExtension') }), detail: tr('files.confirm.extensionDetail'), ok: tr('files.confirm.extensionOk'), run: go });
      return;
    }
    go();
  };
  // actions receive the paths to use (the context menu fixes them when it opens: the selection
  // at that moment, or the right-clicked item if it was not selected)
  const askDelete = (paths = selPaths) => setConfirm({ text: tr('files.confirm.delete', { count: paths.length }), detail: tr('files.confirm.cantUndo'), ok: tr('files.menu.deleteNow'), destructive: true,
    run: () => { for (const p of paths) { try { fs.removeFromTrash(p); } catch (err) { report(err); } } bump(); } });
  const askEmpty = () => {
    const run = () => { fs.emptyTrash(); bump(); };
    if (!prefs.warnEmptyTrash) { run(); return; }
    setConfirm({ text: tr('files.confirm.empty'), detail: tr('files.confirm.cantUndo'), ok: tr('files.menu.emptyTrash'), destructive: true, run });
  };
  // Settings → Advanced: what has been in the Trash over 30 days goes when Files starts
  useEffect(() => {
    if (!prefs.trash30) return;
    const limit = Date.now() - 30 * 86400000;
    let n = 0;
    for (const x of fs.trashList()) { const at = Date.parse(x.deleted); if (at && at < limit) { try { fs.removeFromTrash(x.id); n++; } catch (e) { /* stays */ } } }
    if (n) bump();
  }, []);
  // Open in Terminal: the Terminal app in a window of its own, at this folder
  const openTerminal = (path = curDir) => { if (path) wm.open('terminal', { new: true, args: { cwd: path } }); };
  // Connect to Server: returns { error, store } to show, or nothing when it went on
  // how: for an http(s) address, 'folder' (WebDAV, in Files) or 'web' (the browser); asked when the address doesn't say
  const connectTo = (text, how = null) => {
    let a = parseAddress(text);
    if (!a.error && a.kind === 'web' && how !== 'web') {
      if (how !== 'folder' && !looksDav(a.path)) return { choose: true };
      a = davOf(a);
    }
    if (a.error) return { error: tr(`files.connect.err.${a.error}`, { scheme: a.scheme || '' }) };
    const clientFor = (scheme) => launcherApps.find((x) => x.kind === 'linux' && (x.mimeTypes || []).includes(`x-scheme-handler/${scheme}`));
    if (a.kind === 'mount') {
      if (a.port) return { error: tr('files.connect.err.port') };
      const addr = hostAddress(lan.available() ? lan.poll().hosts : [], a.host);
      if (!addr) return { error: tr('files.connect.err.name', { name: a.host }) };
      noteServer(a.text);
      setSheet(null);
      setConnAsk({ kind: a.scheme, addr, host: a.host, user: a.user, share: a.scheme === 'smb' ? a.share : a.path });
      return null;
    }
    if (a.kind === 'folder') {
      noteServer(a.text);
      setSheet(null);
      setRemoteAsk({ kind: a.scheme === 'ssh' ? 'sftp' : a.scheme, host: a.host, port: a.port, user: a.user, path: a.path, ssh: a.scheme === 'ssh' ? sshLine(a) : null });
      return null;
    }
    if (a.kind === 'remote') {
      noteServer(a.text);
      setSheet(null);
      wm.open('remote-desk', { new: true, args: { proto: a.scheme, host: a.host, port: a.port, user: a.user } });
      return null;
    }
    const app = clientFor(a.scheme);
    if (!app) return { error: tr('files.connect.err.noClient', { scheme: a.scheme }) };
    noteServer(a.text);
    if (!lord.apps.launchUri(app.desktopId, a.text)) return { error: tr('files.connect.err.launch', { name: app.name }) };
    setSheet(null);
    return null;
  };
  const toTrash = (paths = selPaths) => {
    if (!paths.length || readOnly || loc.type === 'network') return;
    if (loc.type === 'trash') { askDelete(paths); return; }
    // Cloud (docs/CLOUD.md 17.2): what is in a cloud account is deleted there, after asking, and never moved into a
    // .Trash folder inside the cloud; it goes to the provider's own trash where it has one, so Restore isn't offered
    const split = cloudModel.splitTrash(paths, cf.accounts);
    let n = 0;
    // on a slow folder (a network share, Connect to Server) the move to its Trash runs off the thread: the items leave the
    // view at once and come back only if the move failed
    const slow = split.local.filter((p) => fs.isRemote(p));
    for (const p of split.local) { if (slow.includes(p)) continue; try { fs.trash(p); n++; } catch (e) { report(e); } }
    if (n) { setMsg(tr('files.trashed', { count: n })); bump(); }
    if (slow.length) {
      rdirs.hide(slow);
      setSelected(new Set());
      Promise.all(slow.map((p) => fs.trashAsync(p).then(() => 1, (e) => { report(e); return 0; }))).then((done) => {
        rdirs.settle(slow);
        const k = done.reduce((a, b) => a + b, 0);
        if (k) { setMsg(tr('files.trashed', { count: k })); bump(); }
      });
    }
    const ask = (i) => {
      const g = split.cloud[i];
      if (!g) return;
      const pr = cloudModel.deletePrompt(g.acc, g.paths.length);
      setConfirm({ text: tr(pr.title.key, pr.title.vars), detail: tr(pr.detail.key, pr.detail.vars), ok: tr('cloud.delete.ok'), destructive: true, run: () => {
        // the delete runs on the cloud's thread; the items leave the view now, and a new listing (off this thread too)
        // brings back any the provider refused
        rdirs.hide(g.paths);
        setSelected(new Set());
        cloud.delete(g.paths).then((r) => {
          rdirs.settle(g.paths);
          const failed = (r && r.failed) || [];
          setMsg(r && r.error ? errorText(r.error) : failed.length ? errorText(failed[0].error)
            : tr('cloud.delete.done', { count: g.paths.length, provider: cloudModel.providerName(g.acc) }));
          bump();
        });
        setTimeout(() => ask(i + 1), 0);
      } });
    };
    ask(0);
  };
  const trashRef = useRef(toTrash);
  trashRef.current = toTrash;
  const restore = (paths = selPaths) => { let n = 0; for (const p of paths) { try { fs.restore(p); n++; } catch (err) { report(err); } } if (n) { setMsg(tr('files.restored', { count: n })); bump(); } };
  // disks: the same closed actions as the Disks app (an internal disk asks for an administrator)
  const mount = (p) => {
    if (needsDiskFormats(p.fstype)) {
      setConfirm({ text: tr('files.disk.formatsTitle', { name: p.label || p.name }), detail: tr('files.disk.formatsDetail'), ok: tr('files.disk.formatsGet'), run: openDiskFormats });
      return;
    }
    setMsg(tr('files.disk.mounting', { name: p.label || p.name }));
    admin.run('disk-mount', p.name, null, { quiet: true }).then(
      (path) => { setVolumes(fs.volumes()); setMsg(null); const at = String(path || '').trim(); if (at.startsWith('/')) navigate(dirLoc(at)); },
      (e) => setMsg(e.message));
  };
  const release = (r, path) => {
    if (r.remote) { if (lan.disconnect(path)) { setVolumes(fs.volumes()); if (curDir && (curDir === path || curDir.startsWith(`${path}/`))) navigate(homeLoc()); } return; }
    admin.run(r.action, r.name, null, { quiet: true }).then(
      () => { setVolumes(fs.volumes()); setMsg(tr(r.action === 'disk-eject' ? 'disks.ejected' : 'disks.unmounted')); if (curDir && (curDir === path || curDir.startsWith(`${path}/`))) navigate(homeLoc()); },
      (e) => setMsg(e.message));
  };
  const duplicate = (paths = selPaths) => { if (wDir && paths.length) transfer(paths, wDir, false, knownNames(wDir)); };
  // Files another app copied are copied for real: a sheet first says how many, from which app and which ones
  // (Enter cancels), so a list put on the clipboard behind the person's back is never pasted unseen.
  const doPaste = () => {
    if (!wDir) return;
    const c = pastePlan();
    const ask = pasteAsk(c);
    if (!ask) { paste(wDir, knownNames(wDir), c); return; }
    const dir = wDir;
    const app = shortAppName(linuxAppFor(ask.from)) || ask.from || tr('files.confirm.pasteAnotherApp');
    const lines = [...ask.names, ...(ask.more ? [tr('files.confirm.pasteMore', { count: formatNumber(ask.more) })] : [])];
    setConfirm({ text: tr('files.confirm.pasteFrom', { count: ask.count, app }), detail: lines.join('\n'), ok: tr('files.menu.paste'), cautious: true,
      run: () => paste(dir, knownNames(dir), c) });
  };
  const ctx = { navigate, linuxApps: launcherApps, t: tr, quickLook: (e) => { setSelected(new Set([e.path])); setQuick(true); },
    openArc: (e) => { setMsg(tr('docs.opening', { name: e.name })); extractEntry(e).then((real) => openEntry({ name: e.name, path: real, size: e.size, mtime: e.mtime }, ctx)).catch((err) => setMsg(tr('docs.extractError', { name: e.name, error: err.message }))); } };
  const arcReport = (m) => setMsg(m);
  // a computer: its best way in; connected shares open as folders
  const openWay = (w, h) => {
    if (w.volume) { navigate(dirLoc(w.volume.path)); return; }
    if (w.mode === 'screen') { wm.open('remote-desk', { new: true, args: { proto: w.app, host: h.addr, port: w.port, user: '' } }); return; }
    if (w.mode === 'remote') { setRemoteAsk({ ...w.ask, ssh: w.ask.kind === 'sftp' ? `ssh ${h.addr}` : null }); return; }
    // a share open to guests, or an NFS export, mounts by itself; anything else asks who connects
    if (w.share && (w.kind === 'nfs' || ['anonymous', 'guest'].includes(h.session))) {
      setMsg(tr('files.net.connecting'));
      lan.mount({ kind: w.kind, addr: h.addr, share: w.share }).then((at) => { rememberHostName(h.addr, hostName(h)); setMsg(null); setVolumes(fs.volumes()); navigate(dirLoc(at)); },
        () => { setMsg(null); setConnAsk({ kind: w.kind, addr: h.addr, host: hostName(h), user: '', share: w.share }); });
      return;
    }
    setConnAsk({ kind: w.kind, addr: h.addr, host: hostName(h), user: '', share: w.share });
  };
  const open = (e) => {
    if (e.netWay) { openWay(e.netWay, e.netParent); return; }
    if (e.netPlace) { if (e.netPlace.kind === 'pin') reconnect(e.netPlace.pin); else navigate(dirLoc(e.netPlace.path)); return; }
    if (e.netHost) { if (e.ways[0]) openWay(e.ways[0], e.netHost); else setMsg(tr('files.net.nothing')); return; }
    if (e.trash) { setSelected(new Set([e.path])); setQuick(true); return; }
    if (!e.dir && cloudModel.cantOpen(cf.accountOf(e.path), cf.status[e.path])) { setOfflineAsk(e); return; }
    if (prefs.foldersInTabs && e.dir && !e.arc && !e.app) { newTabAt({ type: 'dir', path: e.path }); return; }
    openEntry(e, ctx);
  };
  const openSel = (list = sel) => { for (const e of list.slice(0, 8)) open(e); };
  const newTabAt = (l) => { setTabs((ts) => [...ts, newTab(l || tab.loc, tab)]); setActive(tabs.length); setSelected(new Set()); };
  const closeTab = (i) => {
    if (tabs.length === 1) { wm.close(wid); return; }
    setTabs((ts) => ts.filter((_, j) => j !== i));
    setActive((a) => (a >= i && a > 0 ? a - 1 : a));
  };
  const setView = (view) => { patchTab({ view }); keepView(view, tab.sort); };
  // zoom (issue #33): icon and text size of the icon and list views, a step per view, kept with the place
  const shownView = tab.view === 'columns' && loc.type !== 'dir' ? 'list' : tab.view;
  const zoomable = ZOOM_VIEWS.includes(shownView);
  const zoom = zoomOf(tab.zoom, shownView);
  const setZoom = (z) => {
    if (!zoomable) return;
    const next = Math.max(ZOOM_MIN, Math.min(ZOOM_MAX, Math.round(z)));
    if (next === zoom) return;
    const map = zoomWith(tab.zoom, shownView, next);
    patchTab({ zoom: map });
    keepView(tab.view, tab.sort, map);
  };
  const zoomStep = (d) => setZoom(zoom + d);
  // Ctrl + wheel (or two fingers on a touchpad with Ctrl) zooms instead of scrolling
  const onZoomWheel = (e) => {
    if (!e.ctrl) return;
    e.stopPropagation();
    if (!e.precise || e.notches) zoomStep(e.deltaY < 0 ? 1 : -1);
  };
  const setSort = (by) => {
    const sort = { by, desc: tab.sort.by === by ? !tab.sort.desc : by === 'date' || by === 'size' };
    patchTab({ sortChosen: true, sort });
    keepView(tab.view, sort);
  };

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
    if (!e.app && !e.netHost && !e.netWay && !e.netPlace && loc.type !== 'trash') press.current = { paths: next.has(e.path) ? [...next] : [e.path], x: ev.x, y: ev.y };
  };

  // a shortcut, not a copy: a folder on a server remembers how to get back to it
  const addFavorite = (path) => {
    const known = byPath.get(path);
    if (known) { if (!known.dir) return; } else { try { if (!fs.stat(path).dir) return; } catch (e) { return; } }
    const v = fs.volumes().find((x) => isShareFs(x.fs) && (path === x.path || path.startsWith(`${x.path}/`)));
    addFavoriteFolder({ path, name: v && path === v.path ? v.name : fs.basename(path), address: v ? originOf(v.path, v.fs, v.name) : '' });
  };
  const offline = (f) => !!f.address && !volumes.some((v) => f.path === v.path || f.path.startsWith(`${v.path}/`));
  const openFavorite = (f) => {
    if (offline(f)) { reconnect(f); return; }
    // on a Cloud account or a share the folder opens at once: its listing (off the thread) says if it is gone
    if (fs.isRemote(f.path)) { navigate(dirLoc(f.path)); return; }
    try { if (fs.stat(f.path).dir) { navigate(dirLoc(f.path)); return; } } catch (e) { /* below */ }
    setMsg(tr('files.fav.gone', { name: f.name }));
  };
  // ---- drag and drop (⌥ copies; otherwise it moves) ----
  const dropOf = (inst) => { for (let n = inst; n; n = n.parent) { if (n.props?.dropPath) return n.props.dropPath; if (n.props?.dropTrash) return 'trash:'; } return null; };
  // another app's window under the pointer that takes files (an onFilesDrop prop, e.g. the Terminal: their paths)
  const filesDropOf = (inst) => { for (let n = inst; n; n = n.parent) if (typeof n.props?.onFilesDrop === 'function') return n.props.onFilesDrop; return null; };
  // and its onFilesOver(paths, how), if it has one: told while the drag is over it, and with null once it leaves or drops
  // (the Terminal says where the files will go)
  const filesOverOf = (inst) => { for (let n = inst; n; n = n.parent) if (typeof n.props?.onFilesDrop === 'function') return typeof n.props.onFilesOver === 'function' ? n.props.onFilesOver : null; return null; };
  const overNow = useRef(null);
  const tellOver = (fn, paths, how) => {
    if (overNow.current && overNow.current !== fn) { try { overNow.current(null); } catch (e) { /* its app is gone */ } }
    overNow.current = fn;
    if (fn) { try { fn(paths, how); } catch (e) { overNow.current = null; } }
  };
  useEffect(() => {
    const mv = addListener('pointermove', (e) => {
      const p = press.current;
      if (!p) return;
      if (!p.moved && Math.hypot(e.x - p.x, e.y - p.y) < 6) return;
      p.moved = true;
      const target = dropOf(e.target);
      // over another app's window that takes files: the ghost says so (a +)
      setDrag({ paths: p.paths, x: e.x, y: e.y, alt: e.alt, favOnly: p.favOnly, takes: !target && !p.favOnly && !!filesDropOf(e.target) });
      tellOver(!target && !p.favOnly ? filesOverOf(e.target) : null, p.paths, { alt: !!e.alt });
      setDropTarget(target && !p.paths.includes(target) && (!p.favOnly || target === 'fav:') ? target : null);
    });
    const up = addListener('pointerup', (e) => {
      const p = press.current;
      press.current = null;
      if (!p || !p.moved) return;
      const target = dropOf(e.target);
      setDrag(null); setDropTarget(null);
      tellOver(null);
      const other = !target && !p.favOnly ? filesDropOf(e.target) : null;
      if (other) { try { other(p.paths, { alt: !!e.alt }); } catch (err) { console.warn('files drop:', err); } return; }
      if (target === 'fav:') { p.paths.forEach(addFavorite); return; }
      if (p.favOnly || !target || p.paths.includes(target)) return;      // a mounted volume is never moved
      if (target === 'trash:') { trashRef.current(p.paths); return; }       // (cloud paths are asked about and deleted there)
      transfer(p.paths, target, !e.alt, knownNames(target));
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
    if (e.netWay) return [{ label: tr('files.net.connect'), onClick: () => open(e) }];
    if (e.netPlace) return placeMenu(e.netPlace, e);
    if (e.netHost) {
      return [{ label: tr('files.net.connect'), onClick: () => open(e), disabled: !e.ways.length },
        ...(e.ways.length > 1 ? [{ label: tr('files.net.connectAs'), submenu: e.ways.map((w) => ({ label: w.label, onClick: () => openWay(w, e.netHost) })) }] : []), '-',
        { label: tr('files.net.copyAddress'), onClick: () => { clipboard.set(e.netHost.addr); setMsg(tr('files.net.copied', { text: e.netHost.addr })); } }];
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
      ...(e.dir ? [prefs.favoriteFolders.some((f) => f.path === e.path) ? { label: tr('files.fav.remove'), onClick: () => removeFavoriteFolder(e.path) } : { label: tr('files.fav.add'), onClick: () => addFavorite(e.path) }, { label: tr('files.menu.openTab'), onClick: () => newTabAt({ type: 'dir', path: e.path }) }, { label: tr('files.menu.openTerminal'), onClick: () => openTerminal(e.path) }] : []),
      ...(!e.dir && openers.length ? [{ label: tr('files.menu.openWith'), submenu: openers.map((o) => ({ label: o.label, onClick: o.run })) }] : []),
      ...(() => { const ci = cloudMenuItems(list, cf, { onShare: setSharing, onMsg: setMsg }); return ci.length ? ['-', ...ci] : []; })(),
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
    if (loc.type === 'network') {
      return [{ label: tr('files.net.search'), onClick: searchLan, disabled: netState.running }, { label: tr('files.menu.connect'), hint: '❖K', onClick: () => setSheet('connect') }, '-',
        { label: tr('files.net.allDevices'), checked: !!prefs.netAllDevices, onClick: () => setPrefs({ netAllDevices: !prefs.netAllDevices }) }, '-',
        { label: tr('files.menu.view'), submenu: viewItems }];
    }
    if (loc.type === 'trash') return [{ label: tr('files.menu.emptyTrash'), danger: true, onClick: askEmpty, disabled: !entries.length }, '-', { label: tr('files.menu.view'), submenu: viewItems }];
    return [
      ...(wDir ? [{ label: tr('files.menu.newFolder'), hint: '❖⇧N', onClick: () => newFolder(null) }, { label: tr('files.menu.paste'), hint: 'Ctrl+V', onClick: doPaste, disabled: !canPaste(clip) }, '-'] : []),
      { label: tr('files.menu.view'), submenu: viewItems },
      { label: tr('files.menu.sortBy'), submenu: sortItems },
      { label: tr('files.menu.groupBy'), submenu: groupItems },
      '-',
      { label: tr('files.menu.showHidden'), hint: '❖⇧.', checked: hidden, onClick: () => setHidden((h) => !h) },
      ...(curDir ? [{ label: tr('files.menu.getInfo'), onClick: () => { setSelected(new Set()); setInfo(true); } }, { label: tr('files.menu.openTerminal'), onClick: () => openTerminal() }] : []),
    ];
  }
  const onContext = (e, ev) => {
    if (!selected.has(e.path)) setSelected(new Set([e.path]));
    const p = local(ev);
    setMenu({ x: p.x, y: p.y, items: itemMenu(e) });
  };
  // a server in Locations: stays there, dim, when it isn't connected; a click connects again
  const pinMenu = (ev, v) => {
    ev.stopPropagation?.();
    const address = originOf(v.path, v.fs, v.name);
    if (!address) return;
    const pinned = prefs.pins.some((p) => p.path === v.path);
    const at = local(ev);
    setMenu({ x: at.x, y: at.y, items: [{ label: tr(pinned ? 'files.pin.remove' : 'files.pin.add'), onClick: () => togglePin({ address, path: v.path, name: v.name }) }] });
  };
  const reconnect = (p) => { const r = connectTo(p.address); if (r?.error) setMsg(r.error); };
  // a connected place on the Network page: open it, and what its sidebar entry offers
  function placeMenu(p, e) {
    if (p.kind === 'pin') return [{ label: tr('files.net.connect'), onClick: () => open(e) }, '-', { label: tr('files.pin.remove'), onClick: () => togglePin(p.pin) }];
    const items = [{ label: tr('files.menu.open'), onClick: () => open(e) }, { label: tr('files.menu.openTab'), onClick: () => newTabAt(dirLoc(p.path)) }];
    if (p.kind === 'cloud') return [...items, '-', { label: tr('cloud.settingsMenu'), onClick: () => wm.open('settings', { args: { section: 'cloud', page: p.id, nonce: Date.now() } }) }];
    const rel = volumeRelease(diskList, p.path, p.fs);
    const address = originOf(p.path, p.fs, p.name);
    const pinned = prefs.pins.some((x) => x.path === p.path);
    return [...items, '-',
      ...(address ? [{ label: tr(pinned ? 'files.pin.remove' : 'files.pin.add'), onClick: () => togglePin({ address, path: p.path, name: p.name }) }] : []),
      ...(rel ? [{ label: tr('files.net.disconnect'), onClick: () => release(rel, p.path) }] : [])];
  }
  const onBgContext = (ev) => { setSelected(new Set()); const p = local(ev); setMenu({ x: p.x, y: p.y, items: bgMenu() }); };

  // ---- keyboard ----
  const cols = () => iconColumns(root.current ? lord.getLayout(root.current.id)[2] - sideW : 600, zoom);
  const moveSel = (d, ev) => {
    if (!flat.length) return;
    const i = first ? flat.findIndex((x) => x.path === (anchor.current && selected.has(anchor.current) ? anchor.current : first.path)) : -1;
    const j = Math.max(0, Math.min(flat.length - 1, i < 0 ? 0 : i + d));
    const p = flat[j].path;
    if (ev?.shift && first) setSelected(new Set([...selected, p])); else setSelected(new Set([p]));
    anchor.current = p;
  };
  const goDir = (label) => { const d = dirs.find((x) => x.label === label); if (d) navigate({ type: 'dir', path: d.path }); };
  const GO_KEYS = { h: () => navigate(homeLoc()), d: () => goDir('desktop'), o: () => goDir('documents'), c: () => navigate(computerLoc()), a: () => navigate({ type: 'apps' }),
    f: () => navigate({ type: 'recent' }), k: () => { if (lan.available()) navigate({ type: 'network' }); }, g: () => setSheet('goto') };
  useKeyboard((e) => {
    if (renaming || confirm || menu || sheet || connAsk || remoteAsk || sharing || offlineAsk) return;
    const mod = e.meta || e.ctrl;
    const b = (e.base || e.char || '').toLowerCase();
    const space = e.char === ' ' || e.key === ' ' || b === ' ';
    if (quick) {
      if (e.key === 'Escape' || space) { setQuick(false); return; }
      if (e.key === 'ArrowRight' || e.key === 'ArrowDown') { moveSel(1); return; }
      if (e.key === 'ArrowLeft' || e.key === 'ArrowUp') { moveSel(-1); return; }
      return;
    }
    if (e.meta && e.ctrl) return;                 // ❖⌃K and the like belong to the shell
    if (mod && e.shift && GO_KEYS[b]) { GO_KEYS[b](); return; }
    if (mod && e.alt && b === 'l') { goDir('downloads'); return; }
    if (mod && !e.shift && b === 'k') { setSheet('connect'); return; }
    if (mod && !e.shift && b === ',') { setSheet('settings'); return; }
    if (mod && e.key === 'Tab') { setActive((a) => (a + (e.shift ? tabs.length - 1 : 1)) % tabs.length); return; }
    if (mod && e.shift && b === 'n') { newFolder(null); return; }
    if (mod && b === 'n') { wm.open('files', { new: true }); return; }
    if (mod && b === 't') { newTabAt(); return; }
    if (mod && b === 'w') { closeTab(active); return; }
    if (mod && e.shift && (b === '.' || b === '>' || b === ':')) { setHidden((h) => !h); return; }
    if (mod && ['1', '2', '3', '4'].includes(b)) { setView(VIEWS[Number(b) - 1]); return; }
    // Ctrl and + (= on most layouts, without Shift), -, 0: the zoom of the icon and list views
    if (mod && (b === '+' || b === '=' || e.char === '+' || e.key === '+')) { zoomStep(1); return; }
    if (mod && (b === '-' || b === '_' || e.key === '-')) { zoomStep(-1); return; }
    if (mod && b === '0') { setZoom(0); return; }
    if (mod && b === 'i') { setInfo((v) => !v); return; }
    if (mod && b === 'd') { duplicate(); return; }
    if (mod && b === 'c') { if (selPaths.length) copyFiles(selPaths); return; }
    if (mod && b === 'x') { if (selPaths.length && !readOnly) cutFiles(selPaths); return; }
    if (e.ctrl && b === 'v') { doPaste(); return; }
    if (mod && b === 'a') { setSelected(new Set(flat.map((x) => x.path))); return; }
    // [ and ] need a modifier on many layouts: the character itself counts, and ⌥← and ⌥→ work on all of them
    if ((mod && (b === '[' || e.key === '[')) || (e.alt && e.key === 'ArrowLeft')) { goBack(); return; }
    if ((mod && (b === ']' || e.key === ']')) || (e.alt && e.key === 'ArrowRight')) { goForward(); return; }
    if (mod && e.key === 'ArrowUp') { goUp(); return; }
    if (mod && e.key === 'ArrowDown') { openSel(); return; }
    if (mod && (e.key === 'Backspace' || e.key === 'Delete')) { toTrash(); return; }
    if (e.key === 'Delete') { toTrash(); return; }
    if (e.key === 'Enter') { if (first?.netHost || first?.netWay || first?.netPlace) open(first); else startRename(); return; }
    if (e.key === 'Escape') { setSelected(new Set()); setInfo(false); return; }
    if (space) { if (sel.length && !first?.netHost && !first?.netWay && !first?.netPlace) setQuick(true); return; }
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
    wm.setMenus(wid, localKeys([
      { label: tr('files.menu.file'), items: [
        { label: tr('files.menu.newWindow'), hint: '❖N', onClick: () => wm.open('files', { new: true }) },
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
        { label: tr('files.menu.paste'), hint: 'Ctrl+V', onClick: doPaste, disabled: !canPaste(clip) || !wDir },
        { label: tr('files.menu.selectAll'), hint: '❖A', onClick: () => setSelected(new Set(flat.map((x) => x.path))) },
      ] },
      { label: tr('files.menu.view'), items: [
        ...VIEWS.map((v, i) => ({ label: tr(`files.view.${v}`), hint: `❖${i + 1}`, checked: tab.view === v, onClick: () => setView(v) })), '-',
        { label: tr('files.zoom.in'), hint: 'Ctrl++', onClick: () => zoomStep(1), disabled: !zoomable || zoom >= ZOOM_MAX },
        { label: tr('files.zoom.out'), hint: 'Ctrl+-', onClick: () => zoomStep(-1), disabled: !zoomable || zoom <= ZOOM_MIN },
        { label: tr('files.zoom.reset'), hint: 'Ctrl+0', onClick: () => setZoom(0), disabled: !zoomable || zoom === 0 }, '-',
        { label: tr('files.menu.showHidden'), hint: '❖⇧.', checked: hidden, onClick: () => setHidden((h) => !h) },
      ] },
      { label: tr('files.menu.go'), items: [
        { label: tr('files.menu.back'), hint: '⌥←', onClick: goBack, disabled: !tab.back.length },
        { label: tr('files.menu.forward'), hint: '⌥→', onClick: goForward, disabled: !tab.fwd.length },
        { label: tr('files.menu.up'), hint: '❖↑', onClick: goUp, disabled: !curDir || curDir === fs.home() }, '-',
        { label: tr('files.place.recent'), hint: '❖⇧F', onClick: () => navigate({ type: 'recent' }) },
        ...dirs.filter((d) => ['documents', 'desktop', 'downloads'].includes(d.label)).map((d) => ({ label: tr(`files.place.${d.label}`), hint: { documents: '❖⇧O', desktop: '❖⇧D', downloads: '❖⌥L' }[d.label], onClick: () => navigate({ type: 'dir', path: d.path }) })),
        { label: tr('files.place.home'), hint: '❖⇧H', onClick: () => navigate(homeLoc()) },
        { label: computerName(), hint: '❖⇧C', onClick: () => navigate(computerLoc()) },
        ...(lan.available() ? [{ label: tr('files.place.network'), hint: '❖⇧K', onClick: () => navigate({ type: 'network' }) }] : []),
        { label: tr('files.place.apps'), hint: '❖⇧A', onClick: () => navigate({ type: 'apps' }) },
        { label: tr('files.place.trash'), onClick: () => navigate({ type: 'trash' }) }, '-',
        { label: tr('files.menu.recentFolders'), disabled: !prefs.recentFolders.length, submenu: prefs.recentFolders.map((p) => ({ label: fs.basename(p) || '/', onClick: () => navigate({ type: 'dir', path: p }) })) }, '-',
        { label: tr('files.menu.goToFolder'), hint: '❖⇧G', onClick: () => setSheet('goto') },
        ...(lan.available() ? [{ label: tr('files.menu.connect'), hint: '❖K', onClick: () => setSheet('connect') }] : []),
      ] },
      { label: tr('files.settings.title'), items: [{ label: tr('files.settings.open'), hint: '❖,', onClick: () => setSheet('settings') }] },
    ]));
  }, [tab.view, tab.back.length, tab.fwd.length, tab.loc, hidden, sel.length, clip, curDir, readOnly, active, tabs.length, prefs.recentFolders.length, zoom, zoomable]);

  // ---- view ----
  const columns = columnsOf(prefs);
  const onHeaderMenu = (ev) => {
    const p = local(ev);
    setMenu({ x: p.x, y: p.y, items: ALL_COLUMNS.map((k) => ({ label: tr(`files.col.${k}`), checked: columns.shown.includes(k), onClick: () => setPrefs({ columns: toggleColumn(columnsOf(filesPrefs()), k) }) })) });
  };
  const content = (rootWidthRef.current || 960) - sideW;
  const toggleExpanded = (p) => setExpanded((x) => { const n = new Set(x); if (n.has(p)) n.delete(p); else n.add(p); return n; });
  const cut = new Set(clip?.mode === 'cut' ? clip.paths : []);
  // a cloud account waiting for a new sign-in is not tried again by itself: its folder says so instead of "empty", with
  // the way to Sign In Again in Settings and, for a provider with a known problem in rclone, why and rclone's issue
  const waitAcc = curAcc && loc.type === 'dir' && curAcc.state === 'signin' ? curAcc : null;
  const known = waitAcc ? cloudModel.knownIssue(waitAcc.kind) : null;
  const viewCtx = {
    scrollKey: `${active}|${loc.type}|${loc.path || ''}|${loc.inner || ''}|${loc.color || ''}|${loc.root || ''}|${loc.query || ''}`,   // a new place opens at the top
    selected, cut, renaming, dropTarget, hidden, expanded, showFolder: loc.type === 'search' || loc.type === 'recent' || loc.type === 'tag',
    emptyText: waitAcc ? tr('cloud.files.signin', { name: waitAcc.name }) : (error === 'archive-invalid' ? tr('docs.err.invalid') : error) || (entries.loading ? tr('files.loading') : null) || (loc.type === 'archive' && archiveState(loc.path)?.state === 'loading' ? tr('docs.loading') : null) || (loc.type === 'network' && netState.running ? lanStatus(netState, 0) : null) || (loc.type === 'trash' ? tr('files.trashEmpty') : loc.type === 'search' ? tr('files.noResults') : tr('files.empty')),
    emptyHint: known ? tr(known.key) : null,
    emptyAction: waitAcc ? (
      <View style={{ flexDirection: 'row', gap: 8, flexWrap: 'wrap', justifyContent: 'center' }}>
        <Button primary label={tr('cloud.files.signinOpen')} onClick={() => wm.open('settings', { args: { section: 'cloud', page: waitAcc.id, nonce: Date.now() } })} />
        {known ? <Button label={tr('cloud.known.open')} onClick={() => openUrl(known.url)} /> : null}
      </View>
    ) : null,
    onItemDown, onOpen: open, onContext, statOf, zoom, onZoomWheel,
    cloud: { on: !!curAcc && loc.type === 'dir', status: cf.status }, cloudOnly: cf.cloudOnly,
    kidsOf: (e) => (e.netHost ? e.ways.map((w) => ({ name: w.label, path: `${e.path}/${w.id}`, dir: false, size: 0, mtime: 0, netWay: w, netParent: e.netHost, sub: w.sub || w.label })) : null), sortOpts, columns, contentW: content,
    onColumns: (c) => setPrefs({ columns: c }), onHeaderMenu,
    shownName: prefs.showExtensions ? null : (e) => { if (e.dir || e.app) return e.name; const i = e.name.lastIndexOf('.'); return i > 0 ? e.name.slice(0, i) : e.name; },
    onRenameChange: (v) => setRenaming((r) => (r ? { ...r, value: v } : r)), onRenameCommit: commitRename, onRenameCancel: () => setRenaming(null),
    onToggleExpand: (p) => {
      // a computer that shows its folders only after signing in: the chevron asks first
      const e = p.startsWith('net:') ? byPath.get(p) : null;
      if (e?.netHost?.smb && !e.netHost.shares.length && !expanded.has(p)) { openWay(e.ways[0], e.netHost); return; }
      toggleExpanded(p);
    },
    listOf: (dir) => listOf(dir),
    isDir: (p) => { const e = byPath.get(p); if (e) return e.dir; try { return lord.fs.stat(p).dir; } catch (err) { return false; } },
    entryAt: (p) => byPath.get(p) || (() => { try { const s = lord.fs.stat(p); return { name: s.name, path: s.path, dir: s.dir, size: s.size, mtime: s.mtime }; } catch (err) { return null; } })(),
  };
  // columns only in folders; elsewhere, the list
  const view = shownView;
  const entriesOf = (dir) => { try { return listOf(dir).filter((x) => hidden || !x.name.startsWith('.')).map((x) => ({ ...x, path: `${dir}/${x.name}` })); } catch (e) { return []; } };
  // not in a cloud account: its status bar tells its own space, and statfs there waits for the provider (rclone asks it)
  const freeSpace = useMemo(() => {
    if (curRemote) return remoteInfo && remoteInfo.path === curDir ? remoteInfo.free || null : null;
    try { return curDir && !curAcc && !curDir.includes('/.local/share/lordos/remote/') ? fs.free(curDir) : null; } catch (e) { return null; }
  }, [curDir, version, !!curAcc, curRemote, remoteInfo]);
  // what a folder in the column view or an unfolded one in the list shows: a slow one from rdirs, never on this thread
  const listOf = (dir) => (fs.isRemote(dir) ? rdirs.get(dir).entries || [] : lord.fs.list(dir));
  const quickEntry = quick ? (first || flat[0]) : null;
  const infoEntry = info ? (first || (curDir ? { name: fs.basename(curDir), path: curDir, dir: true, size: 0 } : null)) : null;
  const job = jobs[0];
  const crumbs = (() => {
    if (loc.type !== 'dir') return [{ label: title, loc }];
    return pathCrumbs(loc.path, fs.home()).map((c) => ({ label: c.home ? tr('files.place.home') : c.label, loc: { type: 'dir', path: c.path } }));
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
  const visibleSort = () => ALL_COLUMNS.filter((k) => columns.shown.includes(k));
  const sortLabel = (k) => tr(['name', 'kind', 'date', 'size'].includes(k) ? `files.sort.${k}` : `files.col.${k}`);
  const groupSortItems = () => [
    { header: tr('files.menu.groupBy') }, ...['none', 'kind', 'date', 'size'].map((k) => ({ label: tr(`files.groupBy.${k}`), checked: tab.group === k, onClick: () => patchTab({ group: k }) })), '-',
    { header: tr('files.menu.sortBy') }, ...['name', ...visibleSort()].map((k) => ({ label: sortLabel(k), checked: tab.sort.by === k, onClick: () => setSort(k) })),
    { label: tab.sort.desc ? tr('files.sort.desc') : tr('files.sort.asc'), onClick: () => { const sort = { ...tab.sort, desc: !tab.sort.desc }; patchTab({ sort }); keepView(tab.view, sort); } },
  ];
  const viewMenuItems = () => VIEWS.map((v, i) => ({ label: tr(`files.view.${v}`), hint: `❖${i + 1}`, checked: tab.view === v, onClick: () => setView(v) }));
  const tagItems = () => (sel.length ? [{ header: tr('files.menu.tags') }, tagSwatches(selPaths)] : [{ header: tr('files.menu.tags') }, { label: tr('files.noSelection'), disabled: true }]);
  const actionItems = () => (sel.length && first ? itemMenu(first) : bgMenu());
  // narrow windows: one view button with a menu, tags inside "⋯", a shorter search field
  const width = useWidth(root, 960);
  rootWidthRef.current = width;
  const narrow = content < 600;
  const tight = content < 470;
  const viewBtn = useRef(null), sortBtn = useRef(null), tagBtn = useRef(null), moreBtn = useRef(null);
  const side = (id) => !prefs.hiddenSide.includes(id);
  const sideScroll = (
    <ScrollView style={{ flex: 1 }}>
      <View style={{ paddingHorizontal: 10, paddingBottom: 12 }}>
        <SideTitle t={t} first>{tr('files.favorites')}</SideTitle>
        {side('recent') && <SideItem t={t} label={tr('files.place.recent')} icon={<PlaceGlyph ch="◷" t={t} />} selected={sideSel({ type: 'recent' })} onClick={() => navigate({ type: 'recent' })} />}
        {side('apps') && <SideItem t={t} label={tr('files.place.apps')} icon={<PlaceGlyph ch="⌘" t={t} />} selected={sideSel({ type: 'apps' })} onClick={() => navigate({ type: 'apps' })} />}
        {dirs.filter((d) => side(d.label)).map((d) => (
          <SideItem key={d.path} t={t} label={tr(`files.place.${d.label}`)} icon={<FolderGlyph size={16} />} selected={sideSel(dirLoc(d.path))}
            onClick={() => navigate(dirLoc(d.path))} drop={{ dropPath: d.path }} hot={dropTarget === d.path} />
        ))}
        {prefs.favoriteFolders.map((f) => (
          <SideItem key={f.path} t={t} dim={offline(f)} label={shareTitle(f.name)} icon={<FolderGlyph size={16} />} selected={sideSel(dirLoc(f.path))} onClick={() => openFavorite(f)}
            drop={offline(f) ? undefined : { dropPath: f.path }} hot={dropTarget === f.path}
            onContext={(ev) => { ev.stopPropagation?.(); const at = local(ev); setMenu({ x: at.x, y: at.y, items: [{ label: tr('files.fav.remove'), onClick: () => removeFavoriteFolder(f.path) }] }); }} />
        ))}
        {drag && (drag.favOnly || dragEntry?.dir) ? <SideItem t={t} dim label={tr('files.fav.add')} icon={<PlaceGlyph ch="+" t={t} />} drop={{ dropPath: 'fav:' }} hot={dropTarget === 'fav:'} /> : null}
        <CloudSection t={t} accounts={cf.accounts} hidden={userSettings.cloudHidden || []} sideSel={(p) => sideSel(dirLoc(p))} navigate={(p) => navigate(dirLoc(p))} dropTarget={dropTarget}
          onContext={(ev, it) => { ev.stopPropagation?.(); const at = local(ev); setMenu({ x: at.x, y: at.y, items: [
            { label: tr('files.menu.openTab'), onClick: () => newTabAt(dirLoc(it.path)) },
            { label: tr('cloud.settingsMenu'), onClick: () => wm.open('settings', { args: { section: 'cloud', page: it.id, nonce: Date.now() } }) }] }); }} />
        <SideTitle t={t}>{tr('files.locations')}</SideTitle>
        {side('home') && <SideItem t={t} label={tr('files.place.home')} icon={<PlaceGlyph ch="⌂" t={t} />} selected={sideSel(homeLoc())} onClick={() => navigate(homeLoc())}
          drop={{ dropPath: fs.home() }} hot={dropTarget === fs.home()} />}
        {side('computer') && <SideItem t={t} label={computerName()} icon={<DriveGlyph color={t.accent} />} selected={sideSel(computerLoc())} onClick={() => navigate(computerLoc())} />}
        {lan.available() && side('network') && <SideItem t={t} label={tr('files.place.network')} icon={<NetGlyph color={t.accent} />} selected={sideSel({ type: 'network' })} onClick={() => navigate({ type: 'network' })} />}
        {side('volumes') && volumes.filter((v) => !cloudModel.isCloudVolume(v, fs.home())).map((v) => {
          const rel = volumeRelease(diskList, v.path, v.fs);
          return (
            <SideItem key={v.path} t={t} label={isShareFs(v.fs) ? shareTitle(v.name) : v.name} icon={<DriveGlyph color={t.accent} />} selected={sideSel(dirLoc(v.path))} onClick={() => navigate(dirLoc(v.path))}
              drop={{ dropPath: v.path }} hot={dropTarget === v.path} trail={rel ? { glyph: '⏏', onClick: () => release(rel, v.path) } : null}
              onContext={isShareFs(v.fs) ? (ev) => pinMenu(ev, v) : undefined}
              onPress={(ev) => { press.current = { paths: [v.path], x: ev.x, y: ev.y, favOnly: true }; }} />
          );
        })}
        {side('volumes') && prefs.pins.filter((p) => !volumes.some((v) => v.path === p.path)).map((p) => (
          <SideItem key={p.path} t={t} dim label={shareTitle(p.name)} icon={<DriveGlyph color={t.textDim} />} onClick={() => reconnect(p)}
            onContext={(ev) => { ev.stopPropagation?.(); const at = local(ev); setMenu({ x: at.x, y: at.y, items: [{ label: tr('files.pin.remove'), onClick: () => togglePin(p) }] }); }} />
        ))}
        {side('volumes') && unmountedDisks(diskList).map((p) => (
          <SideItem key={p.name} t={t} dim label={p.label || tr('files.disk.unnamed', { size: formatBytes(p.size) })} icon={<DriveGlyph color={t.textDim} />}
            onClick={() => mount(p)} />
        ))}
        {side('trash') && <SideItem t={t} label={tr('files.place.trash')} icon={<TrashGlyph color={t.accent} />} selected={sideSel({ type: 'trash' })} onClick={() => navigate({ type: 'trash' })}
          drop={{ dropTrash: true }} hot={dropTarget === 'trash:'} />}
        {side('tags') && <SideTitle t={t}>{tr('files.tags')}</SideTitle>}
        {side('tags') && Object.entries(TAG_COLORS).filter(([color]) => !prefs.hiddenTags.includes(color)).map(([color, hex]) => (
          <SideItem key={color} t={t} label={tagName(color)} icon={<Dot color={hex} />} selected={sideSel({ type: 'tag', color })} onClick={() => navigate({ type: 'tag', color })} />
        ))}
      </View>
    </ScrollView>
  );

  return (
    <View ref={root} style={{ flex: 1, flexDirection: 'row' }}>
      {/* sidebar (source list) */}
      <View style={{ width: sideW, backgroundColor: t.sidebarBg }}>
        <Hairline edge="end" />
        {sideScroll}
        <View onMouseDown={startSide} style={{ position: 'absolute', top: 0, bottom: 0, ...(direction() === 'rtl' ? { left: 0 } : { right: 0 }), width: 6, cursor: 'ew-resize' }} />
      </View>

      <View style={{ flex: 1 }}>
        {/* toolbar */}
        <Toolbar>
          <ToolbarGroup>
            <ToolbarButton icon={direction() === 'rtl' ? '›' : '‹'} iconSize={20} small title={tr('files.menu.back')} onClick={goBack} disabled={!tab.back.length} />
            <ToolbarButton icon={direction() === 'rtl' ? '‹' : '›'} iconSize={20} small title={tr('files.menu.forward')} onClick={goForward} disabled={!tab.fwd.length} />
          </ToolbarGroup>
          <ToolbarButton icon={<NewFolderGlyph />} title={tr('files.menu.newFolder')} onClick={() => newFolder(null)} disabled={!wDir} />
          {/* the folder's name is in the window's title bar, every theme draws it there */}
          <View style={{ flex: 1, minWidth: 0 }} />
          {curAcc && !tight ? <TransfersPill t={t} accounts={cf.accounts} acc={curAcc} /> : null}
          {narrow ? (
            <View ref={viewBtn}><ToolbarButton icon={VIEW_GLYPH[tab.view]} title={tr('files.menu.view')} onClick={() => menuUnder(viewBtn, viewMenuItems())} /></View>
          ) : (
            <ToolbarGroup style={{ direction: 'ltr' }}>
              {VIEWS.map((v) => <ToolbarButton key={v} small icon={VIEW_GLYPH[v]} iconSize={14} title={tr(`files.view.${v}`)} active={tab.view === v} onClick={() => setView(v)} />)}
            </ToolbarGroup>
          )}
          <View ref={sortBtn}><ToolbarButton icon="⇅" title={tr('files.groupSort')} onClick={() => menuUnder(sortBtn, groupSortItems())} /></View>
          {!narrow && <View ref={tagBtn}><ToolbarButton icon="●" iconSize={11} color={t.textDim} title={tr('files.menu.tags')} disabled={!sel.length} onClick={() => menuUnder(tagBtn, tagItems())} /></View>}
          {loc.type === 'network' && <ToolbarButton icon="↻" iconSize={13} title={tr('files.net.search')} disabled={netState.running} onClick={searchLan} />}
          {!tight && <ToolbarButton icon=">_" iconSize={12} title={tr('files.menu.openTerminal')} disabled={!curDir} onClick={() => openTerminal()} />}
          <View ref={moreBtn}><ToolbarButton icon="⋯" title={tr('files.actions')} onClick={() => menuUnder(moreBtn, actionItems())} /></View>
          <SearchField value={query} onChange={setQuery} placeholder={tr('files.search')} width={tight ? 120 : narrow ? 150 : 180} />
        </Toolbar>
        {query.trim() !== '' && (
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.xs, paddingHorizontal: spacing.m, height: 32 }}>
            <Hairline edge="bottom" />
            <Text style={{ fontSize: typeScale.callout, color: t.textDim, marginHorizontal: spacing.xs }}>{tr('files.searchIn')}</Text>
            {(baseLoc.type === 'dir' ? ['home', 'here'] : ['home']).map((sc) => (
              <ScopeChip key={sc} t={t} on={scope === sc || baseLoc.type !== 'dir'} onClick={() => { setScope(sc); setPrefs({ lastScope: sc }); }}
                label={sc === 'here' ? `“${locTitle(baseLoc, dirs)}”` : tr('files.place.home')} />
            ))}
            {searchAcc && cs && (cs.busy || !cs.complete) ? (
              <Text key={cs.busy ? 'busy' : 'partial'} style={{ flex: 1, fontSize: typeScale.callout, color: t.textDim, textAlign: 'end', whiteSpace: 'nowrap', overflow: 'hidden' }}>
                {cs.busy ? tr('cloud.search.running', { name: searchAcc.name }) : tr('cloud.search.partial', { count: cs.hits.length })}</Text>
            ) : null}
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
            {view === 'icons' && <IconView groups={groups} ctx={viewCtx} />}
            {view === 'list' && <ListView groups={groups} ctx={viewCtx} sort={sortBy} onSort={setSort} />}
            {view === 'columns' && <ColumnView root={curDir || fs.home()} chain={tab.chain} setChain={(chain) => patchTab({ chain })} entriesOf={entriesOf} ctx={viewCtx} sort={tab.sort} />}
            {view === 'gallery' && <GalleryView entries={flat} ctx={viewCtx} />}
        </View>
        {/* path bar and status bar */}
        <View style={{ height: 26, flexDirection: 'row', alignItems: 'center', gap: spacing.xs, paddingHorizontal: spacing.m, overflow: 'hidden' }}>
          <Hairline edge="top" />
          <PathCrumbs t={t} crumbs={crumbs.map((c) => ({ label: c.label, folder: c.loc.type === 'dir', drop: c.loc.type === 'dir' ? { dropPath: c.loc.path } : null }))} onGo={(i) => navigate(crumbs[i].loc)} />
        </View>
        <View style={{ height: 24, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 10, paddingHorizontal: spacing.m,
          backgroundColor: t.mode === 'dark' ? 'rgba(0,0,0,0.12)' : 'rgba(0,0,0,0.025)' }}>
          <Hairline edge="top" />
          {zoomable && (
            <ZoomControl t={t} zoom={zoom} slider={!narrow} onStep={zoomStep} onSet={setZoom} style={ZOOM_AT} />
          )}
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
              {msg || (loc.type === 'network' ? netStatus(netState, entries.foundCount || 0, entries.hiddenDevices || 0) : [sel.length ? tr('files.status.selected', { count: sel.length, total: flat.length }) : tr('files.items', { count: flat.length }),
                waitShown ? tr('files.status.waiting') : '',
                curAcc && entries.capped ? tr('cloud.listCapped', { count: formatNumber(LIST_CAP) }) : '',
                curAcc ? footerLine(curAcc) : freeSpace ? tr('files.status.free', { free: formatBytes(freeSpace.free) }) : ''].filter(Boolean).join(curAcc ? ' · ' : ', '))}
            </Text>
          )}
        </View>
      </View>

      {/* what is being dragged: in the desktop's drag layer, above every window, so it follows the pointer into another
          app (a window that takes files shows a +); in its own window only when there are no desktop layers */}
      {drag && (shellLayers() || dragLocal) && (() => {
        const ghost = (
        <View style={{ position: 'absolute', left: (shellLayers() ? drag.x : dragLocal.x) - 20, top: (shellLayers() ? drag.y : dragLocal.y) - 20, pointerEvents: 'none', opacity: 0.85 }}>
          {drag.favOnly || dragEntry?.dir ? <FolderGlyph size={40} /> : <DocGlyph size={40} kind="other" ext="" dark={t.mode === 'dark'} />}
          {drag.paths.length > 1 && (
            <View style={{ position: 'absolute', right: -6, top: -6, minWidth: 18, height: 18, borderRadius: 9, paddingHorizontal: 4, backgroundColor: '#ff3b30', alignItems: 'center', justifyContent: 'center' }}>
              <Text style={{ fontSize: 10, fontWeight: 700, color: '#fff' }}>{drag.paths.length}</Text>
            </View>
          )}
          {(drag.alt || drag.takes) && (
            <View style={{ position: 'absolute', left: 28, top: 26, width: 18, height: 18, borderRadius: 9, backgroundColor: '#30d158', alignItems: 'center', justifyContent: 'center' }}>
              <Text style={{ fontSize: 13, fontWeight: 700, color: '#fff', lineHeight: 13 }}>+</Text>
            </View>
          )}
        </View>
        );
        return shellLayers() ? <Layer name="drag">{ghost}</Layer> : ghost;
      })()}
      {quickEntry && <QuickLook e={quickEntry} index={flat.indexOf(quickEntry)} count={flat.length} onClose={() => setQuick(false)} />}
      {infoEntry && <InfoPanel e={infoEntry} onClose={() => setInfo(false)} openers={infoEntry.dir || infoEntry.app ? [] : openersFor(infoEntry, launcherApps, tr)}
        extra={cf.accountOf(infoEntry.path) ? <CloudInfo e={infoEntry} cf={cf} onShare={(x) => { setInfo(false); setSharing(x); }} /> : null} />}
      {sharing && cf.accountOf(sharing.path) && <ShareSheet e={sharing} cf={cf} onClose={() => setSharing(null)} />}
      {offlineAsk && <OfflineDialog e={offlineAsk} onClose={() => setOfflineAsk(null)} />}
      {confirm && <Confirm text={confirm.text} detail={confirm.detail} okLabel={confirm.ok} destructive={!!confirm.destructive} cautious={!!confirm.cautious} onCancel={() => setConfirm(null)} onOk={() => { const c = confirm; setConfirm(null); c.run(); }} />}
      {sheet === 'goto' && <GoToFolder start={curDir ? `${curDir}/` : ''} onClose={() => setSheet(null)} onGo={(p) => { setSheet(null); navigate(dirLoc(p)); }} />}
      {sheet === 'connect' && <ConnectServer onClose={() => setSheet(null)} onConnect={connectTo} onBrowse={() => { setSheet(null); navigate({ type: 'network' }); }} />}
      {sheet === 'settings' && <FilesSettings onClose={() => setSheet(null)} />}
      {connAsk && <ConnectSheet ask={connAsk} onClose={() => setConnAsk(null)} onMounted={(at, note, name) => { if (at.startsWith('/')) noteOrigin(at, `${connAsk.kind}://${connAsk.host || connAsk.addr}/${String(name).replace(/^\/+/, '')}`); setConnAsk(null); setVolumes(fs.volumes()); setMsg(note); if (at.startsWith('/')) navigate(dirLoc(at)); }} />}
      {remoteAsk && <RemoteSheet ask={remoteAsk} onClose={() => setRemoteAsk(null)} onMounted={(at) => { const a = remoteAsk; noteOrigin(at, `${a.kind}://${a.user ? `${a.user}@` : ''}${a.host}${a.port ? `:${a.port}` : ''}${a.path ? `/${a.path.replace(/^\/+/, '')}` : ''}`); setRemoteAsk(null); setVolumes(fs.volumes()); navigate(dirLoc(at)); }}
        onStore={(pkg) => { setRemoteAsk(null); wm.open('store', { args: { search: pkg, package: pkg } }); }}
        onTerminal={remoteAsk.ssh ? () => { const typed = remoteAsk.ssh; setRemoteAsk(null); wm.open('terminal', { new: true, args: { typed } }); } : null} />}
      {menu && <ContextMenu x={menu.x} y={menu.y} items={localKeys(menu.items)} onClose={() => setMenu(null)} bounds={bounds()} />}
    </View>
  );
}
