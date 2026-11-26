// always_on_top_scene.mjs — always on top in every theme (docs/WINDOWS.md "Always on top"), run by
// tests/workspaces_test.mjs: node tests/workspaces_test.mjs always_on_top_scene.mjs <default|classic|workspaces|celeste>.
// One 1920×1080 display, three windows. It checks, in the theme's real tree (the stub keeps each node's children in
// order): the window set always on top is drawn over the others while a normal window under it has the keyboard; a
// click on a normal window raises it within the normal layer only; the theme's own window menu (a right click on an app's
// own title bar, w.windowMenu) offers Always on Top, checked while it is, and turning it off puts the window back among
// the others; the Window menu (sdk/windowmenu.js) has the same item; a dialog of an always-on-top window stays over it;
// a window that skips the pager is not drawn in the Overview. Prints "ok <name>" or "FAIL <name> <detail>".
const React = (await import('react')).default;
const rt = await import('@lordos/runtime');
const sdk = await import('@lordos/sdk');
const theme = globalThis.__themeUnderTest;
if (theme === 'default') await import('../../themes/default/index.jsx');
else if (theme === 'classic') await import('../../themes/classic/index.jsx');
else if (theme === 'celeste') await import('../../themes/celeste/index.jsx');
else await import('../../themes/workspaces/index.jsx');
const { texts, parents, removed, kids } = globalThis.__lordTest;
const flush = async () => { for (let i = 0; i < 30; i++) await new Promise((res) => setImmediate(res)); await new Promise((res) => setTimeout(res, 5)); for (let i = 0; i < 10; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, got, want) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  console.log(g === w ? `ok ${name}` : `FAIL ${name} got ${g}, want ${w}`);
};
const attached = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return parents.has(id); };
const shown = (v) => [...texts].filter(([id, x]) => x === v && attached(id)).map(([id]) => id);
// a menu row: the node above the label's text node (a Text holds its string in a child node)
const rowOf = (textId) => parents.get(parents.get(textId));
const under = (id, v) => texts.get(id) === v || (kids.get(id) || []).some((k) => under(k, v));
const click = async (id) => { globalThis.__lordEvent('down', id, 0, 0, 0); globalThis.__lordEvent('up', id, 0, 0, 0); await flush(); };

// Drawing order: a node's path from the root, as child indices; two paths compare like words (lord-server paints and
// hit-tests the tree in this order, docs/LAYERS.md).
const path = (id) => { const p = []; for (let n = id; parents.has(n); n = parents.get(n)) p.unshift((kids.get(parents.get(n)) || []).indexOf(n)); return p; };
const cmp = (a, b) => { for (let i = 0; i < Math.min(a.length, b.length); i++) if (a[i] !== b[i]) return a[i] - b[i]; return a.length - b.length; };
const drawn = (ids) => ids.filter((id) => sdk.wm.frameNode(id) && attached(sdk.wm.frameNode(id)))
  .map((id) => [id, path(sdk.wm.frameNode(id))]).sort((x, y) => cmp(x[1], y[1])).map(([id]) => id);

const D = { id: 1, name: 'LordOS', x: 0, y: 0, w: 1920, h: 1080, scale: 100, main: true };
lord.screen.displays = [D]; lord.screen.width = 1920; lord.screen.height = 1080; globalThis.__lordScreen();
sdk.settings.set({ welcomeShown: true });
rt.render(React.createElement(() => globalThis.__shellElement));
await flush();

const LABEL = sdk.t('shell.window.alwaysOnTop');
// a: an app with its own title bar (as a Linux app that draws its own frame), so every theme draws w.windowMenu for it
const a = sdk.wm.open('files', { frame: { x: 200, y: 150, w: 700, h: 480 }, args: { csd: true } });
const b = sdk.wm.open('terminal', { frame: { x: 260, y: 190, w: 700, h: 480 } });
const c = sdk.wm.open('calculator', { frame: { x: 320, y: 230, w: 300, h: 400 } });
await flush();
const all = [a, b, c];
check('three windows drawn, the last opened on top', drawn(all), [a, b, c]);

// the theme's window menu on a (a right click on its own bar): Always on Top
sdk.wm.focus(a);
sdk.wm.showWindowMenu(a, 300, 200);
await flush();
const item = shown(LABEL);
check('the theme\'s window menu offers Always on Top', item.length, 1);
if (item.length) await click(rowOf(item[0]));
check('chosen: the window is always on top', sdk.wm.layer(a), 'above');
check('...the menu closed', shown(LABEL).length, 0);
sdk.wm.closeWindowMenu(a);
await flush();
sdk.wm.focus(b);
await flush();
check('a normal window brought forward has the keyboard', [sdk.wm.focused(), sdk.wm.hasKeyboard(b), sdk.wm.hasKeyboard(a)], [b, true, false]);
check('...and is drawn under the always-on-top one', drawn(all), [c, b, a]);
sdk.wm.focus(c);
await flush();
check('a click on another normal window raises it within the normal layer only', drawn(all), [b, c, a]);
check('...with the keyboard', sdk.wm.focused(), c);

// the menu shows it checked; the Window menu (menu bar, Classic's and Celeste's in-window menus) has the same item
sdk.wm.showWindowMenu(a, 300, 200);
await flush();
const again = shown(LABEL);
const row = again.length ? rowOf(again[0]) : 0;
check('the item is checked while it is on', under(row, '✓'), true);
const wmenu = sdk.windowMenuItems(a).find((x) => x && x.label === LABEL);
check('the Window menu has the item, checked', [!!wmenu, !!wmenu?.checked], [true, true]);
if (again.length) await click(row);
sdk.wm.closeWindowMenu(a);
await flush();
check('turned off: a normal window again', sdk.wm.layer(a), null);
sdk.wm.focus(b);
await flush();
check('...so a normal window brought forward is drawn over it', drawn(all), [c, a, b]);

// a dialog of an always-on-top window stays over it, though a normal window came forward after it
sdk.wm.setLayer(b, 'above');
const d = sdk.wm.open('settings', { frame: { x: 300, y: 260, w: 400, h: 300 }, args: { parent: b } });
sdk.wm.focus(c);
await flush();
check('the dialog of an always-on-top window stays over it and over the normal ones', drawn([a, b, c, d]), [a, c, b, d]);
check('...while the normal window has the keyboard', sdk.wm.focused(), c);
sdk.wm.close(d);
sdk.wm.setLayer(b, null);
await flush();

// a window that asks to be left out of the pager (X11 _NET_WM_STATE_SKIP_PAGER) isn't drawn in the Overview
sdk.wm.setArgs(c, { surface: 9, skipPager: true });
await flush();
check('skip pager: drawn on the desktop', drawn(all).includes(c), true);
sdk.openOverview();
await flush();
check('skip pager: not drawn in the Overview', [drawn(all).includes(c), drawn(all).includes(a)], [false, true]);
sdk.closeOverview();
await flush();
check('skip pager: back after it', drawn(all).includes(c), true);
process.exit(0);
