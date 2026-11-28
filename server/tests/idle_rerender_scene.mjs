// idle_rerender_scene.mjs — builds the test bundles of tests/idle_rerender_headless.sh (not run by itself): each desktop
// as tools/build-theme.mjs builds it, with a probe in the React host (runtime/index.js commitUpdate tells which component
// each changed node belongs to) and a driver that opens five apps with desktop widgets on, counts the nodes React
// changes in a quiet phase, then spins one window's title as a terminal's does (Claude Code's "◐ ◓ ◑ ◒") for as long
// and counts again, then switches to the next desktop through the SDK's themes.use. It logs to shell.log
// "PROBE idle step=<n> theme=<id> idle=<n> spun=<n> titles=<n> windows=<n>" and what drew again while the title spun,
// most first ("PROBE comp <n> <chain>"). Test only: the bundles go to the folder given and are never shipped. BUG-9 of the
// 0.8.9.6.2 QA: one title changing drew ~4300 nodes again (the whole desktop: every window, widget, bar and Dock icon).
//   node server/tests/idle_rerender_scene.mjs OUT_DIR
import { fileURLToPath } from 'node:url';
import { build } from 'esbuild';
import fs from 'node:fs';
import path from 'node:path';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
if (!process.argv[2]) { console.error('usage: idle_rerender_scene.mjs OUT_DIR'); process.exit(2); }
const outDir = path.resolve(process.argv[2]);
const ORDER = (process.env.ORDER || 'default,classic,celeste,luna,workspaces,default').split(',');
const appsPlugin = {
  name: 'lordos-apps',
  setup(b) {
    b.onResolve({ filter: /^@lordos\/apps$/ }, () => ({ path: 'apps', namespace: 'lordos-apps' }));
    b.onLoad({ filter: /.*/, namespace: 'lordos-apps' }, () => {
      const dir = path.join(root, 'apps');
      const ids = fs.readdirSync(dir).filter((d) => fs.existsSync(path.join(dir, d, 'manifest.json'))).sort();
      const lines = ids.map((id, i) => `import * as A${i} from ${JSON.stringify(path.join(dir, id, 'index.jsx'))};\n` +
        `import M${i} from ${JSON.stringify(path.join(dir, id, 'manifest.json'))};`);
      const iso = ids.map((id) => JSON.parse(fs.readFileSync(path.join(dir, id, 'manifest.json'), 'utf8')).process === 'isolated');
      const list = ids.map((_, i) => (iso[i]
        ? `{ ...M${i}, system: true, isolated: true, Component: isolatedApp(M${i}.id, A${i}.default), Icon: A${i}.Icon }`
        : `{ ...M${i}, system: true, Component: A${i}.default, Icon: A${i}.Icon }`)).join(',\n  ');
      const head = iso.some(Boolean) ? `import { isolatedApp } from ${JSON.stringify(path.join(root, 'sdk', 'packages.js'))};\n` : '';
      return { contents: `${head}${lines.join('\n')}\nexport const apps = [\n  ${list}\n];\n`, resolveDir: root, loader: 'js' };
    });
  },
};
const probePlugin = {
  name: 'probe',
  setup(b) {
    b.onLoad({ filter: /runtime[\/\\]index\.js$/ }, (a) => {
      let s = fs.readFileSync(a.path, 'utf8');
      s = s.replace('commitUpdate(inst, _type, prev, next) {', 'commitUpdate(inst, _type, prev, next, fiber) { globalThis.__probeHit && globalThis.__probeHit(fiber, prev, next);');
      return { contents: s, loader: 'js', resolveDir: path.dirname(a.path) };
    });
  },
};
const driver = `
import { themes } from ${JSON.stringify(path.join(root, 'sdk', 'index.js'))};
import { launchApp } from ${JSON.stringify(path.join(root, 'sdk', 'apps.js'))};
import { wm } from ${JSON.stringify(path.join(root, 'sdk', 'windows.js'))};
const ORDER = ${JSON.stringify(ORDER)};
const APPS = ${JSON.stringify((process.env.APPS || 'files,help,about,settings,notes').split(',').filter(Boolean))};
const SPIN = ${JSON.stringify(process.env.TITLE || 'about')};
const PHASE = ${+(process.env.PHASE || 6000)};
const file = () => lord.fs.home() + '/.scene-step';
let step = 0;
try { step = parseInt(lord.fs.read(file()), 10) || 0; } catch (e) { step = 0; }
const tally = new Map();
let on = false, spinning = false, titles = 0, spin = 0;
const chainOf = (f) => { const out = []; for (let n = f && f.return; n && out.length < 8; n = n.return) { const t = n.type; if (typeof t === 'function') out.push(t.displayName || t.name || '?'); } return out.join('<'); };
globalThis.__probeHit = (fiber) => { if (on && spinning) { const k = chainOf(fiber); tally.set(k, (tally.get(k) || 0) + 1); } };
if (step === 0) setTimeout(() => { for (const a of APPS) launchApp(a); }, 1500);
setInterval(() => { if (!spinning) return; titles++; try { wm.setTitle(SPIN, '◐◓◑◒'[spin++ % 4] + ' claude'); } catch (e) { console.log('settitle ' + e); } }, 250);
const ops = () => globalThis.__lordReactOps();
setTimeout(() => {
  on = true;
  const t0 = ops();
  setTimeout(() => {
    const idle = ops() - t0;
    spinning = true;
    const t1 = ops();
    setTimeout(() => {
      spinning = false; on = false;
      const spun = ops() - t1;
      const wins = wm.get ? APPS.filter((a) => wm.get(a)).length : -1;
      console.log('PROBE idle step=' + step + ' theme=' + themes.current() + ' idle=' + idle + ' spun=' + spun + ' titles=' + titles + ' windows=' + wins + ' phase=' + PHASE);
      for (const [k, v] of [...tally.entries()].sort((x, y) => y[1] - x[1]).slice(0, 8)) console.log('PROBE comp ' + v + ' ' + k);
      if (step + 1 >= ORDER.length) { console.log('PROBE done'); return; }
      lord.fs.write(file(), String(step + 1));
      console.log('PROBE switch ' + themes.use(ORDER[step + 1]));
    }, PHASE);
  }, PHASE);
}, ${+(process.env.SETTLE || 8000)});
`;
fs.mkdirSync(outDir, { recursive: true });
for (const id of new Set(ORDER)) {
  const dir = path.join(outDir, id);
  fs.mkdirSync(dir, { recursive: true });
  await build({
    stdin: { contents: `import ${JSON.stringify(path.join(root, 'themes', id, 'index.jsx'))};\n${driver}`, resolveDir: root, loader: 'js' },
    outfile: path.join(dir, 'theme.bundle.js'), bundle: true, format: 'iife', target: 'es2020', minify: false, keepNames: true, jsx: 'automatic',
    alias: { '@lordos/runtime': path.join(root, 'runtime', 'index.js'), '@lordos/sdk': path.join(root, 'sdk', 'index.js') },
    plugins: [appsPlugin, probePlugin], define: { 'process.env.NODE_ENV': '"production"' }, logLevel: 'error',
  });
  fs.copyFileSync(path.join(root, 'themes', id, 'theme.json'), path.join(dir, 'theme.json'));
}
console.log(`idle scene bundles: ${[...new Set(ORDER)].join(', ')} -> ${outDir}`);
