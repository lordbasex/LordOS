// osk_model_test.mjs — the on-screen keyboard's model (sdk/oskmodel.js): rows from real labels, finding
// a symbol in the active layout, opening rules, sizes, and keeping the field in view.
//
//   node tests/osk_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { KEY, fallbackLabels, keyFor, letterRows, symbolRows, punctuation, panelSize, opensByItself, planAvoid } =
  await import(path.join(here, '../../sdk/oskmodel.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

const us = fallbackLabels();
// the parts of the Latin American layout that differ
const latam = fallbackLabels();
latam.altgr = true;
Object.assign(latam.keys, {
  39: ['ñ', 'Ñ', '~', ''], 26: ['dead:´', 'dead:¨', '', ''], 27: ['+', '*', 'dead:~', ''], 16: ['q', 'Q', '@', ''], 3: ['2', '"', '', ''],
  40: ['{', '[', 'dead:^', ''], 12: ["'", '?', '\\', ''], 41: ['|', '°', '', ''],
});
latam.keys[39][2] = '';

const text = (rows) => rows.map((r) => r.map((k) => k.label).join(''));
eq('US letters', text(letterRows(us)), ['qwertyuiop', 'asdfghjkl', 'zxcvbnm']);
eq('US shift', text(letterRows(us, 1)), ['QWERTYUIOP', 'ASDFGHJKL', 'ZXCVBNM']);
eq('levels', letterRows(us, 2)[0][0], { code: 16, level: 1, label: 'Q' });
eq('plain level', letterRows(us)[1][1], { code: 31, level: 0, label: 's' });
eq('latam adds ñ and the accent', text(letterRows(latam)), ['qwertyuiop´', 'asdfghjklñ', 'zxcvbnm']);
eq('a dead key keeps its code', letterRows(latam)[0][10], { code: 26, level: 0, label: '´' });

// a layout with no Latin letters: what it says, not what QWERTY says
const arabic = { altgr: false, keys: {} };
for (const [c, s] of [[16, 'ض'], [30, 'ش'], [44, 'ئ'], [39, 'ك'], [51, 'و'], [2, '١']]) arabic.keys[c] = [s, '', '', ''];
eq('Arabic rows', text(letterRows(arabic)), ['ض', 'شك', 'ئو']);
eq('Arabic shift falls back to the plain symbol', letterRows(arabic, 1)[0][0], { code: 16, level: 0, label: 'ض' });
eq('digits row with no Latin digits', symbolRows(arabic, 1)[0].map((k) => k.label), ['١']);

eq('keyFor plain', keyFor(us, 'a'), { code: 30, level: 0, label: 'a' });
eq('keyFor shift', keyFor(us, '@'), { code: 3, level: 1, label: '@' });
eq('keyFor AltGr', keyFor(latam, '@'), { code: 16, level: 2, label: '@' });
eq('keyFor refuses a level the layout does not have', keyFor({ ...latam, altgr: false }, '@'), null);
eq('keyFor nothing', keyFor(us, '€'), null);
eq('keyFor dead then space', keyFor(latam, '~'), { code: 27, level: 2, label: '~', after: KEY.SPACE });
eq('a real symbol wins over a dead key', keyFor(us, '`').after, undefined);

eq('symbols are only what the layout types', text(symbolRows(us, 2)).slice(0, 2), ['~`|\\^_=[]{}', '<>']);
eq('caret keys', symbolRows(us, 2)[2].map((k) => k.code), [KEY.TAB, KEY.LEFT, KEY.DOWN, KEY.UP, KEY.RIGHT, KEY.ESC]);
eq('digits', symbolRows(us, 1)[0].map((k) => k.label).join(''), '1234567890');
eq('punctuation', punctuation(us).map((k) => [k.code, k.level]), [[51, 0], [52, 0]]);
eq('no comma anywhere: its usual key', punctuation(arabic)[0].code, KEY.COMMA);

for (const [w, h] of [[1280, 800], [800, 480], [1920, 1080], [3840, 2160], [1024, 600]]) {
  const s = panelSize({ w, h });
  eq(`${w}x${h} fits`, s.width <= w && s.height <= h * 0.5, true);
  eq(`${w}x${h} keys for a finger`, s.keyH >= 40, true);
  eq(`${w}x${h} rows add up`, s.pad * 2 + s.keyH * 4 + s.gap * 3 <= s.height, true);
}

eq('off never', [opensByItself('off', false), opensByItself('off', true)], [false, false]);
eq('automatic: only without a keyboard', [opensByItself('auto', false), opensByItself('auto', true)], [true, false]);
eq('always', [opensByItself('always', false), opensByItself('always', true)], [true, true]);

const area = { x: 0, y: 30, w: 1280, h: 770 };
const top = 480;
const frame = { x: 100, y: 200, w: 600, h: 500 };
eq('field above the keyboard', planAvoid({ caret: { y: 300, h: 20 }, frame, area, top }), { dock: 'bottom' });
eq('just fits', planAvoid({ caret: { y: 448, h: 20 }, frame, area, top }), { dock: 'bottom' });
eq('the window rises what it must', planAvoid({ caret: { y: 500, h: 20 }, frame, area, top }).frame, { ...frame, y: 148 });
eq('it never goes under the bar: it shrinks', planAvoid({ caret: { y: 690, h: 20 }, frame, area, top }).frame, { ...frame, y: 30, h: 450 });
eq('unknown caret: the whole window', planAvoid({ caret: null, frame, area, top }).frame, { ...frame, y: 30, h: 450 });
eq('no window: the keyboard goes up', planAvoid({ caret: { y: 600, h: 20 }, frame: null, area: null, top }), { dock: 'top' });
eq('no window, field above', planAvoid({ caret: { y: 100, h: 20 }, frame: null, area: null, top }), { dock: 'bottom' });
eq('no field known, no window', planAvoid({ caret: null, frame: null, area: null, top }), { dock: 'bottom' });
const tall = { x: 0, y: 30, w: 1280, h: 450 };
eq('already as far as it goes: the keyboard goes up', planAvoid({ caret: { y: 470, h: 20 }, frame: tall, area, top }), { dock: 'top' });
eq('as far as it goes, unknown caret: stays', planAvoid({ caret: null, frame: { ...tall, h: 455 }, area, top }).frame, { ...tall });
eq('a window that kept its size: the keyboard goes up', planAvoid({ caret: { y: 500, h: 20 }, frame: { x: 0, y: 30, w: 800, h: 696 }, area, top, refused: true }), { dock: 'top' });
eq('kept its size, unknown caret: stays', planAvoid({ caret: null, frame: { x: 0, y: 30, w: 800, h: 696 }, area, top, refused: true }), { dock: 'bottom' });

console.log(bad ? `osk-model-test: ${bad} of ${n} failed` : `osk-model-test: ${n} ok`);
process.exit(bad ? 1 : 0);
