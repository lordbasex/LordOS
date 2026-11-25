// build-web.mjs — builds the desktop app's window (dist/) from the SAME UI as LordOS's native imager
// (apps/imager): @lordos/sdk becomes web/sdk.jsx (DOM versions of the few primitives it uses) and
// apps/imager/platform.js becomes web/platform.js (the Tauri commands). docs/IMAGER.md.
//
//   node build-web.mjs        → dist/index.html, dist/app.js, dist/assets/…
import * as esbuild from 'esbuild';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.join(here, '../..');
const dist = path.join(here, 'dist');
const nm = path.join(here, 'node_modules');

const swap = {
  name: 'lordos-desktop',
  setup(b) {
    b.onResolve({ filter: /^@lordos\/sdk$/ }, () => ({ path: path.join(here, 'web/sdk.jsx') }));
    // the native platform module → the desktop one (only apps/imager's own)
    b.onResolve({ filter: /platform\.js$/ }, (a) => {
      const p = path.resolve(a.resolveDir, a.path);
      return p === path.join(repo, 'apps/imager/platform.js') ? { path: path.join(here, 'web/platform.js') } : undefined;
    });
    // one React for everything (the shared files live outside this folder)
    b.onResolve({ filter: /^(react|react-dom|react-dom\/client|react\/jsx-runtime|scheduler)$/ }, (a) => ({ path: require.resolve(a.path, nm) }));
  },
};
import { createRequire } from 'node:module';
const require = createRequire(path.join(nm, 'x.js'));
require.resolve = ((orig) => (id, base) => orig(id, { paths: [base] }))(require.resolve);

fs.rmSync(dist, { recursive: true, force: true });
fs.mkdirSync(path.join(dist, 'assets/mascot'), { recursive: true });
fs.mkdirSync(path.join(dist, 'assets/icons'), { recursive: true });
fs.mkdirSync(path.join(dist, 'assets/fonts'), { recursive: true });
const t0 = Date.now();
await esbuild.build({
  entryPoints: [path.join(here, 'web/main.jsx')],
  bundle: true, minify: true, format: 'esm', target: 'es2022', jsx: 'automatic', legalComments: 'none',
  outfile: path.join(dist, 'app.js'), plugins: [swap], loader: { '.json': 'json' },
  define: { 'process.env.NODE_ENV': '"production"' },
});
for (const f of fs.readdirSync(path.join(repo, 'assets/mascot'))) if (/^lumo-(wave|shield|celebrate|confused|sleep|run)-(128|256)\.png$/.test(f)) fs.copyFileSync(path.join(repo, 'assets/mascot', f), path.join(dist, 'assets/mascot', f));
fs.copyFileSync(path.join(repo, 'assets/icons/lordos-icon-128.png'), path.join(dist, 'assets/icons/lordos-icon-128.png'));
for (const f of ['Inter-Regular.ttf', 'Inter-Medium.ttf', 'Inter-SemiBold.ttf', 'Inter-Bold.ttf', 'Inter-LICENSE.txt', 'JetBrainsMono-Regular.ttf', 'JetBrainsMono-LICENSE.txt'])
  fs.copyFileSync(path.join(repo, 'assets/fonts', f), path.join(dist, 'assets/fonts', f));
fs.copyFileSync(path.join(here, 'web/index.html'), path.join(dist, 'index.html'));
fs.copyFileSync(path.join(here, 'web/style.css'), path.join(dist, 'style.css'));
const kb = (p) => Math.round(fs.statSync(p).size / 1024);
console.log(`dist/app.js ${kb(path.join(dist, 'app.js'))} KB in ${Date.now() - t0} ms`);
