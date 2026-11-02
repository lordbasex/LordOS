// snap_test.mjs — window snapping geometry (sdk/snap.js): zones fill the work area without gaps or
// overlaps at odd sizes, the drop zones at the edges, and the Super+arrow moves.
//
//   node tests/snap_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { SNAP_ZONES, snapRect, zoneAt, snapNext, SNAP_EDGE } = await import(path.join(here, '../../sdk/snap.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};
const area = (r) => r.w * r.h;
const overlap = (a, b) => a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;

// a bottom taskbar (48 px) at 1280×800, an odd work area, a second display to the right, a left bar
const areas = [{ x: 0, y: 0, w: 1280, h: 752 }, { x: 0, y: 0, w: 1365, h: 719 }, { x: 1280, y: 0, w: 1921, h: 1033 }, { x: 56, y: 0, w: 1224, h: 800 }];
const layouts = [['left', 'right'], ['top-left', 'top-right', 'bottom-left', 'bottom-right'], ['left-third', 'center-third', 'right-third'],
  ['left-two-thirds', 'right-third'], ['left-third', 'right-two-thirds']];
for (const a of areas) {
  for (const l of layouts) {
    const rs = l.map((z) => snapRect(z, a));
    eq(`${l.join('+')} fills ${a.w}×${a.h}`, rs.reduce((s, r) => s + area(r), 0), area(a));
    let ov = 0;
    for (let i = 0; i < rs.length; i++) for (let j = i + 1; j < rs.length; j++) if (overlap(rs[i], rs[j])) ov++;
    eq(`${l.join('+')} no overlap at ${a.w}×${a.h}`, ov, 0);
    eq(`${l.join('+')} inside`, rs.every((r) => r.x >= a.x && r.y >= a.y && r.x + r.w <= a.x + a.w && r.y + r.h <= a.y + a.h), true);
  }
  eq(`max is the area ${a.w}`, snapRect('max', a), a);
  for (const z of SNAP_ZONES) eq(`${z} whole pixels`, Object.values(snapRect(z, a)).every(Number.isInteger), true);
}

// drop zones
const a = areas[0];
eq('left edge', zoneAt(0, 400, a), 'left');
eq('left edge within the margin', zoneAt(SNAP_EDGE, 400, a), 'left');
eq('just past the margin', zoneAt(SNAP_EDGE + 1, 400, a), null);
eq('right edge', zoneAt(1279, 400, a), 'right');
eq('top-left corner band', zoneAt(2, 40, a), 'top-left');
eq('bottom-right corner band', zoneAt(1279, 740, a), 'bottom-right');
eq('over a bottom taskbar, left side', zoneAt(0, 780, a), 'bottom-left');
eq('top edge', zoneAt(640, 0, a), 'max');
eq('middle', zoneAt(640, 400, a), null);
eq('left taskbar: its own strip counts as the left edge', zoneAt(20, 400, areas[3]), 'left');
eq('second display left edge', zoneAt(1281, 500, areas[2]), 'left');

// the top and bottom halves (the Window menu's Move & Resize, 0.8.5) share the middle line
{
  const t = snapRect('top', areas[0]), b = snapRect('bottom', areas[0]);
  eq('top + bottom fill the area', t.h + b.h, areas[0].h);
  eq('top and bottom touch', t.y + t.h, b.y);
  eq('top and bottom never overlap', overlap(t, b), false);
}

// Super+arrows
eq('free → left', snapNext(null, 'left'), 'left');
eq('free → up maximizes', snapNext(null, 'up'), 'max');
eq('free → down minimizes', snapNext(null, 'down'), 'minimize');
eq('left → up is the top quarter', snapNext('left', 'up'), 'top-left');
eq('left → right restores', snapNext('left', 'right'), 'restore');
eq('left → left stays', snapNext('left', 'left'), null);
eq('top-left → right', snapNext('top-left', 'right'), 'top-right');
eq('top-right → up maximizes', snapNext('top-right', 'up'), 'max');
eq('bottom-right → down minimizes', snapNext('bottom-right', 'down'), 'minimize');
eq('max → down restores', snapNext('max', 'down'), 'restore');
eq('max → up stays', snapNext('max', 'up'), null);
eq('a third → left is the half', snapNext('center-third', 'left'), 'left');
eq('unknown direction', snapNext('left', 'sideways'), null);

console.log(`snap-test: ${n - bad}/${n} ok`);
process.exit(bad ? 1 : 0);
