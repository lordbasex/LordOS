// menubar_displays_scene.mjs — the second scene of tests/menubar_test.mjs (not run by itself), 0.8.8.6: Lord Flow's
// menu bar on every display. Three monitors side by side over the stub `lord` (the bundle's banner); <MenuBars> from
// themes/default/components/MenuBar.jsx through runtime/index.js and the real SDK. It checks one bar per display,
// the app's menus lit only on the focused window's display, a click on a dimmed bar lighting it, one clock timer
// for all the bars, the setting turned off, a monitor unplugged, and a clock panel opening under the clock on the
// display it was clicked on. Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import { MenuBars } from '../../themes/default/components/MenuBar.jsx';
import { NotificationCenter } from '../../themes/default/components/NotificationCenter.jsx';
import { makeTheme } from '../../themes/default/tokens.js';
import { ThemeProvider } from '../../sdk/index.js';

const { texts, styles, parents, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, got, want) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  console.log(g === w ? `ok ${name}` : `FAIL ${name} got ${g}, want ${w}`);
};
const attached = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return parents.has(id); };
// the <Text> nodes that show s now, in creation order (the bars are drawn in display order)
const textNodes = (s) => [...texts].filter(([id, v]) => v === s && attached(id)).map(([id]) => parents.get(id)).sort((a, b) => a - b);
const click = async (id) => {
  globalThis.__lordEvent('down', id, 10, 10, 0); await flush();
  globalThis.__lordEvent('up', id, 10, 10, 0); await flush();
};

// the timers alive (the clock's must be one, however many bars there are)
const live = new Set();
const realSet = globalThis.setInterval, realClear = globalThis.clearInterval;
globalThis.setInterval = (fn, ms, ...a) => { const id = realSet(fn, ms, ...a); live.add(id); return id; };
globalThis.clearInterval = (id) => { live.delete(id); realClear(id); };

// three monitors, the main one in the middle
const L = { id: 41, name: 'DP-1', x: 0, y: 0, w: 1920, h: 1080, scale: 100, main: false };
const M = { id: 42, name: 'DP-2', x: 1920, y: 0, w: 2560, h: 1440, scale: 100, main: true };
const R = { id: 43, name: 'HDMI-A-1', x: 4480, y: 0, w: 1920, h: 1080, scale: 100, main: false };
const plug = (list) => {
  lord.screen.displays = list;
  lord.screen.width = Math.max(...list.map((d) => d.x + d.w));
  lord.screen.height = Math.max(...list.map((d) => d.y + d.h));
  globalThis.__lordScreen();
};
plug([L, M, R]);

enableShellLayers();
const t = makeTheme({ mode: 'dark', accent: 'azul', effects: true });
const props = { every: true, focusRect: { x: 4600, y: 40, w: 900, h: 700 }, clock: null };
let rerender = null;
function Scene() {
  const [, set] = React.useState(0);
  rerender = () => set((v) => v + 1);
  const all = rt.displays();
  return h(ThemeProvider, { tokens: t },
    h(MenuBars, {
      t, height: 28, displays: all, every: props.every, mirror: false, focused: 'files', focusRect: props.focusRect,
      control: null, clock: props.clock ? props.clock.area.id : null, unread: false,
      onClock: (d, at) => { props.clock = { area: d, at }; rerender(); }, onControlCenter: () => {}, onLaunchpad: () => {},
    }),
    props.clock ? h(NotificationCenter, { t, top: 28, area: props.clock.area, at: props.clock.at, onClose: () => {} }) : null);
}
rt.render(h(Scene));
await flush();

const colorOf = (id) => (styles.get(id) || {}).color;
const lit = () => textNodes('LordOS').map((id) => colorOf(id) === colorOf(textNodes('⌕')[0]));
check('a bar on each of the three displays', textNodes('⌕').length, 3);
check('the app\'s name on each bar', textNodes('LordOS').length, 3);
check('lit only on the focused window\'s display (the right one)', lit(), [false, false, true]);
check('the dimmed color is a different one', colorOf(textNodes('LordOS')[0]) !== colorOf(textNodes('LordOS')[2]), true);
const timers3 = live.size;

// a click on the left bar's dimmed app name lights that bar (and opens the menu there)
await click(textNodes('LordOS')[0]);
check('a click on a dimmed bar lights it', lit(), [true, false, false]);
// the focused window moves to the main display: the pin is stale, the main bar is lit
props.focusRect = { x: 2000, y: 40, w: 900, h: 700 };
rerender(); await flush();
check('the focus moves to the main display', lit(), [false, true, false]);

// the setting off: the main display's bar alone, and no more timers than with three bars
props.every = false;
rerender(); await flush();
check('setting off: one bar', textNodes('⌕').length, 1);
check('one clock timer for three bars', timers3, live.size);
props.every = true;
rerender(); await flush();
check('setting on again: three bars', textNodes('⌕').length, 3);
check('and still the same timers', live.size, timers3);

// unplug the right monitor: its bar goes; plug it back: it comes back
plug([L, M]);
rerender(); await flush();
check('unplugged: two bars', textNodes('⌕').length, 2);
plug([L, M, R]);
rerender(); await flush();
check('plugged back: three bars', textNodes('⌕').length, 3);

// the clock's panel opens on the display whose clock was clicked, under the clock: its right edge on the clock's
const before = new Set(styles.keys());
props.clock = { area: R, at: [6200, 80] };
rerender(); await flush();
const panel = [...styles].find(([id, st]) => !before.has(id) && st.width === 340 && st.left != null);
check('clock panel: on the right display, under the clock', panel ? panel[1].left : null, 6200 + 80 - 340);
check('clock panel: below that display\'s bar', panel ? panel[1].top : null, 0 + 28 + 6);
process.exit(0);
