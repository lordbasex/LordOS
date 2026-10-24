// Bundles a React theme (with the SDK and every app in apps/) into a single .js that
// lord-server loads with QuickJS.
// Usage: node tools/build-theme.mjs [theme ...]   (no arguments: every theme)
import { build } from 'esbuild';
import { fileURLToPath } from 'node:url';
import fs from 'node:fs';
import path from 'node:path';
import { writeManifest } from './code-manifest.mjs';

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const profile = process.env.LORD_PROFILE || 'desktop';
if (!['desktop', 'os'].includes(profile)) throw new Error('LORD_PROFILE must be desktop or os');
const standalone = profile === 'desktop';
const profileFile = path.join(root, 'build-profile.json');
const previous = fs.existsSync(profileFile) ? JSON.parse(fs.readFileSync(profileFile, 'utf8')) : null;
if (previous !== profile) {
  // A bundle from another profile must never leak into the new code manifest.
  for (const name of fs.readdirSync(path.join(root, 'themes'))) {
    if (!fs.statSync(path.join(root, 'themes', name)).isDirectory()) continue;
    fs.rmSync(path.join(root, 'themes', name, 'theme.bundle.js'), { force: true });
  }
  fs.rmSync(path.join(root, 'assets', 'sysapps'), { recursive: true, force: true });
  fs.rmSync(path.join(root, 'assets', 'app-host.bundle.js'), { force: true });
}
const defines = { 'process.env.NODE_ENV': '"production"', __LORD_STANDALONE__: String(standalone) };
const includeApp = (id) => !standalone || !['imager', 'installer'].includes(id);
const hostStorePlugin = {
  name: 'lord-de-host-store',
  setup(b) {
    if (standalone) b.onResolve({ filter: /^\.\/debian\.js$/ }, (a) =>
      a.importer === path.join(root, 'apps/store/index.jsx')
        ? { path: path.join(root, 'apps/store/host.jsx') } : null);
  },
};

// Virtual module '@lordos/apps': one entry per apps/<id>/manifest.json.
const appsPlugin = {
  name: 'lordos-apps',
  setup(b) {
    b.onResolve({ filter: /^@lordos\/apps$/ }, () => ({ path: 'apps', namespace: 'lordos-apps' }));
    b.onLoad({ filter: /.*/, namespace: 'lordos-apps' }, () => {
      const dir = path.join(root, 'apps');
      const ids = fs.readdirSync(dir).filter((d) => includeApp(d) && fs.existsSync(path.join(dir, d, 'manifest.json'))).sort();
      const lines = ids.map((id, i) => {
        const base = path.join(dir, id);
        return `import * as A${i} from ${JSON.stringify(path.join(base, 'index.jsx'))};\n` +
               `import M${i} from ${JSON.stringify(path.join(base, 'manifest.json'))};`;
      });
      // "process": "isolated": a built-in app that runs as its own sandboxed process (its window in
      // the theme only starts it; see sdk/packages.js isolatedApp and buildSysApps below)
      const iso = ids.map((id) => JSON.parse(fs.readFileSync(path.join(dir, id, 'manifest.json'), 'utf8')).process === 'isolated');
      // every app in apps/ is a system app (system: true): part of LordOS, verified with it and never
      // removable from the Store (docs/SIGNING.md, "Verified system code")
      const list = ids.map((_, i) => (iso[i]
        ? `{ ...M${i}, system: true, isolated: true, Component: isolatedApp(M${i}.id, A${i}.default), Icon: A${i}.Icon }`
        : `{ ...M${i}, system: true, Component: A${i}.default, Icon: A${i}.Icon }`)).join(',\n  ');
      const head = iso.some(Boolean) ? `import { isolatedApp } from ${JSON.stringify(path.join(root, 'sdk', 'packages.js'))};\n` : '';
      return { contents: `${head}${lines.join('\n')}\nexport const apps = [\n  ${list}\n];\n`, resolveDir: root, loader: 'js' };
    });
  },
};

async function buildTheme(name) {
  const dir = path.join(root, 'themes', name);
  const t0 = Date.now();
  const result = await build({
    entryPoints: [path.join(dir, 'index.jsx')],
    outfile: path.join(dir, 'theme.bundle.js'),
    bundle: true,
    format: 'iife',
    target: 'es2020',
    minify: true,
    jsx: 'automatic',
    alias: {
      '@lordos/runtime': path.join(root, 'runtime', 'index.js'),
      '@lordos/sdk': path.join(root, 'sdk', 'index.js'),
    },
    plugins: [hostStorePlugin, appsPlugin],
    define: defines,
    metafile: true,
    logLevel: 'warning',
  });
  const bytes = Object.values(result.metafile.outputs)[0].bytes;
  console.log(`theme ${name}: ${(bytes / 1024).toFixed(0)} KB in ${Date.now() - t0} ms`);
}

