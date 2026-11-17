// slideshow_model_test.mjs — the wallpaper slideshow's decisions (sdk/slideshowmodel.js, 0.8.9): the settings, the
// categories, the shuffled deck of each display and no picture on two displays at once, when each display changes,
// the transition (and Reduce motion's), and when it waits. node server/tests/slideshow_model_test.mjs
import { readFileSync } from 'node:fs';
import {
  SLIDESHOW_EVERY, SLIDESHOW_DEFAULTS, PRELOAD_MS, slideshowPrefs, slideshowPatch, slideshowStop, slideshowCategories, slideshowIds,
  shuffled, drawNext, staggerMs, nextDue, preloadAt, transitionFor, transitionFrame, easeInOut, slideshowPause, MARGIN, DRIFT_MS, FADE_MS,
} from '../../sdk/slideshowmodel.js';

let ok = 0, bad = 0;
const eq = (name, a, b) => {
  if (JSON.stringify(a) === JSON.stringify(b)) ok++;
  else { bad++; console.error(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); }
};
// a seeded generator, so every run is the same
function rng(seed) {
  let x = seed >>> 0 || 1;
  return () => { x ^= x << 13; x >>>= 0; x ^= x >> 17; x ^= x << 5; x >>>= 0; return x / 4294967296; };
}

// ---- settings ----
eq('off by default', slideshowPrefs({}), SLIDESHOW_DEFAULTS);
eq('defaults: Cyberpunk City HD, every minute, one picture for all displays', [SLIDESHOW_DEFAULTS.category, SLIDESHOW_DEFAULTS.every, SLIDESHOW_DEFAULTS.perDisplay], ['cyberpunk', 60, false]);
eq('intervals: 1, 5, 15, 30 min, 1 h', SLIDESHOW_EVERY, [60, 300, 900, 1800, 3600]);
eq('stored', slideshowPrefs({ slideshow: { on: true, category: 'cosmos', every: 300, perDisplay: true } }), { on: true, category: 'cosmos', every: 300, perDisplay: true });
eq('junk', slideshowPrefs({ slideshow: { on: 'yes', category: '../x', every: 7, perDisplay: 1 } }), SLIDESHOW_DEFAULTS);
eq('junk object', slideshowPrefs({ slideshow: [1] }), SLIDESHOW_DEFAULTS);
eq('turning on turns the moving wallpapers off', slideshowPatch({ motion: { '*': 'crown' } }, { on: true }), { slideshow: { ...SLIDESHOW_DEFAULTS, on: true }, motion: {} });
eq('another change keeps the rest', slideshowPatch({ slideshow: { on: true, every: 300 } }, { perDisplay: true }), { slideshow: { on: true, category: 'cyberpunk', every: 300, perDisplay: true } });
eq('a picture chosen stops it', slideshowStop({ slideshow: { on: true, category: 'cosmos' } }), { slideshow: { on: false, category: 'cosmos', every: 60, perDisplay: false } });
eq('nothing to stop', slideshowStop({ slideshow: { on: false } }), {});

// ---- categories: from the collection itself ----
const index = JSON.parse(readFileSync(new URL('../../assets/wallpapers/index.json', import.meta.url)));
const list = index.map((e) => ({ id: e.id, category: e.category }));
const cyber = list.filter((w) => w.category === 'cyberpunk').map((w) => w.id);
eq('Cyberpunk City HD has eleven pictures', cyber.length, 11);
eq('the panorama is one of them', cyber.includes('cyberpunk-panorama'), true);
eq('categories with two pictures or more, in order', slideshowCategories([...list, { id: 'mine1', category: 'mine' }, { id: 'mine2', category: 'mine' }], ['signature', 'lumo', 'cosmos', 'cyberpunk', 'gradients']),
  ['signature', 'lumo', 'cosmos', 'cyberpunk']);
