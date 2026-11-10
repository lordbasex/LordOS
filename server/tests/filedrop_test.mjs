// filedrop_test.mjs — which of the files dragged from Files an app takes (sdk/filedrop.js).
import { sortDrop, dropCount, dropSorter, dropName } from '../../sdk/filedrop.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};
const DIRS = new Set(['/home/u/Pictures/Trip', '/home/u/Music']);
const isDir = (p) => DIRS.has(p);
const image = (name) => /\.(png|jpe?g)$/i.test(name);

eq('images only', sortDrop(['/home/u/a.png', '/home/u/b.JPG'], { accepts: image, isDir }),
  { files: ['/home/u/a.png', '/home/u/b.JPG'], folders: [], skipped: 0 });
eq('what it does not take is counted', sortDrop(['/home/u/a.png', '/home/u/notes.txt', '/home/u/c.pdf'], { accepts: image, isDir }),
  { files: ['/home/u/a.png'], folders: [], skipped: 2 });
eq('a folder, when the app takes them', sortDrop(['/home/u/Pictures/Trip', '/home/u/a.png'], { accepts: image, folders: true, isDir }),
  { files: ['/home/u/a.png'], folders: ['/home/u/Pictures/Trip'], skipped: 0 });
eq('a folder is skipped otherwise', sortDrop(['/home/u/Pictures/Trip', '/home/u/a.png'], { accepts: image, isDir }),
  { files: ['/home/u/a.png'], folders: [], skipped: 1 });
eq('a folder named like a file is still a folder', sortDrop(['/home/u/Music'], { accepts: () => true, isDir }),
  { files: [], folders: [], skipped: 1 });
eq('the extension is the name\'s, not the folder\'s', sortDrop(['/home/u/x.png/readme'], { accepts: image, isDir }),
  { files: [], folders: [], skipped: 1 });
eq('every file by default', sortDrop(['/home/u/a', '/home/u/b.bin']), { files: ['/home/u/a', '/home/u/b.bin'], folders: [], skipped: 0 });
eq('an isDir that throws counts as a file', sortDrop(['/gone/a.png'], { accepts: image, isDir: () => { throw new Error('ENOENT'); } }),
  { files: ['/gone/a.png'], folders: [], skipped: 0 });
eq('empty and missing', sortDrop(null, { accepts: image }), { files: [], folders: [], skipped: 0 });
eq('empty entries are left out', sortDrop(['', null, '/home/u/a.png'], { accepts: image }), { files: ['/home/u/a.png'], folders: [], skipped: 0 });
eq('order is kept', sortDrop(['/z.png', '/a.png', '/m.png'], { accepts: image }).files, ['/z.png', '/a.png', '/m.png']);

eq('dropCount: files and folders', dropCount({ files: ['a'], folders: ['b', 'c'], skipped: 4 }), 3);
eq('dropCount: nothing over the window', dropCount(null), 0);

eq('dropName: a file', dropName('/home/u/a.png'), 'a.png');
eq('dropName: a trailing slash', dropName('/home/u/Trip/'), 'Trip');

// the same paths array during one drag is sorted once (Files passes it on every move)
let calls = 0;
const sorter = dropSorter({ accepts: image, isDir: (p) => { calls++; return DIRS.has(p); } });
const paths = ['/home/u/a.png', '/home/u/Pictures/Trip'];
const first = sorter(paths);
eq('sorter: same paths, same result', sorter(paths) === first, true);
eq('sorter: asked once per path', calls, 2);
sorter(['/home/u/a.png']);
eq('sorter: a new drag is sorted again', calls, 3);

console.log(`filedrop: ${ok} ok, ${bad} failed`);
if (bad) process.exit(1);
