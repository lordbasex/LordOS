// NoteEditor — a note's editor: markdown with line wrapping, one cursor with selection,
// undo, markdown shortcuts and emoji suggestions with ":". The font is monospaced
// (JetBrains Mono): a row is measured the way it is drawn, as one string (wrapmodel.js), and lines are wrapped at
// words according to the area's width. It takes the SDK text focus (takeTextFocus), so it works the same in the window
// and in a desktop note.
// Fluid on long notes (0.8.9.6.2): the editor keeps the text itself and tells onChange, which no longer redraws the
// window (store.js editBody); only the line being edited is wrapped again (a per-line cache); only the rows in sight
// are drawn, each a memoized Row that redraws only when its text, place or style changes; the caret and the selection
// are drawn over them.
import { View, Text, ime, clipboard, takeTextFocus, releaseTextFocus, textFocusOwner, addListener, useTheme, useEditorSpell, EditorSquiggles, squiggleColorFor, spellHitAt } from '@lordos/sdk';
import { memo, useEffect, useMemo, useRef, useState } from 'react';
import { makeMeasure, wrapText, rowAt, xIn, offsetAt, display } from './wrapmodel.js';

const MONO = 1400, MONO_BOLD = 1700;   // JetBrains Mono, regular and bold (TEXT_MONO in server/src/ui/ui.h)
// lord.measureText rounds a width up to a whole pixel: one 8.4 px cell alone reads 9, so a cell is measured over 64
const measurers = new Map();
function measurerFor(size) {
  let m = measurers.get(size);
  if (!m) {
    const cell = lord.measureText('0'.repeat(64), size, MONO) / 64 || size * 0.6;
    const boldCell = lord.measureText('0'.repeat(64), size, MONO_BOLD) / 64 || cell;
    m = makeMeasure((s, bold) => lord.measureText(s, size, bold ? MONO_BOLD : MONO), cell, boldCell);
    measurers.set(size, m);
  }
  return m;
}
const WORD = /[\p{L}\p{N}_]/u;

// one drawn row: redrawn only when its text, place or style changes
const Row = memo(function Row({ text, left, top, size, weight, color }) {
  return <Text style={{ position: 'absolute', left, top, fontFamily: 'monospace', fontSize: size, whiteSpace: 'nowrap', fontWeight: weight, color }}>{text}</Text>;
});