eq('a category of one is left out', slideshowCategories([{ id: 'a', category: 'x' }, { id: 'b', category: 'y' }, { id: 'c', category: 'y' }]), ['y']);
eq('ids of a category', slideshowIds(list, 'cyberpunk'), cyber);
eq('an unknown category: the first with pictures', slideshowIds(list, 'gone').length >= 2, true);

// ---- the deck ----
const ids = ['a', 'b', 'c', 'd', 'e'];
const sh = shuffled(ids, rng(7));
eq('a shuffle keeps every picture once', [...sh].sort(), ids);
eq('the picture on screen never comes first', Array.from({ length: 50 }, (_, i) => shuffled(ids, rng(i + 1), 'a')[0] !== 'a').every(Boolean), true);
eq('two pictures: the other first', shuffled(['a', 'b'], () => 0, 'a')[0], 'b');

// one display: a whole round before any repeat, and never the same picture twice in a row
{
  const r = rng(11);
  let deck = null, cur = null;
  const seen = [];
  for (let i = 0; i < 25; i++) { const n = drawNext(deck, ids, [], r, cur); deck = n.deck; seen.push(n.id); cur = n.id; }
  eq('one display: each round has every picture', [0, 5, 10, 15, 20].map((k) => [...seen.slice(k, k + 5)].sort().join('')), ['abcde', 'abcde', 'abcde', 'abcde', 'abcde']);
  eq('one display: never the same twice in a row', seen.every((x, i) => i === 0 || x !== seen[i - 1]), true);
}
// three displays, eleven pictures: never the same picture on two displays at once, each with its own order
{
  const r = rng(3);
  const disp = [{ deck: null, cur: null }, { deck: null, cur: null }, { deck: null, cur: null }];
  let clash = 0, steps = 0;
  const orders = disp.map(() => []);
  for (let step = 0; step < 300; step++) {
    const i = step % 3;          // staggered: one display changes at a time
    const busy = disp.filter((_, k) => k !== i).map((d) => d.cur).filter(Boolean);
    const n = drawNext(disp[i].deck, cyber, busy, r, disp[i].cur);
    disp[i].deck = n.deck; disp[i].cur = n.id;
    orders[i].push(n.id);
    steps++;
    if (new Set(disp.map((d) => d.cur).filter(Boolean)).size !== disp.filter((d) => d.cur).length) clash++;
  }
  eq('three displays: never one picture on two at once', clash, 0);
  eq('three displays: every picture shows on each', orders.map((o) => new Set(o).size), [11, 11, 11]);
  eq('three displays: their orders differ', new Set(orders.map((o) => o.slice(0, 11).join())).size, 3);
  eq('three displays: no display repeats itself', orders.every((o) => o.every((x, k) => k === 0 || x !== o[k - 1])), true);
}
// the arriving pictures count as busy too
eq('busy includes an arriving picture', drawNext({ order: ['a', 'b', 'c'], pos: 0 }, ['a', 'b', 'c'], ['a', 'b'], rng(1), null).id, 'c');
// a picture skipped as busy stays for later in the round
{
  const n = drawNext({ order: ['a', 'b', 'c', 'd'], pos: 0 }, ['a', 'b', 'c', 'd'], ['a'], rng(1), null);
  eq('skipped busy picture stays in the round', [n.id, n.deck.order, n.deck.pos], ['b', ['b', 'a', 'c', 'd'], 1]);
}
// fewer pictures than displays: it still changes (a repeat across displays can't be helped)
eq('too few pictures: still a picture', drawNext(null, ['a', 'b'], ['a', 'b'], rng(2), 'a').id, 'b');
eq('no pictures', drawNext(null, [], [], rng(2), null).id, null);
eq('a deck of another category starts again', drawNext({ order: ['x', 'y'], pos: 1 }, ['a', 'b'], [], () => 0, null).deck.order.sort(), ['a', 'b']);

