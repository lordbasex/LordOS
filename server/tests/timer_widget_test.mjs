// timer_widget_test.mjs — the Timer desktop widget's layout (sdk/timerwidgetmodel.js, used by sdk/deskwidgets.jsx
// TimerBody): at each of its sizes (s 1×1, m 2×1 of sdk/widgetgrid.js), in every state and in every UI language (en,
// es and de first, then German's and Finnish's long words and the rest), every part's box stays inside the card's
// content box, no row of pills is wider than its column, and the two columns of the medium size don't overlap.
// Text is measured on the wide side (wider than the UI font draws it), so a pass here leaves room on screen.
// node server/tests/timer_widget_test.mjs
import { readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { timerLayout, fitRow, contentOf, idleSeconds, PRESETS, PRESET_DROP, STYLES, DEFAULT_SECONDS } from '../../sdk/timerwidgetmodel.js';
import { CELL, GAP, SIZES } from '../../sdk/widgetgrid.js';

let ok = 0, bad = 0;
const check = (name, cond, detail = '') => { if (cond) ok++; else { bad++; console.error(`FAIL ${name} ${detail}`); } };
const eq = (name, a, b) => check(name, JSON.stringify(a) === JSON.stringify(b), `${JSON.stringify(a)} != ${JSON.stringify(b)}`);

// a pessimistic measure: CJK and Hangul a full em, capitals and digits 0.7 em, a space 0.3 em, the rest 0.6 em; bold 5% wider
const wide = (c) => /[ᄀ-ᇿ⺀-鿿가-힯豈-﫿＀-￯]/u.test(c);
const measure = (s, size, weight = 400) => {
  let w = 0;
  for (const c of String(s)) w += wide(c) ? 1 : c === ' ' ? 0.3 : /[A-Z0-9ÄÖÜ]/u.test(c) ? 0.7 : 0.6;
  return w * size * (weight >= 600 ? 1.05 : 1);
};

const root = fileURLToPath(new URL('../../assets/locales/', import.meta.url));
const load = (lang, ns) => { try { return JSON.parse(readFileSync(`${root}${lang}/${ns}.json`, 'utf8')); } catch (e) { return {}; } };
const en = { clock: load('en', 'clock'), shell: load('en', 'shell') };
const textsOf = (lang) => {
  const clock = load(lang, 'clock'), shell = load(lang, 'shell');
  const c = (k) => clock[k] || en.clock[k], s = (k) => shell[k] || en.shell[k];
  return {
    presets: PRESETS.map((n) => c('timer.preset').replace('{n}', n)),
    running: [c('timer.pause'), c('cancel')], paused: [c('timer.resume'), c('cancel')],
    sw: [[c('timer.start'), s('widgets.timer.reset')], [c('timer.pause'), s('widgets.timer.reset')]],
    title: s('widgets.timer.stopwatch'),
  };
};
const px = (size) => { const [c, r] = SIZES[size]; return { w: c * CELL + (c - 1) * GAP, h: r * CELL + (r - 1) * GAP }; };
const inside = (b, box) => b.x >= box.x - 0.01 && b.y >= box.y - 0.01 && b.x + b.w <= box.x + box.w + 0.01 && b.y + b.h <= box.y + box.h + 0.01;

const langs = ['en', 'es', 'de', 'fi', ...readdirSync(root).filter((l) => !['en', 'es', 'de', 'fi'].includes(l)).sort()];
const times = ['5:00', '25:00', '59:59', '1:00:00', '10:00:00'];
let cases = 0;
for (const lang of langs) {
  const tx = textsOf(lang);
  for (const size of ['s', 'm']) {
    const { w, h } = px(size);
    for (const [state, labels, drop] of [['idle', tx.presets, PRESET_DROP], ['running', tx.running, []], ['paused', tx.paused, []]]) {
      for (const swLabels of tx.sw) {
        for (const time of times) {
          const lay = timerLayout({ size, w, h, measure, timer: { text: time, labels, drop }, stopwatch: { title: tx.title, text: time, labels: swLabels } });
          const name = `${lang} ${size} ${state} ${time}`;
          cases++;
          for (const b of lay.boxes) check(`${name}: a part inside the card`, inside(b, lay.content), JSON.stringify({ b, content: lay.content }));
          for (const [i, col] of lay.cols.entries()) check(`${name}: column ${i} inside`, inside({ ...col, y: lay.content.y, h: 1 }, lay.content));
          check(`${name}: timer row fits its column`, lay.timer.row.width <= lay.cols[0].w, `${lay.timer.row.width} > ${lay.cols[0].w}`);
          check(`${name}: the ring's time fits inside the ring`, measure(time, lay.timer.ringFont, 600) <= lay.timer.ring - 20 || lay.timer.ringFont === 11);
          check(`${name}: one row of pills`, new Set(lay.timer.boxes.slice(1).map((b) => b.y)).size <= 1);
          if (size === 'm') {
            check(`${name}: stopwatch row fits its column`, lay.stopwatch.row.width <= lay.cols[1].w);
            check(`${name}: the columns don't overlap`, lay.cols[0].x + lay.cols[0].w <= lay.cols[1].x);
            check(`${name}: the timer's parts in the timer's column`, lay.timer.boxes.every((b) => b.x + b.w <= lay.cols[0].x + lay.cols[0].w + 0.01));
            check(`${name}: the stopwatch's parts in its column`, lay.stopwatch.boxes.every((b) => b.x >= lay.cols[1].x - 0.01));
            check(`${name}: balanced columns`, lay.cols[0].w === lay.cols[1].w);
          }
          // the three presets whole in en and es (as the maintainer's screenshots asked for)
          if (state === 'idle' && (lang === 'en' || lang === 'es')) eq(`${name}: all three presets`, lay.timer.row.keep, [0, 1, 2]);
        }
      }
    }
  }
}

// the model's own rules
const s = contentOf(px('s').w, px('s').h);
eq('contentOf: a small card is 140×143 inside its padding', [s.w, s.h], [140, 143]);
eq('fitRow: short labels keep the largest style', fitRow(['a', 'b'], 300, measure).style, STYLES[0]);
const tight = fitRow(['1 minute', '5 minutes', '25 minutes'], 140, measure, PRESET_DROP);
eq('fitRow: too wide, 1 min goes first, then 25', tight.keep, [1]);
const cut = fitRow(['Zurücksetzenxxxxxx', 'Startenxxxxxxxxxx'], 140, measure);
check('fitRow: cut with an ellipsis, still inside', cut.width <= 140 && cut.keep.length === 2, JSON.stringify(cut));
eq('idleSeconds: the last duration', idleSeconds({ state: 'idle', duration: 25 * 60000 }), 1500);
eq('idleSeconds: 5 min with none yet', idleSeconds({ state: 'idle', duration: 0 }), DEFAULT_SECONDS);

console.log(bad ? `timer-widget-test: ${bad} FAILED (${cases} layouts)` : `timer-widget-test: ${ok} ok (${cases} layouts, ${langs.length} languages)`);
if (bad) process.exit(1);
