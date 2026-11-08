// capture_layer_scene.mjs — the scene of tests/capture_layer_test.mjs (not run by itself): the SDK's
// FloatingWindows rendered through runtime/index.js with a stub `lord` that keeps the node tree, the shell
// layers on, a sheet open in the modal layer, Screenshots' selection (a capture window) and a plain
// fullscreen window. Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { FloatingWindows } from '../../sdk/components.jsx';
import { enableShellLayers, Layer, LAYERS } from '../../sdk/layers.js';
import { wm, windows, registerBuiltinApp } from '../../sdk/windows.js';

const { styles, parents } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

// the zIndex of the layer a node is drawn in (0: the base tree)
const layerOf = (id) => {
  for (let n = id; n; n = parents.get(n)) if ((styles.get(n) || {}).zIndex > 0) return styles.get(n).zIndex;
  return 0;
};

const ids = {};
const pressed = [];
const Shot = () => h(rt.View, { ref: (r) => { if (r) ids.shot = r.id; }, style: { flex: 1 }, onMouseDown: () => pressed.push('shot') });
const Film = () => h(rt.View, { ref: (r) => { if (r) ids.film = r.id; }, style: { flex: 1 } });
registerBuiltinApp({ id: 'shot', Component: Shot, builtin: true });
registerBuiltinApp({ id: 'film', Component: Film, builtin: true });

const Sheet = () => h(Layer, { name: 'modal' },
  h(rt.View, { ref: (r) => { if (r) ids.sheet = r.id; }, style: { position: 'absolute', left: 0, top: 0, width: 1920, height: 1080 } }));

enableShellLayers();
rt.render(h(React.Fragment, null, h(Sheet), h(Layer, { name: 'floating' }, h(FloatingWindows))));   // as startShell places it
const film = wm.open('film', { frame: { x: 0, y: 0, w: 800, h: 600 } });
wm.setFullscreen(film, true);
const shot = wm.open('shot', { instance: 'overlay0', frame: { x: 0, y: 0, w: 1920, h: 1080 } });
wm.setFullscreen(shot, true, { capture: true });
await flush();

const top = Math.max(...Object.values(LAYERS));
check('capture is the topmost layer', LAYERS.capture === top, `capture ${LAYERS.capture}, top ${top}`);
check('the sheet is in the modal layer', layerOf(ids.sheet) === LAYERS.modal, `got ${layerOf(ids.sheet)}`);
check('a plain fullscreen window stays in floating', layerOf(ids.film) === LAYERS.floating, `got ${layerOf(ids.film)}`);
check('the selection is in the capture layer', layerOf(ids.shot) === LAYERS.capture, `got ${layerOf(ids.shot)}`);
check('the selection is above the sheet', layerOf(ids.shot) > layerOf(ids.sheet));

// a press on the selection reaches it, and no global 'pointerdown' listener (a menu or sheet that closes on
// a click outside, a text field that loses focus) hears it
let outside = 0;
const off = rt.addListener('pointerdown', () => { outside++; });
globalThis.__lordEvent('down', ids.shot, 100, 100, 0);
globalThis.__lordEvent('up', ids.shot, 100, 100, 0);
check('a press on the selection reaches it', pressed.length === 1, `got ${pressed.length}`);
check('a press on the selection is not a click outside', outside === 0, `heard ${outside}`);
globalThis.__lordEvent('down', ids.film, 100, 100, 0);
globalThis.__lordEvent('up', ids.film, 100, 100, 0);
check('a press on a plain fullscreen window still is', outside === 1, `heard ${outside}`);
off();

// Esc is the selection's own (its shortcut ends it): the SDK's «Esc leaves fullscreen» passes it by and
// takes the plain fullscreen window out instead
globalThis.__lordKey(1, 1, 'Escape', '', '', 0);
globalThis.__lordKey(1, 0, 'Escape', '', '', 0);
await flush();
const w = (id) => windows.get().find((x) => x.id === id);
check('Esc leaves the selection fullscreen', !!w(shot).fullscreen && !!w(shot).capture);
check('Esc takes the plain window out of fullscreen', !w(film).fullscreen);

wm.setFullscreen(shot, false);
check('leaving fullscreen drops the capture mark', !w(shot).capture);
