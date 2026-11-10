// Grapheme clusters and mixed directions in a text field without a screen (sdk/textfieldmodel.js, 0.8.8.8): the caret
// steps over emoji ZWJ sequences, flags, skin tones and combining marks whole, and in Hebrew or Arabic with numbers it is
// drawn, selects and moves on screen as the text is drawn. The layouts here stand in for lord.text.carets: each grapheme
// 10 px wide, in the order the levels give (rule L2). node server/tests/textfield_cluster_test.mjs
import { fieldKey, editKey, wordAt, caretAt, stops, prevStop, nextStop, caretX, selRects, posAtX, visualStep } from '../../sdk/textfieldmodel.js';
import { areaEdit, wrapRows } from '../../sdk/textareamodel.js';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };
const k = (key, more = {}) => ({ key, ...more });
const cps = (s) => [...s];

// what lord.text.graphemes returns: where clusters start, in code points, and the end
const seg = new Intl.Segmenter('en', { granularity: 'grapheme' });
function breaks(value) {
  const out = [0];
  let n = 0;
  for (const g of seg.segment(value)) { n += cps(g.segment).length; out.push(n); }
  return out;
}

// what lord.text.carets returns, with levels given per code point (0 left to right, 1 right to left, 2 a number in it)
function layOf(value, levels) {
  const b = breaks(value);
  const gs = [];
  for (let i = 0; i + 1 < b.length; i++) gs.push({ s: b[i], e: b[i + 1], lv: levels[b[i]] });
  const order = gs.map((g, i) => i);
  const top = Math.max(0, ...gs.map((g) => g.lv));
  const low = Math.min(...gs.map((g) => g.lv).filter((l) => l & 1), Infinity);
  for (let lv = top; lv >= low && lv >= 1; lv--) {
    for (let i = 0; i < order.length;) {
      if (gs[order[i]].lv < lv) { i++; continue; }
      let j = i;
      while (j < order.length && gs[order[j]].lv >= lv) j++;
      order.splice(i, j - i, ...order.slice(i, j).reverse());
      i = j;
    }
  }
  const n = cps(value).length;
  const left = Array(n).fill(0), right = Array(n).fill(0);
  order.forEach((gi, v) => { for (let i = gs[gi].s; i < gs[gi].e; i++) { left[i] = v * 10; right[i] = v * 10 + 10; } });
  return { width: gs.length * 10, rtl: (levels[0] & 1) === 1, left, right, levels, breaks: b };
}

// ---- grapheme clusters ----
const family = 'a\u{1F468}‍\u{1F469}‍\u{1F467}b';          // a, a ZWJ family (5 code points), b
eq('family: stops', breaks(family), [0, 1, 6, 7]);
let fl = { breaks: breaks(family) };
eq('family: → over it', fieldKey(family, 1, k('ArrowRight'), false, fl).pos, 6);
eq('family: ← over it', fieldKey(family, 6, k('ArrowLeft'), false, fl).pos, 1);
eq('family: Backspace takes it whole', fieldKey(family, 6, k('Backspace'), false, fl), { value: 'ab', pos: 1 });
eq('family: Delete takes it whole', fieldKey(family, 1, k('Delete'), false, fl), { value: 'ab', pos: 1 });
eq('without breaks: a code point a step, as before', fieldKey(family, 1, k('ArrowRight')).pos, 2);

const flags = '\u{1F1E6}\u{1F1F7}\u{1F1E7}\u{1F1F7}';                // two flags, two regional indicators each
fl = { breaks: breaks(flags) };
eq('flags: stops', fl.breaks, [0, 2, 4]);
eq('flags: Backspace removes one flag', fieldKey(flags, 4, k('Backspace'), false, fl), { value: '\u{1F1E6}\u{1F1F7}', pos: 2 });
eq('flags: Shift+← selects one flag', editKey({ value: flags, pos: 4, anchor: null }, k('ArrowLeft', { shift: true }), false, fl), { value: flags, pos: 2, anchor: 4 });

