// Text Editor — our own TextMate-style code editor: project on the left,
// tabs, multiple cursors, find and replace (text or regex), go to line (❖L), open
// project file (❖T), our own syntax highlighting and input methods.
// See docs/EDITOR.md.
import {
  View, Text, TextInput, Button, ScrollView, ToolbarButton, direction, useTheme, useLocale, useAppWindow, useKeyboard, wm, fs, formatBytes, addListener, hasTextFocus, print, speechItems, registerSelection, ContextMenu,
} from '@lordos/sdk';
import { useEffect, useMemo, useReducer, useRef, useState } from 'react';
import { Doc, cursorAt, hasSel, selStart, selEnd } from './buffer.js';
import { LANGUAGES, languageFor, languageById, paletteFor } from './syntax.js';
import { EditorView } from './EditorView.jsx';

export function Icon({ size: s }) {
  const line = (w, c, top) => <View style={{ position: 'absolute', left: s * 0.3, top: s * top, width: s * w, height: s * 0.055, borderRadius: s * 0.03, backgroundColor: c }} />;
  return (
    <View style={{ width: s, height: s, borderRadius: s * 0.225, background: 'linear-gradient(160deg, #8b5cf6, #3b1f8f)',
      boxShadow: `0 ${s * 0.04}px ${s * 0.12}px rgba(0,0,0,0.35)`, overflow: 'hidden' }}>
      <View style={{ position: 'absolute', left: s * 0.2, top: s * 0.16, width: s * 0.6, height: s * 0.7, borderRadius: s * 0.06, backgroundColor: '#fbfaff',
        boxShadow: `0 ${s * 0.02}px ${s * 0.06}px rgba(0,0,0,0.3)` }} />
      {line(0.36, '#fc5fa3', 0.27)}{line(0.26, '#5dd8ff', 0.37)}{line(0.4, '#8b5cf6', 0.47)}{line(0.2, '#d0bf69', 0.57)}{line(0.32, '#67b7a4', 0.67)}
      <View style={{ position: 'absolute', left: s * 0.22, top: s * 0.255, width: s * 0.03, height: s * 0.48, backgroundColor: '#c4b5fd' }} />
    </View>
  );
}

const MAX_OPEN = 8 * 1024 * 1024;
const SKIP = new Set(['node_modules', '.git', 'build', 'dist', '.cache', '__pycache__', 'target']);

const tokensFor = (mode) => (mode === 'light'
  ? { editorBg: '#ffffff', gutterBg: '#f6f6f8', gutterText: '#a1a1aa', gutterActive: '#3f3f46', lineHighlight: '#f4f4fa', caret: '#2563eb',
      selection: '#b4d7ff', selectionBlur: '#e4e4e7', match: '#fde68a99', matchCurrent: '#f59e0b', scrollThumb: 'rgba(0,0,0,0.28)' }
  : { editorBg: '#1e1e23', gutterBg: '#1a1a1e', gutterText: '#5c5c66', gutterActive: '#d4d4d8', lineHighlight: '#26262c', caret: '#7aa8ff',
      selection: '#3b4f7a', selectionBlur: '#33333b', match: '#a1803066', matchCurrent: '#d6a12e', scrollThumb: 'rgba(255,255,255,0.28)' });

const relOf = (abs) => { const h = fs.home(); return abs === h ? '' : abs.startsWith(`${h}/`) ? abs.slice(h.length + 1) : abs; };
const baseName = (p) => String(p).split('/').filter(Boolean).pop() || '';
const dirOf = (p) => String(p).split('/').slice(0, -1).join('/');

let nextTabId = 1;
function newTab(path, text) {
  const doc = new Doc(text);
  return { id: nextTabId++, path, doc, langId: languageFor(path || '').id, tabSize: /\.(go|c|h|cpp|mk)$|Makefile/.test(path || '') ? 4 : 2,
    useTabs: /\t/.test(text.slice(0, 20000).split('\n').find((l) => /^\s/.test(l)) || '') || /Makefile$|\.go$/.test(path || '') };
}

// Fuzzy score (like TextMate's Go to File): letters in order; worth more at the start of a word.
function fuzzy(q, s) {
  const a = q.toLowerCase(), b = s.toLowerCase();
  let score = 0, j = 0, prev = -2;
  for (let i = 0; i < a.length; i++) {
    const k = b.indexOf(a[i], j);
    if (k < 0) return -1;
    score += k === prev + 1 ? 5 : 1;
    if (k === 0 || /[/._ -]/.test(b[k - 1])) score += 4;
    prev = k;
    j = k + 1;
  }
  return score - b.length * 0.01;
}

function listFiles(root, limit = 4000) {
  const out = [];
  const walk = (rel, depth) => {
    if (out.length >= limit || depth > 8) return;
    let entries = [];
    try { entries = fs.list(rel || '.'); } catch (e) { return; }
    for (const e of entries) {
      const p = rel ? `${rel}/${e.name}` : e.name;
      if (e.dir) { if (!SKIP.has(e.name)) walk(p, depth + 1); } else out.push(p);
      if (out.length >= limit) return;
    }
  };
  walk(root, 0);
  return out;
}

