// Where a status item's panel opens (sdk/statusplace.js): toward the middle of the display, so a bar at the bottom
// (the Classic taskbar) opens it above. node server/tests/statusplace_test.mjs
import { panelPlacement } from '../../sdk/statusplace.js';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };
const d = { x: 0, y: 0, w: 1440, h: 900 };
// Lord Flow's menu bar: items at the top right
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
console.log(`statusplace-test: ${ok} ok`);