const thumbs = '\u{1F44D}\u{1F3FD}x';                                // a thumbs up with a skin tone
fl = { breaks: breaks(thumbs) };
eq('skin tone: Backspace takes the hand and its tone', fieldKey(thumbs, 2, k('Backspace'), false, fl), { value: 'x', pos: 0 });
eq('skin tone: Delete after it', fieldKey(thumbs, 0, k('Delete'), false, fl), { value: 'x', pos: 0 });

const accents = 'café déjà';                       // decomposed accents
fl = { breaks: breaks(accents) };
eq('accents: → steps over e + accent', fieldKey(accents, 3, k('ArrowRight'), false, fl).pos, 5);
eq('accents: Backspace removes e with its accent', fieldKey(accents, 5, k('Backspace'), false, fl), { value: 'caf déjà', pos: 3 });
eq('accents: the word with its marks', wordAt(accents, 1, fl), [0, 5]);
eq('accents: the last word to its last mark', wordAt(accents, 7, fl), [6, 12]);
eq('accents: Ctrl+Backspace by word', editKey({ value: accents, pos: 12, anchor: null }, k('Backspace', { ctrl: true }), false, fl), { value: 'café ', pos: 6, anchor: null });

const deva = 'क्षि';                             // a Devanagari conjunct with its vowel sign
eq('conjunct: one stop', breaks(deva), [0, 4]);
eq('conjunct: Backspace takes it', fieldKey(deva, 4, k('Backspace'), false, { breaks: breaks(deva) }), { value: '', pos: 0 });

eq('stops without breaks', stops('ab', null), [0, 1, 2]);
eq('stops: breaks of another text are ignored', stops('abc', { breaks: [0, 2] }), [0, 1, 2, 3]);
eq('prevStop / nextStop', [prevStop([0, 2, 5], 5), prevStop([0, 2, 5], 3), nextStop([0, 2, 5], 2), nextStop([0, 2, 5], 5)], [2, 2, 5, 5]);

// a click (TextArea's rows measure with lord.measureText): never inside a cluster
const measure = (s) => cps(s).length * 10;
eq('click in the family lands at an edge', caretAt(family, 25, measure, { breaks: breaks(family) }), 1);
eq('click past the middle of the family', caretAt(family, 40, measure, { breaks: breaks(family) }), 6);
eq('click without breaks: inside it', caretAt(family, 22, measure), 2);

// TextArea: Backspace and the arrows by cluster, also across rows
const area = 'x\u{1F44D}\u{1F3FD}\ny';
const rows = wrapRows(area, 200, measure);
eq('area: Backspace takes the tone and the hand', areaEdit({ value: area, pos: 3, anchor: null }, k('Backspace'), rows, measure, { breaks: breaks(area) }), { value: 'x\ny', pos: 1, anchor: null });
eq('area: → over the emoji', areaEdit({ value: area, pos: 1, anchor: null }, k('ArrowRight'), rows, measure, { breaks: breaks(area) }).pos, 3);
const area2 = 'x\u{1F44D}\u{1F3FD}\nyz', rows2 = wrapRows(area2, 200, measure);
eq('area: ↑ lands on a stop', areaEdit({ value: area2, pos: 6, anchor: null }, k('ArrowUp'), rows2, measure, { breaks: breaks(area2) }).pos, 3);
eq('area: ↑ without breaks lands inside', areaEdit({ value: area2, pos: 6, anchor: null }, k('ArrowUp'), rows2, measure).pos, 2);

