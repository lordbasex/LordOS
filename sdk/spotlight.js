// spotlight.js — LordOS Search, the global search. ❖Space or Ctrl+Space open it
// from anywhere; <SpotlightPanel /> is mounted by startShell, so every theme has it.
//
// Searches apps, files in the home folder, Settings sections, notes, the clipboard
// history and the Store; it also calculates (2*(3+4), 15% of 2400, sqrt(2)) and converts units
// (10 km to mi, 72 f to c, 1,5 gb to mb). ↑↓ choose, Enter opens, ❖Enter shows in Files, Esc closes.
// What is opened often rises in the results (~/.local/share/lordos/spotlight.json).
import { createElement as h, useEffect, useMemo, useRef, useState } from 'react';
import { View, Text, Image, addListener, screen, mainBox, displays } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { useTheme } from './theme.js';
import { t, tOptional, formatNumber, formatDate, locale } from './i18n.js';
import { settings } from './settings.js';
import { wm, getApp, windows } from './windows.js';
import { useLauncherApps, launchApp } from './apps.js';
import { clipboard, clipboardHistory } from './clipboard.js';
import { logOut } from './sessionrestore.jsx';
import { TextInput, focusToken, restoreFocus } from './components.jsx';
import * as fs from './fs.js';

export const spotlightOpen = createStore(false);
let prevFocus = null;
// display: where it opens (the display whose menu bar's ⌕ was clicked); without one, the main display
let openOn = null;
export const spotlight = {
  open(display) { if (!spotlightOpen.get()) { prevFocus = focusToken(); openOn = display && display.id != null ? display.id : null; spotlightOpen.set(true); } },
  close() { if (spotlightOpen.get()) { spotlightOpen.set(false); setTimeout(() => restoreFocus(prevFocus), 0); } },
  toggle(display) { if (spotlightOpen.get()) spotlight.close(); else spotlight.open(display); },
};
// the box it opens in: that display while it is still connected, else the main one
function searchBox() {
  const d = openOn == null ? null : displays().find((x) => x.id === openOn);
  return d ? { left: d.x, top: d.y, width: d.w, height: d.h } : mainBox();
}

// ---- recent use (for ranking) ----

const USE_FILE = '.local/share/lordos/spotlight.json';
let usage = null;
function loadUsage() {
  if (usage) return usage;
  usage = {};
  try { usage = JSON.parse(fs.read(USE_FILE)) || {}; } catch (e) { usage = {}; }
  return usage;
}
function ensureDir(rel) {
  let cur = '';
  for (const part of rel.split('/')) { cur = cur ? `${cur}/${part}` : part; try { fs.mkdir(cur); } catch (e) { /* already exists */ } }
}
function noteUse(key) {
  const u = loadUsage();
  u[key] = { n: (u[key]?.n || 0) + 1, at: Date.now() };
  const keys = Object.keys(u);
  if (keys.length > 300) for (const k of keys.sort((a, b) => u[a].at - u[b].at).slice(0, keys.length - 300)) delete u[k];
  try { ensureDir('.local/share/lordos'); fs.write(USE_FILE, JSON.stringify(u)); } catch (e) { /* no disk: memory only */ }
}
function usageBoost(key) {
  const u = loadUsage()[key];
  if (!u) return 0;
  const days = (Date.now() - u.at) / 86400000;
  return Math.min(30, u.n * 4) * Math.max(0.2, 1 - days / 30);
}

// ---- matching: prefix > word > substring > fuzzy ----

const fold = (s) => String(s || '').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '');
function score(text, q, fuzzy = true) {
  const a = fold(text);
  if (!a || !q) return 0;
  if (a === q) return 100;
  if (a.startsWith(q)) return 80;
  if (new RegExp(`(^|[\\s._\\-/(])${q.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}`).test(a)) return 60;
  if (a.includes(q)) return 40;
  if (!fuzzy) return 0;
  let i = 0;
  for (const ch of a) if (ch === q[i]) i++;
  return i === q.length && q.length >= 2 ? 15 : 0;
}
// The first text is the name (fuzzy matching also counts); the others only count if
// the text really appears (word or substring), and they weigh less.
const best = (q, ...texts) => Math.max(0, ...texts.map((x, i) => (i === 0 ? score(x, q) : score(x, q, false) * 0.7)));