// ---- when ----
const M = 60000;
eq('stagger: three displays over a minute', [0, 1, 2].map((i) => staggerMs(i, 3, M)), [0, 20000, 40000]);
eq('stagger: one display', staggerMs(0, 1, M), 0);
eq('next due: on the grid from the start', [nextDue(1000, 0, M), nextDue(60000, 0, M), nextDue(60001, 0, M)], [60000, 60000, 120000]);
eq('next due with an offset', nextDue(30000, 0, M, 20000), 80000);
eq('next due before the start', nextDue(0, 1000, M, 20000), 21000);
eq('preload: 8 s before', preloadAt(600000, 300000), 600000 - PRELOAD_MS);
eq('preload: at most half the interval', preloadAt(60000, 10000), 55000);
// one timer per change: between two changes nothing is due (the scheduler sleeps until preloadAt)
eq('wake-ups per change: preload and change only', [preloadAt(120000, M) > 60000, preloadAt(120000, M) < 120000], [true, true]);

// ---- how ----
const drift = transitionFor({ reduceMotion: false }, rng(5));
eq('a drift: 2.4 s', [drift.kind, drift.ms], ['drift', DRIFT_MS]);
// the picture is MARGIN larger than the display on each side: a drift within the margin never shows an edge
const froms = Array.from({ length: 40 }, (_, i) => transitionFor({}, rng(i * 7919 + 12345)).from);
eq('the drift stays inside the margin (the picture always covers the display)', froms.every((f) => Math.abs(f.x) <= MARGIN + 1e-9 && Math.abs(f.y) <= MARGIN + 1e-9), true);
eq('every frame inside the margin', Array.from({ length: 21 }, (_, i) => transitionFrame(drift, i / 20)).every((f) => Math.abs(f.x) <= MARGIN && Math.abs(f.y) <= MARGIN), true);
eq('directions vary', new Set(froms.map((o) => JSON.stringify(o))).size > 3, true);
eq('it moves (the whole margin on a side)', froms.every((f) => Math.max(Math.abs(f.x), Math.abs(f.y)) === MARGIN), true);
eq('Reduce motion: a short plain crossfade', transitionFor({ reduceMotion: true }), { kind: 'fade', ms: FADE_MS, from: { x: 0, y: 0 } });
eq('start: hidden, off its place', transitionFrame(drift, 0), { opacity: 0, x: drift.from.x, y: drift.from.y });
eq('end: shown, in its place', transitionFrame(drift, 1), { opacity: 1, x: 0, y: 0 });
eq('past the end: the end', transitionFrame(drift, 3), transitionFrame(drift, 1));
const mid = transitionFrame(drift, 0.5);
eq('half way: half shown, half way there', [mid.opacity, Math.abs(mid.x - drift.from.x / 2) < 1e-4], [0.5, true]);
eq('the fade only fades', transitionFrame(transitionFor({ reduceMotion: true }), 0.5), { opacity: 0.5, x: 0, y: 0 });
eq('eased in and out', [easeInOut(0), easeInOut(0.1) < 0.1, easeInOut(0.9) > 0.9, easeInOut(1)], [0, true, true, 1]);

// ---- whether ----
eq('nothing in the way', slideshowPause({}), null);
eq('nobody sees the screen (off, locked)', slideshowPause({ seen: false }), 'unseen');
eq('Power Saver', slideshowPause({ powerProfile: 'power-saver' }), 'powerSaver');
eq('a low battery', slideshowPause({ battery: { discharging: true, percent: 15 } }), 'powerSaver');
eq('a battery that charges', slideshowPause({ battery: { discharging: false, percent: 15 } }), null);
eq('balanced', slideshowPause({ powerProfile: 'balanced' }), null);
eq('hidden by windows', slideshowPause({ covered: true }), 'covered');
eq('a full-screen window', slideshowPause({ fullscreen: true }), 'fullscreen');

console.log(bad ? `slideshow-model-test: ${bad} FAILED` : `slideshow-model-test: ${ok} ok`);
process.exit(bad ? 1 : 0);
