// clipboard.js — the session clipboard and its (advanced) history.
//
// A single one for the whole session: shared by the terminal, text fields and Linux apps
// (Wayland; the compositor bridges both ways). The history keeps everything that gets
// copied in any of them —text, links, colors, code, paths and images— with its
// origin and time. It is stored in ~/.local/share/axia/clipboard/ (history.json + img-*.png) and
// survives a reboot; it is turned off or limited in Settings → Clipboard.
//
// Never stored: what was copied from a password field (clipboard.set(t, { secure: true })), what
// a Linux app marks as secret (x-kde-passwordManagerHint, like password
// managers do) and what comes from apps the user excluded.
//
//   clipboard.get() / clipboard.set(text, { secure, origin })
//   useStore(clipboardHistory) → [{ id, kind: 'text'|'image', type, text?, image?, origin, time, pinned }]
//   <ClipboardPanel /> (❖V quick, ❖⇧V full) is mounted by startShell: every theme has it.
import { createElement as h, useEffect, useRef, useState } from 'react';
import { View, Text, Image, addListener, screen, mainBox } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { useTheme } from './theme.js';
import { t, formatDate } from './i18n.js';
import { settings } from './settings.js';
import { wm } from './windows.js';
import { TextInput, focusToken, restoreFocus, pasteIntoFocused } from './components.jsx';
import * as fs from './fs.js';

