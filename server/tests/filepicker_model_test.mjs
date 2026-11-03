// The system file dialog without a screen. node server/tests/filepicker_model_test.mjs
import { localDisks, startFolder, pathCrumbs, fitCrumbs, sortRows, stepRow, rangeOf, validName, primaryAction, enterAction } from '../../sdk/filepickermodel.js';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };
const home = '/home/ana';
const usb = { path: '/run/media/ana/USB', fs: 'vfat', name: 'USB' };

eq('local disks only', localDisks([usb, { path: '/media/ana/nas', fs: 'cifs' }, { path: `${home}/.local/share/lordos/remote/x`, fs: 'fuse.sshfs' }, { path: `${home}/.local/share/lordos/remote/y`, fs: 'ext4' }], home), [usb]);
eq('no list', localDisks(null, home), []);

eq('empty start', startFolder('', home), home);
eq('relative', startFolder('Downloads', home), `${home}/Downloads`);
eq('absolute in home', startFolder(`${home}/.ssh/`, home), `${home}/.ssh`);
eq('the home folder', startFolder(home, home), home);
eq('on a disk', startFolder(`${usb.path}/a`, home, [usb]), `${usb.path}/a`);
eq('not on a listed disk', startFolder(`${usb.path}/a`, home, []), home);
eq('outside', startFolder('/etc', home, [usb]), home);
eq('a neighbor with the same start', startFolder('/home/anabel', home), home);
eq('climbing', startFolder('../../etc', home), home);
eq('climbing absolute', startFolder(`${home}/../root`, home), home);

const labels = (p) => pathCrumbs(p, home).map((c) => c.label);
eq('home crumb', pathCrumbs(home, home), [{ label: 'ana', path: home, home: true }]);
eq('in home', labels(`${home}/a/b`), ['ana', 'a', 'b']);
eq('crumb paths', pathCrumbs(`${home}/a/b`, home).map((c) => c.path), [home, `${home}/a`, `${home}/a/b`]);
eq('run media', labels(`${usb.path}/x`), ['USB', 'x']);
eq('media', labels('/media/ana/D/x'), ['D', 'x']);
eq('server', labels(`${home}/.local/share/lordos/remote/s/x`), ['s', 'x']);
eq('whole tree', labels('/etc/ssh'), ['/', 'etc', 'ssh']);
eq('root', pathCrumbs('/', home).map((c) => c.path), ['/']);
eq('short fits', fitCrumbs([1, 2, 3]), [1, 2, 3]);
eq('long keeps ends', fitCrumbs([1, 2, 3, 4, 5, 6, 7]), [1, null, 5, 6, 7]);

const rows = [{ name: 'b.txt', path: '/b', size: 1, mtime: 3 }, { name: 'Z', path: '/z', dir: true, size: 0, mtime: 1 }, { name: 'a10', path: '/a10', size: 5, mtime: 2 }, { name: 'a2', path: '/a2', size: 9, mtime: 9 }];
const names = (s) => sortRows(rows, s).map((r) => r.name);
eq('by name', names({ by: 'name' }), ['Z', 'a2', 'a10', 'b.txt']);
eq('name down', names({ by: 'name', desc: true }).slice(1), ['b.txt', 'a10', 'a2']);
eq('by size', names({ by: 'size' }), ['Z', 'b.txt', 'a10', 'a2']);
eq('by date, newest first', names({ by: 'date', desc: true }), ['Z', 'a2', 'b.txt', 'a10']);

const list = [{ path: '/d', dir: true }, { path: '/x' }, { path: '/y' }];
eq('first', stepRow(list, null, 1).path, '/d');
eq('last', stepRow(list, null, -1).path, '/y');
eq('next', stepRow(list, '/d', 1).path, '/x');
eq('stops at the end', stepRow(list, '/y', 1).path, '/y');
eq('stops at the start', stepRow(list, '/d', -1).path, '/d');
eq('empty', stepRow([], null, 1), null);
eq('range skips folders', rangeOf(list, '/y', '/d'), ['/x', '/y']);
eq('range without anchor', rangeOf(list, null, '/x'), null);

for (const bad of ['', '  ', 'a/b', '.', '..']) eq(`bad name ${bad}`, validName(bad), false);
eq('good name', validName('nuevo ñ.txt'), true);

const file = { path: '/h/f' }, folder = { path: '/h/d', dir: true };
const base = { mode: 'open', dir: '/h', row: null, chosen: [] };
eq('open nothing', primaryAction(base), null);
eq('open file', primaryAction({ ...base, row: file }), { pick: '/h/f' });
eq('open folder enters', primaryAction({ ...base, row: folder }), { go: '/h/d' });
eq('multiple none', primaryAction({ ...base, multiple: true, row: file }), null);
eq('multiple', primaryAction({ ...base, multiple: true, chosen: ['/a', '/b'] }), { pickMany: ['/a', '/b'] });
eq('multiple folder', primaryAction({ ...base, multiple: true, row: folder }), { go: '/h/d' });
eq('folder in view', primaryAction({ ...base, mode: 'folder' }), { pick: '/h' });
eq('folder selected', primaryAction({ ...base, mode: 'folder', row: folder }), { pick: '/h/d' });
eq('save', primaryAction({ ...base, mode: 'save', name: 'a' }), { save: true });
eq('save no name', primaryAction({ ...base, mode: 'save', name: '' }), null);
eq('save read-only', primaryAction({ ...base, mode: 'save', name: 'a', writable: false }), null);
eq('enter on a folder', enterAction({ ...base, mode: 'folder', row: folder }), { go: '/h/d' });
eq('enter, folder in view', enterAction({ ...base, mode: 'folder' }), { pick: '/h' });
eq('enter in save, on a folder', enterAction({ ...base, mode: 'save', name: 'a', row: folder }), { go: '/h/d' });
eq('enter in save', enterAction({ ...base, mode: 'save', name: 'a', row: file }), { save: true });
eq('enter, multiple, one row', enterAction({ ...base, multiple: true, row: file }), { pickMany: ['/h/f'] });
eq('enter, multiple, chosen', enterAction({ ...base, multiple: true, row: file, chosen: ['/a'] }), { pickMany: ['/a'] });

console.log(process.exitCode ? 'FAILED' : `ok ${ok}`);
