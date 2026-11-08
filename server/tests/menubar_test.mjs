// menubar_test.mjs — (0.8.8.6, #58) what the bar and Lord Flow's panels show, and (0.8.8.6) a bar on every display: the Control Center's tiles that
// Settings → Desktop & Dock turns off, the output list by the volume (a click makes that output the default one)
// and the clock panel's calendar. Bundles tests/menubar_scene.mjs with esbuild (as the theme is built) over a stub
// `lord` with two sound outputs, a Wi-Fi radio and a Bluetooth adapter that records lord.sound calls, runs it, and
// reads its checks. Needs node and the repository's node_modules (esbuild, react, react-reconciler).
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
for (const d of ['sdk', 'runtime', 'themes/default', 'themes/default/components']) {
  for (const f of readdirSync(path.join(root, d))) {
    if (!/\.(m?js|jsx)$/.test(f)) continue;
    for (const m of readFileSync(path.join(root, d, f), 'utf8').matchAll(/lord\.([A-Za-z_]+)\.[A-Za-z_]+\(/g)) NS.add(m[1]);
  }
}

// A stub lord: nodes, their styles and texts (and which ones left the tree); two sound outputs whose
// lord.sound.setDefault calls are recorded; a Wi-Fi radio and a Bluetooth adapter; any other native call is a no-op.
const MOCK = `globalThis.__lordTest = (() => {
  const styles = new Map(), texts = new Map(), parents = new Map(), removed = new Set(), calls = [];
  let nextId = 1;
  const base = {
    screen: { width: 1920, height: 1080, displays: [], mirror: false },
    version: 'test', now: () => Date.now(), stats: () => ({}),
    getLayout: () => [0, 0, 100, 100],
    createNode: () => { const id = nextId++; styles.set(id, {}); return id; },
    setStyle: (id, k, v) => { const s = styles.get(id); if (!s) return; if (v === undefined) delete s[k]; else s[k] = v; },
    setText: (id, s) => { texts.set(id, s); },
    appendChild: (p, c) => { parents.set(c, p); removed.delete(c); },
    insertBefore: (p, c) => { parents.set(c, p); removed.delete(c); },
    removeChild: (p, c) => { removed.add(c); },
    locale: { languages: () => ['en'], get: () => 'en', catalog: () => null },
    destroy: (id) => { styles.delete(id); texts.delete(id); parents.delete(id); removed.add(id); },
    settings: { load: () => '{}', save: () => true },
    system: { info: () => ({ user: 'test', memTotalKB: 8000000, memAvailableKB: 4000000, cores: 4 }), cpuTimes: () => [100, 50] },
    sound: {
      state: () => ({ connected: true, sinks: [
        { index: 31, name: 'alsa_output.pci.analog', description: 'Built-in Audio', default: true, level: 0.4, mute: false },
        { index: 52, name: 'bluez_output.AA_BB', description: 'Headset', default: false, level: 0.8, mute: false },
      ], sources: [], inputs: [], outputs: [] }),
      setDefault: (kind, name) => { calls.push('setDefault:' + kind + ':' + name); return true; },
    },
    net: { interfaces: () => [
      { name: 'lo', type: 'loopback', up: true, addresses: [] },
      { name: 'wlan0', type: 'wifi', up: true, addresses: [{ family: 4, scope: 'global', address: '192.168.1.5' }] },
    ] },
    bt: { state: () => ({ adapter: { powered: true }, devices: [] }) },
  };
  for (const ns of ${JSON.stringify([...NS])}) if (!(ns in base)) base[ns] = undefined;
  const noop = () => undefined;
  globalThis.lord = new Proxy(base, { get: (o, k) => (k in o ? o[k] : typeof k === 'string' ? noop : undefined) });
  return { styles, texts, parents, removed, calls };
})();`;

const dir = mkdtempSync(path.join(tmpdir(), 'lord-menubar-'));
let bad = 0, n = 0;
try {
  // menubar_scene.mjs: the panels (#58); menubar_displays_scene.mjs: a bar on every display (0.8.8.6)
  for (const scene of ['menubar_scene.mjs', 'menubar_displays_scene.mjs']) {
    const out = await esbuild.build({
      entryPoints: [path.join(here, scene)], bundle: true, write: false, format: 'esm',
      platform: 'node', target: 'es2022', nodePaths, logLevel: 'error', jsx: 'automatic',
      alias: { '@lordos/runtime': path.join(root, 'runtime', 'index.js'), '@lordos/sdk': path.join(root, 'sdk', 'index.js') },
      define: { 'process.env.NODE_ENV': '"production"' },
      banner: { js: MOCK },
    });
    const file = path.join(dir, scene);
    writeFileSync(file, out.outputFiles[0].contents);
    const r = spawnSync(process.execPath, [file], { encoding: 'utf8' });
    let m = 0;
    for (const line of r.stdout.split('\n').filter(Boolean)) {
      n++; m++;
      if (!line.startsWith('ok ')) { bad++; console.log(`✗ ${scene}: ${line}`); }
    }
    if (r.status !== 0 || !m) { bad++; console.log(`✗ ${scene} failed (${r.status}): ${r.stderr.slice(0, 2000)}`); }
  }
  console.log(`menubar: ${n} checks, ${bad} failures`);
} finally {
  rmSync(dir, { recursive: true, force: true });
}
process.exit(bad ? 1 : 0);
