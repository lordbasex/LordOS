// files_remote_test.mjs — Files' listings of slow folders (apps/files/remotedirs.js, 0.8.8.7): a folder on a Cloud
// account or a network share is listed off the interface's thread, so asking for it never waits; going back shows the
// last listing at once; a delete hides its items until a new listing says whether they are gone.
//
//   node tests/files_remote_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { createRemoteDirs } = await import(path.join(here, '../../apps/files/remotedirs.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};
const flush = () => new Promise((r) => setTimeout(r, 0));

// a slow file system: every listing waits until the test answers it
const disk = { '/c': ['a', 'b', 'c'], '/c/sub': ['x'], '/': ['c'] };
const calls = [];
const list = (dir) => new Promise((resolve, reject) => calls.push({ dir, answer: () => (disk[dir] ? resolve(disk[dir].map((name) => ({ name, dir: false }))) : reject(new Error('gone'))) }));
const answerAll = async () => { while (calls.length) { calls.shift().answer(); await flush(); } };
let changes = 0;
const R = createRemoteDirs({ list, onChange: () => { changes++; } });
const names = (v) => (v.entries ? v.entries.map((e) => e.name) : null);

// ---- the first visit: nothing waits, the listing comes later ----
let v = R.get('/c');
eq('first get answers at once, busy', [names(v), v.busy], [null, true]);
eq('one listing asked', calls.length, 1);
R.get('/c'); R.get('/c');
eq('asking again while it runs lists once', calls.length, 1);
await answerAll();
v = R.get('/c');
eq('the listing arrived', [names(v), v.busy, v.error], [['a', 'b', 'c'], false, null]);
eq('onChange told Files', changes > 0, true);
eq('a known folder is not listed again by get', calls.length, 0);

// ---- going into a subfolder and back: the parent's last listing shows at once ----
R.get('/c/sub');
await answerAll();
eq('subfolder', names(R.get('/c/sub')), ['x']);
R.refresh('/c');                                   // Files revalidates the folder it comes back to
v = R.get('/c');
eq('back: last listing shown while the new one runs', [names(v), v.busy], [['a', 'b', 'c'], true]);
await answerAll();

// ---- refresh while busy: one more listing after the running one, never two at once ----
R.refresh('/c');
R.refresh('/c');
R.refresh('/c');
eq('one at a time', calls.length, 1);
disk['/c'] = ['a', 'b', 'c', 'd'];
calls.shift().answer(); await flush();
eq('the asked-again listing follows', calls.length, 1);
await answerAll();
eq('new file seen', names(R.get('/c')), ['a', 'b', 'c', 'd']);

// ---- delete: hidden at once, gone after the new listing; a failed one comes back ----
R.hide(['/c/b', '/c/d']);
eq('hidden at once (optimistic)', names(R.get('/c')), ['a', 'c']);
eq('hiding lists nothing', calls.length, 0);
R.refresh('/c');                                   // an unrelated refresh while the delete runs
disk['/c'] = ['a', 'b', 'c', 'd'];                 // the provider hasn't finished yet
await answerAll();
eq('still hidden while the delete runs', names(R.get('/c')), ['a', 'c']);
disk['/c'] = ['a', 'c', 'd'];                      // b deleted; d failed
R.settle(['/c/b', '/c/d']);
eq('settle lists the folder again', calls.length, 1);
eq('hidden until that listing answers', names(R.get('/c')), ['a', 'c']);
await answerAll();
eq('the failed one comes back, the deleted one stays gone', names(R.get('/c')), ['a', 'c', 'd']);
eq('nothing hidden any more', [R.hidden('/c/b'), R.hidden('/c/d')], [false, false]);

// settle while a listing is already running: that one (asked before the end) doesn't decide
R.hide(['/c/a']);
R.refresh('/c');
eq('running', calls.length, 1);
disk['/c'] = ['c', 'd'];
R.settle(['/c/a']);
calls.shift().answer(); await flush();             // the listing asked before the delete ended
eq('still hidden after the older listing', [R.hidden('/c/a'), names(R.get('/c'))], [true, ['c', 'd']]);
await answerAll();
eq('gone after the listing asked after it', [names(R.get('/c')), R.hidden('/c/a')], [['c', 'd'], false]);

// ---- errors keep what was shown, and say why ----
delete disk['/c/sub'];
R.refresh('/c/sub');
await answerAll();
v = R.get('/c/sub');
eq('error kept with the last listing', [names(v), v.error], [['x'], 'gone']);
v = R.get('/nope');
await answerAll();
v = R.get('/nope');
eq('a folder that fails the first time: empty and the error', [names(v), v.error], [[], 'gone']);

// ---- a list function that throws at once (the request was refused: fs.busy) ----
const R2 = createRemoteDirs({ list: () => { throw new Error('busy'); } });
R2.get('/x');
await flush(); await flush();
eq('refused at once: an error, not an exception', R2.get('/x').error, 'busy');

// ---- the cache is bounded ----
const R3 = createRemoteDirs({ list: async () => [], max: 4 });
for (let i = 0; i < 10; i++) { R3.get(`/d${i}`); await flush(); await flush(); }
let kept = 0;
for (let i = 0; i < 10; i++) if (R3.has(`/d${i}`)) kept++;
eq('at most max folders kept', kept <= 4, true);
eq('the last one is kept', R3.has('/d9'), true);

// ---- which folders are slow: the mount table (sdk/mountmodel.js) ----
const { remoteOf } = await import(path.join(here, '../../sdk/mountmodel.js'));
const table = [{ path: '/', remote: false }, { path: '/home', remote: false }, { path: '/home/ana/Cloud/iCloud', remote: true },
  { path: '/media/ana/NAS', remote: true }, { path: '/media/ana/NAS/usb', remote: false }];
eq('a Cloud account folder', remoteOf('/home/ana/Cloud/iCloud/Photos/a.jpg', table), true);
eq('the account itself', remoteOf('/home/ana/Cloud/iCloud', table), true);
eq('its parent is local', remoteOf('/home/ana/Cloud', table), false);
eq('a name that only starts the same', remoteOf('/home/ana/Cloud/iCloud2', table), false);
eq('a share', remoteOf('/media/ana/NAS/docs', table), true);
eq('a local disk inside a share: the longest mount decides', remoteOf('/media/ana/NAS/usb/x', table), false);
eq('no table', remoteOf('/x', []), false);

console.log(`files-remote-test: ${n - bad}/${n} passed`);
if (bad) process.exit(1);