const DIR = '.local/share/axia/clipboard';
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
const imageName = (p) => String(p).split('/').pop();
const ownImage = (p) => String(p).includes('/.local/share/axia/clipboard/img-');

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
    try { ensureDir(); fs.write(FILE, JSON.stringify({ version: 1, items: clipboardHistory.get() })); } catch (e) { console.log(`portapapeles: no se pudo guardar: ${e.message}`); }
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
  // re-copies a history entry (and moves it to the top)
  use(item) {
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
  beforeLogout() { if (conf().clearOnLogout) { clipboard.clear({ keepPinned: true }); if (saveTimer) { clearTimeout(saveTimer); saveTimer = null; } try { fs.write(FILE, JSON.stringify({ version: 1, items: clipboardHistory.get() })); } catch (e) { /* */ } } },
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
settings.subscribe(() => { if (!conf().save) { try { fs.write(FILE, JSON.stringify({ version: 1, items: [] })); } catch (e) { /* */ } } });
setTimeout(load, 0);

// ---- panel (❖V quick, ❖⇧V full) ----

export const clipboardPanel = createStore(false);       // false | 'quick' | 'full'
let target = null;                                       // where to paste on choosing
function openPanel(mode) {
  if (!clipboardPanel.get()) {
    const top = wm.focused();
    target = { text: focusToken(), linux: !!(top && String(top).startsWith('linux#')) };
  }
  clipboardPanel.set(mode);
}
export const closeClipboardPanel = () => { clipboardPanel.set(false); setTimeout(() => restoreFocus(target?.text), 0); };
// ❖V and ❖⇧V: the keys are in sdk/shortcuts.js.
export function toggleClipboardPanel(mode) {
  if (clipboardPanel.get() === mode) clipboardPanel.set(false); else openPanel(mode);
}
addListener('keydown', (e) => { if (e.key === 'Escape' && clipboardPanel.get()) clipboardPanel.set(false); });

const FILTERS = ['all', 'text', 'link', 'image', 'code', 'color', 'file', 'pinned'];
const TYPE_GLYPH = { text: '¶', link: '🔗', image: '🖼', code: '‹›', color: '●', file: '📁' };

function relTime(ms) {
  const s = Math.round((Date.now() - ms) / 1000);
  if (s < 60) return t('shell.clipboard.now');
  if (s < 3600) return t('shell.clipboard.minutes', { count: Math.floor(s / 60) });
  if (s < 86400) return t('shell.clipboard.hours', { count: Math.floor(s / 3600) });
  return formatDate(new Date(ms), 'short');
}
const originLabel = (o) => (!o || o === 'lordos' ? 'Axia DE' : o === 'terminal' ? t('shell.clipboard.fromTerminal') : o === 'linux' ? 'Linux' : o.replace(/^.*\./, ''));

function Swatch({ color, size = 14 }) {
  return h(View, { style: { width: size, height: size, borderRadius: 3, backgroundColor: color, border: '1px solid rgba(128,128,128,0.5)' } });
}

function Row({ item, active, th, onPick, onHover, compact }) {
  const fg = active ? '#fff' : th.text, dim = active ? 'rgba(255,255,255,0.8)' : th.textDim;
  return h(View, {
    onClick: onPick, onMouseEnter: onHover,
    style: { flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 8, paddingVertical: compact ? 5 : 7, borderRadius: 7,
      backgroundColor: active ? th.accent : 'transparent' },
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
    if (e.key === 'ArrowDown') { setSel((s) => Math.min(shown.length - 1, s + 1)); return true; }
    if (e.key === 'ArrowUp') { setSel((s) => Math.max(0, s - 1)); return true; }
    if (e.key === 'Enter') { if (shown[sel]) pick(shown[sel]); return true; }
    if (/^[1-9]$/.test(e.char || '') && !q) { const it = shown[Number(e.char) - 1]; if (it) pick(it); return true; }
    return false;
  };
  return h(View, { onClick: close, style: { position: 'absolute', ...mainBox() } },
    h(View, {
      onClick: (e) => e.stopPropagation(),
      style: { position: 'absolute', left: Math.round((mainBox().width - W) / 2), top: 70, width: W, padding: 10, gap: 4, borderRadius: 12,
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

// Full (❖⇧V): filters, list, preview and actions.
function FullPanel() {
  const th = useTheme();
  useEffect(() => load(), []);
  const list = useStore(clipboardHistory);
  const [q, setQ] = useState('');
  const [filter, setFilter] = useState('all');
  const [sel, setSel] = useState(0);
  const [confirm, setConfirm] = useState(false);
  const close = closeClipboardPanel;
  const shown = sortPinned(list.filter((x) => match(x, q) && (filter === 'all' || (filter === 'pinned' ? x.pinned : x.type === filter))));
  const cur = shown[Math.min(sel, shown.length - 1)] || null;
  const W = Math.min(860, mainBox().width - 40), H = Math.min(540, mainBox().height - 120);
  const onKey = (e) => {
    if (e.key === 'ArrowDown') { setSel((s) => Math.min(shown.length - 1, s + 1)); return true; }
    if (e.key === 'ArrowUp') { setSel((s) => Math.max(0, s - 1)); return true; }
    if (e.key === 'Enter') { if (cur) pick(cur); return true; }
    if (e.key === 'Delete' && cur) { clipboard.remove(cur); return true; }
    return false;
  };
  const btn = (label, onClick, danger) => h(View, { onClick, style: { paddingHorizontal: 10, paddingVertical: 5, borderRadius: 6, backgroundColor: danger ? '#ff453a' : th.control } },
    h(Text, { style: { fontSize: 11, fontWeight: 600, color: danger ? '#fff' : th.text } }, label));
  const count = (f) => (f === 'all' ? list.length : f === 'pinned' ? list.filter((x) => x.pinned).length : list.filter((x) => x.type === f).length);
  return h(View, { onClick: close, style: { position: 'absolute', ...mainBox(), backgroundColor: 'rgba(0,0,0,0.25)' } },
    h(View, {
      onClick: (e) => e.stopPropagation(),
      style: { position: 'absolute', left: Math.round((mainBox().width - W) / 2), top: Math.round((mainBox().height - H) / 2), width: W, height: H,
        borderRadius: 14, backgroundColor: th.windowBg, border: `1px solid ${th.separator}`, boxShadow: '0 24px 60px rgba(0,0,0,0.5)', overflow: 'hidden' },
    },
    h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: 10, padding: 12 } },
      h(Text, { style: { fontSize: 15, fontWeight: 700, color: th.text } }, t('shell.clipboard.title')),
      h(TextInput, { value: q, onChange: (v) => { setQ(v); setSel(0); }, autoFocus: true, placeholder: t('shell.clipboard.search'), onKeyDown: onKey, onCancel: close, style: { flex: 1 } }),
      confirm ? btn(t('shell.clipboard.confirmClear'), () => { clipboard.clear({ keepPinned: true }); setConfirm(false); }, true) : btn(t('shell.clipboard.clear'), () => setConfirm(true)),
      btn('✕', close)),
    h(View, { style: { height: 1, backgroundColor: th.separator } }),
    h(View, { style: { flex: 1, flexDirection: 'row' } },
      h(View, { style: { width: 150, padding: 8, gap: 2, backgroundColor: th.sidebarBg } },
        ...FILTERS.map((f) => h(View, { key: f, onClick: () => { setFilter(f); setSel(0); },
          style: { flexDirection: 'row', alignItems: 'center', paddingHorizontal: 8, height: 28, borderRadius: 6, backgroundColor: filter === f ? th.accent : 'transparent' } },
        h(Text, { style: { flex: 1, fontSize: 12, color: filter === f ? '#fff' : th.text } }, t(`shell.clipboard.filter.${f}`)),
        h(Text, { style: { fontSize: 10, color: filter === f ? '#fff' : th.textDim } }, String(count(f)))))),
      h(View, { style: { flex: 1, padding: 6, gap: 1, overflow: 'hidden' } },
        shown.length === 0 && h(Text, { style: { fontSize: 12, color: th.textDim, padding: 10 } }, t('shell.clipboard.empty')),
        ...shown.slice(Math.max(0, Math.min(sel - 8, shown.length - 16)), Math.max(16, sel + 8)).map((item) => {
          const i = shown.indexOf(item);
          return h(Row, { key: item.id || i, item, th, active: i === sel, onHover: () => setSel(i), onPick: () => pick(item) });
        })),
      h(View, { style: { width: 1, backgroundColor: th.separator } }),
      h(View, { style: { width: 280, padding: 12, gap: 8 } },
        cur ? [
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
            btn(t('shell.clipboard.delete'), () => clipboard.remove(cur))),
        ] : h(Text, { style: { fontSize: 12, color: th.textDim } }, t('shell.clipboard.noSelection')))),
    h(View, { style: { height: 1, backgroundColor: th.separator } }),
    h(Text, { style: { fontSize: 10, color: th.textDim, paddingHorizontal: 12, paddingVertical: 6 } },
      conf().save ? t('shell.clipboard.fullHint') : t('shell.clipboard.notSaved'))));
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
  return h(View, { style: { position: 'absolute', left: Math.round(mainBox().left + mainBox().width / 2 - 170), width: 340, top: mainBox().top + mainBox().height - 150, alignItems: 'center', pointerEvents: 'none' } },
    h(View, { style: { paddingHorizontal: 14, paddingVertical: 8, borderRadius: 10, backgroundColor: th.windowBg, border: `1px solid ${th.separator}`, boxShadow: '0 8px 24px rgba(0,0,0,0.35)' } },
      h(Text, { style: { fontSize: 12, color: th.text } }, msg)));
}
