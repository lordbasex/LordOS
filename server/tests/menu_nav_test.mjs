// How a menu answers the keyboard and the pointer (sdk/menunav.js): the highlight's moves, the keys, type-ahead,
// the submenu's side and the safe triangle. node server/tests/menu_nav_test.mjs
import { selectable, stepIndex, edgeIndex, menuAction, typeAhead, matchIndex, subPlace, aiming, barIndex, TYPE_MS } from '../../sdk/menunav.js';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };

const items = [
  { header: 'Recent' },
  { label: 'Open' },
  '-',
  { label: 'Save', disabled: true },
  { label: 'Save As…' },
  { swatches: [] },
  { label: 'Print' },
  { label: 'Properties' },
  '-',
];

// what the highlight can rest on
eq('separator', selectable('-'), false);
eq('header', selectable({ header: 'x' }), false);
eq('disabled', selectable({ label: 'x', disabled: true }), false);
eq('swatches', selectable({ swatches: [] }), false);
eq('null', selectable(null), false);
eq('a row', selectable({ label: 'x' }), true);

// ↓ and ↑ skip separators, headers and disabled rows, and wrap
eq('down from nothing', stepIndex(items, -1, 1), 1);
eq('up from nothing', stepIndex(items, -1, -1), 7);
eq('down skips', stepIndex(items, 1, 1), 4);
eq('down skips swatches', stepIndex(items, 4, 1), 6);
eq('down wraps', stepIndex(items, 7, 1), 1);
eq('up wraps', stepIndex(items, 1, -1), 7);
eq('up skips', stepIndex(items, 4, -1), 1);
eq('active out of range', stepIndex(items, 40, 1), 1);
eq('nothing selectable', stepIndex(['-', { label: 'x', disabled: true }], -1, 1), -1);
eq('empty', stepIndex([], -1, 1), -1);
eq('one row stays', stepIndex([{ label: 'x' }], 0, 1), 0);
eq('home', edgeIndex(items), 1);
eq('end', edgeIndex(items, true), 7);

// the keys
eq('down', menuAction({ key: 'ArrowDown' }), 'next');
eq('up', menuAction({ key: 'ArrowUp' }), 'prev');
eq('home key', menuAction({ key: 'Home' }), 'first');
eq('end key', menuAction({ key: 'End' }), 'last');
eq('enter', menuAction({ key: 'Enter' }), 'activate');
eq('space', menuAction({ key: ' ', char: ' ' }), 'activate');
eq('escape', menuAction({ key: 'Escape' }), 'close');
eq('right opens', menuAction({ key: 'ArrowRight' }), 'open');
eq('left goes back', menuAction({ key: 'ArrowLeft' }), 'back');
eq('rtl: left opens', menuAction({ key: 'ArrowLeft' }, true), 'open');
eq('rtl: right goes back', menuAction({ key: 'ArrowRight' }, true), 'back');
eq('a letter types', menuAction({ key: 'p', char: 'p' }), 'type');
eq('a shifted letter types', menuAction({ key: 'P', char: 'P', shift: true }), 'type');
eq('a non-Latin letter types', menuAction({ key: 'ж', char: 'ж' }), 'type');
eq('ctrl+letter does not', menuAction({ key: 'p', char: 'p', ctrl: true }), null);
eq('alt+letter does not', menuAction({ key: 'p', char: 'p', alt: true }), null);
eq('tab does nothing', menuAction({ key: 'Tab', char: '\t' }), null);
eq('no event', menuAction(null), null);

// type-ahead: letters typed close together add up, a pause starts again
let s = typeAhead(null, 'P', 1000);
eq('first letter', s.buf, 'p');
s = typeAhead(s, 'r', 1000 + 200);
eq('second letter', s.buf, 'pr');
s = typeAhead(s, 'o', 1200 + TYPE_MS + 1);
eq('after a pause', s.buf, 'o');

eq('first match', matchIndex(items, -1, 'p'), 6);
eq('next match round', matchIndex(items, 6, 'p'), 7);
eq('wraps to the first match', matchIndex(items, 7, 'p'), 6);
eq('more letters narrow', matchIndex(items, -1, 'pro'), 7);
eq('current row kept while it matches', matchIndex(items, 6, 'pr'), 6);
eq('the same letter again cycles', matchIndex(items, 6, 'pp'), 7);
eq('disabled rows are skipped', matchIndex(items, -1, 'save'), 4);
eq('case and spaces', matchIndex([{ label: '  Élan' }], -1, 'é'), 0);
eq('no match', matchIndex(items, 1, 'z'), -1);
eq('empty buffer', matchIndex(items, 1, ''), -1);

// the submenu's side: beside the parent in the reading direction, the other side when there is no room
const area = { x: 0, w: 1000 };
eq('ltr: right', subPlace({ x: 100, w: 200 }, 180, area), { x: 296, side: 'right' });
eq('rtl: left', subPlace({ x: 500, w: 200 }, 180, area, true), { x: 324, side: 'left' });
eq('ltr near the right edge: left', subPlace({ x: 700, w: 200 }, 180, area), { x: 524, side: 'left' });
eq('rtl near the left edge: right', subPlace({ x: 20, w: 200 }, 180, area, true), { x: 216, side: 'right' });
eq('second display', subPlace({ x: 1900, w: 200 }, 180, { x: 1000, w: 1280 }), { x: 2096, side: 'right' });
eq('second display, no room', subPlace({ x: 2000, w: 200 }, 180, { x: 1000, w: 1280 }), { x: 1824, side: 'left' });
// room on neither side: the wider side, kept inside
eq('no room: wider side', subPlace({ x: 100, w: 250 }, 300, { x: 0, w: 500 }), { x: 196, side: 'right' });
eq('no room: wider left', subPlace({ x: 300, w: 150 }, 330, { x: 0, w: 500 }), { x: 4, side: 'left' });

// the safe triangle: moving toward the submenu keeps it, moving down the menu does not
const box = { x: 300, y: 100, w: 180, h: 200 };
eq('toward the submenu', aiming({ x: 200, y: 110 }, { x: 240, y: 130 }, box, 'right'), true);
eq('straight down the menu', aiming({ x: 200, y: 110 }, { x: 200, y: 140 }, box, 'right'), false);
eq('away from it', aiming({ x: 200, y: 110 }, { x: 160, y: 130 }, box, 'right'), false);
eq('a submenu on the left', aiming({ x: 600, y: 110 }, { x: 560, y: 150 }, { x: 380, y: 100, w: 180, h: 200 }, 'left'), true);
eq('not moving', aiming({ x: 200, y: 110 }, { x: 200, y: 110 }, box, 'right'), false);
eq('no start', aiming(null, { x: 200, y: 110 }, box, 'right'), false);

// between the menus of a bar
eq('next menu', barIndex(5, 1, 1), 2);
eq('wraps to the first', barIndex(5, 4, 1), 0);
eq('wraps to the last', barIndex(5, 0, -1), 4);
eq('no menus', barIndex(0, 0, 1), -1);

console.log(`menu-nav: ${ok} ok`);