// ---- project panel ----

function Tree({ t, root, open, expanded, onToggle, active }) {
  const rows = [];
  const walk = (rel, depth) => {
    let entries = [];
    try { entries = fs.list(rel || '.'); } catch (e) { return; }
    for (const e of entries) {
      const p = rel ? `${rel}/${e.name}` : e.name;
      rows.push({ p, name: e.name, dir: e.dir, depth });
      if (e.dir && expanded.has(p) && rows.length < 3000) walk(p, depth + 1);
      if (rows.length >= 3000) return;
    }
  };
  walk(root, 0);
  return rows.map((r) => (
    <View key={r.p} onClick={() => (r.dir ? onToggle(r.p) : open(`${fs.home()}/${r.p}`))}
      style={{ height: 24, flexDirection: 'row', alignItems: 'center', paddingLeft: 6 + r.depth * 14, paddingRight: 8, gap: 4,
        backgroundColor: active === `${fs.home()}/${r.p}` ? t.accent : 'transparent', borderRadius: 6 }}>
      <Text style={{ width: 12, fontSize: 10, textAlign: 'center', color: active === `${fs.home()}/${r.p}` ? '#fff' : t.textDim }}>{r.dir ? (expanded.has(r.p) ? '▾' : '▸') : ''}</Text>
      <Text style={{ flex: 1, fontSize: 13, color: active === `${fs.home()}/${r.p}` ? '#fff' : t.text, whiteSpace: 'nowrap', overflow: 'hidden' }}>{r.name}</Text>
    </View>
  ));
}

// ---- file dialog (open, open folder, save as) ----

