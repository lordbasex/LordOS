// sysopen_untrusted_test.mjs — openFileWithApps({ untrusted }) never hands a file that would run (a launcher, a program,
// a script, Java, Wine, a package) to a Linux app: it is shown in Files instead; documents still open.
import { runsCode } from '../../sdk/runscode.js';
let fail = 0, n = 0;
const ok = (c, m) => { n++; if (!c) { fail++; console.log('FAIL ' + m); } };
for (const f of ['a.desktop', 'X.AppImage', 'setup.exe', 'i.msi', 'tool.jar', 'r.sh', 'p.py', 'k.deb', 'x.flatpakref', 'w.bat', 'lib.so', 'a.run', 'app.lpk'])
  ok(runsCode(f), 'runs code: ' + f);
for (const f of ['report.pdf', 'photo.jpg', 'notes.txt', 'sheet.ods', 'doc.docx', 'song.mp3', 'archive.zip', 'page.html', ''])
  ok(!runsCode(f), 'does not run code: ' + f);
globalThis.lord = { mime: { guess: (x) => (x === 'noext' ? 'application/x-executable' : x === 'odd' ? 'application/x-java-archive' : 'application/octet-stream') } };
ok(runsCode('noext'), 'an ELF without extension (by its type)');
ok(runsCode('odd'), 'a Java archive without extension (by its type)');
ok(!runsCode('blob'), 'an unknown binary is not called a program');

// openFileWithApps itself (bundled with esbuild, its SDK neighbours stubbed): an untrusted file that would run never
// reaches lord.apps.launchWith or an app window, whichever of its names says so; a trusted one still does
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath, pathToFileURL } from 'node:url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
let esbuild = null;
for (const p of [path.join(root, 'node_modules'), ...(process.env.NODE_PATH || '').split(':').filter(Boolean)]) {
  try { esbuild = createRequire(path.join(p, 'x.js'))('esbuild'); break; } catch (e) { /* next */ }
}
if (esbuild) {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'sysopen-'));
  const stub = path.join(tmp, 'stub.mjs');
  fs.writeFileSync(stub, `export const opened = [];
export const wm = { open: (id, o) => { opened.push(['wm', id]); return true; } };
export const getApp = () => true;
export const defaultOpenerId = () => 'org.example.Runner.desktop';
export const defaultFor = () => null;
export const nativeOf = () => null;
export const events = {}; export const books = {}; export const contacts = {};
export const basename = (p) => String(p).split('/').pop();`);
  const out = path.join(tmp, 'b.mjs');
  await esbuild.build({ entryPoints: [path.join(root, 'sdk/sysopen.js')], bundle: true, format: 'esm', platform: 'node', outfile: out, logLevel: 'silent',
    plugins: [{ name: 's', setup(b) { b.onResolve({ filter: /^\.\/(windows|fs|defaults|calendar|contacts)\.js$/ }, () => ({ path: stub })); } }] });
  fs.writeFileSync(path.join(tmp, 'e.mjs'), `export * from ${JSON.stringify(out)}; export { opened } from ${JSON.stringify(stub)};`);
  const launched = [];
  globalThis.lord = { mime: { guess: () => 'application/octet-stream' }, apps: { launchWith: (id, p) => { launched.push([id, p]); return true; } },
    fs: { stat: () => ({}) } };
  const M = await import(pathToFileURL(path.join(tmp, 'e.mjs')).href);
  const cases = [['/h/Notes/a/x.desktop', 'x.desktop'], ['/h/Notes/a/t.jar', 't.jar'], ['/h/Notes/a/run.sh', 'readme.txt'], ['/h/Notes/a/doc.txt', 'evil.AppImage']];
  for (const [p, nm] of cases) {
    launched.length = 0; M.opened.length = 0;
    const r = M.openFileWithApps(p, nm, { untrusted: true, reveal: false });
    ok(r === false && launched.length === 0 && M.opened.length === 0, `untrusted ${nm} (${p}) never opened: ${JSON.stringify([r, launched, M.opened])}`);
  }
  launched.length = 0;
  M.openFileWithApps('/h/d/sheet.ods', 'sheet.ods', { untrusted: true, reveal: false });
  ok(launched.length === 1, 'an untrusted document still opens in its app');
  launched.length = 0;
  M.openFileWithApps('/h/d/t.jar', 't.jar', {});
  ok(launched.length === 1, 'Files (trusted) still opens a .jar with its app');
  fs.rmSync(tmp, { recursive: true, force: true });
} else console.log('openFileWithApps checks skipped: needs node_modules (esbuild)');

console.log(`sysopen-untrusted-test: ${n - fail} ok, ${fail} failed`);
process.exit(fail ? 1 : 0);
