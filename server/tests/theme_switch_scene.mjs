// theme_switch_scene.mjs — builds the test bundles of tests/theme_switch_headless.sh's live round (not run by itself):
// each of the four desktops exactly as tools/build-theme.mjs builds it, plus a small driver in the same SDK module
// instances that, in a real headless session, opens two LordOS apps and then switches to the next desktop through the
// SDK's own themes.use (what Settings → Appearance and the first-start chooser call), round all four and back to the
// first. It logs one line per JS context to shell.log: "SCENE <step> current=<id> apps=<ids>". Test only: the
// bundles go to the folder given and are never shipped.
//   node server/tests/theme_switch_scene.mjs OUT_DIR     (writes OUT_DIR/<id>/theme.bundle.js and theme.json)
import { build } from 'esbuild';
import { fileURLToPath } from 'node:url';
import fs from 'node:fs';
import path from 'node:path';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const outDir = path.resolve(process.argv[2] || '');
if (!process.argv[2]) { console.error('usage: theme_switch_scene.mjs OUT_DIR'); process.exit(2); }
export const ORDER = ['default', 'classic', 'workspaces', 'celeste'];

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

// the driver: step n (kept in ~/.scene-step, the JS context is discarded at each switch) logs what it sees, the first
// one opens Settings and Files, and every one but the last switches to the next desktop through sdk themes.use
const driver = `
import { themes } from ${JSON.stringify(path.join(root, 'sdk', 'index.js'))};
import { launchApp } from ${JSON.stringify(path.join(root, 'sdk', 'apps.js'))};
import { windows } from ${JSON.stringify(path.join(root, 'sdk', 'windows.js'))};
import { settings } from ${JSON.stringify(path.join(root, 'sdk', 'settings.js'))};
import { themeSwitching } from ${JSON.stringify(path.join(root, 'sdk', 'themeswitch.jsx'))};
const ORDER = ${JSON.stringify(ORDER)};
const file = () => lord.fs.home() + '/.scene-step';
let step = 0;
try { step = parseInt(lord.fs.read(file()), 10) || 0; } catch (e) { step = 0; }
const apps = () => windows.get().map((w) => w.app).sort().join(',');
setTimeout(() => { if (step === 0) { launchApp('settings'); launchApp('files'); } }, 2500);
setTimeout(() => {
  const cur = themes.current();
  console.log('SCENE ' + step + ' current=' + cur + ' apps=' + apps());
  if (step >= ORDER.length) {
    // one that can't be switched to: false, and the choice stays the theme that runs
    const r = themes.use('no-such-theme');
    console.log('SCENE refused ' + r + ' theme=' + settings.get().theme);
    return;
  }
  lord.fs.write(file(), String(step + 1));
  const next = ORDER[(ORDER.indexOf(cur) + 1) % ORDER.length];
  const ok = themes.use(next);
  console.log('SCENE switch ' + cur + ' -> ' + next + ': ' + ok + ' overlay=' + (themeSwitching() ? 1 : 0));
}, 5000);
`;

fs.mkdirSync(outDir, { recursive: true });
for (const id of ORDER) {
  const dir = path.join(outDir, id);
  fs.mkdirSync(dir, { recursive: true });
  await build({
    stdin: { contents: `import ${JSON.stringify(path.join(root, 'themes', id, 'index.jsx'))};\n${driver}`, resolveDir: root, loader: 'js' },
    outfile: path.join(dir, 'theme.bundle.js'), bundle: true, format: 'iife', target: 'es2020', minify: true, jsx: 'automatic',
    alias: { '@lordos/runtime': path.join(root, 'runtime', 'index.js'), '@lordos/sdk': path.join(root, 'sdk', 'index.js') },
    plugins: [appsPlugin], define: { 'process.env.NODE_ENV': '"production"' }, logLevel: 'warning',
  });
  fs.copyFileSync(path.join(root, 'themes', id, 'theme.json'), path.join(dir, 'theme.json'));
}
console.log(`scene bundles: ${ORDER.join(', ')} -> ${outDir}`);
