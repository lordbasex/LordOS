// focus_scene_test.mjs — (0.8.8.8) the keyboard focus between the SDK's controls (sdk/focus.js): Tab order, the ring,
// Space and Enter, the arrows of a Slider, a Stepper, a Segmented and a MenuButton, a TextInput in the order, a press
// that ends the focus, a listener that keeps Tab and a confirmation dialog that keeps its keys. Bundles
// tests/focus_scene.mjs with esbuild (as the theme is built) over a stub `lord` that keeps the node tree, its styles and
// texts, runs it, and reads its checks. Needs node and the repository's node_modules (esbuild, react, react-reconciler).
import { createRequire } from 'node:module';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
const nodePaths = [path.join(root, 'node_modules'), ...(process.env.NODE_PATH || '').split(':').filter(Boolean)];
let esbuild = null;
for (const p of nodePaths) { try { esbuild = createRequire(path.join(p, 'x.js'))('esbuild'); break; } catch (e) { /* next */ } }
if (!esbuild) { console.log('needs node_modules (esbuild)'); process.exit(1); }

// A stub lord: nodes, their parents, styles and texts (and which were taken out of the tree), every character 10 px
// wide; the node that is translated (a scroll view's content) is 1000 px high, every other one 100 px. Any other
// native call is a no-op.
const MOCK = `globalThis.__lordTest = (() => {
  const styles = new Map(), parents = new Map(), texts = new Map(), removed = new Set();
  let nextId = 1;
  const noop = () => undefined;
  const base = {
    screen: { width: 1920, height: 1080, displays: [], mirror: false },
    version: 'test', now: () => Date.now(), stats: () => ({}),
    getLayout: (id) => [0, 0, 300, 'translateY' in (styles.get(id) || {}) ? 1000 : 100],
    measureText: (s) => [...String(s)].length * 10,
    createNode: () => { const id = nextId++; styles.set(id, {}); return id; },
    setStyle: (id, k, v) => { const s = styles.get(id); if (!s) return; if (v === undefined) delete s[k]; else s[k] = v; },
    setText: (id, s) => { texts.set(id, s); },
    appendChild: (p, c) => { parents.set(c, p); removed.delete(c); },
    insertBefore: (p, c) => { parents.set(c, p); removed.delete(c); },
    removeChild: (p, c) => { if (parents.get(c) === p) { parents.delete(c); removed.add(c); } },
    destroy: (id) => { removed.add(id); },
    clipboard: { get: () => '', set: noop },
    ime: new Proxy({}, { get: () => noop }),
    text: { direction: () => 'ltr' },
    locale: { languages: () => ['en'], get: () => 'en', catalog: () => null },
    settings: { load: () => '{}' },
  };
  globalThis.lord = new Proxy(base, { get: (o, k) => (k in o ? o[k] : typeof k === 'string' ? noop : undefined) });
  return { styles, parents, texts, removed };
})();`;

const dir = mkdtempSync(path.join(tmpdir(), 'lord-focus-scene-'));
let bad = 0, n = 0;
try {
  const out = await esbuild.build({
    entryPoints: [path.join(here, 'focus_scene.mjs')], bundle: true, write: false, format: 'esm',
    platform: 'node', target: 'es2022', nodePaths, logLevel: 'error', jsx: 'automatic',
    alias: { '@lordos/runtime': path.join(root, 'runtime', 'index.js') }, define: { 'process.env.NODE_ENV': '"production"' },
    banner: { js: MOCK },
  });
  const file = path.join(dir, 'scene.mjs');
  writeFileSync(file, out.outputFiles[0].contents);
  const r = spawnSync(process.execPath, [file], { encoding: 'utf8' });
  for (const line of r.stdout.split('\n').filter(Boolean)) {
    n++;
    if (!line.startsWith('ok ')) { bad++; console.log(`✗ ${line}`); }
  }
  if (r.status !== 0 || !n) { bad++; console.log(`✗ the scene failed (${r.status}): ${r.stderr.slice(0, 2000)}`); }
  console.log(`focus-scene: ${n} checks, ${bad} failures`);
} finally {
  rmSync(dir, { recursive: true, force: true });
}
process.exit(bad ? 1 : 0);
