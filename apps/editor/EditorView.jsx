// EditorView — the text area: draws only the visible lines (plus a margin), with highlighting,
// selections, multiple cursors and search matches; and handles keyboard, mouse, wheel and
// the input method. The font is monospaced (JetBrains Mono): the width of a cell is
// measured with lord.measureText and positions with wide characters (CJK, emoji) are measured for real.
import { View, Text, useAppWindow, addListener, ime, clipboard, hasTextFocus } from '@lordos/sdk';
import { memo, useEffect, useRef, useState } from 'react';
import { cmp, head, anchor, hasSel, selStart, selEnd, cursorAt } from './buffer.js';
import { tokenizeLine } from './syntax.js';
import { BAR, thumbOf, topOfThumb } from './scrollbar.js';

export const FONT = 13;
export const LH = 20;
const MONO = 1400;                   // weight 400 + monospaced (see TEXT_MONO in server/src/ui/ui.h)
const PAD = 10;
const MARGIN = 6;                    // extra lines above and below the visible ones
const WORD = /[\p{L}\p{N}_$]/u;
const PAIRS = { '(': ')', '[': ']', '{': '}', '"': '"', "'": "'", '`': '`' };
const CLOSERS = new Set([')', ']', '}', '"', "'", '`']);

let cellW = 0;
export const cell = () => cellW || (cellW = lord.measureText('0000000000', FONT, MONO) / 10 || 7.8);
const ascii = (s) => /^[\x00-\x7f]*$/.test(s);

// Expands tabs to spaces (visual column).
export function expand(s, tab) {
  if (s.indexOf('\t') < 0) return s;
  let out = '', col = 0;
  for (const ch of s) {
    if (ch === '\t') { const n = tab - (col % tab); out += ' '.repeat(n); col += n; } else { out += ch; col++; }
  }
  return out;
}

const measure = (s) => (ascii(s) ? s.length * cell() : lord.measureText(s, FONT, MONO));

// Column (index in the string) closest to the given x.
function colAt(line, x, tab) {
  if (x <= 0) return 0;
  let w = 0, col = 0;
  for (let i = 0; i < line.length;) {
    const cp = line.codePointAt(i);
    const n = cp > 0xffff ? 2 : 1;
    let adv;
    if (line[i] === '\t') { const k = tab - (col % tab); adv = k * cell(); col += k; } else { adv = cp < 0x80 ? cell() : lord.measureText(String.fromCodePoint(cp), FONT, MONO); col++; }
    if (x < w + adv / 2) return i;
    w += adv;
    i += n;
  }
  return line.length;
}

const isWord = (ch) => !!ch && WORD.test(ch);

// Word jump: to the left or to the right of p.
function wordLeft(doc, p) {
  if (p.c === 0) return p.l > 0 ? { l: p.l - 1, c: doc.line(p.l - 1).length } : p;
  const s = doc.line(p.l);
  let c = p.c;
  while (c > 0 && /\s/.test(s[c - 1])) c--;
  const w = isWord(s[c - 1]);
  while (c > 0 && !/\s/.test(s[c - 1]) && isWord(s[c - 1]) === w) c--;
  return { l: p.l, c };
}
function wordRight(doc, p) {
  const s = doc.line(p.l);
  if (p.c >= s.length) return p.l < doc.lineCount - 1 ? { l: p.l + 1, c: 0 } : p;
  let c = p.c;
  while (c < s.length && /\s/.test(s[c])) c++;
  const w = isWord(s[c]);
  while (c < s.length && !/\s/.test(s[c]) && isWord(s[c]) === w) c++;
  return { l: p.l, c };
}
export function wordAt(doc, p) {
  const s = doc.line(p.l);
  let a = p.c, b = p.c;
  if (!isWord(s[a]) && isWord(s[a - 1])) a--, b--;
  if (!isWord(s[a])) return { from: p, to: { l: p.l, c: Math.min(s.length, p.c + 1) } };
  while (a > 0 && isWord(s[a - 1])) a--;
  while (b < s.length && isWord(s[b])) b++;
  return { from: { l: p.l, c: a }, to: { l: p.l, c: b } };
}

const lead = (s) => /^[ \t]*/.exec(s)[0];

// ---- one line (redrawn only if its text or its highlighting state changes) ----

