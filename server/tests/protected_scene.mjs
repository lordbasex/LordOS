// protected_scene.mjs — builds the test bundle of tests/protected_headless.sh (not run by itself): Lord Flow exactly as
// tools/build-theme.mjs builds it, plus a small driver in the same SDK module instances that, in a real headless
// session, opens Calculator protected (wm.open's `protected`, as Machines opens a machine with the anonymity profile) and
// Notes unprotected side by side, tries to turn the locked protection off, and logs to shell.log:
//   "SCENE win <app> node=<frame node> rect=<x,y,w,h> protected=<0|1>" for each, then
//   "SCENE ready count=<lord.media.protectedCount()> locked=<0|1>".
// Test only: the bundle goes to the folder given and is never shipped.
//   node server/tests/protected_scene.mjs OUT_DIR     (writes OUT_DIR/theme.bundle.js and theme.json)
import { build } from 'esbuild';
import { fileURLToPath } from 'node:url';
import fs from 'node:fs';
import path from 'node:path';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const outDir = path.resolve(process.argv[2] || '');
if (!process.argv[2]) { console.error('usage: protected_scene.mjs OUT_DIR'); process.exit(2); }

// the same virtual '@lordos/apps' module as tools/build-theme.mjs
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

const driver = `
import { wm } from ${JSON.stringify(path.join(root, 'sdk', 'windows.js'))};
const ids = {};
setTimeout(() => {
  ids.calculator = wm.open('calculator', { protected: true, frame: { x: 80, y: 120, w: 420, h: 520 } });
  ids.notes = wm.open('notes', { frame: { x: 640, y: 120, w: 560, h: 520 } });
}, 2500);
setTimeout(() => {
  for (const app of Object.keys(ids)) {
    const id = ids[app], n = wm.frameNode(id), l = n ? lord.getLayout(n) : [0, 0, 0, 0];
    console.log('SCENE win ' + app + ' node=' + n + ' rect=' + l.map((v) => Math.round(v)).join(',') + ' protected=' + (wm.get(id)?.protected ? 1 : 0));
  }
  wm.setProtected(ids.calculator, false);           // opened protected: locked, this changes nothing
  setTimeout(() => console.log('SCENE ready count=' + lord.media.protectedCount() + ' locked=' + (wm.get(ids.calculator)?.protected ? 1 : 0)), 500);
}, 6000);
`;

fs.mkdirSync(outDir, { recursive: true });
await build({
  stdin: { contents: `import ${JSON.stringify(path.join(root, 'themes', 'default', 'index.jsx'))};\n${driver}`, resolveDir: root, loader: 'js' },
  outfile: path.join(outDir, 'theme.bundle.js'), bundle: true, format: 'iife', target: 'es2020', minify: true, jsx: 'automatic',
  alias: { '@lordos/runtime': path.join(root, 'runtime', 'index.js'), '@lordos/sdk': path.join(root, 'sdk', 'index.js') },
  plugins: [appsPlugin], define: { 'process.env.NODE_ENV': '"production"' }, logLevel: 'warning',
});
fs.copyFileSync(path.join(root, 'themes', 'default', 'theme.json'), path.join(outDir, 'theme.json'));
console.log(`scene bundle -> ${outDir}`);
