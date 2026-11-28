// window_requests_scene.mjs — three window requests in every theme (GitHub #151, #219), run by tests/workspaces_test.mjs:
// node tests/workspaces_test.mjs window_requests_scene.mjs <default|classic|workspaces|celeste>.
// In the theme's real tree: Settings → Appearance → Window options → Window buttons makes the title bar and its buttons (their hit
// areas) larger, and the theme's chrome tells the window manager; ❖ + drag anywhere in a window (sdk/moddrag.js) starts
// the same move or resize as the title bar and the borders, from the corner nearest the pointer, brings a maximized
// window out of its place, and leaves full-screen windows and presses outside windows to the app; the modifier the person
// chose reaches lord-server; the tiling switch takes the layouts out of the Window menu and the theme's own title bar
// menu. Prints "ok <name>" or "FAIL <name> <detail>".
const React = (await import('react')).default;
const rt = await import('@lordos/runtime');
const sdk = await import('@lordos/sdk');
const { startModDrag } = await import('../../sdk/moddrag.js');
const theme = globalThis.__themeUnderTest;
if (theme === 'default') await import('../../themes/default/index.jsx');
else if (theme === 'classic') await import('../../themes/classic/index.jsx');
else if (theme === 'celeste') await import('../../themes/celeste/index.jsx');
else await import('../../themes/workspaces/index.jsx');
const { texts, parents, removed, kids, styles } = globalThis.__lordTest;
const flush = async () => { for (let i = 0; i < 30; i++) await new Promise((res) => setImmediate(res)); await new Promise((res) => setTimeout(res, 5)); for (let i = 0; i < 10; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, got, want) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  console.log(g === w ? `ok ${name}` : `FAIL ${name} got ${g}, want ${w}`);
};
const attached = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return parents.has(id); };
const shown = (v) => [...texts].filter(([id, x]) => x === v && attached(id)).map(([id]) => id);
const subtree = (id) => { const out = [id]; for (let i = 0; i < out.length; i++) out.push(...(kids.get(out[i]) || [])); return out; };

// lord-server's side, as the SDK sees it: the drag in C (lord.wm.grab) and the modifier the person chose
const grabs = [], mods = [];
lord.wm = {
  grab: (node, edges) => { grabs.push(edges); return true; },
  cancel: () => {}, pointer: () => ({ x: 0, y: 0, active: false, edges: 0 }),
  dragModifier: (m) => { mods.push(m); return true; },
};

const D = { id: 1, name: 'LordOS', x: 0, y: 0, w: 1920, h: 1080, scale: 100, main: true };
lord.screen.displays = [D]; lord.screen.width = 1920; lord.screen.height = 1080; globalThis.__lordScreen();
sdk.settings.set({ welcomeShown: true });
rt.render(React.createElement(() => globalThis.__shellElement));
startModDrag();                                   // startShell does it in a session (the scene's shim takes startShell)
await flush();

// each theme's title bar and buttons at the normal size: [bar height, button width, button height, chrome over the bar]
const M = { default: [40, 13, 13, 3], classic: [34, 44, 34, 2], workspaces: [36, 28, 28, 2], celeste: [38, 13, 13, 3] }[theme];
const a = sdk.wm.open('calculator', { frame: { x: 200, y: 150, w: 700, h: 480 } });
await flush();
// The title bar as drawn, every node's style in tree order: nothing saved must draw exactly what each theme drew before
// the setting existed (the fingerprints below were taken from the frames before #151, with the same scene); a theme that
// changes its own title bar on purpose takes its new fingerprint from VERBOSE=1's "bar fingerprint" line.
const fnv = (str) => { let h = 0x811c9dc5; for (let i = 0; i < str.length; i++) { h ^= str.charCodeAt(i); h = Math.imul(h, 0x01000193) >>> 0; } return h.toString(16); };
const styleOf = (n) => JSON.stringify(Object.keys(styles.get(n) || {}).sort().map((k) => [k, styles.get(n)[k]]));
function barPrint() {
  const nodes = subtree(sdk.wm.frameNode(a)).filter(attached);
  const bar = nodes.find((n) => styles.get(n)?.height === M[0] && styles.get(n)?.flexDirection === 'row');
  if (!bar) return 'none';
  const walk = (n) => [styleOf(n), ...(kids.get(n) || []).filter(attached).map(walk)].join('|');
  return fnv(walk(bar));
}
const BEFORE = { default: '256f8f7c', classic: '368f48b4', workspaces: '838a758d', celeste: 'f33f7e59' }[theme];
const print0 = barPrint();
if (process.env.VERBOSE || BEFORE === 'TBD') console.log(`ok bar fingerprint ${theme} ${print0}`);
check('nothing saved: the title bar is drawn exactly as before the setting existed', print0, BEFORE);
const sizes = { normal: 1, large: 1.25, xlarge: 1.5 };
for (const [size, k] of Object.entries(sizes)) {
  sdk.settings.set({ windowButtonSize: size });
  await flush();
  const bar = Math.round(M[0] * k), bw = Math.round(M[1] * k), bh = Math.round(M[2] * k);
  const nodes = subtree(sdk.wm.frameNode(a)).filter(attached);
  check(`${size}: a title bar ${bar} px high`, nodes.some((n) => styles.get(n)?.height === bar && styles.get(n)?.flexDirection === 'row'), true);
  check(`${size}: three buttons of ${bw} × ${bh} px (their hit areas)`, nodes.filter((n) => styles.get(n)?.width === bw && styles.get(n)?.height === bh).length, 3);
  check(`${size}: the window manager knows the frame (chrome)`, sdk.windowChrome().h, bar + M[3]);
}
sdk.settings.set({ windowButtonSize: 'normal' });
await flush();
check('back to normal: drawn as with nothing saved', barPrint(), print0);

