// The tray icons of Linux apps without a screen (sdk/traymodel.js): which show, the letter without an icon, a dbusmenu
// layout as the SDK's menu, where the menu opens, a late menu opening nothing, and the menu opening only on the
// display that was clicked. node server/tests/tray_model_test.mjs
import { trayShown, trayInitial, trayHeader, trayMenu, trayMenuAt, trayMenuWanted, TRAY_MENU_WAIT, trayAsk, trayMenuShow, trayMenuHere, trayMenuUpdate } from '../../sdk/traymodel.js';

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

// one menu bar per display: the menu opens only on the display whose copy of the item was clicked
const entries = [{ id: 5, label: 'Quit', enabled: true }];
const ask = trayAsk(3, 2, 1000);
eq('asked from display 2', ask, { id: 3, at: 1000, display: 2 });
eq('still wanted', trayMenuWanted(ask, 3, 1100), true);
const open3 = trayMenuShow(ask, 3, entries);
eq('kept with its display', open3, { 3: { items: entries, display: 2 } });
const copies = [0, 1, 2].map((d) => trayMenuHere(open3, 3, d));
eq('one copy draws it, three displays', copies.filter(Boolean).length, 1);
eq('the clicked display', trayMenuHere(open3, 3, 2), entries);
eq('not the main display', trayMenuHere(open3, 3, 0), null);
eq('not another item', trayMenuHere(open3, 4, 2), null);
eq('display 0 is a display', trayMenuHere(trayMenuShow(trayAsk(3, 0, 1), 3, entries), 3, 0), entries);
eq('an empty menu still opens (no entries row)', trayMenuHere(trayMenuShow(ask, 3, null), 3, 2), []);
eq('one bar, no display known', trayMenuHere(trayMenuShow(trayAsk(3, undefined, 1), 3, entries), 3, null), entries);
eq('nothing shown', trayMenuHere({}, 3, 2), null);
eq('old shape is not drawn', trayMenuHere({ 3: entries }, 3, 2), null);

// the app built its menu again (new ids) while it is open: the open menu takes the new rows, on the same display
const rebuilt = [{ id: 15, label: 'Quit', enabled: true }];
const upd = trayMenuUpdate(open3, 3, rebuilt);
eq('update: new ids', trayMenuHere(upd, 3, 2), rebuilt);
eq('update: same display', trayMenuHere(upd, 3, 0), null);
eq('update: a closed menu stays closed', trayMenuUpdate({}, 3, rebuilt), {});
eq('update: another item untouched', trayMenuUpdate(open3, 4, rebuilt), open3);
eq('update: bad items', trayMenuHere(trayMenuUpdate(open3, 3, 'x'), 3, 2), []);

console.log(`tray-model-test: ${ok} ok`);
