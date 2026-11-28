// clipboard.js — the session clipboard and its (advanced) history.
//
// A single one for the whole session: shared by the terminal, text fields and Linux apps
// (Wayland; the compositor bridges both ways). The history keeps everything that gets
// copied in any of them —text, links, colors, code, paths and images— with its
// origin and time. It is stored in ~/.local/share/lordos/clipboard/ (history.json + img-*.png) and
// survives a reboot; it is turned off or limited in Settings → Clipboard.
//
// Never stored: what was copied from a password field (clipboard.set(t, { secure: true })), what
// a Linux app marks as secret (x-kde-passwordManagerHint, like password
// managers do) and what comes from apps the user excluded.
//
//   clipboard.get() / clipboard.set(text, { secure, origin })
//   clipboard.setFiles(paths, { cut }) / clipboard.files() → { paths, cut, own } | null   (Files)
//   useStore(clipboardHistory) → [{ id, kind: 'text'|'image', type, text?, image?, origin, time, pinned }]
//   <ClipboardPanel /> (❖V quick, ❖⇧V full) is mounted by startShell: every theme has it.
//   useStore(clipboardSnippets) → [{ id, name, text, created, updated }]; snippets.add/update/remove/move (GitHub #205)
//
// Snippets: texts the person keeps to paste again, in the full panel's second tab (sdk/snippetsmodel.js has the rules and
// the limits). They are kept in snippets.json next to the history, whatever the history's settings say, and a paste of
// one goes the same way as a history entry's. Both files are written by lord.clipboard.saveStore: 0600 in a 0700 folder.
// No global keys paste them yet: that needs its own security review (docs/ROADMAP.md).
import { createElement as h, useEffect, useRef, useState } from 'react';
import { View, Text, Image, addListener, screen, mainBox, displayFor, displays } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { useTheme } from './theme.js';
import { t, formatDate } from './i18n.js';
import { settings } from './settings.js';
import { wm } from './windows.js';
import { TextInput, TextArea, ScrollView, Segmented, hasTextFocus, focusToken, restoreFocus, pasteIntoFocused } from './components.jsx';
import { ContextMenu } from './contextmenu.jsx';
import { stepSel, scrollFor, hoverGate, rowWindow } from './clipboardpanelmodel.js';
import { addSnippet, updateSnippet, removeSnippet, moveSnippet, matchSnippet, parseSnippets, serializeSnippets, nameFor, SNIPPET_MAX_COUNT, SNIPPET_MAX_BYTES } from './snippetsmodel.js';
import * as fs from './fs.js';

const DIR = '.local/share/lordos/clipboard';
const FILE = `${DIR}/history.json`;
export const DEFAULTS = { save: true, max: 500, maxMB: 200, expireDays: 30, ignoreApps: [], clearOnLogout: false };
const conf = () => ({ ...DEFAULTS, ...(settings.get().clipboard || {}) });

export const clipboardHistory = createStore([]);

// ---- kind of each entry (for the filters) ----

export function clipboardKind(item) {
  if (item.image) return 'image';
  const s = String(item.text || '').trim();
  if (/^(https?|ftp):\/\/\S+$/i.test(s) || /^www\.\S+\.\S+$/i.test(s) || /^mailto:\S+$/i.test(s)) return 'link';
  if (/^#([0-9a-f]{3}|[0-9a-f]{4}|[0-9a-f]{6}|[0-9a-f]{8})$/i.test(s) || /^(rgba?|hsla?)\([^)]*\)$/i.test(s)) return 'color';
  if (/^(~?\/|file:\/\/)[^\n]*$/.test(s) && !/\s{2,}/.test(s)) return 'file';
  const lines = s.split('\n');
  const codey = /[{};]\s*$|^\s*(function|def|class|import|export|const|let|var|if|for|while|return|#include|SELECT|fn|func|package|public|private)\b|=>|\)\s*\{/m;
  if ((lines.length > 1 && codey.test(s)) || (lines.length > 2 && lines.filter((l) => /^\s{2,}|\t/.test(l)).length >= 2)) return 'code';
  return 'text';
}

// ---- persistence ----

let loaded = false;
let saveTimer = null;
const ensureDir = () => {
  let cur = '';
  for (const part of DIR.split('/')) { cur = cur ? `${cur}/${part}` : part; try { fs.mkdir(cur); } catch (e) { /* already exists */ } }
};
// the history's and the snippets' files: 0600 in a 0700 folder (lord.clipboard.saveStore); lord.fs where it is missing
function writeStore(name, text) {
  if (lord.clipboard.saveStore) { lord.clipboard.saveStore(name, text); return; }
  ensureDir();
  fs.write(`${DIR}/${name}`, text);
}
const imageName = (p) => String(p).split('/').pop();
const ownImage = (p) => String(p).includes('/.local/share/lordos/clipboard/img-');

function prune(list) {
  const c = conf();
  const now = Date.now();
  let out = list.filter((x) => x.pinned || !c.expireDays || now - x.time < c.expireDays * 86400000);
  // limit on count (pinned ones don't count) and on image space
  const free = out.filter((x) => !x.pinned);
  if (free.length > c.max) { const drop = new Set(free.slice(c.max)); out = out.filter((x) => !drop.has(x)); }
  let bytes = out.reduce((a, x) => a + (x.bytes || 0), 0);
  for (let i = out.length - 1; i >= 0 && bytes > c.maxMB * 1048576; i--) {
    if (out[i].image && !out[i].pinned) { bytes -= out[i].bytes || 0; out.splice(i, 1); }
  }
  // images left out are deleted from disk
  const keep = new Set(out.filter((x) => x.image).map((x) => x.image));
  for (const x of list) if (x.image && ownImage(x.image) && !keep.has(x.image)) { try { lord.clipboard.deleteSaved(imageName(x.image)); } catch (e) { /* already gone */ } }
  return out;
}

function scheduleSave() {
  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    saveTimer = null;
    if (!conf().save) return;
    try { writeStore('history.json', JSON.stringify({ version: 1, items: clipboardHistory.get() })); } catch (e) { console.log(`clipboard: history not saved: ${e.message}`); }
  }, 600);
}

