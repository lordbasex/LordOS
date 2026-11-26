// Where a status item's panel opens (sdk/statusplace.js): toward the middle of the display, so a bar at the bottom
// (the Classic taskbar) opens it above. node server/tests/statusplace_test.mjs
import { panelPlacement, panelRoom } from '../../sdk/statusplace.js';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };
const d = { x: 0, y: 0, w: 1440, h: 900 };
// Flow's menu bar: items at the top right
eq('menu bar', panelPlacement([1300, 2, 26, 22], d), { position: 'absolute', top: 26, right: 0 });
// the Classic taskbar at the bottom (issue #29): above the item, never below the screen
eq('taskbar bottom', panelPlacement([1300, 856, 26, 40], d), { position: 'absolute', bottom: 44, right: 0 });
eq('taskbar top', panelPlacement([1300, 4, 26, 40], d), { position: 'absolute', top: 44, right: 0 });
// right-to-left: the tray is at the start of the bar
eq('rtl tray', panelPlacement([40, 856, 26, 40], d), { position: 'absolute', bottom: 44, left: 0 });
eq('gap', panelPlacement([1300, 856, 26, 8], d, 14).bottom, 22);
// a second display below the first one: its own middle counts
const d2 = { x: 0, y: 900, w: 1920, h: 1080 };
eq('second display top', panelPlacement([1800, 904, 26, 40], d2).top, 44);
eq('second display bottom', panelPlacement([1800, 1936, 26, 40], d2).bottom, 44);
// #145: how tall a panel may be before it leaves the screen. A 1920x1080 screen at 150% is 1280x720 logical; Lord
// Flow's menu bar on top (24) and the Dock at the bottom (work area 24..650): from the gap under the item to the
// Dock, less the margin
const s150 = { x: 0, y: 0, w: 1280, h: 720 };
eq('room under the menu bar, above the Dock', panelRoom([1150, 1, 26, 22], s150, { x: 0, y: 24, w: 1280, h: 626 }), 650 - (23 + 4) - 8);
// the Dock hidden or at the side: down to the display's edge
eq('room with no Dock below', panelRoom([1150, 1, 26, 22], s150, { x: 0, y: 24, w: 1280, h: 696 }), 720 - 27 - 8);
eq('room with no work area', panelRoom([1150, 1, 26, 22], s150), 720 - 27 - 8);
// a work area larger than its display (a stale one) never gives more than the display
eq('room clamped to the display', panelRoom([1150, 1, 26, 22], s150, { x: 0, y: 0, w: 1280, h: 4000 }), 720 - 27 - 8);
// the Classic taskbar at the bottom: up from the item to the top of the display (or of its work area)
eq('room above a bottom taskbar', panelRoom([1150, 684, 26, 32], s150, { x: 0, y: 0, w: 1280, h: 680 }), 684 - 4 - 8);
eq('room above a bottom taskbar, top bar', panelRoom([1150, 684, 26, 32], s150, { x: 0, y: 30, w: 1280, h: 650 }), 684 - 4 - 30 - 8);
// a second display below the first: its own edges
eq('room on a second display', panelRoom([1800, 904, 26, 22], d2, { x: 0, y: 926, w: 1920, h: 1000 }), 1926 - 930 - 8);
// a tiny display: never less than the head and the buttons need
eq('room least', panelRoom([100, 1, 26, 22], { x: 0, y: 0, w: 320, h: 120 }), 160);
eq('room least given', panelRoom([100, 1, 26, 22], { x: 0, y: 0, w: 320, h: 120 }, null, 4, 8, 90), 90);
console.log(`statusplace-test: ${ok} ok`);