// ---- calculator (our own parser, no eval) ----

const FUNCS = { sqrt: Math.sqrt, raiz: Math.sqrt, sin: Math.sin, sen: Math.sin, cos: Math.cos, tan: Math.tan, log: Math.log10, ln: Math.log,
  abs: Math.abs, round: Math.round, redondear: Math.round, floor: Math.floor, ceil: Math.ceil, exp: Math.exp };
const CONSTS = { pi: Math.PI, e: Math.E };

export function calc(input) {
  let s = String(input).trim().toLowerCase().replace(/×/g, '*').replace(/÷/g, '/').replace(/\s+/g, ' ');
  // "15% de 2400", "15 % of 2400", "15% do 2400"
  const pct = /^(.+?)\s*%\s*(?:de|of|do|da|del)\s+(.+)$/.exec(s);
  const val = (x) => (/^-?[\d.,]+$/.test(x.trim()) ? parseFloat(x.trim().replace(',', '.')) : calc(x));
  if (pct) { const a = val(pct[1]), b = val(pct[2]); return a == null || b == null || Number.isNaN(a) || Number.isNaN(b) ? null : (a / 100) * b; }
  const hasOp = /[-+*/^%()]/.test(s.replace(/^-/, '')) || /\b(sqrt|raiz|sin|sen|cos|tan|log|ln|abs|round|redondear|floor|ceil|exp)\b/.test(s);
  if (!/\d|\bpi\b/.test(s) || !hasOp) return null;
  // decimal comma: "1,5" (if there are no dots or ambiguous thousands separators)
  if (!s.includes('.') && /\d,\d/.test(s)) s = s.replace(/(\d),(\d)/g, '$1.$2');
  let i = 0;
  const peek = () => s[i];
  const skip = () => { while (s[i] === ' ') i++; };
  function primary() {
    skip();
    if (peek() === '(') { i++; const v = expr(); skip(); if (s[i] !== ')') throw new Error('falta )'); i++; return v; }
    if (peek() === '-') { i++; return -factor(); }
    if (peek() === '+') { i++; return factor(); }
    const num = /^\d+(?:\.\d+)?(?:e[-+]?\d+)?|^\.\d+/.exec(s.slice(i));
    if (num) { i += num[0].length; return parseFloat(num[0]); }
    const id = /^[a-z]+/.exec(s.slice(i));
    if (id) {
      i += id[0].length;
      if (id[0] in CONSTS) return CONSTS[id[0]];
      if (id[0] in FUNCS) { skip(); return FUNCS[id[0]](primary()); }
    }
    throw new Error('expression');
  }
  function factor() {
    let v = primary();
    skip();
    while (peek() === '%' ) { i++; v /= 100; skip(); }
    if (peek() === '^' || (peek() === '*' && s[i + 1] === '*')) { i += peek() === '^' ? 1 : 2; v = v ** factor(); }
    return v;
  }
  function term() {
    let v = factor();
    for (;;) {
      skip();
      if (peek() === '*' && s[i + 1] !== '*') { i++; v *= factor(); } else if (peek() === '/') { i++; v /= factor(); } else return v;
    }
  }
  function expr() {
    let v = term();
    for (;;) { skip(); if (peek() === '+') { i++; v += term(); } else if (peek() === '-') { i++; v -= term(); } else return v; }
  }
  try {
    const v = expr();
    skip();
    return i === s.length && Number.isFinite(v) ? v : null;
  } catch (e) { return null; }
}

// ---- unit conversion ----