function load() {
  if (loaded) return;
  loaded = true;
  if (!conf().save) return;
  let items = [];
  try { items = JSON.parse(fs.read(FILE)).items || []; } catch (e) { items = []; }
  const exists = new Set();
  try { for (const e of fs.list(DIR)) exists.add(e.name); } catch (e) { /* no folder */ }
  items = items.filter((x) => x && (x.text != null || (x.image && exists.has(imageName(x.image)))));
  clipboardHistory.set(prune(items));
}

let seq = Date.now();
const newId = () => (seq++).toString(36);

function push(item, same) {
  load();
  clipboardHistory.set((list) => {
    const old = list.find(same);
    const entry = { ...item, id: old?.id || newId(), time: Date.now(), pinned: old?.pinned || false };
    entry.type = clipboardKind(entry);
    return prune([entry, ...list.filter((x) => !same(x))]);
  });
  scheduleSave();
}

const ignored = (origin) => conf().ignoreApps.includes(origin);

const remember = (text, origin = 'lordos') => {
  if (!text || ignored(origin)) return;
  push({ kind: 'text', text: text.length > 1048576 ? text.slice(0, 1048576) : text, origin }, (x) => x.text === text);
};

function rememberImage(tmpPath, origin, gen) {
  if (!tmpPath || ignored(origin)) return;
  // the compositor's preview lives in $XDG_RUNTIME_DIR (lost on reboot): copy it to home
  let path = tmpPath;
  let bytes = 0;
  if (conf().save && lord.clipboard.saveImage) {
    try {
      ensureDir();
      const name = `img-${Date.now().toString(36)}-${gen || 0}.png`;
      if (lord.clipboard.saveImage(`${DIR}/${name}`)) {
        path = `${fs.home()}/${DIR}/${name}`;
        try { bytes = fs.list(DIR).find((e) => e.name === name)?.size || 0; } catch (e) { bytes = 0; }
      }
    } catch (e) { /* keep the temporary one */ }
  }
  push({ kind: 'image', image: path, origin, bytes, src: tmpPath }, (x) => x.src === tmpPath && x.image);
}

const state = () => (lord.clipboard.state ? lord.clipboard.state() : { kind: 'text' });
let skipNext = false;           // we generate the next notification ourselves (re-copying)
globalThis.__lordClipboard = () => {
  const s = state();
  if (skipNext) { skipNext = false; return; }
  if (s.sensitive) return;                          // password: not added to history
  if (s.kind === 'image') rememberImage(s.image, s.origin || 'linux', s.imageGen);
  else remember(lord.clipboard.get(), s.origin || 'lordos');
};

// ---- API ----

export const clipboard = {
  get: () => lord.clipboard.get(),
  // secure: comes from a password field (not added to history).
  set(text, { secure = false, origin = 'lordos' } = {}) {
    const s = String(text);
    lord.clipboard.set(s, { secure, origin });
    if (!secure) remember(s, origin);
    else skipNext = true;
  },
  // Files: copied (or cut) files, as Linux apps read them too (text/uri-list, gnome-copied-files; the text is
  // the paths). Only absolute paths; false if one isn't (the clipboard stays as it was).
  setFiles(paths, { cut = false } = {}) {
    if (!lord.clipboard.setFiles || !Array.isArray(paths)) return false;
    return lord.clipboard.setFiles(paths.map(String), { cut: !!cut });
  },
  // the files on the clipboard: { paths, cut, own } (own: copied in Files; cut is only ever set for those) or null
  files: () => (lord.clipboard.files ? lord.clipboard.files() : null),
  // re-copies a history entry (and moves it to the top)
  use(item) {
    // a snippet is copied as it is and stays out of the history (it is kept already)
    if (item.snippet) { lord.clipboard.set(item.text, { origin: 'lordos' }); skipNext = true; return; }
    if (item.image) { skipNext = true; lord.clipboard.useImage(item.image); }
    else if (item.text != null) { lord.clipboard.set(item.text, { origin: item.origin || 'lordos' }); skipNext = true; }
    clipboardHistory.set((list) => [{ ...item, time: Date.now() }, ...list.filter((x) => x.id !== item.id)]);
    scheduleSave();
  },
  pin(item) { clipboardHistory.set((list) => list.map((x) => (x.id === item.id ? { ...x, pinned: !x.pinned } : x))); scheduleSave(); },
  remove(item) {
    clipboardHistory.set((list) => list.filter((x) => x.id !== item.id));
    if (item.image && ownImage(item.image)) { try { lord.clipboard.deleteSaved(imageName(item.image)); } catch (e) { /* already gone */ } }
    scheduleSave();
  },
  clear({ keepPinned = true } = {}) {
    const list = clipboardHistory.get();
    const keep = keepPinned ? list.filter((x) => x.pinned) : [];
    for (const x of list) if (!keep.includes(x) && x.image && ownImage(x.image)) { try { lord.clipboard.deleteSaved(imageName(x.image)); } catch (e) { /* */ } }
    clipboardHistory.set(keep);
    scheduleSave();
  },
  // On logout (session.quit): clears the history if the user asked for it.
  beforeLogout() { if (conf().clearOnLogout) { clipboard.clear({ keepPinned: true }); if (saveTimer) { clearTimeout(saveTimer); saveTimer = null; } try { writeStore('history.json', JSON.stringify({ version: 1, items: clipboardHistory.get() })); } catch (e) { /* */ } } if (snipTimer) saveSnippetsNow(); },
  // Re-copies and pastes: into the text field that had focus or into the frontmost Linux app
  // (synthetic Ctrl+V for that app only). Returns 'pasted' or 'copied'.
  paste(item, target) {
    clipboard.use(item);
    if (target?.text && (restoreFocus(target.text), pasteIntoFocused())) return 'pasted';
    if (target?.linux && typeof lord.wayland !== 'undefined' && lord.wayland.hasKeyboard() && lord.wayland.paste()) return 'pasted';
    return 'copied';
  },
  remember,
  load,
};

