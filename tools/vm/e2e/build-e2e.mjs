// build-e2e.mjs — test-only theme bundle for Machines' end-to-end bench (tools/vm/e2e/README.md), the approach of the
// Cloud bench (branch cloud-e2e, tools/cloud/e2e): the default theme exactly as tools/build-theme.mjs builds it, plus
// driver.js (same module instances of the SDK). Writes themes/default/theme.vm-e2e.bundle.js and
// code-manifest.vm-e2e.json (git-ignored; never shipped).
//   node tools/vm/e2e/build-e2e.mjs
import { build } from 'esbuild';
import { fileURLToPath } from 'node:url';
import fs from 'node:fs';
import { createHash } from 'node:crypto';
import path from 'node:path';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');

// the same virtual '@lordos/apps' module as tools/build-theme.mjs
const appsPlugin = {
  name: 'lordos-apps',
  setup(b) {
    b.onResolve({ filter: /^@lordos\/apps$/ }, () => ({ path: 'apps', namespace: 'lordos-apps' }));
    b.onLoad({ filter: /.*/, namespace: 'lordos-apps' }, () => {
      const dir = path.join(root, 'apps');
      const ids = fs.readdirSync(dir).filter((d) => fs.existsSync(path.join(dir, d, 'manifest.json'))).sort();
      const lines = ids.map((id, i) => {
        const base = path.join(dir, id);
        return `import * as A${i} from ${JSON.stringify(path.join(base, 'index.jsx'))};\n` +
               `import M${i} from ${JSON.stringify(path.join(base, 'manifest.json'))};`;
      });
      const iso = ids.map((id) => JSON.parse(fs.readFileSync(path.join(dir, id, 'manifest.json'), 'utf8')).process === 'isolated');
      const list = ids.map((_, i) => (iso[i]
        ? `{ ...M${i}, system: true, isolated: true, Component: isolatedApp(M${i}.id, A${i}.default), Icon: A${i}.Icon }`
        : `{ ...M${i}, system: true, Component: A${i}.default, Icon: A${i}.Icon }`)).join(',\n  ');
      const head = iso.some(Boolean) ? `import { isolatedApp } from ${JSON.stringify(path.join(root, 'sdk', 'packages.js'))};\n` : '';
      return { contents: `${head}${lines.join('\n')}\nexport const apps = [\n  ${list}\n];\n`, resolveDir: root, loader: 'js' };
    });
  },
};

const entry = {
  name: 'e2e-entry',
  setup(b) {
    b.onResolve({ filter: /^e2e-entry$/ }, () => ({ path: 'e2e', namespace: 'e2e-entry' }));
    b.onLoad({ filter: /.*/, namespace: 'e2e-entry' }, () => ({
      contents: `import ${JSON.stringify(path.join(root, 'themes/default/index.jsx'))};\nimport ${JSON.stringify(path.join(root, 'tools/vm/e2e/driver.js'))};\n`,
      resolveDir: root, loader: 'js',
    }));
  },
};

const t0 = Date.now();
const out = path.join(root, 'themes/default/theme.vm-e2e.bundle.js');
await build({
  entryPoints: ['e2e-entry'], outfile: out, bundle: true, format: 'iife', target: 'es2020', minify: true, jsx: 'automatic',
  alias: { '@lordos/runtime': path.join(root, 'runtime', 'index.js'), '@lordos/sdk': path.join(root, 'sdk', 'index.js') },
  plugins: [entry, appsPlugin], define: { 'process.env.NODE_ENV': '"production"' }, logLevel: 'warning',
});
// lord-server loads only code listed in code-manifest.json (docs/SIGNING.md): session.sh install puts this bundle in
// place of themes/default/theme.bundle.js in /opt/lordos-e2e and this manifest (the same, with that one entry changed)
// in place of code-manifest.json. The bench's copy is unsigned, like any development checkout.
const data = fs.readFileSync(out);
const man = fs.readFileSync(path.join(root, 'code-manifest.json'), 'utf8').replace(
  /\{ "path": "themes\/default\/theme\.bundle\.js", "size": \d+, "sha256": "[0-9a-f]+" \}/,
  `{ "path": "themes/default/theme.bundle.js", "size": ${data.length}, "sha256": "${createHash('sha256').update(data).digest('hex')}" }`);
fs.writeFileSync(path.join(root, 'themes/default/code-manifest.vm-e2e.json'), man);
console.log(`e2e theme: ${(fs.statSync(out).size / 1024).toFixed(0)} KB in ${Date.now() - t0} ms -> ${path.relative(root, out)}`);
