// classic_startmenu_power_test.mjs — (0.8.8.6, #48) Lord Classic's power menu: Lock Screen, Switch User,
// Log Out, Restart… and Shut Down… did nothing. The menu is a portal into the popover layer, so its rows
// were not inside the start menu's node: the start menu's «press outside» listener closed it on the
// press, the menu went with it, and the release found no row to click. Bundles
// tests/classic_startmenu_power_scene.mjs with esbuild (as the theme is built) over a stub `lord` that
// records lord.session calls, runs it, and reads its checks. Needs node and the repository's
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

// The native namespaces the SDK, the runtime and the theme use (lord.<ns>.<fn>(…)): absent in the stub, so
// the SDK takes its «not available» path for each, as in a session without that service.
const NS = new Set();
for (const d of ['sdk', 'runtime', 'themes/classic', 'themes/classic/components']) {
  for (const f of readdirSync(path.join(root, d))) {
    if (!/\.(m?js|jsx)$/.test(f)) continue;
    for (const m of readFileSync(path.join(root, d, f), 'utf8').matchAll(/lord\.([A-Za-z_]+)\.[A-Za-z_]+\(/g)) NS.add(m[1]);
  }
}

// A stub lord: nodes, their styles and texts; a managed session whose lock and switchUser are recorded;
// any other native call is a no-op.
const MOCK = `globalThis.__lordTest = (() => {
  const styles = new Map(), texts = new Map(), parents = new Map(), calls = [];
  let nextId = 1;
  const base = {
    screen: { width: 1920, height: 1080, displays: [], mirror: false },
    version: 'test', now: () => Date.now(), stats: () => ({}),
    getLayout: () => [0, 0, 100, 100],
    createNode: () => { const id = nextId++; styles.set(id, {}); return id; },
    setStyle: (id, k, v) => { const s = styles.get(id); if (!s) return; if (v === undefined) delete s[k]; else s[k] = v; },
    setText: (id, s) => { texts.set(id, s); },
    appendChild: (p, c) => { parents.set(c, p); },
    insertBefore: (p, c) => { parents.set(c, p); },
    locale: { languages: () => ['en'], get: () => 'en', catalog: () => null },
    destroy: (id) => { styles.delete(id); texts.delete(id); parents.delete(id); },
    settings: { load: () => '{}' },
    system: { info: () => ({ user: 'test' }) },
    session: { managed: () => true, lock: () => { calls.push('lock'); }, switchUser: () => { calls.push('switchUser'); } },
  };
  for (const ns of ${JSON.stringify([...NS])}) if (!(ns in base)) base[ns] = undefined;
  const noop = () => undefined;
  globalThis.lord = new Proxy(base, { get: (o, k) => (k in o ? o[k] : typeof k === 'string' ? noop : undefined) });
  return { styles, texts, parents, calls };
})();`;

const dir = mkdtempSync(path.join(tmpdir(), 'lord-classic-power-'));
let bad = 0, n = 0;
try {
  const out = await esbuild.build({
    entryPoints: [path.join(here, 'classic_startmenu_power_scene.mjs')], bundle: true, write: false, format: 'esm',
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
  console.log(`classic-startmenu-power: ${n} checks, ${bad} failures`);
} finally {
  rmSync(dir, { recursive: true, force: true });
}
process.exit(bad ? 1 : 0);