// What was saved is read when the theme starts; if saving is turned off, the file is cleared.
settings.subscribe(() => { if (!conf().save) { try { writeStore('history.json', JSON.stringify({ version: 1, items: [] })); } catch (e) { /* */ } } });
setTimeout(load, 0);

// ---- snippets (GitHub #205) ----

const SNIPPETS = `${DIR}/snippets.json`;
export const clipboardSnippets = createStore([]);
let snipLoaded = false;
let snipTimer = null;
function loadSnippets() {
  if (snipLoaded) return;
  snipLoaded = true;
  let text = null;
  try { text = fs.read(SNIPPETS); } catch (e) { text = null; }       // none yet
  if (text != null) clipboardSnippets.set(parseSnippets(text));
}
function saveSnippetsNow() {
  if (snipTimer) { clearTimeout(snipTimer); snipTimer = null; }
  try { writeStore('snippets.json', serializeSnippets(clipboardSnippets.get())); } catch (e) { console.log(`clipboard: snippets not saved: ${e.message}`); }
}
const changed = (list) => { clipboardSnippets.set(list); if (snipTimer) clearTimeout(snipTimer); snipTimer = setTimeout(saveSnippetsNow, 300); };

// add and update → { snippet } or { error, snippet? } (the errors of sdk/snippetsmodel.js)
export const snippets = {
  load: loadSnippets,
  add({ name, text }) {
    loadSnippets();
    const r = addSnippet(clipboardSnippets.get(), { name, text }, { id: newId(), time: Date.now() });
    if (r.list) changed(r.list);
    return r;
  },
  update(id, { name, text }) {
    loadSnippets();
    const r = updateSnippet(clipboardSnippets.get(), id, { name, text }, Date.now());
    if (r.list) changed(r.list);
    return r;
  },
  remove(id) { loadSnippets(); changed(removeSnippet(clipboardSnippets.get(), id)); },
  move(id, step) { loadSnippets(); const l = clipboardSnippets.get(), m = moveSnippet(l, id, step); if (m !== l) changed(m); },
};
setTimeout(loadSnippets, 0);

// ---- panel (❖V quick, ❖⇧V full) ----

export const clipboardPanel = createStore(false);       // false | 'quick' | 'full'
let target = null;                                       // where to paste on choosing
// The display it opens on: the focused window's (Settings → Clipboard → Open panel from a window on another display
// opened it on the main one; BUG-27 of the ws170 QA), else the main display.
let panelOn = null;
const focusedDisplay = () => {
  try { const w = wm.get(wm.focused()); const d = w ? displayFor(wm.frame(w)) : null; return d ? d.id : null; } catch (e) { return null; }
};
function panelBox() {
  const d = panelOn == null ? null : displays().find((x) => x.id === panelOn);
  return d ? { left: d.x, top: d.y, width: d.w, height: d.h } : mainBox();
}
function openPanel(mode) {
  if (!clipboardPanel.get()) {
    const top = wm.focused();
    target = { text: focusToken(), linux: !!(top && String(top).startsWith('linux#')) };
    panelOn = focusedDisplay();
  }
  clipboardPanel.set(mode);
}
export const openClipboardPanel = (mode) => openPanel(mode);
export const closeClipboardPanel = () => { clipboardPanel.set(false); setTimeout(() => restoreFocus(target?.text), 0); };
// ❖V and ❖⇧V: the keys are in sdk/shortcuts.js.
export function toggleClipboardPanel(mode) {
  if (clipboardPanel.get() === mode) clipboardPanel.set(false); else openPanel(mode);
}
// Escape closes the panel; while a snippet is being written it closes the editor, and an open menu closes itself
let escHook = null;
addListener('keydown', (e) => { if (e.key === 'Escape' && clipboardPanel.get()) { if (escHook) escHook(); else clipboardPanel.set(false); } });

const FILTERS = ['all', 'text', 'link', 'image', 'code', 'color', 'file', 'pinned'];
const TYPE_GLYPH = { text: '¶', link: '🔗', image: '🖼', code: '‹›', color: '●', file: '📁' };

