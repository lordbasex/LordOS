// The rows and the caret of a TextArea without a screen (sdk/textareamodel.js). node server/tests/textarea_model_test.mjs
import { wrapRows, rowOf, areaKey, posAt, areaEdit } from '../../sdk/textareamodel.js';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };
const m = (s) => [...s].length * 10;                 // every character 10 wide
const k = (key, more = {}) => ({ key, ...more });

eq('one short line', wrapRows('ssh pi', 100, m), [{ start: 0, end: 6 }]);
eq('empty', wrapRows('', 100, m), [{ start: 0, end: 0 }]);
eq('two lines', wrapRows('cd /srv\nls', 100, m), [{ start: 0, end: 7 }, { start: 8, end: 10 }]);
eq('a trailing line break makes an empty row', wrapRows('ls\n', 100, m), [{ start: 0, end: 2 }, { start: 3, end: 3 }]);
eq('wrapped after a space', wrapRows('ssh -o StrictHostKeyChecking=no host', 100, m).map((r) => [r.start, r.end]),
  [[0, 7], [7, 17], [17, 27], [27, 36]]);
eq('a word longer than the width is cut', wrapRows('abcdefghijkl', 50, m), [{ start: 0, end: 5 }, { start: 5, end: 10 }, { start: 10, end: 12 }]);
eq('no width: one row per line', wrapRows('a b c\nd', 0, m), [{ start: 0, end: 5 }, { start: 6, end: 7 }]);
eq('every character in some row', (() => { const v = 'one two three four five six seven'; const r = wrapRows(v, 60, m); return r[0].start === 0 && r[r.length - 1].end === [...v].length && r.every((x, i) => !i || x.start === r[i - 1].end); })(), true);

const rows = wrapRows('abcdefghij', 50, m);   // [0,5) [5,10)
eq('the caret at a wrap is on the next row', rowOf(rows, 5), 1);
eq('the caret inside the first row', rowOf(rows, 3), 0);
const lines = wrapRows('ab\ncd', 100, m);
eq('the end of a line stays on it', rowOf(lines, 2), 0);
eq('after the break, the next line', rowOf(lines, 3), 1);

eq('Enter adds a line break', areaKey('ab', 1, k('Enter'), wrapRows('ab', 100, m), m), { value: 'a\nb', pos: 2 });
eq('Ctrl+Enter is left alone', areaKey('ab', 1, k('Enter', { ctrl: true }), wrapRows('ab', 100, m), m), null);
eq('down keeps the column', areaKey('abcd\nefgh', 2, k('ArrowDown'), wrapRows('abcd\nefgh', 100, m), m), { value: 'abcd\nefgh', pos: 7 });
eq('up keeps the column', areaKey('abcd\nefgh', 8, k('ArrowUp'), wrapRows('abcd\nefgh', 100, m), m), { value: 'abcd\nefgh', pos: 3 });
eq('up on a shorter line stops at its end', areaKey('ab\nefgh', 7, k('ArrowUp'), wrapRows('ab\nefgh', 100, m), m), { value: 'ab\nefgh', pos: 2 });
eq('up on the first row goes to the start', areaKey('abc', 2, k('ArrowUp'), wrapRows('abc', 100, m), m).pos, 0);
eq('down on the last row goes to the end', areaKey('abc', 1, k('ArrowDown'), wrapRows('abc', 100, m), m).pos, 3);
eq('down through a wrap', areaKey('abcdefghij', 1, k('ArrowDown'), rows, m).pos, 6);
eq('Home is the row start', areaKey('ab\ncdef', 6, k('Home'), wrapRows('ab\ncdef', 100, m), m).pos, 3);
eq('End is the row end', areaKey('ab\ncdef', 0, k('End'), wrapRows('ab\ncdef', 100, m), m).pos, 2);
eq('Backspace joins two lines', areaKey('ab\ncd', 3, k('Backspace'), wrapRows('ab\ncd', 100, m), m), { value: 'abcd', pos: 2 });
eq('left and right still move', areaKey('ab', 2, k('ArrowLeft'), wrapRows('ab', 100, m), m).pos, 1);
eq('a click on the second line', posAt('ab\ncdef', wrapRows('ab\ncdef', 100, m), 21, 1, m), 5);
eq('a click past the rows is on the last', posAt('ab\ncd', wrapRows('ab\ncd', 100, m), 100, 9, m), 5);

// with a selection
const T2 = 'ssh -o A=1\n-o B=2';
const R2 = wrapRows(T2, 1000, m);
eq('select all in the box', areaEdit({ value: T2, pos: 3, anchor: null }, k('a', { ctrl: true, base: 'a' }), R2, m), { value: T2, pos: 17, anchor: 0 });
eq('shift+↓ selects to the next line', areaEdit({ value: T2, pos: 2, anchor: null }, k('ArrowDown', { shift: true }), R2, m), { value: T2, pos: 13, anchor: 2 });
eq('shift+End selects to the row end', areaEdit({ value: T2, pos: 4, anchor: null }, k('End', { shift: true }), R2, m), { value: T2, pos: 10, anchor: 4 });
eq('Enter replaces the selection', areaEdit({ value: 'ab cd', pos: 3, anchor: 2 }, k('Enter'), wrapRows('ab cd', 1000, m), m), { value: 'ab\ncd', pos: 3, anchor: null });
eq('typing keys over two lines deletes them', areaEdit({ value: T2, pos: 17, anchor: 0 }, k('Backspace'), R2, m), { value: '', pos: 0, anchor: null });
eq('↑ without shift drops the selection', areaEdit({ value: T2, pos: 13, anchor: 2 }, k('ArrowUp'), R2, m).anchor, null);

console.log(`textarea_model_test: ${ok} checks`);
