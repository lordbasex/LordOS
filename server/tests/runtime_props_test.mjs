// runtime_props_test.mjs — (0.8.6) applyProps in runtime/index.js skips normalize and applyStyle when a
// node's flattened style is the same as last time (same keys, same order, same primitive values). This
// checks that the skip changes nothing the server sees: tests/runtime_props_scene.mjs is bundled twice
// with esbuild, as the theme is built, once as it is and once with the skip turned off (the runtime
// before the change), and both must send exactly the same lord.* calls, in the same order, over fuzzed
// trees, and end with the same styles. Also checks that the skip really ran. Needs node and the
// repository's node_modules (esbuild, react, react-reconciler); run by tests/run_suites.sh.
import { createRequire } from 'node:module';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, rmSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
const nodePaths = [path.join(root, 'node_modules'), ...(process.env.NODE_PATH || '').split(':').filter(Boolean)];
let esbuild = null;
for (const p of nodePaths) { try { esbuild = createRequire(path.join(p, 'x.js'))('esbuild'); break; } catch (e) { /* next */ } }
if (!esbuild) { console.log('needs node_modules (esbuild)'); process.exit(1); }

// The recording lord: every native call as a line, and each node's style as the server would hold it.
const MOCK = `globalThis.__lordTest = (() => {
  const log = [], state = new Map();
  const fmt = (v) => (typeof v === 'number' ? (Object.is(v, -0) ? '-0' : String(v)) : v === undefined ? 'undef'
    : JSON.stringify(v, (k, x) => (typeof x === 'number' && (x !== x || Object.is(x, -0)) ? String(x) + 'n' : x)));
  let nextId = 1;
  globalThis.lord = {
    screen: { width: 1920, height: 1080, displays: [], mirror: false },
    version: 'test', now: () => 0, stats: () => ({}), getLayout: () => [0, 0, 0, 0],
    createNode: (t) => { const id = nextId++; log.push('create ' + t + ' ' + id); state.set(id, {}); return id; },
    setStyle: (id, k, v) => { log.push('style ' + id + ' ' + k + ' ' + fmt(v)); const s = state.get(id); if (v === undefined) delete s[k]; else s[k] = v; },
    setProp: (id, k, v) => log.push('prop ' + id + ' ' + k + ' ' + fmt(v)),
    setText: (id, t) => log.push('text ' + id + ' ' + JSON.stringify(t)),
    appendChild: (p, c) => log.push('append ' + p + ' ' + c),
    insertBefore: (p, c, b) => log.push('insert ' + p + ' ' + c + ' ' + b),
    removeChild: (p, c) => log.push('remove ' + p + ' ' + c),
    destroy: (id) => log.push('destroy ' + id),
    setRoot: (id) => log.push('root ' + id),
  };
  globalThis.__skips = 0;
  process.on('exit', () => process.stderr.write('skips ' + globalThis.__skips + '\\n'));
  return { log, fmt, state };
})();`;

const runtime = path.join(root, 'runtime', 'index.js');
const SKIP_TEST = 'if (!sameFlat(flats.get(inst), flat)) {';
// skip-off: the skip's test always fails, so every render normalizes and diffs as before the change.
// Both variants count the applyProps calls the skip spares (or would spare).
function variant(off) {
  return {
    name: off ? 'skip-off' : 'skip-on',
    setup(b) {
      b.onLoad({ filter: /runtime[\\/]index\.js$/ }, () => {
        let src = readFileSync(runtime, 'utf8');
        if (!src.includes(SKIP_TEST)) throw new Error('runtime/index.js: the skip test is not where this test expects it');
        src = src.replace(SKIP_TEST, `if (sameFlat(flats.get(inst), flat)) globalThis.__skips++;\n  if (${off ? 'true' : '!sameFlat(flats.get(inst), flat)'}) {`);
        return { contents: src, loader: 'js', resolveDir: path.dirname(runtime) };
      });
    },
  };
}

const dir = mkdtempSync(path.join(tmpdir(), 'lord-runtime-props-'));
let bad = 0;
try {
  const files = {};
  for (const off of [false, true]) {
    const out = await esbuild.build({
      entryPoints: [path.join(here, 'runtime_props_scene.mjs')], bundle: true, write: false, format: 'esm',
      platform: 'node', target: 'es2022', nodePaths, logLevel: 'error',
      alias: { '@lordos/runtime': runtime }, define: { 'process.env.NODE_ENV': '"production"' },
      banner: { js: MOCK }, plugins: [variant(off)],
    });
    files[off ? 'off' : 'on'] = path.join(dir, off ? 'off.mjs' : 'on.mjs');
    writeFileSync(files[off ? 'off' : 'on'], out.outputFiles[0].contents);
  }
  const run = (f, seed) => {
    const r = spawnSync(process.execPath, [f, String(seed), '300'], { encoding: 'utf8', maxBuffer: 64 << 20 });
    if (r.status !== 0) throw new Error(`${path.basename(f)} seed ${seed}: ${r.stderr}`);
    const m = /skips (\d+)/.exec(r.stderr);
    return { out: r.stdout, skips: m ? +m[1] : 0 };
  };
  let skips = 0, lines = 0;
  for (let seed = 1; seed <= 8; seed++) {
    const on = run(files.on, seed), off = run(files.off, seed);
    skips += on.skips;
    lines += on.out.split('\n').length - 1;
    if (on.out !== off.out) {
      bad++;
      const a = on.out.split('\n'), b = off.out.split('\n');
      let i = 0;
      while (i < a.length && a[i] === b[i]) i++;
      console.log(`✗ seed ${seed}: the calls differ at line ${i + 1}\n    with the skip    ${a[i]}\n    without the skip ${b[i]}`);
    }
  }
  if (!skips) { bad++; console.log('✗ the skip never ran: the test no longer exercises it'); }
  console.log(`runtime-props: 8 seeds × 300 renders, ${lines} lines of native calls and final styles compared, ${skips} applyProps calls skipped, ${bad} failures`);
} finally {
  rmSync(dir, { recursive: true, force: true });
}
process.exit(bad ? 1 : 0);
