// files_clipfiles_test.mjs — what a paste in Files copies, and what it offers Linux apps (apps/files/clipfiles.js).
import { pasteFrom, offerable, pasteAsk } from '../../apps/files/clipfiles.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};

const mine = { mode: 'cut', paths: ['/home/u/a'] };
eq('nothing', pasteFrom(null, null), null);
eq('Files: a cut moves', pasteFrom(mine, null), { paths: ['/home/u/a'], move: true, outside: false });
eq('Files: a copy copies', pasteFrom({ mode: 'copy', paths: ['/home/u/a'] }, null), { paths: ['/home/u/a'], move: false, outside: false });
eq('Files: an empty list', pasteFrom({ mode: 'copy', paths: [] }, null), null);
// the session clipboard says it is Files' own: Files' store has the mode
eq('own: Files decides', pasteFrom(mine, { paths: ['/home/u/a'], cut: true, own: true }), { paths: ['/home/u/a'], move: true, outside: false });
eq('own, store gone: nothing', pasteFrom(null, { paths: ['/home/u/a'], cut: true, own: true }), null);
// another app's files are newer than Files' own, and are always a copy
eq('another app: a copy', pasteFrom(mine, { paths: ['/tmp/x', '/tmp/y'], cut: true, own: false }), { paths: ['/tmp/x', '/tmp/y'], move: false, outside: true, from: '' });
eq('another app: bad paths left out', pasteFrom(null, { paths: ['/tmp/x', 'rel', '', '/', '/a\nb', '/a\0b', 7, null], own: false }), { paths: ['/tmp/x'], move: false, outside: true, from: '' });
eq('another app: nothing usable, Files own', pasteFrom(mine, { paths: ['rel'], own: false }), { paths: ['/home/u/a'], move: true, outside: false });
eq('another app: not a list', pasteFrom(null, { paths: 'file:///etc/passwd', own: false }), null);
eq('another app: huge list bounded', pasteFrom(null, { paths: Array.from({ length: 70000 }, (_, i) => `/f${i}`), own: false }).paths.length, 65536);

eq('another app: its name', pasteFrom(null, { paths: ['/tmp/x'], own: false, from: 'org.example.Files' }).from, 'org.example.Files');
eq('another app: a name that is not a string', pasteFrom(null, { paths: ['/tmp/x'], own: false, from: 7 }).from, '');

// a paste of another app's files asks first; Files' own never does
eq('ask: Files own', pasteAsk(pasteFrom(mine, null)), null);
eq('ask: nothing', pasteAsk(null), null);
eq('ask: another app', pasteAsk(pasteFrom(null, { paths: ['/home/u/.ssh/id_ed25519', '/home/u/a b'], own: false, from: 'org.example.Files' })),
  { count: 2, names: ['id_ed25519', 'a b'], more: 0, from: 'org.example.Files' });
eq('ask: the first five names and how many more', pasteAsk({ outside: true, paths: Array.from({ length: 8 }, (_, i) => `/d/f${i}`), from: '' }),
  { count: 8, names: ['/d/f0', '/d/f1', '/d/f2', '/d/f3', '/d/f4'].map((p) => p.slice(3)), more: 3, from: '' });
eq('ask: exactly five', pasteAsk({ outside: true, paths: Array.from({ length: 5 }, (_, i) => `/f${i}`) }).more, 0);
eq('ask: a folder or the root', pasteAsk({ outside: true, paths: ['/home/u/dir', '/'] }).names, ['dir', '/']);
eq('ask: a window title on one line', pasteAsk({ outside: true, paths: ['/a'], from: '  Home\n\u2028 Folder\t' }).from, 'Home Folder');
eq('ask: a long name is cut', [...pasteAsk({ outside: true, paths: ['/a'], from: '😀'.repeat(80) }).from].length, 60);
eq('ask: an empty list', pasteAsk({ outside: true, paths: [] }), null);

eq('offerable: local paths', offerable(['/home/u/a b', '/home/u/c']), ['/home/u/a b', '/home/u/c']);
eq('offerable: one not local refuses all', offerable(['/home/u/a', 'cloud:x']), null);
eq('offerable: a line break', offerable(['/home/u/a\nb']), null);
eq('offerable: empty', offerable([]), null);
eq('offerable: none', offerable(undefined), null);

console.log(`files-clipfiles-test: ${ok} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