function relTime(ms) {
  const s = Math.round((Date.now() - ms) / 1000);
  if (s < 60) return t('shell.clipboard.now');
  if (s < 3600) return t('shell.clipboard.minutes', { count: Math.floor(s / 60) });
  if (s < 86400) return t('shell.clipboard.hours', { count: Math.floor(s / 3600) });
  return formatDate(new Date(ms), 'short');
}
const originLabel = (o) => (!o || o === 'lordos' ? 'LordOS' : o === 'terminal' ? t('shell.clipboard.fromTerminal') : o === 'linux' ? 'Linux' : o.replace(/^.*\./, ''));

function Swatch({ color, size = 14 }) {
  return h(View, { style: { width: size, height: size, borderRadius: 3, backgroundColor: color, border: '1px solid rgba(128,128,128,0.5)' } });
}

// The pointer's moves, for the panels' hover gate (clipboardpanelmodel.js): a row under a pointer at rest never takes the
// selection the arrow keys gave (GitHub #203, #204).
const gate = hoverGate();
addListener('pointermove', (e) => { if (clipboardPanel.get()) gate.move(e.x, e.y); });
const FULL_ROW_H = 46;     // the full panel's rows are all this tall (an image's 36 px and its padding), so it scrolls by rows

function Row({ item, active, th, onPick, onHover, onContext, compact, height }) {
  const fg = active ? '#fff' : th.text, dim = active ? 'rgba(255,255,255,0.8)' : th.textDim;
  return h(View, {
    onClick: onPick, onContextMenu: onContext, onMouseEnter: () => { if (gate.allow()) onHover(); },
    style: { flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 8, paddingVertical: compact ? 5 : 7, borderRadius: 7,
      ...(height ? { height, paddingVertical: 0 } : null), backgroundColor: active ? th.accent : 'transparent' },
  },
  item.type === 'color' ? h(Swatch, { color: item.text.trim() }) : h(Text, { style: { width: 16, fontSize: 11, color: dim, textAlign: 'center' } }, TYPE_GLYPH[item.type] || '¶'),
  item.image
    ? h(Image, { src: item.image, style: { width: 64, height: 36, objectFit: 'contain' } })
    : h(Text, { style: { flex: 1, fontSize: 12, color: fg, whiteSpace: 'nowrap', overflow: 'hidden', fontFamily: item.type === 'code' ? 'monospace' : undefined } },
      item.text.replace(/\s+/g, ' ').slice(0, 160)),
  item.image ? h(View, { style: { flex: 1 } }) : null,
  item.pinned ? h(Text, { style: { fontSize: 11, color: fg } }, '📌') : null,
  h(Text, { style: { fontSize: 10, color: dim } }, relTime(item.time)));
}

export function ClipboardPanel() {
  const mode = useStore(clipboardPanel);
  if (!mode) return null;
  return h(mode === 'full' ? FullPanel : QuickPanel);
}

function pick(item) {
  clipboardPanel.set(false);
  setTimeout(() => {
    const r = clipboard.paste(item, target);
    if (r === 'copied') { restoreFocus(target?.text); toast.set(t('shell.clipboard.copiedOnly')); }
  }, 0);
}
export const toast = createStore(null);

// Quick (❖V): the latest, with search; 1–9 choose.
function QuickPanel() {
  const th = useTheme();
  useEffect(() => load(), []);
  const list = useStore(clipboardHistory);
  const [q, setQ] = useState('');
  const [sel, setSel] = useState(0);
  const close = closeClipboardPanel;
  const shown = sortPinned(list.filter((x) => match(x, q))).slice(0, 12);
  const W = 420;
  const onKey = (e) => {
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { gate.key(); setSel((s) => stepSel(s, shown.length, e.key === 'ArrowUp' ? -1 : 1)); return true; }
    if (e.key === 'Enter') { if (shown[sel]) pick(shown[sel]); return true; }
    if (/^[1-9]$/.test(e.char || '') && !q) { const it = shown[Number(e.char) - 1]; if (it) pick(it); return true; }
    return false;
  };
  return h(View, { onClick: close, style: { position: 'absolute', ...panelBox() } },
    h(View, {
      onClick: (e) => e.stopPropagation(),
      style: { position: 'absolute', left: Math.round((panelBox().width - W) / 2), top: 70, width: W, padding: 10, gap: 4, borderRadius: 12,
        backgroundColor: th.windowBg, border: `1px solid ${th.separator}`, boxShadow: '0 16px 40px rgba(0,0,0,0.45)' },
    },
    h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: 8, marginBottom: 4 } },
      h(Text, { style: { fontSize: 13, fontWeight: 700, color: th.text } }, t('shell.clipboard.title')),
      h(TextInput, { value: q, onChange: (v) => { setQ(v); setSel(0); }, autoFocus: true, placeholder: t('shell.clipboard.search'), onKeyDown: onKey,
        onCancel: close, style: { flex: 1 } }),
      h(View, { onClick: () => openPanel('full'), style: { paddingHorizontal: 8, paddingVertical: 4, borderRadius: 6, backgroundColor: th.control } },
        h(Text, { style: { fontSize: 11, color: th.text } }, t('shell.clipboard.more')))),
    shown.length === 0 && h(Text, { style: { fontSize: 12, color: th.textDim, padding: 8 } }, t('shell.clipboard.empty')),
    ...shown.map((item, i) => h(Row, { key: item.id || i, item, th, active: i === sel, compact: true, onHover: () => setSel(i), onPick: () => pick(item) })),
    h(Text, { style: { fontSize: 10, color: th.textDim, paddingHorizontal: 4, marginTop: 4 } }, t('shell.clipboard.hint'))));
}