// Style of a line according to its markdown (the editor does not highlight inside the line).
function lineStyle(s, inCode) {
  if (inCode || /^(`{3,}|~{3,})/.test(s)) return 'code';
  const h = /^(#{1,6})\s/.exec(s);
  if (h) return `h${h[1].length}`;
  if (/^\s*>/.test(s)) return 'quote';
  if (/^\s*([-*+]|\d+[.)])\s+\[[ xX]\]/.test(s)) return 'task';
  if (/^\s*([-*+]|\d+[.)])\s/.test(s)) return 'list';
  return 'text';
}

export function NoteEditor({ value, onChange, size = 14, colors, onPasteImage, emojiSearch, onEscape, autoFocus, padding = 10, apiRef, onBlur, onCommand, onContextMenu, spellCheck = true, initialSize }) {
  const area = useRef(null);
  const token = useRef({}).current;
  // the first frame's size: the place it opens in (Notes' reading view, measured), until lord-server has laid the area out
  const size0 = initialSize && initialSize.w > 1 && initialSize.h > 1 ? { w: initialSize.w, h: initialSize.h } : { w: 300, h: 200 };
  const st = useRef({ undo: [], redo: [], last: 0, ...size0 });
  // the text is the editor's own: each change goes to onChange, and a value from outside (another program changed the
  // note, a checkbox ticked elsewhere) replaces it only when it is not what the editor itself last gave
  const [text, setText] = useState(value);
  const emitted = useRef(value), seen = useRef(value);
  if (value !== seen.current) {
    seen.current = value;
    if (value !== emitted.current && value !== text) { emitted.current = value; setText(value); }
  }
  const [sel, setSel] = useState({ a: value.length, b: value.length });
  const [scroll, setScroll] = useState(0);
  const [focus, setFocusState] = useState(false);
  const [blink, setBlink] = useState(true);
  const [dims, setDims] = useState(size0);
  const [sugg, setSugg] = useState(null);           // { q, start, items, i }
  const LH = Math.round(size * 1.55);
  const measure = measurerFor(size);
  const wraps = useRef(new Map()).current;       // per line: its rows at a width (wrapmodel.js wrapText)
  const change = (next) => { emitted.current = next; setText(next); onChange(next); };

  const readSize = () => {
    if (!area.current) return;
    const [, , w, h] = lord.getLayout(area.current.id);
    // a node not laid out yet answers 0 × 0: wrapping at that width broke every line every few characters for a frame
    // (Read → Edit, QA ws168); keep the size we have and look again on the next frame
    if (!(w > 1 && h > 1)) { if (!st.current.again) st.current.again = setTimeout(() => { st.current.again = 0; readSize(); }, 16); return; }
    if (Math.abs(w - st.current.w) > 0.5 || Math.abs(h - st.current.h) > 0.5) { st.current.w = w; st.current.h = h; setDims({ w, h }); }
  };
  useEffect(() => {
    readSize();
    const id = setInterval(readSize, 400);
    return () => { clearInterval(id); if (st.current.again) clearTimeout(st.current.again); };
  }, []);

  // each line's markdown kind (headings are drawn and measured bold), then the rows: only changed lines are wrapped
  const lineKinds = useMemo(() => {
    const out = [];
    let inCode = false;
    for (const ln of text.split('\n')) {
      const fence = /^(`{3,}|~{3,})/.test(ln);
      out.push(lineStyle(ln, inCode && !fence));
      if (fence) inCode = !inCode;
    }
    return out;
  }, [text]);
  const boldLine = (li) => (lineKinds[li] || 'text')[0] === 'h';
  const wrapW = Math.max(40, dims.w - padding * 2 - 6);
  const rows = useMemo(() => wrapText(text, wrapW, measure, wraps, (li) => boldLine(li)), [text, wrapW, measure, lineKinds]);
  const rowOf = (off) => rowAt(rows, off);
  const xOf = (off) => { const r = rows[rowOf(off)]; return xIn(text, r, off, measure, boldLine(r.line)); };
  const offAt = (x, y) => {
    const r = Math.max(0, Math.min(rows.length - 1, Math.floor(y / LH)));
    const row = rows[r];
    const off = offsetAt(text, row, x, measure, boldLine(row.line));
    // a soft-wrapped row's end is the next row's start: a click past its text stays on this row
    const next = rows[r + 1];
    return off === row.end && next && next.line === row.line && row.end > row.start ? row.end - 1 : off;
  };

  // ---- editing ----
  const lo = Math.min(sel.a, sel.b), hi = Math.max(sel.a, sel.b);
  const theme = useTheme();
  const spellR = useEditorSpell(text, spellCheck, focus ? sel.b : -1);
  const pushUndo = (kind) => {
    const now = Date.now(), s = st.current;
    if (kind !== 'type' || now - s.last > 900 || s.kind !== kind) s.undo.push({ text, sel });
    if (s.undo.length > 300) s.undo.shift();
    s.last = now;
    s.kind = kind;
    s.redo = [];
  };
  const ensureVisible = (off) => {
    const y = rowOf(off) * LH, h = st.current.h - padding * 2;
    setScroll((t) => (y < t ? y : y + LH > t + h ? y + LH - h : t));
  };
  const commit = (next, a, b = a, kind = 'edit') => {
    pushUndo(kind);
    change(next);
    setSel({ a, b });
    setBlink(true);
    setTimeout(() => ensureVisible(b), 0);
    // emoji suggestions: ":" + name right before the cursor
    const m = /(^|[\s(])(:[a-z0-9_+-]{2,24})$/.exec(next.slice(Math.max(0, b - 26), b));
    if (m && emojiSearch) {
      const items = emojiSearch(m[2].slice(1)).slice(0, 6);
      setSugg(items.length ? { start: b - m[2].length, items, i: 0 } : null);
    } else setSugg(null);
  };
  const replace = (from, to, ins, kind = 'edit', caretAt = null) => {
    const next = text.slice(0, from) + ins + text.slice(to);
    const c = caretAt == null ? from + ins.length : caretAt;
    commit(next, c, c, kind);
  };
  const insert = (s, kind = 'type') => replace(lo, hi, s, kind);
  const moveTo = (off, extend) => { setSel((x) => (extend ? { a: x.a, b: off } : { a: off, b: off })); setBlink(true); setTimeout(() => ensureVisible(off), 0); setSugg(null); };
  const prevChar = (o) => (o > 0 ? o - (/[\udc00-\udfff]/.test(text[o - 1]) ? 2 : 1) : 0);
  const nextChar = (o) => (o < text.length ? o + (/[\ud800-\udbff]/.test(text[o]) ? 2 : 1) : o);
  const wordLeft = (o) => { let i = o; while (i > 0 && !WORD.test(text[i - 1])) i--; while (i > 0 && WORD.test(text[i - 1])) i--; return i; };
  const wordRight = (o) => { let i = o; while (i < text.length && !WORD.test(text[i])) i++; while (i < text.length && WORD.test(text[i])) i++; return i; };
  const lineStart = (o) => text.lastIndexOf('\n', o - 1) + 1;
  const lineEnd = (o) => { const i = text.indexOf('\n', o); return i < 0 ? text.length : i; };

  // wraps the selection (or the word) with marks: **bold**, *italic*…
  const wrap = (before, after = before) => {
    if (lo === hi) { replace(lo, hi, before + after, 'edit', lo + before.length); return; }
    const inner = text.slice(lo, hi);
    if (inner.startsWith(before) && inner.endsWith(after) && inner.length >= before.length + after.length) {
      const next = text.slice(0, lo) + inner.slice(before.length, inner.length - after.length) + text.slice(hi);
      commit(next, lo, hi - before.length - after.length);
      return;
    }
    const next = text.slice(0, lo) + before + inner + after + text.slice(hi);
    commit(next, lo + before.length, hi + before.length);
  };
  // changes the prefix of the chosen lines (headings, lists, checkboxes)
  const linePrefix = (make) => {
    const a = lineStart(lo), b = lineEnd(hi);
    const lines = text.slice(a, b).split('\n');
    const out = lines.map((l, i) => make(l, i));
    const next = text.slice(0, a) + out.join('\n') + text.slice(b);
    const delta = next.length - text.length;
    // a caret (nothing selected) stays a caret, at the end of its line, so what is typed next goes after "- [ ] "
    // instead of replacing it; a selection keeps covering the lines it changed
    if (lo === hi) commit(next, b + delta, b + delta);
    else commit(next, a, b + delta);
  };
  const STRIP = /^(\s*)(#{1,6}\s+|[-*+]\s+\[[ xX]\]\s+|[-*+]\s+|\d+[.)]\s+)?/;
  const heading = (n) => linePrefix((l) => { const m = STRIP.exec(l); const body = l.slice(m[0].length); const had = m[2] && m[2].startsWith('#'.repeat(n) + ' ') && m[2].trim() === '#'.repeat(n); return had ? body : `${'#'.repeat(n)} ${body}`; });
  const listify = (kind) => linePrefix((l, i) => {
    const m = STRIP.exec(l);
    const body = l.slice(m[0].length);
    const cur = m[2] || '';
    const mk = kind === 'num' ? `${i + 1}. ` : kind === 'task' ? '- [ ] ' : '- ';
    const same = (kind === 'task' && /\[[ xX]\]/.test(cur)) || (kind === 'num' && /^\d/.test(cur)) || (kind === 'bullet' && /^[-*+]\s+$/.test(cur));
    return `${m[1]}${same ? '' : mk}${body}`;
  });
  const codeBlock = () => {
    const inner = text.slice(lo, hi);
    const pre = lo === lineStart(lo) ? '' : '\n';
    replace(lo, hi, `${pre}\`\`\`\n${inner}\n\`\`\`\n`, 'edit', lo + pre.length + 4 + inner.length);
  };

  const newline = () => {
    const ls = lineStart(lo);
    const line = text.slice(ls, lo);
    const m = /^(\s*)([-*+]\s+\[[ xX]\]\s+|[-*+]\s+|(\d+)([.)])\s+|>\s?)/.exec(line);
    if (m) {
      if (line.trim() === m[0].trim() && lo === hi && lineEnd(lo) === lo) { replace(ls, lo, '', 'edit'); return; }   // empty item: ends the list
      let mk = m[2];
      if (/\[[ xX]\]/.test(mk)) mk = mk.replace(/\[[xX]\]/, '[ ]');
      if (m[3]) mk = `${Number(m[3]) + 1}${m[4]} `;
      insert(`\n${m[1]}${mk}`, 'edit');
      return;
    }
    insert(`\n${/^\s*/.exec(line)[0]}`, 'edit');
  };

  const acceptSugg = (k = sugg?.i || 0) => {
    const it = sugg.items[k];
    replace(sugg.start, hi, `${it.ch} `, 'edit');
    setSugg(null);
  };

  const undo = () => { const s = st.current; const u = s.undo.pop(); if (!u) return; s.redo.push({ text, sel }); change(u.text); setSel(u.sel); s.last = 0; };
  const redo = () => { const s = st.current; const r = s.redo.pop(); if (!r) return; s.undo.push({ text, sel }); change(r.text); setSel(r.sel); s.last = 0; };

  const onKey = (e) => {
    const prim = e.ctrl || e.meta;
    const k = e.key;
    const b = (e.base || '').toLowerCase();
    if (sugg) {
      if (k === 'ArrowDown') return setSugg({ ...sugg, i: (sugg.i + 1) % sugg.items.length });
      if (k === 'ArrowUp') return setSugg({ ...sugg, i: (sugg.i + sugg.items.length - 1) % sugg.items.length });
      if (k === 'Enter' || k === 'Tab') return acceptSugg();
      if (k === 'Escape') return setSugg(null);
    }
    if (prim && !e.alt) {
      if (b === 'z' && !e.shift) return undo();
      if ((b === 'z' && e.shift) || b === 'y') return redo();
      if (b === 'a') return setSel({ a: 0, b: text.length });
      if (b === 'c' && !e.shift) { if (hi > lo) clipboard.set(text.slice(lo, hi)); return undefined; }
      if (b === 'x') { if (hi > lo) { clipboard.set(text.slice(lo, hi)); replace(lo, hi, '', 'edit'); } return undefined; }
      if (b === 'v') {
        const st2 = lord.clipboard.state ? lord.clipboard.state() : { kind: 'text' };
        if (st2.kind === 'image' && onPasteImage) { const md = onPasteImage(); if (md) insert(`${lo > 0 && text[lo - 1] !== '\n' ? '\n' : ''}${md}\n`, 'edit'); return undefined; }
        const txt = clipboard.get();
        if (txt) insert(txt.replace(/\r\n/g, '\n'), 'edit');
        return undefined;
      }
      if (b === 'b') return wrap('**');
      if (b === 'i' && !e.shift) return wrap('*');
      if (b === 'k') { if (hi > lo) { const s = text.slice(lo, hi); replace(lo, hi, `[${s}](url)`, 'edit', lo + s.length + 3); } else replace(lo, hi, '[](url)', 'edit', lo + 1); return undefined; }
      if (b === '7' && e.shift) return listify('num');
      if (b === '8' && e.shift) return listify('bullet');
      if (b === '9' && e.shift) return listify('task');
      if (b === 'c' && e.shift) return codeBlock();
      if (!e.shift && (b === '1' || b === '2' || b === '3')) return heading(Number(b));
      if (k === 'ArrowLeft') return moveTo(wordLeft(sel.b), e.shift);
      if (k === 'ArrowRight') return moveTo(wordRight(sel.b), e.shift);
      if (k === 'ArrowUp' || k === 'Home') return moveTo(0, e.shift);
      if (k === 'ArrowDown' || k === 'End') return moveTo(text.length, e.shift);
      if (k === 'Backspace') return lo === hi ? replace(wordLeft(lo), lo, '', 'edit') : replace(lo, hi, '', 'edit');
      if (b) onCommand?.(b, e.shift);
      return undefined;
    }
    if (e.alt && (k === 'ArrowLeft' || k === 'ArrowRight')) return moveTo(k === 'ArrowLeft' ? wordLeft(sel.b) : wordRight(sel.b), e.shift);
    const vert = (d) => {
      const r = rowOf(sel.b) + d;
      const goal = st.current.goal ?? xOf(sel.b);
      if (r < 0) return 0;
      if (r >= rows.length) return text.length;
      const off = offAt(goal, r * LH + 1);
      setTimeout(() => { st.current.goal = goal; }, 0);
      return off;
    };
    if (k !== 'ArrowUp' && k !== 'ArrowDown') st.current.goal = null;
    switch (k) {
      case 'ArrowLeft': return moveTo(!e.shift && hi > lo ? lo : prevChar(sel.b), e.shift);
      case 'ArrowRight': return moveTo(!e.shift && hi > lo ? hi : nextChar(sel.b), e.shift);
      case 'ArrowUp': return moveTo(vert(-1), e.shift);
      case 'ArrowDown': return moveTo(vert(1), e.shift);
      case 'Home': { const r = rows[rowOf(sel.b)]; return moveTo(r.start, e.shift); }
      case 'End': { const r = rows[rowOf(sel.b)]; return moveTo(r.end, e.shift); }
      case 'Backspace': return lo === hi ? replace(prevChar(lo), lo, '', 'type') : replace(lo, hi, '', 'edit');
      case 'Delete': return lo === hi ? replace(lo, nextChar(lo), '', 'type') : replace(lo, hi, '', 'edit');
      case 'Enter': return newline();
      case 'Tab': {
        const line = text.slice(lineStart(lo), lineEnd(lo));
        if (/^\s*([-*+]|\d+[.)])\s/.test(line) || hi > lo) return linePrefix((l) => (e.shift ? l.replace(/^ {1,2}/, '') : `  ${l}`));
        return insert('  ');
      }
      case 'Escape': if (hi > lo) return setSel({ a: sel.b, b: sel.b }); return onEscape?.();
      default: break;
    }
    if (e.char && !e.ctrl && !e.meta && e.char >= ' ') insert(e.char, 'type');
    return undefined;
  };

  // the handler changes on every render: the text focus calls the current version
  st.current.onKey = onKey;
  const caretRect = () => {
    if (!area.current) return null;
    const [ax, ay] = lord.getLayout(area.current.id);
    return { x: ax + padding + xOf(sel.b), y: ay + padding + rowOf(sel.b) * LH - scroll, h: LH };
  };
  st.current.caret = caretRect;
  // send: a key from the Edit and Format menus (editmenu.js), handled as if it were typed
  if (apiRef) apiRef.current = { insert: (txt) => insert(txt, 'edit'), focus: () => take(), selection: () => text.slice(lo, hi),
    // spelling and ✨ AI (sdk/editorextras.jsx): the misspelled word under a right click, the selection's offsets, a range replaced
    spellHit: (e) => { try { return spellHitAt(spellR, text, posFromEvent(e)); } catch (err) { return null; } }, selRange: () => [lo, hi], textNow: () => text,
    replaceRange: (a, b, s) => replace(a, b, s, 'edit'),
    send: (e) => { take(); st.current.onKey(e); } };
  const take = () => {
    if (textFocusOwner() === token) return;
    takeTextFocus(token, (e) => st.current.onKey(e), () => setFocusState(textFocusOwner() === token));
    setFocusState(true);
  };
  useEffect(() => {
    if (!focus) return undefined;
    ime.attach(token, () => st.current.caret(), { inline: true });
    const id = setInterval(() => setBlink((v) => !v), 530);
    return () => { clearInterval(id); ime.detach(token); };
  }, [focus]);
  useEffect(() => { if (autoFocus) take(); return () => releaseTextFocus(token); }, []);
  const wasFocused = useRef(false);
  useEffect(() => { if (wasFocused.current && !focus) onBlur?.(); wasFocused.current = focus; }, [focus]);

  // ---- mouse ----
  const drag = useRef(null);
  const clicks = useRef({ t: 0, n: 0 });
  const posFromEvent = (e) => {
    const [ax, ay] = lord.getLayout(area.current.id);
    return offAt(e.x - ax - padding, e.y - ay - padding + scroll);
  };
  const onMouseDown = (e) => {
    take();
    if (e.button === 2) return;                // a right click keeps the selection for its menu
    const p = posFromEvent(e);
    const now = Date.now(), c = clicks.current;
    c.n = now - c.t < 400 ? c.n + 1 : 1;
    c.t = now;
    setSugg(null);
    if (c.n === 2) {                                 // word
      let a = p, b2 = p;
      while (a > 0 && WORD.test(text[a - 1])) a--;
      while (b2 < text.length && WORD.test(text[b2])) b2++;
      setSel({ a, b: b2 });
      return;
    }
    if (c.n >= 3) { setSel({ a: lineStart(p), b: lineEnd(p) }); return; }
    setSel((x) => (e.shift ? { a: x.a, b: p } : { a: p, b: p }));
    drag.current = { anchor: e.shift ? sel.a : p };
  };
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      if (!drag.current || !area.current) return;
      setSel({ a: drag.current.anchor, b: posFromEvent(e) });
    });
    const offUp = addListener('pointerup', () => { drag.current = null; });
    return () => { offMove(); offUp(); };
  });

  const onWheel = (e) => setScroll((s) => Math.max(0, Math.min(Math.max(0, rows.length * LH - st.current.h + padding * 2 + LH), s + e.deltaY)));

  // ---- drawing ----
  const first = Math.max(0, Math.floor(scroll / LH) - 2);
  const last = Math.min(rows.length - 1, first + Math.ceil(dims.h / LH) + 4);
  const kindStyle = (kind) => {
        if (kind[0] === 'h') return { fontWeight: 700, color: colors.heading };
    if (kind === 'code') return { color: colors.code };
    if (kind === 'quote') return { color: colors.dim };
    if (kind === 'task' || kind === 'list') return { color: colors.text };
    return { color: colors.text };
  };
  const out = [];
  const selRects = [];
  for (let r = first; r <= last; r++) {
    const row = rows[r];
    const kind = lineKinds[row.line] || 'text';
    const y = padding + r * LH - scroll;
    if (hi > lo && row.end >= lo && row.start <= hi) {
      const a = Math.max(lo, row.start), b2 = Math.min(hi, row.end);
      const x0 = xIn(text, row, a, measure, boldLine(row.line));
      const x1 = xIn(text, row, b2, measure, boldLine(row.line)) + (hi > row.end ? size * 0.4 : 0);
      if (x1 > x0) selRects.push(<View key={`s${r}`} style={{ position: 'absolute', left: padding + x0, top: y, width: x1 - x0, height: LH, backgroundColor: colors.selection }} />);
    }
    // headings are drawn a bit larger, but on the same grid (the cursor follows the base font)
    const ks = kindStyle(kind);
    out.push(<Row key={r} text={display(text.slice(row.start, row.end))} left={padding} top={y + (LH - size * 1.3) / 2} size={size} weight={ks.fontWeight || 400} color={ks.color} />);
  }
  const caretRow = rowOf(sel.b);
  return (
    <View ref={area} onMouseDown={onMouseDown} onWheel={onWheel} onContextMenu={onContextMenu} style={{ flex: 1, overflow: 'hidden', direction: 'ltr' }}>
      {selRects}
      {out}
      <EditorSquiggles ranges={spellR} rows={rows} first={first} last={last} left={padding} color={squiggleColorFor(theme.mode)}
        xOf={(row, off) => xIn(text, row, off, measure, boldLine(row.line))} top={(r) => Math.round(padding + r * LH - scroll + (LH + size * 1.3) / 2) - 2} />
      {focus && blink && caretRow >= first && caretRow <= last && (
        <View style={{ position: 'absolute', left: padding + xOf(sel.b) - 1, top: padding + caretRow * LH - scroll + 2, width: 2, height: LH - 4, backgroundColor: colors.caret }} />
      )}
      {sugg && (
        <View style={{ position: 'absolute', left: Math.min(padding + xOf(sel.b), Math.max(0, dims.w - 200)), top: padding + (caretRow + 1) * LH - scroll + 2, width: 196,
          padding: 4, borderRadius: 8, backgroundColor: colors.popupBg, border: `1px solid ${colors.border}`, boxShadow: '0 8px 20px rgba(0,0,0,0.3)' }}>
          {sugg.items.map((it, i) => (
            <View key={it.ch + i} onMouseDown={(e) => { e.stopPropagation?.(); acceptSugg(i); }} style={{ flexDirection: 'row', alignItems: 'center', gap: 8, height: 24, paddingHorizontal: 6,
              borderRadius: 5, backgroundColor: i === sugg.i ? colors.accent : 'transparent' }}>
              <Text style={{ fontSize: 15 }}>{it.ch}</Text>
              <Text style={{ fontSize: 12, color: i === sugg.i ? '#fff' : colors.popupText, whiteSpace: 'nowrap' }}>{`:${it.name}:`}</Text>
            </View>
          ))}
        </View>
      )}
    </View>
  );
}