const Line = memo(function Line({ tokens, top, left, palette, tab }) {
  let col = 0;
  return (
    <View style={{ position: 'absolute', top, left, height: LH, flexDirection: 'row', alignItems: 'center' }}>
      {tokens.map(([text, type], i) => {
        let shown = text;
        if (text.indexOf('\t') >= 0) {
          shown = '';
          for (const ch of text) { if (ch === '\t') { const n = tab - (col % tab); shown += ' '.repeat(n); col += n; } else { shown += ch; col++; } }
        } else col += text.length;
        return <Text key={i} style={{ fontFamily: 'monospace', fontSize: FONT, whiteSpace: 'nowrap', color: palette[type] || palette.text,
          fontWeight: type === 'bold' || type === 'heading' ? 700 : 400 }}>{shown}</Text>;
      })}
    </View>
  );
}, (a, b) => a.tokens === b.tokens && a.top === b.top && a.left === b.left && a.palette === b.palette && a.tab === b.tab);

// ---- view ----

export function EditorView({ doc, lang, t, palette, settings, matches, onChange, onCommand, apiRef, overlay, onContextMenu }) {
  const { focused } = useAppWindow();
  const area = useRef(null);
  const [size, setSize] = useState({ w: 600, h: 400 });
  const [scroll, setScroll] = useState({ top: 0, left: 0 });
  const [blink, setBlink] = useState(true);
  const cache = useRef({ states: ['root'], upTo: 0, lines: [], lang: null, version: -1 });
  const imeToken = useRef({}).current;
  const st = useRef({});
  const tab = settings.tabSize;
  const unit = settings.useTabs ? '\t' : ' '.repeat(tab);

  // ---- area size (there are no resize events: checked after every render and every 500 ms) ----
  const readSize = () => {
    if (!area.current) return;
    const [, , w, h] = lord.getLayout(area.current.id);
    // (not yet measured counts as changed: comparing with undefined is never true, and the area stayed 600×400)
    if (!(Math.abs(w - st.current.w) <= 0.5) || !(Math.abs(h - st.current.h) <= 0.5)) { st.current.w = w; st.current.h = h; setSize({ w, h }); }
  };
  useEffect(() => { readSize(); const id = setInterval(readSize, 500); return () => clearInterval(id); }, []);

  const digits = Math.max(3, String(doc.lineCount).length);
  const gutter = Math.round(digits * cell() + 22);
  const visLines = Math.ceil(size.h / LH) + 1;

  // ---- incremental highlighting ----
  const c = cache.current;
  if (c.lang !== lang || c.doc !== doc) { c.lang = lang; c.doc = doc; c.states = ['root']; c.upTo = 0; c.lines = []; }
  if (doc.firstChanged < c.upTo) c.upTo = doc.firstChanged;
  if (doc.firstChanged <= c.lines.length) c.lines.length = Math.min(c.lines.length, doc.firstChanged);
  doc.firstChanged = Infinity;
  const tokensFor = (l) => {
    // far jump (❖↓, go to line): start 200 lines earlier with the initial state, instead of
    // highlighting everything before it (a very long block comment may look wrong for a moment)
    if (l - c.upTo > 1500) {
      for (let i = c.upTo; i < l - 200; i++) c.states[i + 1] = 'root';
      c.upTo = l - 200;
    }
    while (c.upTo < l) {           // states up to line l
      const r = tokenizeLine(lang, doc.line(c.upTo), c.states[c.upTo] || 'root');
      c.states[c.upTo + 1] = r.state;
      c.lines[c.upTo] = r.tokens;
      c.upTo++;
    }
    if (!c.lines[l]) {
      const r = tokenizeLine(lang, doc.line(l), c.states[l] || 'root');
      c.lines[l] = r.tokens;
      if (c.upTo === l) { c.states[l + 1] = r.state; c.upTo = l + 1; }
    }
    return c.lines[l];
  };

  // ---- geometry ----
  const xOf = (p) => measure(expand(doc.line(p.l).slice(0, p.c), tab));
  const posAt = (x, y) => {
    const [ax, ay] = lord.getLayout(area.current.id);
    const l = Math.max(0, Math.min(doc.lineCount - 1, Math.floor((y - ay + scroll.top) / LH)));
    const c2 = colAt(doc.line(l), x - ax - gutter - PAD + scroll.left, tab);
    return { l, c: c2 };
  };

  const ensureVisible = (p = head(doc.cursors[doc.cursors.length - 1])) => {
    setScroll((s) => {
      let { top, left } = s;
      const y = p.l * LH, h = st.current.h || size.h, w = (st.current.w || size.w) - gutter - PAD * 2;
      if (y < top) top = y;
      if (y + LH > top + h) top = y + LH - h;
      // horizontal: with a single cursor it follows it; with several, only if none is in view
      const xs = doc.cursors.length > 1 ? doc.cursors.map((k) => xOf(head(k))) : [xOf(p)];
      if (!xs.some((x) => x >= left && x <= left + w - 10)) {
        const x = xOf(p);
        if (x < left) left = Math.max(0, x - 40);
        if (x > left + w - 10) left = x - w + 60;
      }
      top = Math.max(0, Math.min(top, Math.max(0, doc.lineCount * LH - LH)));
      return top === s.top && left === s.left ? s : { top, left };
    });
  };

  const changed = (seal = false) => { if (seal) doc.seal(); setBlink(true); ensureVisible(); onChange(); };

  // ---- movement ----
  const move = (fn, extend, { keepGoal = false } = {}) => {
    doc.cursors = doc.cursors.map((k) => {
      if (!extend && hasSel(k) && !keepGoal && (fn === left || fn === right)) {
        const p = fn === left ? selStart(k) : selEnd(k);
        return cursorAt(p);
      }
      const goal = keepGoal ? (k.goal >= 0 ? k.goal : xOf(head(k))) : -1;
      const p = doc.clamp(fn(head(k), goal));
      return extend ? { l: p.l, c: p.c, al: k.al, ac: k.ac, goal: keepGoal ? goal : -1 } : { ...cursorAt(p), goal: keepGoal ? goal : -1 };
    });
    doc.normalize();
    changed(true);
  };
  const left = (p) => (p.c > 0 ? { l: p.l, c: p.c - (/[\udc00-\udfff]/.test(doc.line(p.l)[p.c - 1]) ? 2 : 1) } : p.l > 0 ? { l: p.l - 1, c: doc.line(p.l - 1).length } : p);
  const right = (p) => (p.c < doc.line(p.l).length ? { l: p.l, c: p.c + (/[\ud800-\udbff]/.test(doc.line(p.l)[p.c]) ? 2 : 1) } : p.l < doc.lineCount - 1 ? { l: p.l + 1, c: 0 } : p);
  const vert = (d) => (p, goal) => {
    const l = p.l + d;
    if (l < 0) return { l: 0, c: 0 };
    if (l >= doc.lineCount) return { l: doc.lineCount - 1, c: doc.line(doc.lineCount - 1).length };
    return { l, c: colAt(doc.line(l), goal, tab) };
  };
  const home = (p) => { const n = lead(doc.line(p.l)).length; return { l: p.l, c: p.c === n ? 0 : n }; };
  const end = (p) => ({ l: p.l, c: doc.line(p.l).length });
  const page = () => Math.max(1, Math.floor((st.current.h || size.h) / LH) - 1);

  // ---- editing ----
  const typeText = (text) => {
    // automatic pairs and skipping the closing one
    if (text.length === 1 && CLOSERS.has(text) && doc.cursors.every((k) => !hasSel(k) && doc.line(k.l)[k.c] === text)) {
      move(right, false);
      return;
    }
    const close = PAIRS[text];
    if (close && doc.cursors.every((k) => hasSel(k))) {
      // wrap the selection
      doc.normalize();
      const edits = doc.cursors.map((k) => ({ from: selStart(k), to: selEnd(k), text: text + doc.textIn(selStart(k), selEnd(k)) + close }));
      doc.transact(edits, (rs) => rs.map((r) => ({ l: r.end.l, c: r.end.c - 1, al: r.start.l, ac: r.start.c + 1, goal: -1 })));
      changed(true);
      return;
    }
    const quote = text === '"' || text === "'" || text === '`';
    const autoPair = close && doc.cursors.every((k) => {
      const s = doc.line(k.l), next = s[k.c] || '', prev = s[k.c - 1] || '';
      if (next && !/[\s)\]}.,;:]/.test(next)) return false;
      if (quote && (isWord(prev) || prev === text)) return false;
      return true;
    });
    if (autoPair) {
      doc.normalize();
      const edits = doc.cursors.map((k) => ({ from: selStart(k), to: selEnd(k), text: text + close }));
      doc.transact(edits, (rs) => rs.map((r) => cursorAt({ l: r.end.l, c: r.end.c - 1 })), 'type');
    } else doc.insert(text, 'type');
    changed(text === ' ' || text === '\n');
  };

  const newline = () => {
    doc.normalize();
    const edits = doc.cursors.map((k) => {
      const s = doc.line(k.l), from = selStart(k), to = selEnd(k);
      let ind = lead(s).slice(0, Math.min(lead(s).length, from.c));
      const before = s.slice(0, from.c).trimEnd(), after = doc.line(to.l).slice(to.c);
      const opens = /[{[(:]$/.test(before) && !(lang.id === 'plain');
      if (opens && /^[}\])]/.test(after)) return { from, to, text: `\n${ind}${unit}\n${ind}`, back: ind.length + 1 };
      if (opens) ind += unit;
      return { from, to, text: `\n${ind}`, back: 0 };
    });
    doc.transact(edits.map(({ from, to, text }) => ({ from, to, text })), (rs) => rs.map((r, i) => {
      if (!edits[i].back) return cursorAt(r.end);
      return cursorAt({ l: r.end.l - 1, c: doc.line(r.end.l - 1).length });
    }));
    changed(true);
  };

  const backspace = (byWord) => {
    if (doc.cursors.every((k) => !hasSel(k))) {
      // inside an empty pair: delete both
      if (doc.cursors.every((k) => { const s = doc.line(k.l); return PAIRS[s[k.c - 1]] && PAIRS[s[k.c - 1]] === s[k.c]; })) {
        doc.deleteBy((p) => ({ l: p.l, c: p.c - 1 }));
        doc.deleteBy((p) => ({ l: p.l, c: p.c + 1 }));
        changed();
        return;
      }
      // in space indentation: back to the previous tab stop
      if (!byWord && !settings.useTabs && doc.cursors.every((k) => k.c > 0 && /^ +$/.test(doc.line(k.l).slice(0, k.c)))) {
        doc.deleteBy((p) => ({ l: p.l, c: Math.max(0, p.c - ((p.c - 1) % tab + 1)) }));
        changed();
        return;
      }
    }
    doc.deleteBy(byWord ? (p) => wordLeft(doc, p) : left);
    changed();
  };

  const indentLines = (out) => {
    doc.mapLines((s) => {
      if (!out) return s.length ? unit + s : s;
      if (s.startsWith('\t')) return s.slice(1);
      const n = /^ */.exec(s)[0].length;
      return s.slice(Math.min(n, tab));
    });
    changed(true);
  };

  const toggleComment = () => {
    const mark = lang.comment;
    if (!mark && lang.block) {
      const [a, b] = lang.block;
      const ls = doc.cursorLines();
      const all = ls.every((l) => { const s = doc.line(l).trim(); return !s || (s.startsWith(a) && s.endsWith(b)); });
      doc.mapLines((s) => {
        if (!s.trim()) return s;
        const ind = lead(s);
        if (all) return ind + s.trim().slice(a.length, -b.length).trim();
        return `${ind}${a} ${s.slice(ind.length)} ${b}`;
      });
      changed(true);
      return;
    }
    if (!mark) return;
    const ls = doc.cursorLines();
    const nonEmpty = ls.filter((l) => doc.line(l).trim());
    const all = nonEmpty.length > 0 && nonEmpty.every((l) => doc.line(l).trimStart().startsWith(mark));
    const minInd = Math.min(...nonEmpty.map((l) => lead(doc.line(l)).length), Infinity);
    doc.mapLines((s) => {
      if (!s.trim()) return s;
      if (all) { const i = s.indexOf(mark); return s.slice(0, i) + s.slice(i + mark.length + (s[i + mark.length] === ' ' ? 1 : 0)); }
      return `${s.slice(0, minInd)}${mark} ${s.slice(minInd)}`;
    });
    changed(true);
  };

  const duplicateLines = () => {
    const ls = doc.cursorLines();
    const groups = [];
    for (const l of ls) { const g = groups[groups.length - 1]; if (g && g[1] === l - 1) g[1] = l; else groups.push([l, l]); }
    const edits = groups.map(([a, b]) => ({ from: { l: b, c: doc.line(b).length }, to: { l: b, c: doc.line(b).length }, text: `\n${doc.lines.slice(a, b + 1).join('\n')}` }));
    const before = doc.cursors;
    doc.transact(edits, () => before.map((k) => {
      const g = groups.findIndex(([a, b]) => k.l >= a && k.l <= b);
      const shift = groups.slice(0, g + 1).reduce((n, [a, b]) => n + (b - a + 1), 0);
      return { ...k, l: k.l + shift, al: k.al + shift };
    }));
    changed(true);
  };

  const moveLines = (d) => {
    const ls = doc.cursorLines();
    const a = ls[0], b = ls[ls.length - 1];
    if ((d < 0 && a === 0) || (d > 0 && b >= doc.lineCount - 1)) return;
    const block = doc.lines.slice(a, b + 1);
    const before = doc.cursors;
    if (d < 0) {
      const prev = doc.line(a - 1);
      doc.transact([{ from: { l: a - 1, c: 0 }, to: { l: b, c: doc.line(b).length }, text: [...block, prev].join('\n') }],
        () => before.map((k) => ({ ...k, l: k.l - 1, al: k.al - 1 })));
    } else {
      const next = doc.line(b + 1);
      doc.transact([{ from: { l: a, c: 0 }, to: { l: b + 1, c: next.length }, text: [next, ...block].join('\n') }],
        () => before.map((k) => ({ ...k, l: k.l + 1, al: k.al + 1 })));
    }
    changed(true);
  };

  const selectNext = () => {
    const last = doc.cursors[doc.cursors.length - 1];
    if (!hasSel(last)) {
      const w = wordAt(doc, head(last));
      doc.cursors = doc.cursors.map((k) => (k === last ? { l: w.to.l, c: w.to.c, al: w.from.l, ac: w.from.c, goal: -1 } : k));
      changed(true);
      return;
    }
    const needle = doc.textIn(selStart(last), selEnd(last));
    if (!needle || needle.includes('\n')) return;
    const from = selEnd(last);
    for (let i = 0; i <= doc.lineCount; i++) {
      const l = (from.l + i) % doc.lineCount;
      const s = doc.line(l);
      const at = s.indexOf(needle, i === 0 ? from.c : 0);
      if (at < 0) continue;
      const p = { l, c: at };
      if (doc.cursors.some((k) => cmp(selStart(k), p) === 0)) return;
      doc.cursors = [...doc.cursors, { l, c: at + needle.length, al: l, ac: at, goal: -1 }];
      doc.normalize();
      ensureVisible({ l, c: at });
      changed(true);
      return;
    }
  };

  const copyText = () => doc.cursors.filter(hasSel).map((k) => doc.textIn(selStart(k), selEnd(k))).join('\n');
  const copy = () => {
    const txt = copyText() || (doc.cursors.length === 1 ? `${doc.line(doc.cursors[0].l)}\n` : '');
    if (txt) clipboard.set(txt);
  };
  const cut = () => {
    if (!doc.cursors.some(hasSel)) {           // no selection: the whole line, like TextMate
      const l = doc.cursors[0].l;
      clipboard.set(`${doc.line(l)}\n`);
      doc.cursors = [{ l: Math.min(l + 1, doc.lineCount - 1), c: l + 1 < doc.lineCount ? 0 : doc.line(l).length, al: l, ac: 0, goal: -1 }];
      doc.deleteBy((p) => p, 'edit');
      changed(true);
      return;
    }
    copy();
    doc.deleteBy((p) => p, 'edit');
    changed(true);
  };
  const paste = () => {
    const txt = clipboard.get().replace(/\r\n/g, '\n');
    if (!txt) return;
    const parts = txt.replace(/\n$/, '').split('\n');
    doc.normalize();
    if (doc.cursors.length > 1 && parts.length === doc.cursors.length) {
      let i = 0;
      doc.insert(() => parts[i++], 'edit');
    } else doc.insert(txt, 'edit');
    changed(true);
  };

  const selectAll = () => {
    const l = doc.lineCount - 1;
    doc.cursors = [{ l, c: doc.line(l).length, al: 0, ac: 0, goal: -1 }];
    changed(true);
  };

  const commands = {
    undo: () => { if (doc.undo()) changed(true); },
    redo: () => { if (doc.redo()) changed(true); },
    copy, cut, paste, selectAll, selectNext, toggleComment, duplicateLines,
    indent: () => indentLines(false), outdent: () => indentLines(true),
    moveUp: () => moveLines(-1), moveDown: () => moveLines(1),
    selectLine: () => {
      doc.cursors = doc.cursors.map((k) => { const s = selStart(k), e = selEnd(k); return { l: Math.min(e.l + 1, doc.lineCount - 1), c: e.l + 1 < doc.lineCount ? 0 : doc.line(e.l).length, al: s.l, ac: 0, goal: -1 }; });
      changed(true);
    },
    singleCursor: () => { doc.cursors = [doc.cursors[doc.cursors.length - 1]]; changed(true); },
    goto: (p, sel) => {
      const q = doc.clamp(p);
      doc.cursors = [sel ? { l: doc.clamp(sel).l, c: doc.clamp(sel).c, al: q.l, ac: q.c, goal: -1 } : cursorAt(q)];
      changed(true);
      setTimeout(() => {
        setScroll((s) => ({ ...s, top: Math.max(0, q.l * LH - (st.current.h || 400) / 3) }));
      }, 0);
    },
    ensureVisible,
    insertText: (txt) => { doc.insert(txt, 'edit'); changed(true); },
  };
  if (apiRef) apiRef.current = commands;

  // ---- keyboard ----
  const onKey = (e) => {
    const prim = e.ctrl || e.meta;
    const k = e.key;
    const sh = e.shift;
    if (prim && !e.alt) {
      const b = (e.base || '').toLowerCase();
      if (b === 'z' && !sh) return commands.undo();
      if ((b === 'z' && sh) || b === 'y') return commands.redo();
      if (b === 'c') return copy();
      if (b === 'x') return cut();
      if (b === 'v' && (e.ctrl || e.meta)) return paste();
      if (b === 'a') return selectAll();
      if (b === 'd' && !sh) return selectNext();
      if (b === 'd' && sh) return duplicateLines();
      if (b === '/') return toggleComment();
      if (b === ']') return indentLines(false);
      if (b === '[') return indentLines(true);
      if (b === 'l' && sh) return commands.selectLine();
      if (k === 'ArrowLeft' && e.meta) return move(home, sh);
      if (k === 'ArrowRight' && e.meta) return move(end, sh);
      if (k === 'ArrowUp' && e.meta) return move(() => ({ l: 0, c: 0 }), sh);
      if (k === 'ArrowDown' && e.meta) return move(() => ({ l: doc.lineCount - 1, c: doc.line(doc.lineCount - 1).length }), sh);
      if (k === 'Home') return move(() => ({ l: 0, c: 0 }), sh);
      if (k === 'End') return move(() => ({ l: doc.lineCount - 1, c: doc.line(doc.lineCount - 1).length }), sh);
      if (k === 'ArrowLeft') return move((p) => wordLeft(doc, p), sh);
      if (k === 'ArrowRight') return move((p) => wordRight(doc, p), sh);
      if (k === 'Backspace') return backspace(true);
      if (k === 'Delete') { doc.deleteBy((p) => wordRight(doc, p)); return changed(); }
      if (b) onCommand?.(b, sh);
      return;
    }
    if (e.alt && !prim) {
      if (k === 'ArrowUp') return moveLines(-1);
      if (k === 'ArrowDown') return moveLines(1);
      if (k === 'ArrowLeft') return move((p) => wordLeft(doc, p), sh);
      if (k === 'ArrowRight') return move((p) => wordRight(doc, p), sh);
      if (k === 'Backspace') return backspace(true);
    }
    switch (k) {
      case 'ArrowLeft': return move(left, sh);
      case 'ArrowRight': return move(right, sh);
      case 'ArrowUp': return move(vert(-1), sh, { keepGoal: true });
      case 'ArrowDown': return move(vert(1), sh, { keepGoal: true });
      case 'PageUp': return move(vert(-page()), sh, { keepGoal: true });
      case 'PageDown': return move(vert(page()), sh, { keepGoal: true });
      case 'Home': return move(home, sh);
      case 'End': return move(end, sh);
      case 'Backspace': return backspace(false);
      case 'Delete': doc.deleteBy(right); return changed();
      case 'Enter': return newline();
      case 'Tab':
        if (sh) return indentLines(true);
        if (doc.cursors.some((k2) => hasSel(k2) && k2.l !== k2.al)) return indentLines(false);
        if (settings.useTabs) return typeText('\t');
        return doc.insert((k2) => ' '.repeat(tab - (expand(doc.line(k2.l).slice(0, k2.c), tab).length % tab)), 'type'), changed();
      case 'Escape': if (doc.cursors.length > 1) return commands.singleCursor(); if (doc.cursors.some(hasSel)) { doc.cursors = [cursorAt(head(doc.cursors[0]))]; return changed(true); } return onCommand?.('escape');
      default: break;
    }
    if (e.char && !e.ctrl && !e.meta && e.char >= ' ') typeText(e.char);
  };

  // the handler and the cursor position change on every render: they are read through a ref,
  // so the input method focus is not released (and the composition is not lost) while typing
  st.current.onKey = onKey;
  st.current.caret = () => {
    if (!area.current) return null;
    const [ax, ay] = lord.getLayout(area.current.id);
    const k = doc.cursors[doc.cursors.length - 1];
    return { x: ax + gutter + PAD + xOf(head(k)) - scroll.left, y: ay + k.l * LH - scroll.top, h: LH };
  };
  st.current.overlay = !!overlay;
  useEffect(() => {
    if (!focused) return undefined;
    // with focus, the editor receives the input method's text (Japanese, Chinese, Korean)
    ime.attach(imeToken, () => st.current.caret());
    const off = addListener('keydown', (e) => { if (!e.down && e.down !== undefined) return; if (hasTextFocus() || st.current.overlay) return; st.current.onKey(e); });
    return () => { off(); ime.detach(imeToken); };
  }, [focused]);

  useEffect(() => {
    if (!focused) return undefined;
    const id = setInterval(() => setBlink((b) => !b), 530);
    return () => clearInterval(id);
  }, [focused]);

  // ---- mouse ----
  const clicks = useRef({ time: 0, n: 0, p: null });
  const drag = useRef(null);
  const onMouseDown = (e) => {
    if (!area.current || e.button === 2) return;     // a right click keeps the selection for its menu
    const p = posAt(e.x, e.y);
    const now = Date.now(), cl = clicks.current;
    cl.n = now - cl.time < 400 && cl.p && cl.p.l === p.l && Math.abs(cl.p.c - p.c) < 2 ? cl.n + 1 : 1;
    cl.time = now;
    cl.p = p;
    if (e.alt) {                               // ⌥click: another cursor; ⌥drag: column selection
      drag.current = { mode: 'column', start: p, startX: e.x, base: doc.cursors.slice() };
      doc.cursors = [...doc.cursors, cursorAt(p)];
      doc.normalize();
    } else if (e.shift) {
      const k = doc.cursors[doc.cursors.length - 1];
      doc.cursors = [{ l: p.l, c: p.c, al: k.al, ac: k.ac, goal: -1 }];
      drag.current = { mode: 'char', anchor: anchor(k) };
    } else if (cl.n === 2) {
      const w = wordAt(doc, p);
      doc.cursors = [{ l: w.to.l, c: w.to.c, al: w.from.l, ac: w.from.c, goal: -1 }];
      drag.current = { mode: 'word', anchor: w.from, anchorEnd: w.to };
    } else if (cl.n >= 3) {
      const nl = Math.min(p.l + 1, doc.lineCount - 1);
      doc.cursors = [{ l: nl, c: nl > p.l ? 0 : doc.line(p.l).length, al: p.l, ac: 0, goal: -1 }];
      drag.current = { mode: 'line', line: p.l };
    } else {
      doc.cursors = [cursorAt(p)];
      drag.current = { mode: 'char', anchor: p };
    }
    changed(true);
  };
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      const d = drag.current;
      if (!d || !area.current) return;
      const p = posAt(e.x, e.y);
      if (d.mode === 'char') doc.cursors = [{ l: p.l, c: p.c, al: d.anchor.l, ac: d.anchor.c, goal: -1 }];
      else if (d.mode === 'word') {
        const w = wordAt(doc, p);
        doc.cursors = [cmp(p, d.anchor) < 0 ? { l: w.from.l, c: w.from.c, al: d.anchorEnd.l, ac: d.anchorEnd.c, goal: -1 } : { l: w.to.l, c: w.to.c, al: d.anchor.l, ac: d.anchor.c, goal: -1 }];
      } else if (d.mode === 'line') {
        const a = Math.min(p.l, d.line), b = Math.max(p.l, d.line);
        doc.cursors = [{ l: Math.min(b + 1, doc.lineCount - 1), c: b + 1 < doc.lineCount ? 0 : doc.line(b).length, al: a, ac: 0, goal: -1 }];
      } else if (d.mode === 'column') {
        const [ax] = lord.getLayout(area.current.id);
        const x0 = d.startX - ax - gutter - PAD + scroll.left, x1 = e.x - ax - gutter - PAD + scroll.left;
        const a = Math.min(d.start.l, p.l), b = Math.max(d.start.l, p.l);
        const ks = [];
        for (let l = a; l <= b; l++) {
          const s = doc.line(l);
          ks.push({ l, c: colAt(s, x1, tab), al: l, ac: colAt(s, x0, tab), goal: -1 });
        }
        doc.cursors = [...d.base, ...ks];
        doc.normalize();
      }
      changed(true);
    });
    const offUp = addListener('pointerup', () => { drag.current = null; });
    return () => { offMove(); offUp(); };
  });

  // the scroll bar: drag the thumb, or click the track to jump there
  const barDrag = useRef(null);
  const barDown = (e) => {
    e.stopPropagation?.();
    if (!area.current || e.button === 2) return;
    const [, ay] = lord.getLayout(area.current.id);
    const h = st.current.h || size.h;
    const th = thumbOf(h, contentH(), scroll.top);
    if (!th) return;
    const y = e.y - ay;
    const grab = y >= th.pos && y <= th.pos + th.size ? y - th.pos : th.size / 2;
    barDrag.current = { ay, grab };
    setScroll((s) => ({ ...s, top: topOfThumb(h, contentH(), y - grab) }));
  };
  useEffect(() => {
    const offMove = addListener('pointermove', (e) => {
      const b = barDrag.current;
      if (!b) return;
      setScroll((s) => ({ ...s, top: topOfThumb(st.current.h || size.h, contentH(), e.y - b.ay - b.grab) }));
    });
    const offUp = addListener('pointerup', () => { barDrag.current = null; });
    return () => { offMove(); offUp(); };
  });

  const contentH = () => doc.lineCount * LH + LH * 3;
  const onWheel = (e) => {
    setScroll((s) => {
      const max = Math.max(0, contentH() - (st.current.h || size.h));
      const top = Math.max(0, Math.min(max, s.top + (e.shift ? 0 : e.deltaY)));
      const leftS = e.shift ? Math.max(0, s.left + e.deltaY) : s.left;
      return { top, left: leftS };
    });
  };

  // ---- drawing ----
  const first = Math.max(0, Math.floor(scroll.top / LH) - MARGIN);
  const last = Math.min(doc.lineCount - 1, first + visLines + MARGIN * 2);
  const textLeft = gutter + PAD - scroll.left;
  const rows = [];
  for (let l = first; l <= last; l++) rows.push(l);
  const primary = doc.cursors[doc.cursors.length - 1];
  const thumb = thumbOf(st.current.h || size.h, contentH(), scroll.top);

  // selection rectangles per visible line
  const selRects = [];
  for (const k of doc.cursors) {
    if (!hasSel(k)) continue;
    const s = selStart(k), e2 = selEnd(k);
    for (let l = Math.max(s.l, first); l <= Math.min(e2.l, last); l++) {
      const x0 = l === s.l ? xOf(s) : 0;
      const x1 = l === e2.l ? xOf(e2) : xOf({ l, c: doc.line(l).length }) + cell() * 0.6;
      if (x1 > x0) selRects.push([l, x0, x1]);
    }
  }
  const matchRects = [];
  for (const m of matches || []) {
    if (m.l < first || m.l > last) continue;
    matchRects.push([m.l, xOf({ l: m.l, c: m.c }), xOf({ l: m.l, c: m.c + m.len }), m.current]);
  }

  return (
    <View ref={area} onMouseDown={onMouseDown} onWheel={onWheel} onContextMenu={onContextMenu}
      style={{ flex: 1, overflow: 'hidden', backgroundColor: t.editorBg, direction: 'ltr' }}>
      {/* current line */}
      {!doc.cursors.some(hasSel) && primary.l >= first && primary.l <= last && (
        <View style={{ position: 'absolute', left: 0, right: 0, top: primary.l * LH - scroll.top, height: LH, backgroundColor: t.lineHighlight }} />
      )}
      {matchRects.map(([l, x0, x1, cur], i) => (
        <View key={`m${i}`} style={{ position: 'absolute', top: l * LH - scroll.top + 1, height: LH - 2, left: textLeft + x0, width: Math.max(2, x1 - x0),
          borderRadius: 3, backgroundColor: cur ? t.matchCurrent : t.match }} />
      ))}
      {selRects.map(([l, x0, x1], i) => (
        <View key={`s${i}`} style={{ position: 'absolute', top: l * LH - scroll.top, height: LH, left: textLeft + x0, width: x1 - x0, backgroundColor: focused ? t.selection : t.selectionBlur }} />
      ))}
      {rows.map((l) => <Line key={l} tokens={tokensFor(l)} top={l * LH - scroll.top} left={textLeft} palette={palette} tab={tab} />)}
      {focused && blink && doc.cursors.map((k, i) => (k.l >= first && k.l <= last ? (
        <View key={`c${i}`} style={{ position: 'absolute', top: k.l * LH - scroll.top + 2, height: LH - 4, width: 2, left: textLeft + xOf(head(k)) - 1, backgroundColor: t.caret }} />
      ) : null))}
      {/* line numbers */}
      <View style={{ position: 'absolute', left: 0, top: 0, bottom: 0, width: gutter, backgroundColor: t.gutterBg }}>
        {rows.map((l) => (
          <Text key={l} style={{ position: 'absolute', top: l * LH - scroll.top + 2, left: 0, width: gutter - 12, textAlign: 'right', fontFamily: 'monospace',
            fontSize: FONT - 1, color: doc.cursors.some((k) => k.l === l) ? t.gutterActive : t.gutterText }}>{String(l + 1)}</Text>
        ))}
      </View>
      {thumb && (
        <View onMouseDown={barDown} style={{ position: 'absolute', right: 0, top: 0, bottom: 0, width: BAR + 4 }}>
          <View style={{ position: 'absolute', right: 2, top: thumb.pos, width: BAR - 2, height: thumb.size, borderRadius: (BAR - 2) / 2,
            backgroundColor: t.scrollThumb }} />
        </View>
      )}
    </View>
  );
}
