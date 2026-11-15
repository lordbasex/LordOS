// side_buttons_test.mjs — (issue #159) a mouse's back and forward buttons. lord-server now sends them as buttons 3
// and 4 (src/inputkind.h, input_dom_button) so a Linux app under the pointer gets BTN_SIDE and BTN_EXTRA; the
// desktop's own handlers must not see them: most onMouseDown handlers (title bars, buttons, sliders) don't look at
// ev.button, and a thumb press would start a window drag or press a button. Runs runtime/events.js over a stub `lord`
// with a two-node chain and records what each press reaches. Needs the repository's node_modules (esbuild, react-reconciler).
import { createRequire } from 'node:module';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
const nodePaths = [path.join(root, 'node_modules'), ...(process.env.NODE_PATH || '').split(':').filter(Boolean)];
let esbuild = null;
for (const p of nodePaths) { try { esbuild = createRequire(path.join(p, 'x.js'))('esbuild'); break; } catch (e) { /* next */ } }
if (!esbuild) { console.log('side-buttons: skipped: needs node_modules (esbuild)'); process.exit(0); }
globalThis.lord = new Proxy({ now: () => Date.now(), getLayout: () => [0, 0, 10, 10] }, { get: (o, k) => (k in o ? o[k] : () => undefined) });
// events.js imports react-reconciler/constants, which only a bundler resolves (as tools/build-theme.mjs does)
const out = await esbuild.build({
  entryPoints: [path.join(root, 'runtime', 'events.js')], bundle: true, write: false, format: 'esm', platform: 'node',
  target: 'es2022', nodePaths, logLevel: 'error', define: { 'process.env.NODE_ENV': '"production"' },
});
const dir = mkdtempSync(path.join(tmpdir(), 'lord-side-buttons-'));
let ev;
try {
  const file = path.join(dir, 'events.mjs');
  writeFileSync(file, out.outputFiles[0].contents);
  ev = await import(pathToFileURL(file).href);
} finally {
  rmSync(dir, { recursive: true, force: true });
}

let fails = 0;
const check = (name, got, want) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { fails++; console.log(`FAIL ${name}: got ${g}, want ${w}`); }
};
const seen = [];
const rec = (name) => (e) => seen.push(`${name}:${e.button}`);
const parent = { id: 1, parent: null, props: { onMouseDown: rec('parentDown'), onMouseDownCapture: rec('capture') } };
const child = { id: 2, parent, props: { onMouseDown: rec('down'), onMouseUp: rec('up'), onClick: rec('click'), onContextMenu: rec('menu') } };
ev.register(parent);
ev.register(child);
const globalDown = [];
ev.addListener('pointerdown', (e) => globalDown.push(e.button));
const press = (b) => {
  seen.length = 0;
  globalDown.length = 0;
  globalThis.__lordEvent('down', 2, 5, 5, b);
  globalThis.__lordEvent('up', 2, 5, 5, b);
  return [...seen];
};

check('left click', press(0), ['capture:0', 'down:0', 'parentDown:0', 'up:0', 'click:0']);
check('left reaches the global listeners', globalDown, [0]);
check('right click opens a menu', press(2), ['capture:2', 'down:2', 'parentDown:2', 'up:2', 'menu:2']);
check('back reaches no handler', press(3), []);
check('back reaches no global listener', globalDown, []);
check('forward reaches no handler', press(4), []);
// a back press in the middle of a left click doesn't end it
seen.length = 0;
globalThis.__lordEvent('down', 2, 5, 5, 0);
globalThis.__lordEvent('down', 2, 5, 5, 3);
globalThis.__lordEvent('up', 2, 5, 5, 3);
globalThis.__lordEvent('up', 2, 5, 5, 0);
check('left click around a back press', seen, ['capture:0', 'down:0', 'parentDown:0', 'up:0', 'click:0']);

console.log(fails ? `side-buttons: ${fails} failures` : 'side-buttons: ok');
process.exit(fails ? 1 : 0);
