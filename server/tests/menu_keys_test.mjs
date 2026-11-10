// menu_keys_test.mjs — (0.8.8.8) menus from the keyboard in both themes: arrows, Home/End, type-ahead, Enter,
// submenus one level at a time, ← and → along the bar, the hover pause before a submenu, and F10 or ⌥ alone for the
// menu bar. Bundles tests/menu_keys_scene.mjs with esbuild (as the theme is built) over a stub `lord`, runs it, and
// reads its checks. The rules themselves are tested by tests/menu_nav_test.mjs. Needs node and the repository's
// node_modules (esbuild, react, react-reconciler).
import { createRequire } from 'node:module';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, rmSync, readdirSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
const nodePaths = [path.join(root, 'node_modules'), ...(process.env.NODE_PATH || '').split(':').filter(Boolean)];
let esbuild = null;
for (const p of nodePaths) { try { esbuild = createRequire(path.join(p, 'x.js'))('esbuild'); break; } catch (e) { /* next */ } }
if (!esbuild) { console.log('needs node_modules (esbuild)'); process.exit(1); }

// The native namespaces the SDK, the runtime and the themes use (lord.<ns>.<fn>(…)): absent in the stub, so the
// SDK takes its «not available» path for each, as in a session without that service.
const NS = new Set();
for (const d of ['sdk', 'runtime', 'themes/default', 'themes/default/components', 'themes/classic', 'themes/classic/components']) {
  for (const f of readdirSync(path.join(root, d))) {
    if (!/\.(m?js|jsx)$/.test(f)) continue;
    for (const m of readFileSync(path.join(root, d, f), 'utf8').matchAll(/lord\.([A-Za-z_]+)\.[A-Za-z_]+\(/g)) NS.add(m[1]);
  }
}

// A stub lord: nodes, their parents and texts (and which left the tree), every character 10 px wide; any other
// native call is a no-op.
const MOCK = `globalThis.__lordTest = (() => {
  const styles = new Map(), texts = new Map(), parents = new Map(), removed = new Set();
  let nextId = 1;
  const base = {
    screen: { width: 1920, height: 1080, displays: [], mirror: false },
    version: 'test', now: () => Date.now(), stats: () => ({}),
    getLayout: () => [0, 0, 100, 24],
    measureText: (s) => [...String(s)].length * 10,
    createNode: () => { const id = nextId++; styles.set(id, {}); return id; },
    setStyle: (id, k, v) => { const s = styles.get(id); if (!s) return; if (v === undefined) delete s[k]; else s[k] = v; },
    setText: (id, s) => { texts.set(id, s); },
    appendChild: (p, c) => { parents.set(c, p); removed.delete(c); },
    insertBefore: (p, c) => { parents.set(c, p); removed.delete(c); },
    removeChild: (p, c) => { removed.add(c); },
    destroy: (id) => { styles.delete(id); texts.delete(id); parents.delete(id); removed.add(id); },
    locale: { languages: () => ['en'], get: () => 'en', catalog: () => null },
    settings: { load: () => '{}', save: () => true },
  };
  for (const ns of ${JSON.stringify([...NS])}) if (!(ns in base)) base[ns] = undefined;
  const noop = () => undefined;
  globalThis.lord = new Proxy(base, { get: (o, k) => (k in o ? o[k] : typeof k === 'string' ? noop : undefined) });
  return { styles, texts, parents, removed };
})();`;

const dir = mkdtempSync(path.join(tmpdir(), 'lord-menu-keys-'));
let bad = 0, n = 0;
try {
  const out = await esbuild.build({
    entryPoints: [path.join(here, 'menu_keys_scene.mjs')], bundle: true, write: false, format: 'esm',
    platform: 'node', target: 'es2022', nodePaths, logLevel: 'error', jsx: 'automatic',
    alias: { '@lordos/runtime': path.join(root, 'runtime', 'index.js'), '@lordos/sdk': path.join(root, 'sdk', 'index.js') },
    define: { 'process.env.NODE_ENV': '"production"' },
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
  console.log(`menu-keys: ${n} checks, ${bad} failures`);
} finally {
  rmSync(dir, { recursive: true, force: true });
}
process.exit(bad ? 1 : 0);
