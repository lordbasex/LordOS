// files_clipfiles_test.mjs — what a paste in Files copies, and what it offers Linux apps (apps/files/clipfiles.js).
import { pasteFrom, offerable } from '../../apps/files/clipfiles.js';

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
eq('another app: a copy', pasteFrom(mine, { paths: ['/tmp/x', '/tmp/y'], cut: true, own: false }), { paths: ['/tmp/x', '/tmp/y'], move: false, outside: true });
eq('another app: bad paths left out', pasteFrom(null, { paths: ['/tmp/x', 'rel', '', '/', '/a\nb', '/a\0b', 7, null], own: false }), { paths: ['/tmp/x'], move: false, outside: true });
eq('another app: nothing usable, Files own', pasteFrom(mine, { paths: ['rel'], own: false }), { paths: ['/home/u/a'], move: true, outside: false });
eq('another app: not a list', pasteFrom(null, { paths: 'file:///etc/passwd', own: false }), null);
eq('another app: huge list bounded', pasteFrom(null, { paths: Array.from({ length: 70000 }, (_, i) => `/f${i}`), own: false }).paths.length, 65536);

eq('offerable: local paths', offerable(['/home/u/a b', '/home/u/c']), ['/home/u/a b', '/home/u/c']);
eq('offerable: one not local refuses all', offerable(['/home/u/a', 'cloud:x']), null);
eq('offerable: a line break', offerable(['/home/u/a\nb']), null);
eq('offerable: empty', offerable([]), null);
eq('offerable: none', offerable(undefined), null);

console.log(`files-clipfiles-test: ${ok} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