const names = process.argv.slice(2);
// The «app host»: React + runtime + SDK (broker) + the app loader, for lord-server --app
// (see server/src/app/appclient.c). A single bundle in assets/app-host.bundle.js.
async function buildAppHost() {
  const t0 = Date.now();
  const result = await build({
    entryPoints: [path.join(root, 'runtime', 'app-host.js')],
    outfile: path.join(root, 'assets', 'app-host.bundle.js'),
    bundle: true, format: 'iife', target: 'es2020', minify: true, jsx: 'automatic',
    alias: { '@lordos/runtime': path.join(root, 'runtime', 'index.js'), '@lordos/sdk': path.join(root, 'sdk', 'index.js') },
    define: defines, metafile: true, logLevel: 'warning',
  });
  const bytes = Object.values(result.metafile.outputs)[0].bytes;
  console.log(`app-host: ${(bytes / 1024).toFixed(0)} KB in ${Date.now() - t0} ms`);
}

// Built-in apps with "process": "isolated" (Passwords): their app.js for `lord-server --app <id>`,
// built like an .lpk (React and the SDK come from the app host), into assets/sysapps/<id>/.
// lord-server prefers this read-only system package over any user package with the same id.
async function buildSysApps() {
  const dir = path.join(root, 'apps');
  const sharedPlugin = {
    name: 'lordos-shared',
    setup(b) {
      b.onResolve({ filter: /^(react|react\/jsx-runtime|@lordos\/sdk|@lordos\/runtime)$/ }, (a) => ({ path: a.path, namespace: 'lordos-shared' }));
      b.onLoad({ filter: /.*/, namespace: 'lordos-shared' }, (a) => ({
        contents: `module.exports = globalThis.__lordos.require(${JSON.stringify(a.path)});`, loader: 'js',
      }));
    },
  };
  for (const id of fs.readdirSync(dir).sort()) {
    if (!includeApp(id)) continue;
    const mf = path.join(dir, id, 'manifest.json');
    if (!fs.existsSync(mf)) continue;
    const manifest = JSON.parse(fs.readFileSync(mf, 'utf8'));
    if (manifest.process !== 'isolated') continue;
    const t0 = Date.now();
    const out = path.join(root, 'assets', 'sysapps', id);
    fs.mkdirSync(out, { recursive: true });
    const result = await build({
      entryPoints: [path.join(dir, id, 'index.jsx')],
      bundle: true, write: false, format: 'iife', globalName: '__lordosApp', target: 'es2020', minify: true,
      jsx: 'automatic', plugins: [sharedPlugin], define: defines, logLevel: 'warning',
    });
    fs.writeFileSync(path.join(out, 'app.js'), result.outputFiles[0].contents);
    fs.writeFileSync(path.join(out, 'manifest.json'), `${JSON.stringify(manifest, null, 2)}\n`);
    // the app process reads the texts of its own namespace from the package (lord.locale.catalog)
    for (const lang of fs.readdirSync(path.join(root, 'assets', 'locales'))) {
      const src = path.join(root, 'assets', 'locales', lang, `${id}.json`);
      if (!fs.existsSync(src)) continue;
      fs.mkdirSync(path.join(out, 'locales'), { recursive: true });
      fs.copyFileSync(src, path.join(out, 'locales', `${lang}.json`));
    }
    console.log(`system app ${id}: ${(result.outputFiles[0].contents.length / 1024).toFixed(0)} KB in ${Date.now() - t0} ms`);
  }
}

const all = fs.readdirSync(path.join(root, 'themes'))
  .filter((d) => (!standalone || d !== 'greeter') && fs.existsSync(path.join(root, 'themes', d, 'index.jsx')));
for (const name of (names.length ? names : all).filter((n) => n !== 'app-host' && n !== 'sysapps')) await buildTheme(name);
if (!names.length || names.includes('app-host')) await buildAppHost();
if (!names.length || names.includes('sysapps')) await buildSysApps();
// the verified-code manifest of what was just built (unsigned: a development build until
// tools/release/sign-code.sh signs it on the maintainer's machine)
const cm = writeManifest(root);
fs.writeFileSync(profileFile, `${JSON.stringify(profile)}\n`);
console.log(`code-manifest.json: ${cm.files} files${cm.changed ? '' : ' (unchanged)'}`);