const match = (x, q) => !q || (x.text && x.text.toLowerCase().includes(q.toLowerCase())) || (x.origin && x.origin.toLowerCase().includes(q.toLowerCase()));
const sortPinned = (list) => [...list.filter((x) => x.pinned), ...list.filter((x) => !x.pinned)];

// Full (❖⇧V): two tabs. History: filters, list, preview and actions. Snippets (GitHub #205): the person's own texts, in
// their order, with an editor. The search filters both; the keys work alike in both (↑ ↓, Enter pastes, Del deletes).
let lastTab = 'history';
const SNIPPET_SIZE = `${SNIPPET_MAX_BYTES / 1024} KB`;

function SnippetRow({ s, active, th, onPick, onHover, onContext, height }) {
  const fg = active ? '#fff' : th.text, dim = active ? 'rgba(255,255,255,0.8)' : th.textDim;
  return h(View, {
    onClick: onPick, onContextMenu: onContext, onMouseEnter: () => { if (gate.allow()) onHover(); },
    style: { flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 8, height, borderRadius: 7, backgroundColor: active ? th.accent : 'transparent' },
  },
  h(Text, { style: { width: 16, fontSize: 11, color: dim, textAlign: 'center' } }, '✎'),
  h(View, { style: { flex: 1, minWidth: 0, gap: 2 } },
    h(Text, { style: { fontSize: 12, fontWeight: 600, color: fg, whiteSpace: 'nowrap', overflow: 'hidden' } }, s.name),
    h(Text, { style: { fontSize: 11, color: dim, whiteSpace: 'nowrap', overflow: 'hidden' } }, s.text.replace(/\s+/g, ' ').slice(0, 160))));
}

