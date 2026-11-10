// The tray icons of Linux apps without a screen (sdk/traymodel.js): which show, the letter without an icon, a dbusmenu
// layout as the SDK's menu, where the menu opens, and a late menu opening nothing. node server/tests/tray_model_test.mjs
import { trayShown, trayInitial, trayHeader, trayMenu, trayMenuAt, trayMenuWanted, TRAY_MENU_WAIT } from '../../sdk/traymodel.js';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };

// which show
eq('active', trayShown({ id: 1, status: 'Active' }), true);
eq('needs attention', trayShown({ id: 1, status: 'NeedsAttention' }), true);
eq('passive hides', trayShown({ id: 1, status: 'Passive' }), false);
eq('none', trayShown(undefined), false);

// the letter and the header
eq('initial', trayInitial({ title: 'sync' }), 'S');
eq('initial from the tooltip', trayInitial({ title: '', tooltip: ' élan' }), 'É');
eq('initial of nothing', trayInitial({ title: '' }), '•');
eq('initial, not half a pair', trayInitial({ title: '\u{1F600}x' }), '\u{1F600}');
eq('header: tooltip first', trayHeader({ title: 'Chat', tooltip: 'Chat: 3 unread' }), 'Chat: 3 unread');
eq('header: the title', trayHeader({ title: 'Games', tooltip: '' }), 'Games');

// the layout as <ContextMenu> items
const picked = [];
const pick = (id) => picked.push(id);
const layout = [
  { sep: true },
  { id: 1, label: 'Open', enabled: true },
  { sep: true },
  { sep: true },
  { id: 2, label: 'Notifications', enabled: true, toggle: 'checkmark', on: true },
  { id: 3, label: 'Off', enabled: false },
  { id: 4, label: 'Sync', enabled: true, sub: [
    { id: 41, label: 'Pause', enabled: true },
    { id: 42, label: 'Folders', enabled: true, sub: [{ id: 421, label: 'Documents', enabled: true }] },
    { id: 43, label: 'Nothing', enabled: true, sub: [] },
  ] },
  { id: 5, label: 'Empty', enabled: true, sub: [] },
  { sep: true },
  null,
  'junk',
];
const menu = trayMenu(layout, pick);
eq('labels and separators', menu.map((m) => (m === '-' ? '-' : m.label)), ['Open', '-', 'Notifications', 'Off', 'Sync', 'Empty']);
eq('checked', menu[2].checked, true);
eq('disabled', menu[3].disabled, true);
eq('a submenu', menu[4].submenu.map((m) => m.header || m.label), ['Pause', 'Folders', 'Documents']);
eq('a deeper one under a header', menu[4].submenu[1], { header: 'Folders' });
eq('an empty submenu is off', [menu[5].disabled, menu[5].submenu], [true, undefined]);
menu[0].onClick();
menu[4].submenu[2].onClick();
eq('picks', picked, [1, 421]);
eq('nothing', trayMenu(undefined, pick), []);

// where it opens
const d = { x: 0, y: 0, w: 1440, h: 900 };
const top = trayMenuAt([1300, 2, 26, 22], d);
eq('menu bar: below', [top.x, top.y, top.bounds], [1300, 28, { w: 1440, h: 900 }]);
eq('frame: the display, from the item', top.frame, { position: 'absolute', left: -1300, top: -2, width: 1440, height: 900, pointerEvents: 'box-none' });
const bottom = trayMenuAt([1300, 856, 26, 40], d);
eq('taskbar at the bottom: above, bounded by the bar', [bottom.y > 1e5, bottom.bounds], [true, { w: 1440, h: 856 }]);
const d2 = { x: 1440, y: 0, w: 1920, h: 1080 };
eq('second display', trayMenuAt([3300, 1036, 26, 40], d2).x, 1860);

// a late menu opens nothing
eq('asked now', trayMenuWanted({ id: 3, at: 1000 }, 3, 1200), true);
eq('another item', trayMenuWanted({ id: 3, at: 1000 }, 4, 1200), false);
eq('too late', trayMenuWanted({ id: 3, at: 1000 }, 3, 1000 + TRAY_MENU_WAIT), false);
eq('never asked', trayMenuWanted(null, 3, 1000), false);

console.log(`tray-model-test: ${ok} ok`);