function FileDialog({ t, tr, mode, start, initialName, onPick, onCancel }) {
  const [dir, setDir] = useState(start || '');
  const [name, setName] = useState(initialName || '');
  let entries = [];
  let error = null;
  try { entries = fs.list(dir || '.'); } catch (e) { error = e.message; }
  const title = tr(`editor.dialog.${mode}`);
  const pickDir = mode === 'folder';
  return (
    <View onClick={onCancel} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.35)', alignItems: 'center' }}>
      <View onClick={(e) => e.stopPropagation()} style={{ marginTop: 40, width: 480, height: 400, padding: 20, gap: 12, borderRadius: 12,
        backgroundColor: t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 16px 40px rgba(0,0,0,0.45)' }}>
        <Text style={{ fontSize: 15, fontWeight: 600, color: t.text }}>{title}</Text>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
          <ToolbarButton icon={direction() === 'rtl' ? '›' : '‹'} iconSize={18} onClick={() => setDir(dirOf(dir))} />
          <Text style={{ flex: 1, fontSize: 12, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{`~/${dir}`}</Text>
        </View>
        <View style={{ flex: 1, borderRadius: 8, border: `1px solid ${t.separator}` }}>
          <ScrollView style={{ padding: 4 }}>
            {error && <Text style={{ fontSize: 12, color: '#ff453a', margin: 6 }}>{error}</Text>}
            {entries.map((e) => (
              <View key={e.name} onClick={() => { if (e.dir) setDir(dir ? `${dir}/${e.name}` : e.name); else if (mode === 'save') setName(e.name); else if (!pickDir) onPick(`${fs.home()}/${dir ? `${dir}/` : ''}${e.name}`); }}
                style={{ height: 26, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 8, gap: 8, borderRadius: 6 }}>
                <Text style={{ width: 14, fontSize: 11, color: t.textDim }}>{e.dir ? '▸' : ''}</Text>
                <Text style={{ flex: 1, fontSize: 13, color: e.dir || !pickDir ? t.text : t.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{e.name}</Text>
                {!e.dir && <Text style={{ fontSize: 11, color: t.textDim }}>{formatBytes(e.size)}</Text>}
              </View>
            ))}
          </ScrollView>
        </View>
        {mode === 'save' && <TextInput value={name} onChange={setName} placeholder={tr('editor.dialog.name')} autoFocus
          onSubmit={() => name && onPick(`${fs.home()}/${dir ? `${dir}/` : ''}${name}`)} onCancel={onCancel} />}
        <View style={{ flexDirection: 'row', gap: 8, justifyContent: 'flex-end' }}>
          <Button label={tr('editor.cancel')} onClick={onCancel} />
          {mode === 'save' && <Button primary label={tr('editor.save')} onClick={() => name && onPick(`${fs.home()}/${dir ? `${dir}/` : ''}${name}`)} />}
          {pickDir && <Button primary label={tr('editor.dialog.useFolder')} onClick={() => onPick(dir)} />}
        </View>
      </View>
    </View>
  );
}

// List with search (go to file, choose language).
function Picker({ t, title, placeholder, items, onPick, onCancel, render = (x) => x.label }) {
  const { focused } = useAppWindow();   // back from a minimize, the field takes the keyboard again
  const [q, setQ] = useState('');
  const [sel, setSel] = useState(0);
  const shown = useMemo(() => {
    if (!q) return items.slice(0, 40);
    return items.map((x) => [x, fuzzy(q, x.key)]).filter(([, s]) => s >= 0).sort((a, b) => b[1] - a[1]).slice(0, 40).map(([x]) => x);
  }, [q, items]);
  return (
    <View onClick={onCancel} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, alignItems: 'center' }}>
      <View onClick={(e) => e.stopPropagation()} style={{ marginTop: 30, width: 480, padding: 12, gap: 8, borderRadius: 12,
        backgroundColor: t.windowBg, border: `1px solid ${t.separator}`, boxShadow: '0 16px 40px rgba(0,0,0,0.45)' }}>
        {title && <Text style={{ fontSize: 11, fontWeight: 600, color: t.textDim, marginLeft: 4 }}>{title}</Text>}
        <TextInput key={focused ? 'on' : 'off'} value={q} onChange={(v) => { setQ(v); setSel(0); }} placeholder={placeholder} autoFocus onCancel={onCancel}
          onSubmit={() => shown[sel] && onPick(shown[sel])}
          onKeyDown={(e) => {
            if (e.key === 'ArrowDown') { setSel((s) => Math.min(shown.length - 1, s + 1)); return true; }
            if (e.key === 'ArrowUp') { setSel((s) => Math.max(0, s - 1)); return true; }
            return false;
          }} />
        <View style={{ maxHeight: 330, overflow: 'hidden' }}>
          {shown.map((x, i) => (
            <View key={x.key} onClick={() => onPick(x)} style={{ height: 28, justifyContent: 'center', paddingHorizontal: 10, borderRadius: 6,
              backgroundColor: i === sel ? t.accent : 'transparent' }}>
              {render(x, i === sel)}
            </View>
          ))}
        </View>
      </View>
    </View>
  );
}

// ---- search ----

function findMatches(doc, q, { regex, caseSensitive }) {
  if (!q) return { list: [], error: null };
  let re;
  try { re = new RegExp(regex ? q : q.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'), caseSensitive ? 'g' : 'gi'); } catch (e) { return { list: [], error: e.message }; }
  const list = [];
  for (let l = 0; l < doc.lineCount && list.length < 5000; l++) {
    const s = doc.line(l);
    re.lastIndex = 0;
    for (let m = re.exec(s); m && list.length < 5000; m = re.exec(s)) {
      if (m[0].length === 0) { re.lastIndex++; continue; }
      list.push({ l, c: m.index, len: m[0].length, text: m[0] });
    }
  }
  return { list, error: null, re };
}

function FindBar({ t, tr, find, setFind, count, current, error, onNext, onPrev, onReplace, onReplaceAll, onClose }) {
  const toggle = (k) => setFind({ ...find, [k]: !find[k] });
  const Chip = ({ k, label, title }) => (
    <ToolbarButton label={label} title={title} active={find[k]} onClick={() => toggle(k)} style={{ minWidth: 34 }} />
  );
  return (
    <View style={{ paddingVertical: 8, paddingHorizontal: 12, gap: 8, backgroundColor: t.sidebarBg }}>
      <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 1, backgroundColor: t.separator }} />
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        <TextInput value={find.query} onChange={(query) => setFind({ ...find, query })} placeholder={tr('editor.find.placeholder')} autoFocus style={{ flex: 1 }}
          onSubmit={onNext} onCancel={onClose}
          onKeyDown={(e) => { if (e.key === 'Enter' && e.shift) { onPrev(); return true; } return false; }} />
        <Chip k="regex" label=".*" title={tr('editor.find.regex')} />
        <Chip k="caseSensitive" label="Aa" title={tr('editor.find.case')} />
        <Text style={{ width: 92, fontSize: 11, color: error ? '#ff453a' : t.textDim, textAlign: 'center' }}>
          {error ? tr('editor.find.badRegex') : find.query ? tr('editor.find.count', { current: count ? current + 1 : 0, count }) : ''}</Text>
        <ToolbarButton icon="↑" title={tr('editor.find.prev')} onClick={onPrev} />
        <ToolbarButton icon="↓" title={tr('editor.find.next')} onClick={onNext} />
        <ToolbarButton icon="✕" iconSize={12} title={tr('editor.find.close')} onClick={onClose} />
      </View>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
        <TextInput value={find.replace} onChange={(replace) => setFind({ ...find, replace })} placeholder={tr('editor.find.replacePlaceholder')} style={{ flex: 1 }}
          onSubmit={onReplace} onCancel={onClose} />
        <Button label={tr('editor.find.replace')} onClick={onReplace} />
        <Button label={tr('editor.find.replaceAll')} onClick={onReplaceAll} />
      </View>
    </View>
  );
}

// ---- window ----

export default function Editor() {
  const th = useTheme();
  const { t: tr } = useLocale();
  const { id: wid, args, focused } = useAppWindow();
  const [, bump] = useReducer((x) => x + 1, 0);
  const [tabs, setTabs] = useState(() => [newTab(null, '')]);
  const [activeId, setActiveId] = useState(tabs[0].id);
  const [root, setRoot] = useState('');
  const [expanded, setExpanded] = useState(() => new Set());
  const [overlay, setOverlay] = useState(null);       // 'open' | 'folder' | 'saveAs' | 'gotoFile' | 'gotoLine' | 'lang' | {confirm}
  const [dropOver, setDropOver] = useState(null);     // files dragged from Files over the window: how many
  const [find, setFind] = useState({ open: false, query: '', replace: '', regex: false, caseSensitive: false });
  const [msg, setMsg] = useState(null);
  const [lineQuery, setLineQuery] = useState('');
  const api = useRef(null);
  const tab = tabs.find((x) => x.id === activeId) || tabs[0];
  const doc = tab.doc;
  const lang = languageById(tab.langId);
  const palette = paletteFor(th.mode);
  const t = { ...th, ...tokensFor(th.mode) };

  // ---- files ----
  // setRoot: when opening from Files or the dialog (not from the tree), the project becomes the
  // file's folder if none had been chosen yet
  const openPath = (abs, { fromTree = false } = {}) => {
    const existing = tabs.find((x) => x.path === abs);
    if (existing) { setActiveId(existing.id); setOverlay(null); return; }
    try {
      const st = fs.list(dirOf(relOf(abs)) || '.').find((e) => e.name === baseName(abs));
      if (st && st.size > MAX_OPEN) throw new Error(tr('editor.tooBig', { size: formatBytes(st.size) }));
      const text = fs.read(relOf(abs));
      if (text.indexOf('\u0000') >= 0) throw new Error(tr('editor.binary'));
      const nt = newTab(abs, text);
      setTabs((ts) => {
        // an empty "Untitled" tab is replaced
        const only = ts.length === 1 && !ts[0].path && !ts[0].doc.dirty && ts[0].doc.text() === '';
        return only ? [nt] : [...ts, nt];
      });
      setActiveId(nt.id);
      setOverlay(null);
      setMsg(null);
      if (!fromTree && !root && !tabs.some((x) => x.path)) setRoot(dirOf(relOf(abs)));
    } catch (e) { setMsg({ ok: false, text: `${baseName(abs)}: ${e.message}` }); setOverlay(null); }
  };

  const saveTo = (tb, abs) => {
    try {
      fs.write(relOf(abs), tb.doc.text());
      tb.doc.markSaved();
      if (tb.path !== abs) {
        tb.path = abs;
        tb.langId = languageFor(abs).id;
      }
      setMsg({ ok: true, text: tr('editor.saved', { name: baseName(abs) }) });
      setTabs((ts) => [...ts]);
      return true;
    } catch (e) { setMsg({ ok: false, text: tr('editor.saveError', { error: e.message }) }); return false; }
  };
  const save = (tb = tab) => (tb.path ? saveTo(tb, tb.path) : (setOverlay('saveAs'), false));

  const closeTab = (tb, force = false) => {
    if (!force && tb.doc.dirty) { setActiveId(tb.id); setOverlay({ confirm: tb.id }); return; }
    setTabs((ts) => {
      const rest = ts.filter((x) => x.id !== tb.id);
      if (!rest.length) { const n = newTab(null, ''); setActiveId(n.id); return [n]; }
      if (tb.id === activeId) setActiveId(rest[Math.max(0, ts.findIndex((x) => x.id === tb.id) - 1)].id);
      return rest;
    });
  };

  // open from Files: wm.open('editor', { args: { path } })
  useEffect(() => { if (args?.path) openPath(args.path); }, [args?.nonce]);

  // closing the window with unsaved changes: ask
  useEffect(() => wm.setCloseGuard(wid, () => {
    const d = tabs.find((x) => x.doc.dirty);
    if (!d) return true;
    setActiveId(d.id);
    setOverlay({ confirm: d.id, window: true });
    return false;
  }), [tabs]);

  // window title
  useEffect(() => { wm.setTitle(wid, `${tab.path ? baseName(tab.path) : tr('editor.untitled')}${doc.dirty ? ' •' : ''}`); });

  // ---- search ----
  const found = useMemo(() => (find.open ? findMatches(doc, find.query, find) : { list: [] }), [find, doc.version, doc]);
  const primary = doc.cursors[doc.cursors.length - 1];
  const current = Math.max(0, found.list.findIndex((m) => m.l > primary.l || (m.l === primary.l && m.c + m.len >= primary.c)));
  const selectMatch = (m) => { if (m) api.current.goto({ l: m.l, c: m.c }, { l: m.l, c: m.c + m.len }); };
  const nextMatch = () => {
    if (!found.list.length) return;
    const k = doc.cursors[doc.cursors.length - 1];
    const after = selEnd(k);
    const m = found.list.find((x) => x.l > after.l || (x.l === after.l && x.c >= after.c)) || found.list[0];
    selectMatch(m);
  };
  const prevMatch = () => {
    if (!found.list.length) return;
    const before = selStart(doc.cursors[doc.cursors.length - 1]);
    const m = [...found.list].reverse().find((x) => x.l < before.l || (x.l === before.l && x.c < before.c)) || found.list[found.list.length - 1];
    selectMatch(m);
  };
  const replacement = (m) => (find.regex ? m.text.replace(new RegExp(found.re.source, found.re.flags.replace('g', '')), find.replace) : find.replace);
  const replaceOne = () => {
    const k = doc.cursors[doc.cursors.length - 1];
    const s = selStart(k);
    const m = found.list.find((x) => x.l === s.l && x.c === s.c && hasSel(k) && x.len === doc.textIn(selStart(k), selEnd(k)).length);
    if (m) {
      doc.transact([{ from: { l: m.l, c: m.c }, to: { l: m.l, c: m.c + m.len }, text: replacement(m) }], (rs) => [cursorAt(rs[0].end)]);
      bump();
    }
    setTimeout(nextMatch, 0);
  };
  const replaceAll = () => {
    if (!found.list.length) return;
    const n = found.list.length;
    doc.transact(found.list.map((m) => ({ from: { l: m.l, c: m.c }, to: { l: m.l, c: m.c + m.len }, text: replacement(m) })), (rs) => [cursorAt(rs[rs.length - 1].end)]);
    setMsg({ ok: true, text: tr('editor.find.replaced', { count: n }) });
    bump();
  };

  // ---- commands (keyboard and menus) ----
  const run = (name) => {
    const a = api.current;
    switch (name) {
      case 'new': { const n = newTab(null, ''); setTabs((ts) => [...ts, n]); setActiveId(n.id); break; }
      case 'open': setOverlay('open'); break;
      case 'openFolder': setOverlay('folder'); break;
      case 'save': save(); break;
      case 'saveAs': setOverlay('saveAs'); break;
      case 'close': closeTab(tab); break;
      case 'find': setFind((f) => ({ ...f, open: true, query: f.query || (hasSel(primary) && primary.l === primary.al ? doc.textIn(selStart(primary), selEnd(primary)) : '') })); break;
      case 'findNext': nextMatch(); break;
      case 'findPrev': prevMatch(); break;
      case 'gotoLine': setLineQuery(''); setOverlay('gotoLine'); break;
      case 'gotoFile': setOverlay('gotoFile'); break;
      case 'language': setOverlay('lang'); break;
      // print with the system dialog (M9): the text is laid out as a PDF in lord-doc
      case 'print': print.open({ title: tab.path ? baseName(tab.path) : tr('editor.untitled'), text: doc.text(), mono: true }); break;
      case 'toggleProject': setRoot((r) => (r === null ? '' : null)); break;
      case 'nextTab': { const i = tabs.findIndex((x) => x.id === tab.id); setActiveId(tabs[(i + 1) % tabs.length].id); break; }
      case 'prevTab': { const i = tabs.findIndex((x) => x.id === tab.id); setActiveId(tabs[(i - 1 + tabs.length) % tabs.length].id); break; }
      default: if (a && a[name]) a[name]();
    }
  };
  // ❖ or Ctrl shortcuts that are not editing ones (EditorView handles the editing ones)
  const onCommand = (b, shift) => {
    const map = { s: shift ? 'saveAs' : 'save', o: shift ? 'openFolder' : 'open', n: 'new', w: 'close', f: 'find', g: shift ? 'findPrev' : 'findNext',
      l: 'gotoLine', t: 'gotoFile', '\\': 'toggleProject', p: 'print' };
    if (b === 'escape') { if (find.open) setFind((f) => ({ ...f, open: false })); return; }
    if (map[b]) run(map[b]);
  };
  // Ctrl+Tab switches tabs; with focus in a field (find, replace), the window
  // shortcuts (save, open, go to…) keep working: EditorView does not receive them in that case
  const selectedText = () => doc.cursors.filter(hasSel).map((k) => doc.textIn(selStart(k), selEnd(k))).join('\n');
  // Speech reads the selection, or the whole text when nothing is selected
  const speechText = () => latest.current.selectedText() || latest.current.doc.text();
  // right click on the text: the clipboard and Speech (sdk/contextmenu.jsx)
  const column = useRef(null);
  const [ctx, setCtx] = useState(null);
  const onContextMenu = (e) => {
    const p = column.current ? e.local(column.current) : { x: e.x, y: e.y };
    const any = doc.cursors.some(hasSel);
    setCtx({ x: p.x, y: p.y, items: [
      { label: tr('editor.cmd.cut'), hint: '❖X', disabled: !any, onClick: () => run('cut') },
      { label: tr('editor.cmd.copy'), hint: '❖C', disabled: !any, onClick: () => run('copy') },
      { label: tr('editor.cmd.paste'), hint: '❖V', onClick: () => run('paste') },
      '-',
      { label: tr('editor.cmd.selectAll'), hint: '❖A', onClick: () => run('selectAll') },
      '-',
      ...speechItems(speechText),
    ].filter((x, i, a) => !(x === '-' && (i === a.length - 1 || a[i + 1] === '-'))) });
  };
  const latest = useRef(null);
  latest.current = { onCommand, run, selectedText, doc };
  // ❖Esc reads the selection aloud (sdk/voice.js)
  useEffect(() => registerSelection(wid, () => latest.current.selectedText()), [wid]);
  useKeyboard((e) => { if ((e.ctrl || e.meta) && e.key === 'Tab') latest.current.run(e.shift ? 'prevTab' : 'nextTab'); });
  useEffect(() => {
    if (!focused) return undefined;
    return addListener('keydown', (e) => {
      if (!hasTextFocus() || !(e.ctrl || e.meta) || e.alt) return;
      const b = (e.base || '').toLowerCase();
      if (['s', 'o', 'n', 'w', 't', 'l', 'g', 'p'].includes(b)) latest.current.onCommand(b, e.shift);
    });
  }, [focused]);

  // ---- app menus ----
  useEffect(() => {
    const it = (label, action, hint, extra = {}) => ({ label: tr(label), hint, onClick: () => run(action), ...extra });
    wm.setMenus(wid, [
      { label: tr('editor.menu.file'), items: [it('editor.cmd.new', 'new', '❖N'), it('editor.cmd.open', 'open', '❖O'), it('editor.cmd.openFolder', 'openFolder', '❖⇧O'), '-',
        it('editor.cmd.save', 'save', '❖S'), it('editor.cmd.saveAs', 'saveAs', '❖⇧S'), '-',
        // print with the system dialog (M9): the text is laid out as a PDF in lord-doc
        it('editor.cmd.print', 'print', '❖P'), '-',
        it('editor.cmd.close', 'close', '❖W')] },
      { label: tr('editor.menu.edit'), items: [it('editor.cmd.undo', 'undo', '❖Z'), it('editor.cmd.redo', 'redo', '❖⇧Z'), '-', it('editor.cmd.cut', 'cut', '❖X'),
        it('editor.cmd.copy', 'copy', '❖C'), it('editor.cmd.paste', 'paste', 'Ctrl+V'), it('editor.cmd.selectAll', 'selectAll', '❖A'), '-',
        it('editor.cmd.selectNext', 'selectNext', '❖D'), it('editor.cmd.singleCursor', 'singleCursor', 'Esc'), '-', it('editor.cmd.find', 'find', '❖F'),
        it('editor.cmd.findNext', 'findNext', '❖G'), '-', ...speechItems(speechText), it('editor.cmd.comment', 'toggleComment', '❖/'), it('editor.cmd.duplicate', 'duplicateLines', '❖⇧D'),
        it('editor.cmd.indent', 'indent', '❖]'), it('editor.cmd.outdent', 'outdent', '❖['), it('editor.cmd.moveUp', 'moveUp', '⌥↑'), it('editor.cmd.moveDown', 'moveDown', '⌥↓')] },
      { label: tr('editor.menu.view'), items: [it('editor.cmd.toggleProject', 'toggleProject', '❖\\'), it('editor.cmd.language', 'language', '')] },
      { label: tr('editor.menu.go'), items: [it('editor.cmd.gotoLine', 'gotoLine', '❖L'), it('editor.cmd.gotoFile', 'gotoFile', '❖T'),
        it('editor.cmd.nextTab', 'nextTab', 'Ctrl+Tab'), it('editor.cmd.prevTab', 'prevTab', 'Ctrl+⇧Tab')] },
    ]);
  }, [tabs, activeId, find, doc.version]);

  const files = useMemo(() => (overlay === 'gotoFile' ? listFiles(root || '').map((p) => ({ key: p, path: p })) : []), [overlay, root]);
  const confirmTab = overlay?.confirm && tabs.find((x) => x.id === overlay.confirm);
  const k = primary;
  const col = [...doc.line(k.l).slice(0, k.c).replace(/\t/g, ' '.repeat(tab.tabSize))].length + 1;
  const nsel = doc.cursors.filter(hasSel).reduce((n, x) => n + doc.textIn(selStart(x), selEnd(x)).length, 0);
  const editorOverlay = !!overlay;

  // files dragged from Files: a file opens in a tab, a folder becomes the project
  const dropFiles = (paths) => {
    setDropOver(null);
    for (const pth of paths || []) {
      let st = null;
      try { st = fs.stat(pth); } catch (e) { st = null; }
      if (st && st.dir) { setRoot(relOf(pth)); setExpanded(new Set()); } else openPath(pth);
    }
  };
  return (
    <View onFilesDrop={dropFiles} onFilesOver={(paths) => setDropOver(paths ? paths.length : null)}
      style={{ flex: 1, flexDirection: 'row', backgroundColor: t.editorBg }}>
      {root !== null && (
        <View style={{ width: 220, backgroundColor: t.sidebarBg }}>
          <View style={{ position: 'absolute', right: 0, top: 0, bottom: 0, width: 1, backgroundColor: t.separator }} />
          <View style={{ height: 34, flexDirection: 'row', alignItems: 'center', paddingLeft: 18, paddingRight: 8, gap: 6 }}>
            <Text style={{ flex: 1, fontSize: 11, fontWeight: 600, color: t.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>
              {root ? baseName(root) : tr('editor.home')}</Text>
            <ToolbarButton icon="⋯" small title={tr('editor.cmd.openFolder')} onClick={() => setOverlay('folder')} />
          </View>
          <ScrollView style={{ flex: 1, paddingHorizontal: 8 }}>
            <Tree t={t} root={root} open={(p) => openPath(p, { fromTree: true })} active={tab.path} expanded={expanded}
              onToggle={(p) => setExpanded((s) => { const n = new Set(s); if (n.has(p)) n.delete(p); else n.add(p); return n; })} />
          </ScrollView>
        </View>
      )}
      <View ref={column} style={{ flex: 1 }}>
        {/* tabs */}
        <View style={{ height: 34, flexDirection: 'row', backgroundColor: t.sidebarBg, direction: 'ltr' }}>
          <View style={{ position: 'absolute', left: 0, right: 0, bottom: 0, height: 1, backgroundColor: t.separator }} />
          {tabs.map((x) => {
            const on = x.id === tab.id;
            return (
              <View key={x.id} onClick={() => setActiveId(x.id)} style={{ maxWidth: 200, paddingLeft: 14, paddingRight: 8, flexDirection: 'row', alignItems: 'center', gap: 6,
                backgroundColor: on ? t.editorBg : 'transparent' }}>
                <View style={{ position: 'absolute', right: 0, top: 0, bottom: 0, width: 1, backgroundColor: t.separator }} />
                <Text style={{ fontSize: 12, color: on ? t.text : t.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{x.path ? baseName(x.path) : tr('editor.untitled')}</Text>
                <View onClick={(e) => { e.stopPropagation(); closeTab(x); }} style={{ width: 16, height: 16, borderRadius: 4, alignItems: 'center', justifyContent: 'center' }}>
                  <Text style={{ fontSize: x.doc.dirty ? 10 : 12, color: x.doc.dirty ? t.text : t.textDim }}>{x.doc.dirty ? '●' : '×'}</Text>
                </View>
              </View>
            );
          })}
        </View>
        <EditorView key={tab.id} doc={doc} lang={lang} t={t} palette={palette} settings={{ tabSize: tab.tabSize, useTabs: tab.useTabs }}
          matches={find.open ? found.list.map((m, i) => ({ ...m, current: i === current })) : null}
          onChange={bump} onCommand={onCommand} apiRef={api} overlay={editorOverlay} onContextMenu={onContextMenu} />
        {find.open && <FindBar t={t} tr={tr} find={find} setFind={setFind} count={found.list.length} current={current} error={found.error}
          onNext={nextMatch} onPrev={prevMatch} onReplace={replaceOne} onReplaceAll={replaceAll} onClose={() => setFind((f) => ({ ...f, open: false }))} />}
        {ctx && <ContextMenu x={ctx.x} y={ctx.y} items={ctx.items} onClose={() => setCtx(null)}
          bounds={column.current ? { w: lord.getLayout(column.current.id)[2], h: lord.getLayout(column.current.id)[3] } : undefined} />}
        {/* status bar */}
        <View style={{ height: 26, flexDirection: 'row', alignItems: 'center', gap: 16, paddingHorizontal: 14, backgroundColor: t.sidebarBg,
          direction: 'ltr' }}>
          <View style={{ position: 'absolute', left: 0, right: 0, top: 0, height: 1, backgroundColor: t.separator }} />
          <Text style={{ fontSize: 11, color: t.textDim }}>{tr('editor.status.position', { line: k.l + 1, col })}</Text>
          {doc.cursors.length > 1 && <Text style={{ fontSize: 11, color: t.accent }}>{tr('editor.status.cursors', { count: doc.cursors.length })}</Text>}
          {nsel > 0 && <Text style={{ fontSize: 11, color: t.textDim }}>{tr('editor.status.selected', { count: nsel })}</Text>}
          {msg && <Text style={{ fontSize: 11, color: msg.ok ? '#30d158' : '#ff453a', whiteSpace: 'nowrap', overflow: 'hidden' }}>{msg.text}</Text>}
          <View style={{ flex: 1 }} />
          <View onClick={() => setOverlay('lang')}><Text style={{ fontSize: 11, color: t.text }}>{lang.id === 'plain' ? tr('editor.plain') : lang.name}</Text></View>
          <View onClick={() => { tab.useTabs = !tab.useTabs; bump(); }}>
            <Text style={{ fontSize: 11, color: t.text }}>{tab.useTabs ? tr('editor.status.tabs', { n: tab.tabSize }) : tr('editor.status.spaces', { n: tab.tabSize })}</Text>
          </View>
          <View onClick={() => { tab.tabSize = tab.tabSize === 2 ? 4 : tab.tabSize === 4 ? 8 : 2; bump(); }}>
            <Text style={{ fontSize: 11, color: t.textDim }}>{`⇥ ${tab.tabSize}`}</Text>
          </View>
          <Text style={{ fontSize: 11, color: t.textDim }}>UTF-8</Text>
          <View onClick={() => { doc.eol = doc.eol === '\n' ? '\r\n' : '\n'; doc.touch(); bump(); }}>
            <Text style={{ fontSize: 11, color: t.text }}>{doc.eol === '\n' ? 'LF' : 'CRLF'}</Text>
          </View>
        </View>
      </View>

      {(overlay === 'open' || overlay === 'saveAs' || overlay === 'folder') && (
        <FileDialog t={t} tr={tr} mode={overlay === 'saveAs' ? 'save' : overlay === 'folder' ? 'folder' : 'open'}
          start={tab.path ? dirOf(relOf(tab.path)) : root || ''} initialName={tab.path ? baseName(tab.path) : ''}
          onCancel={() => setOverlay(null)}
          onPick={(p) => {
            if (overlay === 'folder') { setRoot(p); setExpanded(new Set()); setOverlay(null); return; }
            if (overlay === 'saveAs') { if (saveTo(tab, p)) setOverlay(null); return; }
            openPath(p);
          }} />
      )}
      {overlay === 'gotoFile' && (
        <Picker t={t} title={tr('editor.gotoFile.title', { folder: root ? baseName(root) : tr('editor.home') })} placeholder={tr('editor.gotoFile.placeholder')}
          items={files} onCancel={() => setOverlay(null)} onPick={(x) => openPath(`${fs.home()}/${x.path}`)}
          render={(x, on) => (
            <View style={{ flexDirection: 'row', gap: 8 }}>
              <Text style={{ fontSize: 12, fontWeight: 600, color: on ? '#fff' : t.text }}>{baseName(x.path)}</Text>
              <Text style={{ fontSize: 11, color: on ? '#fff' : t.textDim, whiteSpace: 'nowrap', overflow: 'hidden' }}>{dirOf(x.path)}</Text>
            </View>
          )} />
      )}
      {overlay === 'lang' && (
        <Picker t={t} title={tr('editor.languageTitle')} placeholder={tr('editor.languagePlaceholder')}
          items={LANGUAGES.map((l) => ({ key: l.id === 'plain' ? tr('editor.plain') : l.name, id: l.id }))}
          onCancel={() => setOverlay(null)} onPick={(x) => { tab.langId = x.id; setOverlay(null); bump(); }}
          render={(x, on) => <Text style={{ fontSize: 12, color: on ? '#fff' : t.text }}>{x.key}</Text>} />
      )}
      {overlay === 'gotoLine' && (
        <View onClick={() => setOverlay(null)} style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, alignItems: 'center' }}>
          <View onClick={(e) => e.stopPropagation()} style={{ marginTop: 30, width: 320, padding: 12, gap: 8, borderRadius: 12, backgroundColor: t.windowBg,
            border: `1px solid ${t.separator}`, boxShadow: '0 16px 40px rgba(0,0,0,0.45)' }}>
            <Text style={{ fontSize: 11, fontWeight: 600, color: t.textDim, marginLeft: 4 }}>{tr('editor.gotoLine.title', { count: doc.lineCount })}</Text>
            <TextInput value={lineQuery} onChange={(v) => setLineQuery(v.replace(/[^\d:]/g, ''))} placeholder={tr('editor.gotoLine.placeholder')} autoFocus
              onCancel={() => setOverlay(null)}
              onSubmit={() => {
                const [l, c2] = lineQuery.split(':').map((x) => parseInt(x, 10));
                if (l) api.current.goto({ l: l - 1, c: (c2 || 1) - 1 });
                setOverlay(null);
              }} />
          </View>
        </View>
      )}
      {confirmTab && (
        <View style={{ position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.35)', alignItems: 'center' }}>
          <View style={{ marginTop: 60, width: 400, padding: 20, gap: 8, borderRadius: 12, backgroundColor: t.windowBg, border: `1px solid ${t.separator}`,
            boxShadow: '0 16px 40px rgba(0,0,0,0.45)' }}>
            <Text style={{ fontSize: 15, fontWeight: 600, color: t.text }}>{tr('editor.confirm.title', { name: confirmTab.path ? baseName(confirmTab.path) : tr('editor.untitled') })}</Text>
            <Text style={{ fontSize: 13, color: t.textDim, lineHeight: 1.4 }}>{tr('editor.confirm.body')}</Text>
            <View style={{ flexDirection: 'row', gap: 8, justifyContent: 'flex-end', marginTop: 12 }}>
              <Button label={tr('editor.confirm.discard')} onClick={() => {
                const win = overlay.window;
                confirmTab.doc.markSaved();
                setOverlay(null);
                if (win) { if (!tabs.some((x) => x.doc.dirty && x !== confirmTab)) wm.close(wid, true); } else closeTab(confirmTab, true);
              }} />
              <Button label={tr('editor.cancel')} onClick={() => setOverlay(null)} />
              <Button primary label={tr('editor.save')} onClick={() => {
                const win = overlay.window;
                if (!confirmTab.path) { setOverlay('saveAs'); return; }
                if (saveTo(confirmTab, confirmTab.path)) {
                  setOverlay(null);
                  if (win) { if (!tabs.some((x) => x.doc.dirty && x !== confirmTab)) wm.close(wid, true); } else closeTab(confirmTab, true);
                }
              }} />
            </View>
          </View>
        </View>
      )}
      {/* over everything else, so it is not drawn under the panes */}
      {dropOver ? (
        <View style={{ position: 'absolute', left: 8, top: 8, right: 8, bottom: 8, borderRadius: 10, pointerEvents: 'none', alignItems: 'center', justifyContent: 'center',
          border: `2px dashed ${t.accent}`, backgroundColor: 'rgba(10,132,255,0.10)' }}>
          <Text style={{ fontSize: 15, fontWeight: 700, color: t.text }}>{tr('editor.drop.open', { count: dropOver })}</Text>
        </View>
      ) : null}
    </View>
  );
}
