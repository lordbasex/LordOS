// capture_layer_test.mjs — (0.8.8.6) Screenshots' area selection (❖⇧4) over an open sheet: the selection
// window goes to the capture layer, above the modal layer and every other one, a press on it is not a
// click outside for what is open under it, and Esc stays the selection's. Bundles
// tests/capture_layer_scene.mjs with esbuild (as the theme is built) over a stub `lord` that keeps the
// node tree, runs it, and reads its checks. Needs node and the repository's node_modules (esbuild, react,
// react-reconciler).
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

// A stub lord: nodes, their styles and their parents; any other native call is a no-op.
const MOCK = `globalThis.__lordTest = (() => {
  const styles = new Map(), parents = new Map();
  let nextId = 1;
  const base = {
    screen: { width: 1920, height: 1080, displays: [], mirror: false },
    version: 'test', now: () => Date.now(), stats: () => ({}),
    getLayout: () => [0, 0, 1920, 1080],
    createNode: () => { const id = nextId++; styles.set(id, {}); return id; },
    setStyle: (id, k, v) => { const s = styles.get(id); if (!s) return; if (v === undefined) delete s[k]; else s[k] = v; },
    appendChild: (p, c) => { parents.set(c, p); },
    insertBefore: (p, c) => { parents.set(c, p); },
    removeChild: (p, c) => { if (parents.get(c) === p) parents.delete(c); },
    locale: { languages: () => ['en'], get: () => 'en' },
    settings: { load: () => '{}' },
  };
  const noop = () => undefined;
  globalThis.lord = new Proxy(base, { get: (o, k) => (k in o ? o[k] : typeof k === 'string' ? noop : undefined) });
  return { styles, parents };
})();`;

const dir = mkdtempSync(path.join(tmpdir(), 'lord-capture-layer-'));
let bad = 0, n = 0;
try {
  const out = await esbuild.build({
    entryPoints: [path.join(here, 'capture_layer_scene.mjs')], bundle: true, write: false, format: 'esm',
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
  console.log(`capture-layer: ${n} checks, ${bad} failures`);
} finally {
  rmSync(dir, { recursive: true, force: true });
}
process.exit(bad ? 1 : 0);