// the modifier reaches lord-server: off (no gesture at all) unless the person chose one
check('by default lord-server is told off', mods[0], 'off');
sdk.settings.set({ windowDragModifier: 'alt' });
await flush();
check('Alt chosen in Settings: lord-server is told', mods.at(-1), 'alt');
sdk.settings.set({ windowDragModifier: 'off' });
await flush();
check('off again: lord-server is told', mods.at(-1), 'off');
grabs.length = 0;
check('off: even a press offered is never taken', [globalThis.__lordEvent('modgrab', sdk.wm.frameNode(a), 500, 400, 0), grabs.length], [false, 0]);
sdk.settings.set({ windowDragModifier: 'super' });
await flush();
check('❖ chosen: lord-server is told', mods.at(-1), 'super');

// a press with the modifier, as lord-server offers it (main.c → __lordEvent('modgrab', node, x, y, button))
const inner = sdk.wm.frameNode(a);
const modgrab = (id, x, y, button) => globalThis.__lordEvent('modgrab', id, x, y, button);
sdk.wm.focus(sdk.wm.open('terminal', { frame: { x: 900, y: 500, w: 500, h: 300 } }));
await flush();
grabs.length = 0;
check('❖ + left press in a window: taken', modgrab(inner, 500, 400, 0), true);
check('...the window\'s move (the title bar\'s path)', grabs, [0]);
check('...and it came to the front', sdk.wm.focused(), a);
grabs.length = 0;
check('❖ + right press near the bottom right corner: taken', modgrab(inner, 880, 610, 2), true);
check('...resized from the bottom right', grabs, [10]);
grabs.length = 0;
modgrab(inner, 210, 160, 2);
check('...near the top left: from the top left', grabs, [5]);
grabs.length = 0;
modgrab(inner, 880, 160, 1);
check('...the middle button near the top right: from the top right', grabs, [9]);
check('a press outside every window: the desktop\'s (not taken)', modgrab(0, 5, 5, 0), false);
check('the back button: the app\'s', modgrab(inner, 500, 400, 3), false);
sdk.wm.setFullscreen(a, true);
await flush();
check('a full-screen window: the app\'s (a game, a video)', modgrab(inner, 500, 400, 0), false);
sdk.wm.setFullscreen(a, false);
await flush();

// a maximized window comes out of its place once the pointer moves, at its own size, under the pointer
sdk.wm.toggleMaximize(a);
await flush();
grabs.length = 0;
const node = sdk.wm.frameNode(a);
check('maximized: taken', modgrab(node, 960, 500, 0), true);
check('...no move in C yet (it is maximized)', grabs, []);
globalThis.__lordEvent('move', node, 962, 501, 0);
check('...a jitter leaves it maximized', !!sdk.wm.get(a).maximized, true);
globalThis.__lordEvent('move', node, 1000, 540, 0);
globalThis.__lordEvent('up', node, 1000, 540, 0);
await flush();
const w = sdk.wm.get(a);
check('...dragged: back to its own size', [!!w.maximized, w.w, w.h], [false, 700, 480]);
check('...under the pointer, where it was grabbed', w.x <= 1000 && w.x + w.w >= 1000 && w.y <= 540, true);
grabs.length = 0;
check('maximized, right press: taken, nothing happens', [(sdk.wm.toggleMaximize(a), modgrab(sdk.wm.frameNode(a), 500, 500, 2)), grabs.length], [true, 0]);
sdk.wm.toggleMaximize(a);
await flush();

// the Window menu's tiling layouts, and each theme's own snap items in its title bar's menu
const MOVE = sdk.t('shell.window.moveResize');
// Celeste's menu flattens the Window menu's submenus (classic/components/Menu.jsx): the halves' header shows
const own = { classic: sdk.t('shell.classic.snapLeft'), workspaces: sdk.t('shell.ws.snapfly.open'), celeste: sdk.t('shell.window.section.halves') }[theme];
const c = sdk.wm.open('files', { frame: { x: 300, y: 200, w: 600, h: 400 }, args: { csd: true } });
await flush();
check('tiling on: Move & Resize in the Window menu', sdk.windowMenuItems(c).some((x) => x && x.label === MOVE), true);
sdk.wm.showWindowMenu(c, 400, 260);
await flush();
if (own) check('tiling on: the theme\'s title bar menu offers its layouts', shown(own).length > 0, true);
sdk.wm.closeWindowMenu(c);
sdk.settings.set({ windowMenuTiling: false });
await flush();
check('tiling off: no Move & Resize in the Window menu', sdk.windowMenuItems(c).some((x) => x && x.label === MOVE), false);
check('...the rest of the menu stays', sdk.windowMenuItems(c).some((x) => x && x.label === sdk.t('shell.window.minimize')), true);
sdk.wm.showWindowMenu(c, 400, 260);
await flush();
if (own) check('tiling off: the theme\'s title bar menu leaves its layouts out', shown(own).length, 0);
check('...and still offers Always on Top', shown(sdk.t('shell.window.alwaysOnTop')).length > 0, true);
sdk.wm.closeWindowMenu(c);
process.exit(0);
