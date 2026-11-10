// textfield_menu_test.mjs — (0.8.8.8) the SDK's text fields with the mouse and the history: a right or middle click
// moves no caret, the right-click menu (Undo, Redo, Cut, Copy, Paste, Delete, Select All) edits as the keys do,
// Ctrl+Z and Ctrl+Shift+Z undo and redo, a triple click selects the line, and a selection is the primary selection a
// middle click pastes. Bundles tests/textfield_menu_scene.mjs
// with esbuild (as the theme is built) over a stub `lord` that keeps the node tree, its texts and a clipboard, runs
// it, and reads its checks. Needs node and the repository's node_modules (esbuild, react, react-reconciler).
// An argument names another scene over the same stub: tests/password_field_scene.mjs (the eye and the Caps Lock hint),
// tests/screencast_scene.mjs (the ScreenCast chooser).
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

// A stub lord: nodes, their parents and texts (and which were taken out of the tree), a clipboard and a primary selection, the English shell catalog, every character 10 px wide; any
// other native call is a no-op.
const shellEn = readFileSync(path.join(root, 'assets', 'locales', 'en', 'shell.json'), 'utf8');
const MOCK = `globalThis.__lordTest = (() => {
  const styles = new Map(), parents = new Map(), texts = new Map(), removed = new Set(), clip = { text: '' }, prim = { text: '' };
  let nextId = 1;
  const noop = () => undefined;
  const base = {
    screen: { width: 1920, height: 1080, displays: [], mirror: false },
    version: 'test', now: () => Date.now(), stats: () => ({}),
    getLayout: () => [0, 0, 1920, 1080],
    measureText: (s) => [...String(s)].length * 10,
    createNode: () => { const id = nextId++; styles.set(id, {}); return id; },
    setStyle: (id, k, v) => { const s = styles.get(id); if (!s) return; if (v === undefined) delete s[k]; else s[k] = v; },
    setText: (id, s) => { texts.set(id, s); },
    appendChild: (p, c) => { parents.set(c, p); removed.delete(c); },
    insertBefore: (p, c) => { parents.set(c, p); removed.delete(c); },
    removeChild: (p, c) => { if (parents.get(c) === p) { parents.delete(c); removed.add(c); } },
    destroy: (id) => { removed.add(id); },
    clipboard: { get: () => clip.text, set: (s) => { clip.text = s; } },
    primary: { get: () => prim.text, set: (s) => { prim.text = s; return true; } },
    ime: new Proxy({}, { get: () => noop }),
    text: { direction: () => 'ltr' },
    locale: { languages: () => ['en'], get: () => 'en', catalog: (lang, ns) => (lang === 'en' && ns === 'shell' ? ${JSON.stringify(shellEn)} : null) },
    settings: { load: () => '{}' },
  };
  globalThis.lord = new Proxy(base, { get: (o, k) => (k in o ? o[k] : typeof k === 'string' ? noop : undefined) });
  return { styles, parents, texts, removed, clip, prim };
})();`;

const scene = process.argv[2] || 'textfield_menu_scene.mjs';
const dir = mkdtempSync(path.join(tmpdir(), 'lord-textfield-menu-'));
let bad = 0, n = 0;
try {
  const out = await esbuild.build({
    entryPoints: [path.join(here, path.basename(scene))], bundle: true, write: false, format: 'esm',
    platform: 'node', target: 'es2022', nodePaths, logLevel: 'error', jsx: 'automatic',
    alias: { '@lordos/runtime': path.join(root, 'runtime', 'index.js') }, define: { 'process.env.NODE_ENV': '"production"' },
    banner: { js: MOCK },
  });
  const file = path.join(dir, 'scene.mjs');
  writeFileSync(file, out.outputFiles[0].contents);
  const r = spawnSync(process.execPath, [file], { encoding: 'utf8' });
  for (const line of r.stdout.split('\n').filter(Boolean)) {
    n++;
    if (!line.startsWith('ok ')) { bad++; console.log(`✗ ${line}`); }
  }
  if (r.status !== 0 || !n) { bad++; console.log(`✗ the scene failed (${r.status}): ${r.stderr.slice(0, 2000)}`); }
  console.log(`${path.basename(scene, '.mjs')}: ${n} checks, ${bad} failures`);
} finally {
  rmSync(dir, { recursive: true, force: true });
}
process.exit(bad ? 1 : 0);
