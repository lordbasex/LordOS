// control_model_test.mjs — (0.8.8.8) the keys of the SDK's choice controls (sdk/controlmodel.js): Tabs and a radio
// group (going round, skipping disabled ones, right to left), a Dropdown open and closed, the wheel on a slider or a
// stepper, where a tooltip goes so it never covers the pointer, and the band of an indeterminate progress bar.
import { groupKey, dropdownKey, wheelValue, tipSide, popoverRoom, progressSpan, disclosureDir, chevronGeometry, DISCLOSURE_SIZE, DISCLOSURE_BOX } from '../../sdk/controlmodel.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};
const key = (k, mods = {}) => ({ key: k, ...mods });

// ---- Tabs and radio groups ----
const tabs = (k, index, o = {}) => groupKey(key(k, o.mods), { index, count: 4, ...o });
eq('→ the next tab', tabs('ArrowRight', 1), 2);
eq('→ goes round', tabs('ArrowRight', 3), 0);
eq('← goes round back', tabs('ArrowLeft', 0), 3);
eq('right to left: ← is the next', tabs('ArrowLeft', 1, { rtl: true }), 2);
eq('right to left: → the previous', tabs('ArrowRight', 1, { rtl: true }), 0);
eq('Home and End', [tabs('Home', 2), tabs('End', 0)], [0, 3]);
eq('tabs leave ↓ to the pane', tabs('ArrowDown', 1), null);
eq('a radio group takes ↓ and ↑', [tabs('ArrowDown', 1, { vertical: true }), tabs('ArrowUp', 0, { vertical: true })], [2, 3]);
eq('a disabled one is skipped', tabs('ArrowRight', 0, { skip: (i) => i === 1 }), 2);
eq('Home skips a disabled first', tabs('Home', 3, { skip: (i) => i === 0 }), 1);
eq('nothing chosen yet: → the first', tabs('ArrowRight', -1), 0);
eq('all disabled', tabs('ArrowRight', 0, { skip: () => true }), null);
eq('Ctrl+→ is not theirs', tabs('ArrowRight', 0, { mods: { ctrl: true } }), null);
eq('no tabs', groupKey(key('ArrowRight'), { index: -1, count: 0 }), null);

// ---- Dropdown ----
const dd = (k, s, mods) => dropdownKey(key(k, mods), { count: 5, ...s });
eq('closed: Space opens on the current', dd(' ', { open: false, lit: -1, current: 2 }), { open: true, lit: 2 });
eq('closed: Enter, ↓ and ↑ open it too', [dd('Enter', { open: false, current: 1 }).open, dd('ArrowDown', { open: false, current: 1 }).open, dd('ArrowUp', { open: false, current: 1 }).open], [true, true, true]);
eq('closed with no choice: lit on the first', dd('ArrowDown', { open: false, lit: -1, current: -1 }), { open: true, lit: 0 });
eq('closed: Escape is not its key', dd('Escape', { open: false, current: 1 }), null);
eq('closed: a letter is left to type-ahead', dd('x', { open: false, current: 1 }), null);
eq('open: ↓ the next', dd('ArrowDown', { open: true, lit: 2 }), { open: true, lit: 3 });
eq('open: ↓ stops at the last', dd('ArrowDown', { open: true, lit: 4 }), { open: true, lit: 4 });
eq('open: ↑ stops at the first', dd('ArrowUp', { open: true, lit: 0 }), { open: true, lit: 0 });
eq('open: ↓ skips a disabled one', dropdownKey(key('ArrowDown'), { open: true, lit: 1, count: 5, skip: (i) => i === 2 }), { open: true, lit: 3 });
eq('open: Home and End', [dd('Home', { open: true, lit: 3 }).lit, dd('End', { open: true, lit: 1 }).lit], [0, 4]);
eq('open: Enter chooses the lit one', dd('Enter', { open: true, lit: 3 }), { open: false, choose: 3 });
eq('open: Space too', dd(' ', { open: true, lit: 1 }), { open: false, choose: 1 });
eq('open: Enter with none lit just closes', dd('Enter', { open: true, lit: -1 }), { open: false });
eq('open: Escape closes', dd('Escape', { open: true, lit: 2 }), { open: false });
eq('open: Tab closes and goes on', dd('Tab', { open: true, lit: 2 }), { open: false, pass: true });
eq('Ctrl+↓ is not its key', dd('ArrowDown', { open: false, current: 1 }, { ctrl: true }), null);

// ---- the wheel ----
const wheel = (e, o) => wheelValue(e, { value: 5, min: 0, max: 10, step: 1, ...o });
eq('a notch up adds a step', wheel({ notches: 1 }), 6);
eq('down takes one', wheel({ notches: -1 }), 4);
eq('several notches', wheel({ notches: 3 }), 8);
eq('it stays inside the range', [wheel({ notches: 20 }), wheel({ notches: -20 })], [10, 0]);
eq('no notch: nothing', wheel({ notches: 0, deltaY: 10 }), null);
eq('a touchpad swipe without the focus leaves the page scrolling', wheel({ notches: 1, precise: true }), null);
eq('with the focus it moves it', wheel({ notches: 1, precise: true }, { focused: true }), 6);
eq('never while it glides', wheel({ notches: 1, precise: true, momentum: true }, { focused: true }), null);
eq('a stepper with no bounds', wheelValue({ notches: -2 }, { value: 3, step: 0.5 }), 2);

