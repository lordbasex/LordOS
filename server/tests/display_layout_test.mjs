// display_layout_test.mjs — Settings → Displays' arrangement math (sdk/displaylayout.js): numbering,
// the magnetic snap while dragging and the drop that always touches and never overlaps.
//
//   node tests/display_layout_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { displayNumbers, snapDisplay, resolveDrop, placementOk, touches } = await import(path.join(here, '../../sdk/displaylayout.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};
const ok = (name, v) => eq(name, !!v, true);
const overlap = (a, b) => a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;

// ---- numbers ----
eq('numbers: main first, then list order', displayNumbers([
  { id: 40, active: true, w: 1280 }, { id: 41, active: true, w: 1920, main: true }, { id: 42, active: false }, { id: 43, active: true, w: 800 },
]), { 41: 1, 40: 2, 43: 3 });
eq('numbers: one display', displayNumbers([{ id: 7, active: true, w: 1280, main: true }]), { 7: 1 });
eq('numbers: nothing', displayNumbers(null), {});

// ---- touching ----
const A = { x: 0, y: 0, w: 1280, h: 800 };
ok('side by side touches', touches(A, { x: 1280, y: 0, w: 100, h: 100 }));
ok('stacked touches', touches(A, { x: 100, y: 800, w: 100, h: 100 }));
eq('a corner alone does not touch', touches(A, { x: 1280, y: 800, w: 100, h: 100 }), false);
eq('a gap does not touch', touches(A, { x: 1281, y: 0, w: 100, h: 100 }), false);

// ---- snapping while dragging (threshold 120 logical px ≈ 12 preview px at k = 0.1) ----
const two = [A, { x: 1280, y: 0, w: 1920, h: 1080 }];
const T = 120;
eq('near the right edge, slightly low: edge to edge, tops aligned', snapDisplay(two, 1, 1330, 60, T), { x: 1280, y: 0 });
eq('bottoms aligned when closer than tops', snapDisplay(two, 1, 1300, -250, T), { x: 1280, y: -280 });
eq('centers aligned', snapDisplay(two, 1, 1290, -130, T), { x: 1280, y: -140 });
eq('left of it', snapDisplay(two, 1, -1900, 10, T), { x: -1920, y: 0 });
eq('below it, lefts aligned', snapDisplay(two, 1, 50, 850, T), { x: 0, y: 800 });
eq('below it, centered', snapDisplay(two, 1, -300, 820, T), { x: -320, y: 800 });
eq('far away: no snap', snapDisplay(two, 1, 3000, 3000, T), { x: 3000, y: 3000 });
eq('far below: x does not stick to its side', snapDisplay(two, 1, 1300, 2000, T), { x: 1300, y: 2000 });

// ---- drop: always touching, never overlapping ----
const valid = (rects, i, p) => placementOk(rects, i, p.x, p.y);
eq('valid snapped drop is kept', resolveDrop(two, 1, 1330, 60, T), { x: 1280, y: 0 });
const p1 = resolveDrop(two, 1, 400, 200, T);                  // dropped on top of the main display
ok('dropped on top: valid', valid(two, 1, p1));
const p2 = resolveDrop(two, 1, 4000, 3000, T);                // dropped in empty space
ok('dropped far away: valid', valid(two, 1, p2));
eq('dropped far away: lands at the nearest corner region', p2, { x: 1280, y: 799 });
const p3 = resolveDrop(two, 1, 1280, 900, 0);                 // corner-only touch, no snapping
ok('corner only: slides to share an edge', valid(two, 1, p3));
eq('diagonal offset allowed while edges touch', resolveDrop(two, 1, 1280, 500, 0), { x: 1280, y: 500 });
// three displays in a row: dropping the third into the middle one slides it out
const three = [A, { x: 1280, y: 0, w: 1920, h: 1080 }, { x: 3200, y: 0, w: 1024, h: 768 }];
const p4 = resolveDrop(three, 2, 1500, 100, T);
ok('three: valid', valid(three, 2, p4));
for (let j = 0; j < 2; j++) eq(`three: no overlap with ${j}`, overlap({ ...p4, w: 1024, h: 768 }, three[j]), false);
// stacked under the main one, centered
eq('stacked, centered', resolveDrop(two, 1, -310, 805, T), { x: -320, y: 800 });

// a sweep: every drop over a grid is valid, for 2 and 3 displays and both thresholds
let sweepBad = 0;
for (const [rects, i] of [[two, 1], [two, 0], [three, 2], [three, 1], [three, 0]]) {
  for (let x = -5000; x <= 6000; x += 173) {
    for (let y = -3000; y <= 3000; y += 157) {
      for (const th of [0, T]) {
        const p = resolveDrop(rects, i, x, y, th);
        if (!valid(rects, i, p) || !Number.isInteger(p.x) || !Number.isInteger(p.y)) sweepBad++;
      }
    }
  }
}
eq('sweep: every drop touches and never overlaps', sweepBad, 0);

console.log(`display-layout: ${n - bad}/${n} checks passed`);
process.exit(bad ? 1 : 0);