const UNITS = {
  length: { mm: 0.001, cm: 0.01, m: 1, km: 1000, in: 0.0254, pulgada: 0.0254, pulgadas: 0.0254, ft: 0.3048, pie: 0.3048, pies: 0.3048,
    yd: 0.9144, mi: 1609.344, milla: 1609.344, millas: 1609.344 },
  mass: { mg: 1e-6, g: 0.001, kg: 1, t: 1000, lb: 0.45359237, libra: 0.45359237, libras: 0.45359237, oz: 0.028349523125 },
  data: { b: 1, kb: 1e3, mb: 1e6, gb: 1e9, tb: 1e12, kib: 1024, mib: 1024 ** 2, gib: 1024 ** 3, tib: 1024 ** 4 },
  time: { ms: 0.001, s: 1, seg: 1, min: 60, h: 3600, hora: 3600, horas: 3600, d: 86400, dia: 86400, dias: 86400, semana: 604800 },
  volume: { ml: 0.001, l: 1, litro: 1, litros: 1, gal: 3.785411784, galon: 3.785411784 },
  speed: { kmh: 1 / 3.6, 'km/h': 1 / 3.6, mph: 0.44704, 'm/s': 1, ms_: 1, nudo: 0.514444, kn: 0.514444 },
};
const TEMP = { c: 'c', '°c': 'c', celsius: 'c', f: 'f', '°f': 'f', fahrenheit: 'f', k: 'k', kelvin: 'k' };
const toK = (v, u) => (u === 'c' ? v + 273.15 : u === 'f' ? (v - 32) * 5 / 9 + 273.15 : v);
const fromK = (v, u) => (u === 'c' ? v - 273.15 : u === 'f' ? (v - 273.15) * 9 / 5 + 32 : v);
const groupOf = (u) => Object.keys(UNITS).find((g) => u in UNITS[g]);

export function convert(input) {
  const m = /^(-?[\d.,]+)\s*([a-zA-Z°/µ_]+)\s*(?:a|to|en|in|para|→|->|=)\s*([a-zA-Z°/µ_]+)\s*$/.exec(fold(input).trim());
  if (!m) return null;
  const v = parseFloat(m[1].replace(',', '.'));
  const a = m[2], b = m[3];
  if (!Number.isFinite(v)) return null;
  const tl = { c: '°C', f: '°F', k: 'K' };
  if (TEMP[a] && TEMP[b]) return { value: fromK(toK(v, TEMP[a]), TEMP[b]), from: tl[TEMP[a]], to: tl[TEMP[b]], input: v };
  const ga = groupOf(a), gb = groupOf(b);
  if (!ga || ga !== gb) return null;
  return { value: (v * UNITS[ga][a]) / UNITS[ga][b], from: m[2], to: m[3], input: v };
}

// ---- file index (if the server doesn't provide lord.fs.search) ----

let fileIndex = { at: 0, list: [], building: false };
function buildFileIndex() {
  if (fileIndex.building || Date.now() - fileIndex.at < 120000) return;
  fileIndex.building = true;
  const out = [];
  const queue = [['', 0]];
  const step = () => {
    let budget = 40;
    while (queue.length && budget-- > 0 && out.length < 5000) {
      const [dir, depth] = queue.shift();
      let entries = [];
      try { entries = fs.list(dir || '.'); } catch (e) { continue; }
      for (const e of entries) {
        const rel = dir ? `${dir}/${e.name}` : e.name;
        out.push({ path: `${fs.home()}/${rel}`, name: e.name, dir: e.dir, size: e.size, mtime: e.mtime });
        if (e.dir && depth < 4) queue.push([rel, depth + 1]);
      }
    }
    if (queue.length && out.length < 5000) { setTimeout(step, 0); return; }
    fileIndex = { at: Date.now(), list: out, building: false };
    resultsVersion.set((x) => x + 1);
  };
  step();
}
const resultsVersion = createStore(0);

function searchFiles(q) {
  if (q.length < 2) return [];
  if (typeof lord.fs.search === 'function') {
    try { return lord.fs.search(fs.home(), q, { limit: 60, depth: 6 }); } catch (e) { /* fall back to the index */ }
  }
  buildFileIndex();
  return fileIndex.list.filter((f) => fold(f.name).includes(q)).slice(0, 60);
}

