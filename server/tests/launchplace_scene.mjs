// launchplace_scene.mjs — builds the test bundle of tests/launchplace_headless.sh (not run by itself): Lord Workspaces
// exactly as tools/build-theme.mjs builds it, plus a driver in the same module instances that, in a real headless
// session, sets up two activities (Desarrollo, Chat), puts Desarrollo's display in Two halves with Files in the first
// half, launches the fake Chrome (tests/launchplace_client.c, through its .desktop entry and lord.apps.launch, as the
// bar's "most used on this screen" does: tiler.js launchOn), switches to Chat while the "profile picker" is up, and logs
// to shell.log (SCENE lines) what happened to every window, asking the script for a screenshot at each step
// ("SCENE shot <name>", then 1.5 s still). Test only: the bundle goes to the folder given and is never shipped.
//   node server/tests/launchplace_scene.mjs OUT_DIR     (writes OUT_DIR/theme.bundle.js and theme.json)
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

const P = (f) => JSON.stringify(path.join(root, f));
const driver = `
import { wm, windows, activeDesktop, switchDesktop, screenDisplays, workAreaOf } from ${P('sdk/index.js')};
import { tileRects } from ${P('themes/workspaces/model/layouts.js')};
import { setUpActivity, newActivity, updateActivity } from ${P('themes/workspaces/state.js')};
import { setLayout, keyOfDisplay, launchOn } from ${P('themes/workspaces/tiler.js')};
const log = (s) => console.log('SCENE ' + s);
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
const shot = async (name) => { log('shot ' + name); await wait(1800); };
const fr = (w) => [w.x, w.y, w.w, w.h].map(Math.round).join(',');
const dump = (tag) => { for (const w of windows.get()) log(tag + ' win ' + w.id + ' app=' + (w.args?.appId || '') + ' desk=' + w.desktop + ' min=' + (w.minimized ? 1 : 0) + ' parent=' + (w.args?.parent || '') + ' rect=' + fr(w)); };
(async () => {
  let lastF = {};
  windows.subscribe(() => { for (const w of windows.get()) { const k = fr(w) + ' d' + w.desktop + ' p' + (w.args?.parent || ''); if (String(w.id).startsWith('linux') && lastF[w.id] !== k) { lastF[w.id] = k; log('trace ' + w.id + ' ' + k + ' active=' + activeDesktop.get()); } } });
  await wait(3000);
  const d = screenDisplays()[0];
  const dk = keyOfDisplay(d);
  const dev = activeDesktop.get();
  setUpActivity(dev, { name: 'Desarrollo', icon: 'code', pins: ['files'] });   // Files opens (a pin opens at setup)
  updateActivity(dev, (m) => ({ ...m, pins: ['files', 'desktop:fakechrome'] }));
  const chat = newActivity();
  setUpActivity(chat, { name: 'Chat', icon: 'chat' });
  switchDesktop(dev);
  await wait(400);
  setLayout(dev, dk, 'halves');
  await wait(1500);
  const halves = tileRects('halves', workAreaOf(d), 2, 0.5) || [];
  log('ready dev=' + dev + ' chat=' + chat + ' dk=' + dk + ' halves=' + halves.map(fr).join(';'));
  await shot('start');
  log('launch ' + launchOn(d, 'desktop:fakechrome'));
  await wait(1300);
  dump('picker');
  await shot('picker');
  switchDesktop(chat);                 // the person goes to Chat while choosing a profile
  log('switched chat');
  await wait(4200);                    // the picker closes, the browser and its dialog come (on Chat, in view)
  dump('after');
  await shot('chat-after');
  switchDesktop(dev);
  await wait(1500);
  dump('back');
  await shot('placed');
  const files = windows.get().find((w) => w.id === 'files' || String(w.id).startsWith('files'));
  if (files) wm.minimize(files.id);
  await wait(1200);
  dump('min');
  await shot('bar-minimized');
  log('click menu');                   // the script right-clicks the browser's icon in the bar (test hooks)
  await wait(2500);
  log('done');
})();
`;

fs.mkdirSync(outDir, { recursive: true });
await build({
  stdin: { contents: `import ${JSON.stringify(path.join(root, 'themes', 'workspaces', 'index.jsx'))};\n${driver}`, resolveDir: root, loader: 'js' },
  outfile: path.join(outDir, 'theme.bundle.js'), bundle: true, format: 'iife', target: 'es2020', minify: true, jsx: 'automatic',
  alias: { '@lordos/runtime': path.join(root, 'runtime', 'index.js'), '@lordos/sdk': path.join(root, 'sdk', 'index.js') },
  plugins: [appsPlugin], define: { 'process.env.NODE_ENV': '"production"' }, logLevel: 'warning',
});
fs.copyFileSync(path.join(root, 'themes', 'workspaces', 'theme.json'), path.join(outDir, 'theme.json'));
console.log(`scene bundle -> ${outDir}`);