function FullPanel() {
  const th = useTheme();
  useEffect(() => { load(); loadSnippets(); }, []);
  const list = useStore(clipboardHistory);
  const snips = useStore(clipboardSnippets);
  const [tab, setTabState] = useState(lastTab);
  const [q, setQ] = useState('');
  const [filter, setFilter] = useState('all');
  const [sel, setSel] = useState(0);
  const [ssel, setSsel] = useState(0);
  const [confirm, setConfirm] = useState(false);
  const [doomed, setDoomed] = useState(null);            // the snippet whose Delete asks once more
  const [editing, setEditing] = useState(null);          // { id (null: a new one), name, text, error, other }
  const [ctx, setCtx] = useState(null);                  // a row's right-click menu
  const [searchKey, setSearchKey] = useState(0);         // a new search field takes the keys back (refocus)
  const scroll = useRef(null), listBox = useRef(null), box = useRef(null);
  const [sight, setSight] = useState({ y: 0, h: 600 });      // the list's scroll offset and height: only those rows are made
  const close = closeClipboardPanel;
  const snip = tab === 'snippets';
  const shown = sortPinned(list.filter((x) => match(x, q) && (filter === 'all' || (filter === 'pinned' ? x.pinned : x.type === filter))));
  const shownS = snips.filter((s) => matchSnippet(s, q));
  const cur = shown[Math.min(sel, shown.length - 1)] || null;
  const curS = shownS[Math.min(ssel, shownS.length - 1)] || null;
  const W = Math.min(860, panelBox().width - 40), H = Math.min(540, panelBox().height - 120);
  // the row the keys chose is scrolled into view, and only then: the list never moves by itself (GitHub #204)
  const reveal = (i) => {
    const sv = scroll.current;
    let viewH = 0;
    try { viewH = listBox.current ? lord.getLayout(listBox.current.id)[3] : 0; } catch (e) { viewH = 0; }
    const y = sv ? scrollFor(i, FULL_ROW_H + 1, sv.offset(), viewH, 6) : null;     // rows 1 px apart, 6 px of padding
    if (y != null) sv.scrollTo(y);
  };
  const toTop = () => scroll.current?.scrollTo(0);
  // a click on anything but a text field takes the keys from the search field: a new one takes them back
  const refocus = () => setSearchKey((k) => k + 1);
  const setTab = (v) => { lastTab = v; setTabState(v); setCtx(null); setDoomed(null); setConfirm(false); toTop(); };

  // ---- snippets ----
  const endEdit = () => { setEditing(null); refocus(); };
  const startEdit = (e) => { if (!snip) setTab('snippets'); setDoomed(null); setEditing({ error: null, other: '', ...e }); };
  const fromClipboard = () => {
    const s = state();
    if (s.sensitive) { toast.set(t('shell.clipboard.snippets.secret')); return; }     // a password is never kept
    const text = s.kind === 'image' ? '' : String(clipboard.get() || '');
    if (!text.trim()) { toast.set(t('shell.clipboard.snippets.noClipText')); return; }
    startEdit({ id: null, name: nameFor(text), text });
  };
  const saveAs = (item) => { if (item && !item.image && item.text != null) startEdit({ id: null, name: nameFor(item.text), text: item.text }); };
  const save = () => {
    const r = editing.id ? snippets.update(editing.id, editing) : snippets.add(editing);
    if (r.error) { setEditing({ ...editing, error: r.error, other: r.snippet?.name || '' }); return; }
    const at = Math.max(0, clipboardSnippets.get().findIndex((s) => s.id === r.snippet.id));
    setQ('');
    setSsel(at);
    endEdit();
    setTimeout(() => reveal(at), 0);
  };
  const errText = (ed) => t(`shell.clipboard.snippets.error.${ed.error}`, { max: ed.error === 'tooMany' ? SNIPPET_MAX_COUNT : SNIPPET_SIZE, name: ed.other });
  const pickSnippet = (s) => pick({ snippet: true, text: s.text });
  const delSnippet = (s) => { snippets.remove(s.id); setDoomed(null); };
  const posOf = (s) => snips.findIndex((x) => x.id === s.id);
  // reordering is done on the whole list, so not while a search shows only part of it
  const canMove = (s, step) => !q && !!s && posOf(s) + step >= 0 && posOf(s) + step < snips.length;
  const move = (s, step) => {
    if (!canMove(s, step)) return;
    const j = posOf(s) + step;
    snippets.move(s.id, step);
    setSsel(j);
    reveal(j);
  };

  // Escape: an open menu closes itself, the editor closes, and only then the panel
  useEffect(() => { escHook = ctx ? () => {} : editing ? endEdit : null; return () => { escHook = null; }; });

  const onKey = (e) => {
    if (e.key === 'Tab' && e.ctrl && !e.alt && !e.meta) { setTab(snip ? 'history' : 'snippets'); return true; }
    const count = snip ? shownS.length : shown.length;
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      const step = e.key === 'ArrowUp' ? -1 : 1;
      if (snip && e.alt) { move(curS, step); return true; }
      gate.key();
      const next = stepSel(Math.min(snip ? ssel : sel, count - 1), count, step);
      if (snip) { setSsel(next); setDoomed(null); } else setSel(next);
      reveal(next);
      return true;
    }
    if (e.key === 'Enter') { if (snip ? curS : cur) { if (snip) pickSnippet(curS); else pick(cur); } return true; }
    if (e.key === 'Delete' && snip && curS) { if (doomed === curS.id) delSnippet(curS); else setDoomed(curS.id); return true; }
    if (e.key === 'Delete' && !snip && cur) { clipboard.remove(cur); return true; }
    return false;
  };
  const btn = (label, onClick, danger, disabled) => h(View, { onClick: disabled ? undefined : onClick,
    style: { paddingHorizontal: 10, paddingVertical: 5, borderRadius: 6, backgroundColor: danger ? '#ff453a' : th.control, opacity: disabled ? 0.4 : 1 } },
  h(Text, { style: { fontSize: 11, fontWeight: 600, color: danger ? '#fff' : th.text } }, label));
  const count = (f) => (f === 'all' ? list.length : f === 'pinned' ? list.filter((x) => x.pinned).length : list.filter((x) => x.type === f).length);
  const menuAt = (e, items) => { const p = box.current && e.local ? e.local(box.current) : { x: e.x, y: e.y }; setCtx({ x: p.x, y: p.y, items }); };
  const historyMenu = (item, i) => (e) => {
    setSel(i);
    menuAt(e, [
      { label: t('shell.clipboard.paste'), onClick: () => pick(item) },
      { label: item.pinned ? t('shell.clipboard.unpin') : t('shell.clipboard.pin'), onClick: () => clipboard.pin(item) },
      { label: t('shell.clipboard.saveAsSnippet'), disabled: !!item.image || item.text == null, onClick: () => saveAs(item) },
      '-',
      { label: t('shell.clipboard.delete'), onClick: () => clipboard.remove(item) },
    ]);
  };
  const snippetMenu = (s, i) => (e) => {
    setSsel(i);
    setDoomed(null);
    menuAt(e, [
      { label: t('shell.clipboard.paste'), onClick: () => pickSnippet(s) },
      { label: t('shell.clipboard.snippets.edit'), onClick: () => startEdit({ id: s.id, name: s.name, text: s.text }) },
      { label: t('shell.clipboard.snippets.moveUp'), disabled: !canMove(s, -1), onClick: () => move(s, -1) },
      { label: t('shell.clipboard.snippets.moveDown'), disabled: !canMove(s, 1), onClick: () => move(s, 1) },
      '-',
      { label: t('shell.clipboard.delete'), onClick: () => delSnippet(s) },
    ]);
  };
  const tabLabel = (k, n) => (q ? `${t(`shell.clipboard.tab.${k}`)} · ${n}` : t(`shell.clipboard.tab.${k}`));
  const sideBtn = (label, onClick) => h(View, { onClick, style: { flexDirection: 'row', alignItems: 'center', paddingHorizontal: 8, height: 28, borderRadius: 6, backgroundColor: th.control } },
    h(Text, { style: { flex: 1, fontSize: 12, color: th.text, whiteSpace: 'nowrap', overflow: 'hidden' } }, `+ ${label}`));
  const fieldLabel = (s) => h(Text, { style: { fontSize: 11, fontWeight: 600, color: th.textDim } }, s);

  const rows = snip ? shownS : shown;
  const selected = Math.min(snip ? ssel : sel, rows.length - 1);
  const emptyText = snip ? (snips.length ? t('shell.clipboard.snippets.noMatch') : t('shell.clipboard.snippets.empty')) : t('shell.clipboard.empty');

  const sidebar = snip
    ? h(View, { style: { width: 150, padding: 8, gap: 6, backgroundColor: th.sidebarBg } },
      sideBtn(t('shell.clipboard.snippets.new'), () => startEdit({ id: null, name: '', text: '' })),
      sideBtn(t('shell.clipboard.snippets.fromClipboard'), fromClipboard),
      h(View, { style: { flex: 1 } }),
      h(Text, { style: { fontSize: 10, color: th.textDim, paddingHorizontal: 8 } }, `${snips.length} / ${SNIPPET_MAX_COUNT}`))
    : h(View, { style: { width: 150, padding: 8, gap: 2, backgroundColor: th.sidebarBg } },
      ...FILTERS.map((f) => h(View, { key: f, onClick: () => { setFilter(f); setSel(0); toTop(); },
        style: { flexDirection: 'row', alignItems: 'center', paddingHorizontal: 8, height: 28, borderRadius: 6, backgroundColor: filter === f ? th.accent : 'transparent' } },
      h(Text, { style: { flex: 1, fontSize: 12, color: filter === f ? '#fff' : th.text } }, t(`shell.clipboard.filter.${f}`)),
      h(Text, { style: { fontSize: 10, color: filter === f ? '#fff' : th.textDim } }, String(count(f))))));

  // the editor of a new snippet or of one being edited (and renamed): it takes the place of the list and the preview
  const editor = editing && h(View, { style: { flex: 1, padding: 14, gap: 6 } },
    h(Text, { style: { fontSize: 13, fontWeight: 700, color: th.text, marginBottom: 4 } }, editing.id ? t('shell.clipboard.snippets.editTitle') : t('shell.clipboard.snippets.new')),
    fieldLabel(t('shell.clipboard.snippets.name')),
    h(TextInput, { value: editing.name, onChange: (v) => setEditing((x) => ({ ...x, name: v, error: null })), placeholder: t('shell.clipboard.snippets.namePlaceholder'),
      autoFocus: true, onSubmit: save }),
    fieldLabel(t('shell.clipboard.snippets.text')),
    h(ScrollView, { style: { flex: 1, minHeight: 80 } },
      h(TextArea, { value: editing.text, onChange: (v) => setEditing((x) => ({ ...x, text: v, error: null })), placeholder: t('shell.clipboard.snippets.textPlaceholder'), minRows: 8 })),
    editing.error ? h(Text, { style: { fontSize: 11, color: '#ff453a' } }, errText(editing)) : null,
    h(View, { style: { flexDirection: 'row', justifyContent: 'flex-end', gap: 6, marginTop: 4 } },
      btn(t('shell.clipboard.snippets.cancel'), endEdit),
      btn(t('shell.clipboard.snippets.save'), save)));

  const historyPane = () => (cur ? [
    cur.image ? h(Image, { key: 'i', src: cur.image, style: { width: 256, height: 200, objectFit: 'contain' } })
      : cur.type === 'color' ? h(View, { key: 'c', style: { flexDirection: 'row', alignItems: 'center', gap: 10 } }, h(Swatch, { color: cur.text.trim(), size: 48 }),
        h(Text, { style: { fontSize: 16, fontWeight: 700, color: th.text, fontFamily: 'monospace' } }, cur.text.trim()))
        : h(Text, { key: 't', style: { fontSize: 11, color: th.text, fontFamily: cur.type === 'code' ? 'monospace' : undefined, maxHeight: 280, overflow: 'hidden', lineHeight: 1.35 } },
          cur.text.slice(0, 3000)),
    h(Text, { key: 'm', style: { fontSize: 11, color: th.textDim, lineHeight: 1.4 } },
      t('shell.clipboard.meta', { type: t(`shell.clipboard.filter.${cur.type}`), origin: originLabel(cur.origin), when: relTime(cur.time),
        size: cur.text ? t('shell.clipboard.chars', { count: [...cur.text].length }) : '' })),
    h(View, { key: 'a', style: { flexDirection: 'row', flexWrap: 'wrap', gap: 6 } },
      btn(t('shell.clipboard.paste'), () => pick(cur)),
      btn(cur.pinned ? t('shell.clipboard.unpin') : t('shell.clipboard.pin'), () => clipboard.pin(cur)),
      cur.image || cur.text == null ? null : btn(t('shell.clipboard.saveAsSnippet'), () => saveAs(cur)),
      btn(t('shell.clipboard.delete'), () => clipboard.remove(cur))),
  ] : h(Text, { style: { fontSize: 12, color: th.textDim } }, t('shell.clipboard.noSelection')));

  const snippetPane = () => (curS ? [
    h(Text, { key: 'n', style: { fontSize: 13, fontWeight: 700, color: th.text } }, curS.name),
    h(Text, { key: 't', style: { fontSize: 11, color: th.text, maxHeight: 280, overflow: 'hidden', lineHeight: 1.35 } }, curS.text.slice(0, 3000)),
    h(Text, { key: 'm', style: { fontSize: 11, color: th.textDim, lineHeight: 1.4 } },
      t('shell.clipboard.snippets.meta', { when: relTime(curS.updated || curS.created || Date.now()), size: t('shell.clipboard.chars', { count: [...curS.text].length }) })),
    h(View, { key: 'a', style: { flexDirection: 'row', flexWrap: 'wrap', gap: 6 } },
      btn(t('shell.clipboard.paste'), () => pickSnippet(curS)),
      btn(t('shell.clipboard.snippets.edit'), () => startEdit({ id: curS.id, name: curS.name, text: curS.text })),
      btn(t('shell.clipboard.snippets.moveUp'), () => move(curS, -1), false, !canMove(curS, -1)),
      btn(t('shell.clipboard.snippets.moveDown'), () => move(curS, 1), false, !canMove(curS, 1)),
      doomed === curS.id ? btn(t('shell.clipboard.snippets.confirmDelete'), () => delSnippet(curS), true) : btn(t('shell.clipboard.delete'), () => setDoomed(curS.id))),
  ] : h(Text, { style: { fontSize: 12, color: th.textDim } }, snips.length ? t('shell.clipboard.noSelection') : t('shell.clipboard.snippets.empty')));

  return h(View, { onClick: close, style: { position: 'absolute', ...panelBox(), backgroundColor: 'rgba(0,0,0,0.25)' } },
    h(View, {
      ref: box,
      onClick: (e) => { e.stopPropagation(); if (!editing && !hasTextFocus()) refocus(); },
      style: { position: 'absolute', left: Math.round((panelBox().width - W) / 2), top: Math.round((panelBox().height - H) / 2), width: W, height: H,
        borderRadius: 14, backgroundColor: th.windowBg, border: `1px solid ${th.separator}`, boxShadow: '0 24px 60px rgba(0,0,0,0.5)', overflow: 'hidden' },
    },
    h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: 10, padding: 12 } },
      h(Text, { style: { fontSize: 15, fontWeight: 700, color: th.text } }, t('shell.clipboard.title')),
      h(Segmented, { options: [['history', tabLabel('history', shown.length)], ['snippets', tabLabel('snippets', shownS.length)]], value: tab, onChange: setTab, small: true, focusable: false }),
      h(TextInput, { key: `q${searchKey}`, value: q, onChange: (v) => { setQ(v); setSel(0); setSsel(0); setDoomed(null); toTop(); }, autoFocus: !editing,
        placeholder: snip ? t('shell.clipboard.searchSnippets') : t('shell.clipboard.search'), onKeyDown: onKey, onCancel: () => { if (!escHook) close(); }, style: { flex: 1 } }),
      snip ? null : confirm ? btn(t('shell.clipboard.confirmClear'), () => { clipboard.clear({ keepPinned: true }); setConfirm(false); }, true) : btn(t('shell.clipboard.clear'), () => setConfirm(true)),
      btn('✕', close)),
    h(View, { style: { height: 1, backgroundColor: th.separator } }),
    h(View, { style: { flex: 1, flexDirection: 'row' } },
      sidebar,
      snip && editing ? editor : [
        // every entry in a list that scrolls (the wheel, its bar, the keys); a row is selected by the keys or by a pointer
        // that moves over it, and the selection is drawn (GitHub #203, #204)
        // (up to 500 entries: only the rows in sight and a few around are made, the rest is empty space of their height)
        h(View, { key: 'list', ref: listBox, style: { flex: 1, minWidth: 0 } },
          h(ScrollView, { scrollRef: scroll, style: { flex: 1 }, onScroll: (y, hh) => setSight((v) => (v.y === y && v.h === hh ? v : { y, h: hh })) },
            (() => {
              const win = rowWindow(sight.y, sight.h, FULL_ROW_H + 1, rows.length, 6);
              return h(View, { style: { padding: 6, gap: 1 } },
                rows.length === 0 && h(Text, { style: { fontSize: 12, color: th.textDim, padding: 10 } }, emptyText),
                win.before > 0 && h(View, { key: 'before', style: { height: win.before - 1 } }),
                ...rows.slice(win.from, win.to).map((item, k) => {
                  const i = win.from + k;
                  return snip
                    ? h(SnippetRow, { key: `s${item.id}`, s: item, th, height: FULL_ROW_H, active: i === selected, onHover: () => { setSsel(i); setDoomed(null); },
                      onPick: () => pickSnippet(item), onContext: snippetMenu(item, i) })
                    : h(Row, { key: item.id || i, item, th, height: FULL_ROW_H, active: i === selected, onHover: () => setSel(i), onPick: () => pick(item), onContext: historyMenu(item, i) });
                }),
                win.after > 0 && h(View, { key: 'after', style: { height: win.after - 1 } }));
            })())),
        h(View, { key: 'sep', style: { width: 1, backgroundColor: th.separator } }),
        h(View, { key: 'pane', style: { width: 280, padding: 12, gap: 8 } }, snip ? snippetPane() : historyPane()),
      ]),
    h(View, { style: { height: 1, backgroundColor: th.separator } }),
    h(Text, { style: { fontSize: 10, color: th.textDim, paddingHorizontal: 12, paddingVertical: 6 } },
      snip ? t('shell.clipboard.snippets.hint') : conf().save ? t('shell.clipboard.fullHint') : t('shell.clipboard.notSaved')),
    ctx && h(ContextMenu, { x: ctx.x, y: ctx.y, items: ctx.items, onClose: () => setCtx(null), bounds: { w: W, h: H } })));
}

// Short notice (for example: "copied; paste with Ctrl+V").
export function ClipboardToast() {
  const msg = useStore(toast);
  const th = useTheme();
  const timer = useRef(null);
  useEffect(() => {
    if (!msg) return undefined;
    clearTimeout(timer.current);
    timer.current = setTimeout(() => toast.set(null), 2500);
    return () => clearTimeout(timer.current);
  }, [msg]);
  if (!msg) return null;
  return h(View, { style: { position: 'absolute', left: Math.round(panelBox().left + panelBox().width / 2 - 170), width: 340, top: panelBox().top + panelBox().height - 150, alignItems: 'center', pointerEvents: 'none' } },
    h(View, { style: { paddingHorizontal: 14, paddingVertical: 8, borderRadius: 10, backgroundColor: th.windowBg, border: `1px solid ${th.separator}`, boxShadow: '0 8px 24px rgba(0,0,0,0.35)' } },
      h(Text, { style: { fontSize: 12, color: th.text } }, msg)));
}
