// files_marquee_test.mjs — the rules of Files' rubber-band selection (apps/files/marquee.js, GitHub #233): the
// rectangle from two corners, which item boxes it touches, what Shift and Ctrl (⌘) do to the selection there was,
// autoscroll near the edges, right to left (mirrored boxes), and at most one update per frame.
import { rectOf, touches, hitPaths, modeOf, combine, sameSet, autoscrollStep, clampScroll, frameGate, DRAG_MIN, EDGE, MAX_STEP } from '../../apps/files/marquee.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  const g = got instanceof Set ? [...got].sort() : got, w = want instanceof Set ? [...want].sort() : want;
  if (JSON.stringify(g) === JSON.stringify(w)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(g)} != ${JSON.stringify(w)}`); }
};

// ---- the rectangle, whichever way it is drawn ----
eq('down-right', rectOf({ x: 10, y: 20 }, { x: 110, y: 70 }), { x: 10, y: 20, w: 100, h: 50 });
eq('up-left', rectOf({ x: 110, y: 70 }, { x: 10, y: 20 }), { x: 10, y: 20, w: 100, h: 50 });
eq('up-right', rectOf({ x: 10, y: 70 }, { x: 110, y: 20 }), { x: 10, y: 20, w: 100, h: 50 });
eq('a point', rectOf({ x: 5, y: 5 }, { x: 5, y: 5 }), { x: 5, y: 5, w: 0, h: 0 });

// ---- touching ----
const card = { path: 'a', x: 100, y: 100, w: 80, h: 90 };
eq('inside', touches({ x: 120, y: 120, w: 5, h: 5 }, card), true);
eq('covering', touches({ x: 0, y: 0, w: 500, h: 500 }, card), true);
eq('grazing the left edge', touches({ x: 40, y: 120, w: 60, h: 5 }, card), true);
eq('grazing the corner', touches({ x: 180, y: 190, w: 10, h: 10 }, card), true);
eq('one pixel short', touches({ x: 40, y: 120, w: 59, h: 5 }, card), false);
eq('below', touches({ x: 100, y: 191, w: 50, h: 50 }, card), false);
eq('an empty box (not laid out)', touches({ x: 0, y: 0, w: 500, h: 500 }, { path: 'z', x: 0, y: 0, w: 0, h: 0 }), false);
eq('no rectangle', touches(null, card), false);

// an icon grid: 4 cards per row, 96 wide, 8 apart, rows 120 high
const grid = [];
for (let i = 0; i < 12; i++) grid.push({ path: `f${i}`, x: 12 + (i % 4) * 104, y: 12 + Math.floor(i / 4) * 128, w: 96, h: 120 });
eq('a rectangle over the first two cards of rows 1 and 2', hitPaths(rectOf({ x: 30, y: 30 }, { x: 150, y: 180 }), grid), ['f0', 'f1', 'f4', 'f5']);
eq('in the gap between cards: nothing', hitPaths({ x: 109, y: 20, w: 5, h: 5 }, grid), []);
eq('the whole grid', hitPaths({ x: 0, y: 0, w: 1000, h: 1000 }, grid).length, 12);
// list rows: full width, 24 high
const rows = Array.from({ length: 10 }, (_, i) => ({ path: `r${i}`, x: 8, y: 4 + i * 24, w: 700, h: 24 }));
eq('a thin rectangle down rows 2..4', hitPaths(rectOf({ x: 300, y: 60 }, { x: 310, y: 110 }), rows), ['r2', 'r3', 'r4']);

// ---- right to left: the layout mirrors the cards; the rectangle covers what is physically under it ----
const W = 440;
const rtl = grid.map((c) => ({ ...c, x: W - c.x - c.w }));
eq('rtl: the first card is at the right', rtl[0].x, 332);
eq('rtl: a rectangle at the right edge takes the first cards', hitPaths(rectOf({ x: 430, y: 30 }, { x: 330, y: 180 }), rtl), ['f0', 'f4']);
eq('rtl: drawn right to left or left to right, the same', hitPaths(rectOf({ x: 330, y: 180 }, { x: 430, y: 30 }), rtl), ['f0', 'f4']);

// ---- modifiers ----
eq('no modifier', modeOf({}), 'replace');
eq('shift', modeOf({ shift: true }), 'add');
eq('ctrl', modeOf({ ctrl: true }), 'toggle');
eq('meta (⌘)', modeOf({ meta: true }), 'toggle');
eq('ctrl wins over shift', modeOf({ ctrl: true, shift: true }), 'toggle');
const base = new Set(['a', 'b']);
eq('replace: only the hits', combine(base, ['b', 'c'], 'replace'), new Set(['b', 'c']));
eq('add: the old ones and the hits', combine(base, ['b', 'c'], 'add'), new Set(['a', 'b', 'c']));
eq('toggle: hits flip', combine(base, ['b', 'c'], 'toggle'), new Set(['a', 'c']));
eq('toggle with no hits: as before', combine(base, [], 'toggle'), base);
eq('the base is not changed', [...base], ['a', 'b']);
eq('sameSet equal', sameSet(new Set(['a', 'b']), new Set(['b', 'a'])), true);
eq('sameSet different', sameSet(new Set(['a']), new Set(['b'])), false);
eq('sameSet sizes', sameSet(new Set(['a']), new Set(['a', 'b'])), false);

// ---- autoscroll ----
eq('middle: none', autoscrollStep(300, 100, 400), 0);
eq('near the top: up', autoscrollStep(110, 100, 400) < 0, true);
eq('near the bottom: down', autoscrollStep(495, 100, 400) > 0, true);
eq('at the bottom edge: the most', autoscrollStep(500, 100, 400), MAX_STEP);
eq('past the top: the most, up', autoscrollStep(40, 100, 400), -MAX_STEP);
eq('closer is faster', autoscrollStep(100 + 4, 100, 400) < autoscrollStep(100 + 30, 100, 400), true);
eq('just inside the band: at least 1', autoscrollStep(100 + EDGE - 1, 100, 400), -1);
eq('outside the band: 0', autoscrollStep(100 + EDGE + 1, 100, 400), 0);
eq('a tiny view: the band is a third', autoscrollStep(130, 100, 60), 0);
eq('no view', autoscrollStep(10, 0, 0), 0);
eq('clamp at 0', clampScroll(10, -28, 500), 0);
eq('clamp at the end', clampScroll(490, 28, 500), 500);
eq('drag threshold', DRAG_MIN, 4);

// ---- at most once per frame ----
{
  const frames = [];
  const applied = [];
  const g = frameGate((v) => applied.push(v), (fn) => frames.push(fn));
  for (let i = 0; i < 50; i++) g.push(i);
  eq('fifty moves ask for one frame', frames.length, 1);
  eq('nothing applied before the frame', applied, []);
  frames.shift()();
  eq('the frame applies the last value only', applied, [49]);
  g.push(50); g.push(51);
  eq('the next moves ask for the next frame', frames.length, 1);
  g.flush();
  eq('flush applies at once (the pointer was released)', applied, [49, 51]);
  frames.shift()();
  eq('the frame after a flush has nothing to do', applied, [49, 51]);
  g.push(52); g.cancel(); frames.shift()();
  eq('cancel drops a pending value', applied, [49, 51]);
}

console.log(`files-marquee-test: ${ok} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
