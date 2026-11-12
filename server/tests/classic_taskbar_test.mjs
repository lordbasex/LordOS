// classic_taskbar_test.mjs — (0.8.8.6) Lord Classic's taskbar on every display. Bundles
// tests/classic_taskbar_scene.mjs with esbuild (as the theme is built) together with the whole theme
// (themes/classic/index.jsx, its startShell caught by a shim so the scene renders the shell itself) over a stub
// `lord` whose screen has three monitors, runs it, and reads its checks. Needs node and the repository's
// node_modules (esbuild, react, react-reconciler). Also (#111) a bar with more pins than it holds.
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

// The native namespaces the SDK, the runtime and the theme use: absent in the stub, so the SDK takes its «not
// available» path for each, as in a session without that service.
const NS = new Set();
for (const d of ['sdk', 'runtime', 'themes/classic', 'themes/classic/components']) {
  for (const f of readdirSync(path.join(root, d))) {
    if (!/\.(m?js|jsx)$/.test(f)) continue;
    for (const m of readFileSync(path.join(root, d, f), 'utf8').matchAll(/lord\.([A-Za-z_]+)\.[A-Za-z_]+\(/g)) NS.add(m[1]);
  }
}

// A stub lord: nodes, their styles and texts (and which ones left the tree); lord.getLayout answers from a map the
// scene fills (the bars' rectangles), far off screen otherwise; settings saved in memory; the network polls counted.
const MOCK = `globalThis.__lordTest = (() => {
  const styles = new Map(), texts = new Map(), parents = new Map(), removed = new Set(), layouts = new Map();
  const counts = { netPolls: 0 };
  let nextId = 1, saved = '{}';
  const base = {
    screen: { width: 1920, height: 1080, displays: [], mirror: false },
    version: 'test', now: () => Date.now(), stats: () => ({}),
    getLayout: (id) => layouts.get(id) || [-10000, -10000, 1, 1],
    createNode: () => { const id = nextId++; styles.set(id, {}); return id; },
    setStyle: (id, k, v) => { const s = styles.get(id); if (!s) return; if (v === undefined) delete s[k]; else s[k] = v; },
    setText: (id, s) => { texts.set(id, s); },
    appendChild: (p, c) => { parents.set(c, p); removed.delete(c); },
    insertBefore: (p, c) => { parents.set(c, p); removed.delete(c); },
    removeChild: (p, c) => { removed.add(c); },
    locale: { languages: () => ['en'], get: () => 'en', catalog: () => null },
    destroy: (id) => { styles.delete(id); texts.delete(id); parents.delete(id); removed.add(id); },
    settings: { load: () => saved, save: (s) => { saved = s; return true; } },
    system: { info: () => ({ user: 'test', memTotalKB: 8000000, memAvailableKB: 4000000, cores: 4 }), cpuTimes: () => [100, 50] },
    net: { interfaces: () => { counts.netPolls++; return [
      { name: 'lo', type: 'loopback', up: true, addresses: [] },
      { name: 'eth0', type: 'ether', up: true, addresses: [{ family: 4, scope: 'global', address: '192.168.1.5' }] },
    ]; } },
  };
  for (const ns of ${JSON.stringify([...NS])}) if (!(ns in base)) base[ns] = undefined;
  const noop = () => undefined;
  globalThis.lord = new Proxy(base, { get: (o, k) => (k in o ? o[k] : typeof k === 'string' ? noop : undefined) });
  return { styles, texts, parents, removed, layouts, counts };
})();`;

const dir = mkdtempSync(path.join(tmpdir(), 'lord-classic-taskbar-'));
let bad = 0, n = 0;
try {
  // the theme's startShell hands its element to the scene; @lordos/apps has the taskbar's default pins and Calculator
  const shim = path.join(dir, 'sdk-shim.js');
  writeFileSync(shim, `export * from ${JSON.stringify(path.join(root, 'sdk', 'index.js'))};\n`
    + 'export function startShell(element) { globalThis.__shellElement = element; }\n');
  const appsStub = path.join(dir, 'apps-stub.js');
  writeFileSync(appsStub, 'const Icon = () => null, Component = () => null;\nexport const apps = '
    + "['files', 'terminal', 'store', 'settings', 'calculator', ...Array.from({ length: 60 }, (_, i) => `extra${i}`)]"
    + ".map((id) => ({ id, name: id, version: '1.0.0', system: true, Icon, Component }));\n");
  const scene = 'classic_taskbar_scene.mjs';
  const out = await esbuild.build({
    entryPoints: [path.join(here, scene)], bundle: true, write: false, format: 'esm',
    platform: 'node', target: 'es2022', nodePaths, logLevel: 'error', jsx: 'automatic',
    alias: { '@lordos/runtime': path.join(root, 'runtime', 'index.js'), '@lordos/sdk': shim, '@lordos/apps': appsStub },
    define: { 'process.env.NODE_ENV': '"production"' },
    banner: { js: MOCK },
  });
  const file = path.join(dir, scene);
  writeFileSync(file, out.outputFiles[0].contents);
  const r = spawnSync(process.execPath, [file], { encoding: 'utf8', timeout: 60000 });
  let m = 0;
  for (const line of r.stdout.split('\n').filter(Boolean)) {
    n++; m++;
    if (!line.startsWith('ok ')) { bad++; console.log(`✗ ${line}`); }
  }
  // a React error in the shell fails the test even when every check passed
  if (/react \(/.test(r.stderr)) { bad++; console.log(`✗ React reported an error: ${r.stderr.slice(0, 2000)}`); }
  if (r.status !== 0 || !m) { bad++; console.log(`✗ ${scene} failed (${r.status}): ${r.stderr.slice(0, 2000)}`); }
  console.log(`classic-taskbar: ${n} checks, ${bad} failures`);
} finally {
  rmSync(dir, { recursive: true, force: true });
}
process.exit(bad ? 1 : 0);
