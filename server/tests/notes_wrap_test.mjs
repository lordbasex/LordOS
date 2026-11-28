// notes_wrap_test.mjs — the rows of Notes' editor (apps/notes/wrapmodel.js): long lines, leading spaces, tabs, a CR,
// non-ASCII, CJK, emoji, combining marks and bold headings, with a fake measure that shapes like the engine (a run of
// wide characters with the spaces next to it narrower), so a row is measured as drawn. Checks that every row fits the
// width (a row wider than the editor was drawn over the next one), that the rows cover the text exactly and in order,
// that no character (a surrogate pair, a letter and its accent) is split, that the caret's x and a click's offset agree,
// and that the per-line cache wraps only the line that changed.
//
//   node tests/notes_wrap_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const W = await import(path.join(here, '../../apps/notes/wrapmodel.js'));
const { makeMeasure, wrapLine, wrapText, rowAt, xIn, offsetAt, display, TAB } = W;

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`FAIL ${name}\n  got  ${g}\n  want ${w}`); }
};

// the fake engine: a cell is 8 px (bold 8.5); a wide character 16; a space between wide characters 5 (shaped with them);
// a combining mark 0; an emoji (surrogate pair) 18
let calls = 0;
function engine(s, bold) {
  calls++;
  const cell = bold ? 8.5 : 8;
  let w = 0;
  const cps = [...s];
  for (let i = 0; i < cps.length; i++) {
    const c = cps[i], cp = c.codePointAt(0);
    if (/[\u0300-\u036f]/.test(c)) continue;
    if (cp > 0xffff) w += 18;
    else if (cp >= 0x3000 && cp <= 0x9fff) w += 16;
    else if (c === ' ' && ((cps[i - 1] || '').codePointAt(0) >= 0x3000 || (cps[i + 1] || '').codePointAt(0) >= 0x3000)) w += 5;
    else w += cell;
  }
  return w;
}
const measure = makeMeasure(engine, 8, 8.5);

eq('display: a tab is four spaces', display('a\tb'), `a${TAB}b`);
eq('display: a CR and other control characters are not drawn', display('end\r'), 'end');
eq('measure: ASCII is cells', measure('hello'), 40);
eq('measure: a tab counts four cells', measure('\t'), 32);
eq('measure: bold cells', measure('ab', true), 17);
eq('measure: a CJK run is shaped whole (narrow spaces)', measure('a 中文 b'), engine('a 中文 b'));

const text = [
  '# Un título de nota bastante largo que no entra en una sola fila del editor y se tiene que partir',
  'Párrafo: El servidor de Git se conecta al repositorio remoto que ya existe en la máquina virtual. Funciona con SSH.',
  '\tLínea con tab\tcolumna\tfinal',
  '    - [ ] tarea sangrada con un texto que también es largo para ver cómo se parte en dos filas',
  'Fin de línea de Windows\r',
  'Emoji 😀🎉 y CJK 中文字符测试中文字符测试中文字符测试 — ñandú, acción, pingüino',
  'Acentos combinados: n\u0303 e\u0301 a\u0300 '.repeat(4),
  'x'.repeat(130),
  '',
  '   leading spaces then words that go on and on until the row is full and wraps',
].join('\n');
const isBold = (li, ln) => /^#{1,6}\s/.test(ln);
for (const width of [120, 200, 333, 520]) {
  const rows = wrapText(text, width, measure, new Map(), isBold);
  const lines = text.split('\n');
  let ok = true, cover = true, split = true;
  let expect = 0;
  for (let r = 0; r < rows.length; r++) {
    const row = rows[r];
    const bold = isBold(row.line, lines[row.line]);
    // a row may end with the space where it was cut (drawn past the edge, never a letter)
    const drawn = text.slice(row.start, row.end).replace(/ +$/, '');
    if (measure(drawn, bold) > width + 0.01) ok = false;
    if (row.start !== expect) cover = false;
    const nextLine = rows[r + 1] && rows[r + 1].line !== row.line;
    expect = nextLine || !rows[r + 1] ? row.end + 1 : row.end;
    const c = text.charCodeAt(row.end), p = text.charCodeAt(row.end - 1);
    if (row.end > row.start && ((c >= 0xdc00 && c <= 0xdfff) || /[\u0300-\u036f]/.test(text[row.end] || '') || (p >= 0xd800 && p <= 0xdbff))) split = false;
  }
  eq(`width ${width}: every row fits (bold headings measured bold)`, ok, true);
  eq(`width ${width}: the rows cover the text, in order`, cover && expect === text.length + 1, true);
  eq(`width ${width}: no character is split`, split, true);
  // the caret's x and a click there give back the same offset (each character boundary of each row)
  let round = true;
  for (const row of rows) {
    const bold = isBold(row.line, lines[row.line]);
    const s = text.slice(row.start, row.end);
    for (let i = 0; i <= s.length; i++) {
      const off = row.start + i;
      // (an invisible control character, the CR, shares its x with the next offset)
      if ((s.charCodeAt(i) >= 0xdc00 && s.charCodeAt(i) <= 0xdfff) || /[\u0300-\u036f\r]/.test(s[i] || '')) continue;
      if (i === s.length) continue;
      const x = xIn(text, row, off, measure, bold);
      if (offsetAt(text, row, x + 0.5, measure, bold) !== off) { round = false; break; }
    }
  }
  eq(`width ${width}: a click at the caret's x is the caret's offset`, round, true);
}
{
  const rows = wrapText('aaa bbb ccc\nddd', 48, measure, new Map());
  eq('rows: cut at the last space, the space ends the row', rows.map((r) => [r.start, r.end, r.line]), [[0, 4, 0], [4, 8, 0], [8, 11, 0], [12, 15, 1]]);
  eq('rowAt: at a soft wrap the offset is on the next row', rowAt(rows, 4), 1);
  eq('rowAt: the end of a line stays on its row', rowAt(rows, 11), 2);
  eq('rowAt: the next line', rowAt(rows, 12), 3);
  eq('a word longer than the row is cut between letters', wrapLine('x'.repeat(20), 80, measure), [[0, 10], [10, 20]]);
  eq('an empty line is one row', wrapLine('', 80, measure), [[0, 0]]);
  eq('a row too narrow for one character still takes one', wrapLine('中中', 4, measure), [[0, 1], [1, 2]]);
  eq('a surrogate pair is never split', wrapLine('😀😀😀', 20, measure), [[0, 2], [2, 4], [4, 6]]);
}
{
  // the cache: typing in one line wraps only that line again
  const cache = new Map();
  const lines = Array.from({ length: 300 }, (_, i) => `Línea ${i}: un texto con acentos y bastante largo para que se parta en varias filas del editor`);
  wrapText(lines.join('\n'), 300, measure, cache);
  const edited = lines.slice();
  edited[150] += ' más';
  const before = calls;
  const rows = wrapText(edited.join('\n'), 300, measure, cache);
  eq('cache: one changed line of 300 is wrapped again (few measures)', calls - before < 40, true);
  eq('cache: the rows are those of a fresh wrap', rows, wrapText(edited.join('\n'), 300, measure, new Map()));
}

console.log(`notes-wrap-test: ${n - bad} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
