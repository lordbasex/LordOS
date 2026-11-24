// portal_file_model_test.mjs — the FileChooser portal's dialog (sdk/portalfilemodel.js): the app's glob filters (Chromium
// writes "*.[pP][dD][fF]") and where the dialog goes. node server/tests/portal_file_model_test.mjs
import { portalFilter, compileGlob, portalBox, MAX_GLOB } from '../../sdk/portalfilemodel.js';
import { overParent } from '../../sdk/dialogmodel.js';

let ok = 0, bad = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) === JSON.stringify(b)) ok++; else { bad++; console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); } };

const pdf = portalFilter(['*.[pP][dD][fF]']);
eq('chromium class: matches', ['a.pdf', 'B.PDF', 'c.Pdf'].map((n) => pdf.test(n)), [true, true, true]);
eq('chromium class: others not', ['a.pdfx', 'a.txt', 'pdf'].map((n) => pdf.test(n)), [false, false, false]);
const two = portalFilter(['*.png', '*.jp?g']);
eq('several patterns, ?', ['x.png', 'y.jpeg', 'z.jpg', 'w.gif'].map((n) => two.test(n)), [true, true, false, false]);
eq('range and negation', [portalFilter(['[a-c]*']).test('banana'), portalFilter(['[!a-c]*']).test('banana'), portalFilter(['[!a-c]*']).test('zed')], [true, false, true]);
eq('regex characters are literal', [portalFilter(['(a|b).txt']).test('(a|b).txt'), portalFilter(['(a|b).txt']).test('a.txt'), portalFilter(['a+b']).test('aab')], [true, false, false]);
eq('an unclosed [ is literal', portalFilter(['[abc']).test('[abc'), true);
eq('every file', [portalFilter([]), portalFilter(['*']), portalFilter(null)], [null, null, null]);
eq('a long pattern is cut', compileGlob('a'.repeat(MAX_GLOB * 4)).length, MAX_GLOB);
eq('hostile: no catastrophic run', (() => { const r = portalFilter(['*a*a*a*a*a*a*a*a*b']); const t0 = Date.now(); r.test('a'.repeat(255)); return Date.now() - t0 < 200; })(), true);

const L = { x: 0, y: 0, w: 1920, h: 1040 }, R = { x: 1920, y: 0, w: 2560, h: 1400 };
eq('over its window', portalBox({ x: 2000, y: 100, w: 1600, h: 1000 }, R, L, L, overParent), { x: 2340, y: 290, w: 920, h: 620 });
eq('no window named: the front window\'s display', portalBox(null, null, R, L, overParent), { x: 2740, y: 390, w: 920, h: 620 });
eq('nothing in front: the main display', portalBox(null, null, null, L, overParent), { x: 500, y: 210, w: 920, h: 620 });
eq('a small display: fits', portalBox(null, null, { x: 0, y: 0, w: 800, h: 500 }, L, overParent), { x: 16, y: 15, w: 768, h: 470 });

console.log(`portal-file-model: ${ok} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