// ---- tooltips ----
const area = { x: 0, y: 0, w: 800, h: 600 };
const size = { w: 120, h: 22 };
eq('below the control', tipSide({ x: 100, y: 100, w: 40, h: 28 }, size, area), 'below');
eq('above near the bottom edge', tipSide({ x: 100, y: 570, w: 40, h: 28 }, size, area), 'above');
eq('above when the pointer is where it would go', tipSide({ x: 100, y: 100, w: 40, h: 28 }, size, area, { x: 150, y: 140 }), 'above');
eq('below when the pointer is elsewhere', tipSide({ x: 100, y: 100, w: 40, h: 28 }, size, area, { x: 400, y: 400 }), 'below');

// popoverRoom: the Screenshots bar's Options menu (a bar 50 px tall near the bottom of a 1920×1080 work area that ends at
// 1010 above the Dock) gets the room above the bar, not the bar's own height
const work = { x: 0, y: 25, w: 1920, h: 985 };
const opt = { x: 1200, y: 918, w: 90, h: 34 };
eq('room above a bar near the bottom', popoverRoom(opt, work, true), 918 - 25 - 12);
eq('room below it is the floor (80)', popoverRoom(opt, work, false), 80);
eq('room below a control near the top', popoverRoom({ x: 0, y: 40, w: 90, h: 34 }, work, false), 25 + 985 - 74 - 12);
eq('second display offset', popoverRoom({ x: 2000, y: 900, w: 90, h: 34 }, { x: 1920, y: 0, w: 1280, h: 1024 }, true), 900 - 12);
eq('no room either side: below unless it covers the pointer', tipSide({ x: 0, y: 10, w: 40, h: 570 }, size, area), 'below');
eq('with no area it goes below', tipSide({ x: 0, y: 0, w: 10, h: 10 }, size, null), 'below');

// ---- progress ----
eq('determinate from the start', progressSpan(0.4), { from: 0, to: 0.4 });
eq('kept between 0 and 1', [progressSpan(-1).to, progressSpan(3).to], [0, 1]);
eq('indeterminate starts at the start edge', progressSpan(null, 0), { from: 0, to: 0 });
const mid = progressSpan(undefined, 750, { period: 1500, band: 0.3 });
eq('halfway the band is in the middle', [+mid.from.toFixed(2), +mid.to.toFixed(2)], [0.35, 0.65]);
eq('it comes round again', progressSpan(null, 1500 + 750, { period: 1500 }), mid);
eq('reduced motion: still over the bar', progressSpan(null, 750, { reduceMotion: true }), { from: 0, to: 1, still: true });
eq('NaN counts as not known', progressSpan(NaN, 0).to, 0);

// ---- the disclosure triangle (issue #143) ----
eq('open points down', [disclosureDir(true), disclosureDir(true, true)], ['down', 'down']);
eq('closed points toward the text', [disclosureDir(false), disclosureDir(false, true)], ['right', 'left']);
{
  const g = chevronGeometry('right');
  eq('a 12 px chevron: a square of 8 rotated', [g.side, g.deg], [8, -45]);
  eq('the default box is 16 px', DISCLOSURE_BOX, 16);
  eq('the chevron is bigger than the old 10 px glyph', Math.round(g.side * Math.SQRT2) >= DISCLOSURE_SIZE - 1, true);
  // right and left are mirror images in the box
  const l = chevronGeometry('left');
  eq('left mirrors right', +(g.left + l.left + g.side).toFixed(2), DISCLOSURE_BOX);
  eq('same height either way', g.top, l.top);
  // down and up are mirror images vertically, and centred horizontally
  const d = chevronGeometry('down', 12, 16, 20), u = chevronGeometry('up', 12, 16, 20);
  eq('down is centred across', d.left, (16 - d.side) / 2);
  eq('up mirrors down', +(d.top + u.top + d.side).toFixed(2), 20);
  eq('down moves up toward its strokes', d.top < (20 - d.side) / 2, true);
  eq('right moves back from its tip', g.left < (DISCLOSURE_BOX - g.side) / 2, true);
  eq('rotations', ['down', 'left', 'up'].map((x) => chevronGeometry(x).deg), [45, 135, -135]);
  eq('an unknown direction draws right', chevronGeometry('sideways').deg, -45);
  eq('a tiny one keeps a 4 px square and a thin stroke', [chevronGeometry('down', 3).side, chevronGeometry('down', 8).stroke], [4, 1.5]);
  // the drawn strokes (the tip and both arm ends) stay inside the box, and their middle is the box's middle
  const ink = (c) => {
    const a = (c.deg * Math.PI) / 180, cx = c.left + c.side / 2, cy = c.top + c.side / 2, h = c.side / 2;
    return [[h, h], [h, -h], [-h, h]].map(([x, y]) => [cx + x * Math.cos(a) - y * Math.sin(a), cy + x * Math.sin(a) + y * Math.cos(a)]);
  };
  for (const sz of [10, 12, 13]) for (const dir of ['right', 'down', 'left', 'up']) {
    const pts = ink(chevronGeometry(dir, sz, 16, 20));
    const xs = pts.map((q) => q[0]), ys = pts.map((q) => q[1]);
    eq(`${dir} ${sz} inside`, Math.min(...xs) >= 0 && Math.max(...xs) <= 16 && Math.min(...ys) >= 0 && Math.max(...ys) <= 20, true);
    eq(`${dir} ${sz} centred`, [Math.abs((Math.min(...xs) + Math.max(...xs)) / 2 - 8) < 0.6, Math.abs((Math.min(...ys) + Math.max(...ys)) / 2 - 10) < 0.6], [true, true]);
  }
}

console.log(`control-model: ${ok} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