const IMG = /\.(png|jpe?g|gif|webp|bmp)$/i;
const TEXT = /\.(txt|md|markdown|json|js|jsx|ts|tsx|mjs|c|h|cpp|hpp|py|go|rs|sh|bash|zsh|css|html?|xml|ya?ml|toml|ini|conf|cfg|log|csv|sql|php|rb|lua|diff|patch)$/i;

function openFile(f) {
  const rel = f.path;
  if (f.dir) wm.open('files', { new: true, args: { path: rel } });
  else if (TEXT.test(f.name) && getApp('editor')) wm.open('editor', { args: { path: rel } });
  else revealFile(f);
}
function revealFile(f) {
  const parent = f.path.split('/').slice(0, -1).join('/') || fs.home();
  wm.open('files', { new: true, args: { path: parent, select: f.name } });
}

// ---- notes (Notes app: ~/Notes/<note>/nota.md; ~/Notas before 0.7.0) ----

let notesCache = { at: 0, list: [] };
function loadNotes() {
  if (!getApp('notes')) return [];
  if (Date.now() - notesCache.at < 30000) return notesCache.list;
  const list = [];
  let dirs = [];
  let base = 'Notes';
  try { dirs = fs.list(base); } catch (e) { base = 'Notas'; try { dirs = fs.list(base); } catch (e2) { dirs = []; } }
  for (const d of dirs.filter((x) => x.dir).slice(0, 400)) {
    let text = '';
    try { text = fs.read(`${base}/${d.name}/nota.md`).slice(0, 65536); } catch (e) { continue; }
    // the Notes app's YAML header (title: …) and the markdown body
    const head = /^---\n([\s\S]*?)\n---\n?/.exec(text);
    const body = head ? text.slice(head[0].length) : text;
    const title = (/^title:\s*(.+)$/m.exec(head?.[1] || '')?.[1] || /^#\s+(.+)$/m.exec(body)?.[1] || body.split('\n').find((l) => l.trim()) || d.name).trim();
    list.push({ id: d.name, title, text: body, mtime: d.mtime });
  }
  notesCache = { at: Date.now(), list };
  return list;
}

// ---- Settings sections ----

// ---- other sources (Help articles, …) ----
// registerSpotlightSource('help', (q, query, { fold, best }) => [{ key, score, title, subtitle, glyph, preview, run }])
// q is the normalized query (lowercase, no accents). The section title is
// t('shell.spotlight.section.<id>').
const sources = new Map();
export function registerSpotlightSource(id, fn) { sources.set(id, fn); return () => sources.delete(id); }

const SETTINGS = ['general', 'appearance', 'wallpaper', 'network', 'sound', 'clipboard', 'sharing', 'language', 'keyboard', 'users', 'updates', 'themes', 'session'];

// ---- building the results ----

function buildResults(raw, apps) {
  const query = raw.trim();
  const q = fold(query);
  if (!q) return [];
  const sections = [];
  const add = (id, items, top) => { if (items.length) sections.push({ id, items, top: top ?? Math.max(...items.map((x) => x.score || 0)) }); };

  // calculator and conversions
  const conv = convert(query);
  if (conv) {
    const res = `${fmtNum(conv.value, Math.abs(conv.value) < 1 ? 6 : 3)} ${conv.to}`;
    add('calc', [{ key: 'calc', title: res, subtitle: `${fmtNum(conv.input, 6)} ${conv.from} → ${conv.to}`, glyph: '⇄', big: res,
      run: () => clipboard.set(String(+conv.value.toFixed(6))), hint: t('shell.spotlight.copy') }], 1000);
  } else {
    const v = calc(query);
    if (v != null) {
      const res = fmtNum(v, 10);
      add('calc', [{ key: 'calc', title: `= ${res}`, subtitle: query, glyph: '=', big: res,
        run: () => clipboard.set(String(+v.toPrecision(12))), hint: t('shell.spotlight.copy') }], 1000);
    }
  }

  // apps
  add('apps', apps.map((a) => ({ a, s: best(q, a.name, a.genericName, a.keywords, tOptional(`shell.spotlight.keywords.${a.id}`), a.id, a.comment) }))
    .filter((x) => x.s > 0).map((x) => ({ ...x, s: x.s + usageBoost(`app:${x.a.id}`) })).sort((x, y) => y.s - x.s).slice(0, 6)
    .map(({ a, s }) => ({ key: `app:${a.id}`, score: s, title: a.name, subtitle: a.comment || a.genericName || t(`store.kind.${a.kind}`),
      Icon: a.Icon, preview: { kind: 'app', app: a }, run: () => launchApp(a.id) })));

  // settings
  add('settings', SETTINGS.map((id) => ({ id, name: tOptional(`settings.section.${id}`), kw: tOptional(`settings.keywords.${id}`) }))
    .filter((x) => x.name).map((x) => ({ ...x, s: best(q, x.name, x.kw) })).filter((x) => x.s > 0).sort((a, b) => b.s - a.s).slice(0, 4)
    .map((x) => ({ key: `settings:${x.id}`, score: x.s + usageBoost(`settings:${x.id}`), title: x.name, subtitle: t('shell.spotlight.inSettings'), glyph: '⚙',
      run: () => wm.open('settings', { args: { section: x.id } }) })));

  // system actions
  const actions = [
    { id: 'dark', name: t('shell.spotlight.action.dark'), run: () => settings.set({ mode: settings.get().mode === 'dark' ? 'light' : 'dark' }) },
    { id: 'logout', name: t('shell.menu.logout'), run: () => logOut() },
  ];
  add('actions', actions.map((x) => ({ ...x, s: best(q, x.name) })).filter((x) => x.s >= 40)
    .map((x) => ({ key: `action:${x.id}`, score: x.s + 5, title: x.name, subtitle: t('shell.spotlight.action.hint'), glyph: '⏎', run: x.run })));

  // files
  const stem = (n) => n.replace(/\.[^.]+$/, '');
  add('files', searchFiles(q).map((f) => ({ f, s: Math.max(score(stem(f.name), q), score(f.name, q, false) * 0.3) }))
    .filter((x) => x.s > 15).map((x) => ({ ...x, s: x.s + usageBoost(`file:${x.f.path}`) - (x.f.dir ? 0 : 1) }))
    .sort((a, b) => b.s - a.s).slice(0, 6)
    .map(({ f, s: sc }) => ({ key: `file:${f.path}`, score: sc * 0.8, title: f.name, subtitle: f.path.replace(fs.home(), '~'), glyph: f.dir ? '📁' : IMG.test(f.name) ? '🖼' : '📄',
      preview: { kind: 'file', file: f }, run: () => openFile(f), reveal: () => revealFile(f) })));

  // notes
  add('notes', loadNotes().map((n) => ({ n, s: Math.max(best(q, n.title), fold(n.text).includes(q) ? 30 : 0) })).filter((x) => x.s > 0)
    .sort((a, b) => b.s - a.s).slice(0, 5)
    .map(({ n, s: sc }) => ({ key: `note:${n.id}`, score: sc, title: n.title, subtitle: formatDate(new Date(n.mtime), 'short'), glyph: '🗒',
      preview: { kind: 'note', note: n }, run: () => wm.open('notes', { args: { slug: n.id } }) })));

  // clipboard
  add('clipboard', clipboardHistory.get().filter((c) => c.text && fold(c.text).includes(q)).slice(0, 4)
    .map((c) => ({ key: `clip:${c.id || c.time}`, score: 20, title: c.text.replace(/\s+/g, ' ').slice(0, 80), subtitle: t('shell.spotlight.fromClipboard'), glyph: '📋',
      preview: { kind: 'text', text: c.text }, run: () => clipboard.use(c), hint: t('shell.spotlight.copy') })));

  // registered sources (Help…)
  for (const [id, fn] of sources) {
    let items = [];
    try { items = fn(q, query, { fold, best }) || []; } catch (e) { console.error(`spotlight ${id}:`, e); }
    add(id, items.slice(0, 6));
  }

  // Store (AppStream apps)
  if (q.length >= 3 && typeof lord.pkg !== 'undefined' && lord.pkg.available()) {
    let res = [];
    try { res = lord.pkg.search(query, { limit: 20, every: true, lang: locale.get().lang.split('-')[0] }).filter((r) => r.app && !r.installed).slice(0, 5); } catch (e) { res = []; }
    add('store', res.map((r) => ({ key: `store:${r.name}`, score: 10, title: r.app.name, subtitle: r.app.summary || r.summary, image: r.app.icon,
      preview: { kind: 'pkg', pkg: r }, run: () => wm.open('store', { args: { search: query, package: r.name } }) })));
  }
  // sections are ordered by their best result (the calculator always on top)
  return sections.sort((a, b) => b.top - a.top);
}

// The same search for another surface (the Classic start menu): sections of results, the file index
// built on first use, and run() that counts the choice like the search panel does.
export const searchEverything = {
  results: (query, apps) => buildResults(query, apps),
  prepare: () => buildFileIndex(),
  version: () => resultsVersion,
  run(item, reveal = false) {
    if (!item) return;
    noteUse(item.key);
    setTimeout(() => (reveal && item.reveal ? item.reveal() : item.run()), 0);
  },
};

// ---- preview ----

const previewCache = new Map();
function filePreview(f) {
  if (previewCache.has(f.path)) return previewCache.get(f.path);
  let p = { kind: 'info' };
  if (f.dir) {
    try { p = { kind: 'dir', items: fs.list(f.path).slice(0, 12) }; } catch (e) { p = { kind: 'info' }; }
  } else if (IMG.test(f.name)) p = { kind: 'image' };
  else if (TEXT.test(f.name) && f.size < 8 * 1024 * 1024) {
    try { p = { kind: 'text', text: fs.read(f.path).slice(0, 2400) }; } catch (e) { p = { kind: 'info' }; }
  }
  previewCache.set(f.path, p);
  if (previewCache.size > 60) previewCache.delete(previewCache.keys().next().value);
  return p;
}

function Preview({ item, th }) {
  if (!item) return null;
  const pv = item.preview || {};
  const W = 260;
  const box = (...kids) => h(View, { style: { flexDirection: 'row' } }, h(View, { style: { width: 1, backgroundColor: th.separator } }),
    h(View, { style: { width: W - 1, padding: 14, gap: 8 } }, ...kids));
  const title = (s) => h(Text, { style: { fontSize: 14, fontWeight: 700, color: th.text } }, s);
  const dim = (s) => h(Text, { style: { fontSize: 11, color: th.textDim, lineHeight: 1.35 } }, s);
  if (item.big) return box(h(Text, { style: { fontSize: 30, fontWeight: 700, color: th.text } }, item.big), dim(item.subtitle), dim(item.hint));
  if (pv.kind === 'app') {
    const a = pv.app;
    return box(h(View, { style: { alignItems: 'center', gap: 8, marginTop: 10 } }, h(a.Icon, { size: 72 }), title(a.name)),
      dim(a.comment || a.genericName || ''), dim(t(`store.kind.${a.kind}`)));
  }
  if (pv.kind === 'file') {
    const f = pv.file;
    const p = filePreview(f);
    return box(
      p.kind === 'image' ? h(Image, { src: f.path, style: { width: W - 28, height: 160, objectFit: 'contain' } }) : null,
      title(f.name),
      dim(`${f.path.replace(fs.home(), '~')}`),
      dim(f.dir ? t('shell.spotlight.folder') : `${fmtSize(f.size)} · ${formatDate(new Date(f.mtime), 'short')}`),
      p.kind === 'text' ? h(Text, { style: { fontSize: 10, fontFamily: 'monospace', color: th.text, maxHeight: 220, overflow: 'hidden', lineHeight: 1.3 } }, p.text) : null,
      p.kind === 'dir' ? h(View, { style: { gap: 2 } }, ...p.items.map((x) => h(Text, { key: x.name, style: { fontSize: 11, color: th.text } }, `${x.dir ? '📁' : '📄'} ${x.name}`))) : null);
  }
  if (pv.kind === 'note') return box(title(pv.note.title), h(Text, { style: { fontSize: 11, color: th.text, maxHeight: 300, overflow: 'hidden', lineHeight: 1.35 } }, pv.note.text.slice(0, 1500)));
  if (pv.kind === 'text') return box(h(Text, { style: { fontSize: 11, color: th.text, maxHeight: 300, overflow: 'hidden', lineHeight: 1.35 } }, pv.text.slice(0, 1500)));
  if (pv.kind === 'pkg') return box(item.image ? h(Image, { src: item.image, style: { width: 64, height: 64 } }) : null, title(item.title), dim(item.subtitle), dim(t('shell.spotlight.inStore')));
  return box(title(item.title), dim(item.subtitle || ''));
}

// Readable number: no extra zeros or exponential notation for common values.
function fmtNum(v, max = 10) {
  const r = Number(v.toFixed(max));
  if (Math.abs(r) >= 1e15 || (r !== 0 && Math.abs(r) < 1e-9)) return String(v.toPrecision(8));
  const [i, f = ''] = r.toFixed(max).split('.');
  const frac = f.replace(/0+$/, '');
  return formatNumber(Number(i), 0) + (frac ? (formatNumber(0.5, 1)[1] || '.') + frac : '');
}

const fmtSize = (n) => (n < 1024 ? `${n} B` : n < 1048576 ? `${formatNumber(n / 1024, 1)} KB` : n < 1073741824 ? `${formatNumber(n / 1048576, 1)} MB` : `${formatNumber(n / 1073741824, 1)} GB`);

// Rows that fit (section headers and items), in a window that follows the selection.
const ROWS_PX = 420, ROW_H = 34, HEAD_H = 22;
function visibleRows(sections, cur) {
  const rows = [];
  for (const s of sections) { rows.push({ header: s.id, h: HEAD_H }); for (const it of s.items) rows.push({ item: it, h: ROW_H }); }
  if (!rows.length) return rows;
  const at = Math.max(0, rows.findIndex((r) => r.item === cur));
  let start = 0, used = 0;
  for (let i = 0; i <= at; i++) used += rows[i].h;
  while (used > ROWS_PX - ROW_H && start < at) used -= rows[start++].h;
  if (start > 0 && rows[start - 1].header) start--;           // keep the section title visible
  const out = [];
  used = 0;
  for (let i = start; i < rows.length && used + rows[i].h <= ROWS_PX; i++) { out.push(rows[i]); used += rows[i].h; }
  return out;
}

// ---- panel ----

export function SpotlightPanel() {
  const open = useStore(spotlightOpen);
  if (!open) return null;
  return h(Panel);
}

function Panel() {
  const th = useTheme();
  const apps = useLauncherApps();
  const [query, setQuery] = useState('');
  const [sel, setSel] = useState(0);
  useStore(resultsVersion);
  useStore(clipboardHistory);
  const sections = useMemo(() => buildResults(query, apps), [query, apps, resultsVersion.get()]);
  const flat = sections.flatMap((s) => s.items);
  const cur = flat[Math.min(sel, flat.length - 1)] || null;
  useEffect(() => setSel(0), [query]);
  useEffect(() => { buildFileIndex(); }, []);
  const run = (item, reveal = false) => {
    if (!item) return;
    noteUse(item.key);
    spotlight.close();
    setTimeout(() => (reveal && item.reveal ? item.reveal() : item.run()), 0);
  };
  const onKey = (e) => {
    if (e.key === 'ArrowDown') { setSel((s) => Math.min(flat.length - 1, s + 1)); return true; }
    if (e.key === 'ArrowUp') { setSel((s) => Math.max(0, s - 1)); return true; }
    if (e.key === 'Enter') { run(cur, e.meta); return true; }
    if (e.key === 'Tab') { const i = sections.findIndex((s) => s.items.includes(cur)); const next = sections[(i + 1) % sections.length]; if (next) setSel(flat.indexOf(next.items[0])); return true; }
    return false;
  };
  const box = searchBox();
  const W = Math.min(720, box.width - 40);
  const listW = cur && query.trim() ? W - 260 : W;
  return h(View, { onClick: () => spotlight.close(), style: { position: 'absolute', ...box } },
    h(View, {
      onClick: (e) => e.stopPropagation(),
      style: {
        position: 'absolute', left: Math.round((box.width - W) / 2), top: Math.round(box.height * 0.16), width: W,
        borderRadius: 14, backgroundColor: th.windowBg, border: `1px solid ${th.separator}`, boxShadow: '0 24px 60px rgba(0,0,0,0.5)', overflow: 'hidden',
      },
    },
    h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: 10, paddingHorizontal: 14, paddingVertical: 10 } },
      h(Text, { style: { fontSize: 20, color: th.textDim } }, '⌕'),
      h(TextInput, { value: query, onChange: setQuery, autoFocus: true, size: 20, placeholder: t('shell.spotlight.placeholder'),
        onKeyDown: onKey, onCancel: () => spotlight.close(), style: { flex: 1, backgroundColor: 'transparent', border: '0 solid transparent' } })),
    query.trim() && h(View, { style: { height: 1, backgroundColor: th.separator } }),
    query.trim() && h(View, { style: { flexDirection: 'row', maxHeight: 440 } },
      h(View, { style: { width: listW, paddingVertical: 6, maxHeight: 440, overflow: 'hidden' } },
        flat.length === 0 && h(Text, { style: { fontSize: 13, color: th.textDim, padding: 14 } }, t('shell.spotlight.none')),
        ...visibleRows(sections, cur).map((r) => (r.header
          ? h(Text, { key: `h:${r.header}`, style: { fontSize: 10, fontWeight: 700, color: th.textDim, paddingHorizontal: 14, paddingTop: 6, paddingBottom: 2 } },
            t(`shell.spotlight.section.${r.header}`).toUpperCase())
          : (() => {
            const it = r.item;
            const mine = flat.indexOf(it);
            const active = it === cur;
            return h(View, {
              key: it.key, onClick: () => run(it), onMouseEnter: () => setSel(mine),
              style: { flexDirection: 'row', alignItems: 'center', gap: 10, marginHorizontal: 6, paddingHorizontal: 8, height: 34, borderRadius: 7,
                backgroundColor: active ? th.accent : 'transparent' },
            },
            it.Icon ? h(it.Icon, { size: 22 })
              : it.image ? h(Image, { src: it.image, style: { width: 22, height: 22, objectFit: 'contain' } })
                : h(Text, { style: { width: 22, fontSize: 15, textAlign: 'center', color: active ? '#fff' : th.text } }, it.glyph || '•'),
            h(Text, { style: { fontSize: 13, color: active ? '#fff' : th.text, whiteSpace: 'nowrap', overflow: 'hidden', maxWidth: listW * 0.55 } }, it.title),
            h(Text, { style: { flex: 1, fontSize: 11, color: active ? 'rgba(255,255,255,0.8)' : th.textDim, whiteSpace: 'nowrap', overflow: 'hidden' } }, it.subtitle || ''));
          })()))),
      cur && h(Preview, { item: cur, th })),
    h(View, { style: { height: 1, backgroundColor: th.separator } }),
    h(View, { style: { flexDirection: 'row', gap: 14, paddingHorizontal: 14, paddingVertical: 6 } },
      h(Text, { style: { fontSize: 10, color: th.textDim } }, t('shell.spotlight.keys')))));
}