// ---- mixed directions ----
// "שלום 123" in a right-to-left field: drawn "123 םולש", the number left to right on the left
const heb = 'שלום 123';
const hl = layOf(heb, [1, 1, 1, 1, 1, 2, 2, 2]);
eq('Hebrew: the layout stand-in', [hl.left[0], hl.left[4], hl.left[5], hl.left[7]], [70, 30, 0, 20]);
// every position its own place on screen: the start on the right, the end right after the 3
eq('Hebrew: caret x of each position', [0, 1, 2, 3, 4, 5, 6, 7, 8].map((p) => caretX(hl, p)), [80, 70, 60, 50, 40, 0, 10, 20, 30]);
// ← moves left on screen and → right, whatever the text's order
const walk = (lay, from, right) => { const out = []; let p = from; for (let i = 0; i < 12; i++) { const n = visualStep(lay, p, right); if (n == null) break; out.push(n); p = n; } return out; };
eq('Hebrew: ← from the start crosses the word, then the number', walk(hl, 0, false), [1, 2, 3, 4, 8, 7, 6, 5]);
eq('Hebrew: → from the left edge', walk(hl, 5, true), [6, 7, 8, 4, 3, 2, 1, 0]);
eq('Hebrew: ← in the field', fieldKey(heb, 4, k('ArrowLeft'), true, hl).pos, 8);
eq('Hebrew: → at the right edge stays', fieldKey(heb, 0, k('ArrowRight'), true, hl).pos, 0);
// the selection "ם 1" (logical 3..6) is two pieces on screen
eq('Hebrew: selection rectangles', selRects(hl, 3, 6), [{ x: 0, w: 10 }, { x: 30, w: 20 }]);
eq('Hebrew: all of it is one rectangle', selRects(hl, 0, 8), [{ x: 0, w: 80 }]);
eq('Hebrew: a click picks the nearest place', [posAtX(hl, 2), posAtX(hl, 12), posAtX(hl, 78), posAtX(hl, 44)], [5, 6, 0, 4]);
// Shift+← from the start selects a letter; then the number
let st = { value: heb, pos: 0, anchor: null };
st = editKey(st, k('ArrowLeft', { shift: true }), true, hl);
eq('Hebrew: Shift+← selects the first letter', st, { value: heb, pos: 1, anchor: 0 });
eq('Hebrew: Backspace on the number goes by code point stops', editKey({ value: heb, pos: 8, anchor: null }, k('Backspace'), true, hl), { value: 'שלום 12', pos: 7, anchor: null });

// "abc אבג" left to right: drawn "abc גבא"
const mix = 'abc אבג';
const ml = layOf(mix, [0, 0, 0, 0, 1, 1, 1]);
eq('mixed: caret x', [0, 1, 2, 3, 4, 5, 6, 7].map((p) => caretX(ml, p)), [0, 10, 20, 30, 70, 60, 50, 40]);
eq('mixed: → goes through the Hebrew on screen', walk(ml, 0, true), [1, 2, 3, 7, 6, 5, 4]);
eq('mixed: ← back', walk(ml, 4, false), [5, 6, 7, 3, 2, 1, 0]);
eq('mixed: selecting "c א" is two pieces', selRects(ml, 2, 5), [{ x: 20, w: 20 }, { x: 60, w: 10 }]);

// Arabic with an emoji: the emoji is one grapheme and one step
const ar = 'مرحبا \u{1F44D}\u{1F3FD}';
const al = layOf(ar, [1, 1, 1, 1, 1, 1, 1, 1]);
eq('Arabic: the emoji is one stop', al.breaks, [0, 1, 2, 3, 4, 5, 6, 8]);
eq('Arabic: ← over the emoji', fieldKey(ar, 6, k('ArrowLeft'), true, al).pos, 8);
eq('Arabic: Backspace takes it', fieldKey(ar, 8, k('Backspace'), true, al), { value: 'مرحبا ', pos: 6 });
eq('Arabic: the emoji selected is one box', selRects(al, 6, 8), [{ x: 0, w: 10 }]);

// a layout of a different text (a stale one) is not used for moving
eq('stale layout: logical step', fieldKey('abcd', 1, k('ArrowRight'), false, ml).pos, 2);

console.log(`textfield-cluster-test: ${ok} ok`);
