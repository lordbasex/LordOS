// The Terminal's Files mode without a screen (apps/terminal/sftp.js): permissions, paths, names, sorting, sizes,
// the free name for a download and the speed graph. node server/tests/terminal_sftp_test.mjs
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const require = createRequire(import.meta.url);
let esbuild;
try { esbuild = require(path.join(root, 'node_modules/esbuild')); } catch (e) { console.log('needs node_modules (esbuild)'); process.exit(1); }
const out = await esbuild.build({
  stdin: { contents: "export * from './apps/terminal/sftp.js';", resolveDir: root }, bundle: true, write: false, format: 'esm', platform: 'node',
  plugins: [{ name: 'sdk', setup(b) { b.onResolve({ filter: /^@lordos\/sdk$/ }, () => ({ path: 'sdk', namespace: 'sdk' })); b.onLoad({ filter: /.*/, namespace: 'sdk' }, () => ({ contents: 'export const createStore = (v) => { let s = v; return { get: () => s, set: (f) => { s = typeof f === "function" ? f(s) : f; } }; };' })); } }],
});
const m = await import(`data:text/javascript;base64,${Buffer.from(out.outputFiles[0].text).toString('base64')}`);

let n = 0, bad = 0;
const eq = (what, got, want) => { n++; if (JSON.stringify(got) !== JSON.stringify(want)) { bad++; console.log(`FAIL ${what}\n  got  ${JSON.stringify(got)}\n  want ${JSON.stringify(want)}`); } };

eq('perm string', [m.permString(0o755, 'd'), m.permString(0o644), m.permString(0o4750), m.permString(0o777, 'l')], ['drwxr-xr-x', '-rw-r--r--', '-rwxr-x---', 'lrwxrwxrwx']);
eq('boxes round trip', m.boxesPerm(m.permBoxes(0o640)), 0o640);
eq('boxes', m.permBoxes(0o750), [[true, true, true], [true, false, true], [false, false, false]]);
eq('octal', [m.octal(0o644), m.octal(0o4755), m.octal(0)], ['0644', '04755', '0000']);
eq('parse octal', [m.parseOctal('644'), m.parseOctal('0755'), m.parseOctal('4755'), m.parseOctal('9'), m.parseOctal('rwx')], [0o644, 0o755, 0o4755, null, null]);
eq('parent', [m.parentOf('/var/www/html'), m.parentOf('/var'), m.parentOf('/'), m.parentOf('/var/www/')], ['/var/www', '/', '/', '/var']);
eq('join', [m.joinPath('/var/www', 'a'), m.joinPath('/', 'etc'), m.joinPath('/x/', 'y')], ['/var/www/a', '/etc', '/x/y']);
eq('route of an upload', m.transferRoute({ kind: 'up', args: { src: '/home/ana/Descargas/a.iso', path: '/home/pi/Downloads/a.iso' } }, '/home/ana', 'pi@lordos-pi'),
  { from: '~/Descargas/a.iso', to: 'pi@lordos-pi:/home/pi/Downloads/' });
eq('route of a download', m.transferRoute({ kind: 'down', args: { path: '/srv/x.log', dest: '/home/ana/Descargas/x.log' } }, '/home/ana/', ''),
  { from: '/srv/x.log', to: '~/Descargas/' });
eq('route outside the home', m.transferRoute({ kind: 'down', args: { path: '/a', dest: '/mnt/disk/a' } }, '/home/ana', 'h'), { from: 'h:/a', to: '/mnt/disk/' });
eq('route of a home look-alike', m.transferRoute({ kind: 'up', args: { src: '/home/anabel/f', path: '/f' } }, '/home/ana', '').from, '/home/anabel/f');
eq('good names', ['a.txt', 'a b', '', '.', '..', 'a/b', 'x\ny', 'x'.repeat(256)].map(m.goodName), [true, true, false, false, false, false, false, false]);
const list = [{ name: 'b.txt', kind: '-', size: 3 }, { name: 'a10', kind: 'd' }, { name: 'a2', kind: 'd' }, { name: '.env', kind: '-', size: 9 }, { name: 'A.txt', kind: '-', size: 1 }];
eq('sorted: folders first, numbers in order', m.sortEntries(list).map((e) => e.name), ['a2', 'a10', '.env', 'A.txt', 'b.txt']);
eq('sorted by size, descending', m.sortEntries(list, 'size', true).map((e) => e.name), ['a10', 'a2', '.env', 'b.txt', 'A.txt']);
eq('hidden and filter', [m.visible(list, false, '').length, m.visible(list, true, '').length, m.visible(list, false, 'TXT').map((e) => e.name)], [4, 5, ['b.txt', 'A.txt']]);
eq('sizes', [m.size(0), m.size(1023), m.size(1536), m.size(5 * 1024 * 1024), m.size(250 * 1024 * 1024 * 1024)], ['0 B', '1023 B', '1.5 KB', '5.0 MB', '250 GB']);
eq('free name', [m.freeName('a.txt', new Set()), m.freeName('a.txt', new Set(['a.txt'])), m.freeName('a.txt', new Set(['a.txt', 'a (2).txt'])), m.freeName('Makefile', new Set(['Makefile']))],
  ['a.txt', 'a (2).txt', 'a (3).txt', 'Makefile (2)']);
const now = 1_000_000;
const s = m.speedSeries([{ at: now - 500, kind: 'up', bytes: 10 }, { at: now - 1500, kind: 'down', bytes: 7 }, { at: now - 90000, kind: 'up', bytes: 99 }], 3, now);
eq('speed graph: the last seconds, older ones out', s, [{ up: 0, down: 0 }, { up: 0, down: 7 }, { up: 10, down: 0 }]);

console.log(`terminal_sftp_test: ${n - bad}/${n} checks`);
process.exit(bad ? 1 : 0);
