// files_names_test.mjs — the free names Files gives copies, duplicates and new folders stay within 255 bytes
// (apps/files/names.js, GitHub #102).
import { freeNameOf, nameBytes, NAME_MAX, renameSelection } from '../../apps/files/names.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};
const S = (...a) => new Set(a);

// renaming selects the name without its extension (ws170 QA MEJORA-1)
eq('rename: up to the last dot', renameSelection('archivo-3.txt'), [0, 9]);
eq('rename: two extensions, the last one kept', renameSelection('backup.tar.gz'), [0, 10]);
eq('rename: a folder whole', renameSelection('fotos.2024', true), [0, 10]);
eq('rename: no dot, whole', renameSelection('README'), [0, 6]);
eq('rename: a hidden file whole', renameSelection('.bashrc'), [0, 7]);
eq('rename: code points, not UTF-16 units', renameSelection('😀ñ.md'), [0, 2]);

eq('bytes: ascii', nameBytes('abc'), 3);
eq('bytes: accents', nameBytes('ñá'), 4);
eq('bytes: CJK', nameBytes('日本'), 6);
eq('bytes: emoji', nameBytes('😀'), 4);

// as before for short names
eq('free', freeNameOf(S(), 'photo.png'), 'photo.png');
eq('taken: 2', freeNameOf(S('photo.png'), 'photo.png'), 'photo 2.png');
eq('taken: 3', freeNameOf(S('photo.png', 'photo 2.png'), 'photo.png'), 'photo 3.png');
eq('a numbered one: next number', freeNameOf(S('photo 2.png'), 'photo 2.png'), 'photo 3.png');
eq('no extension', freeNameOf(S('Folder'), 'Folder'), 'Folder 2');
eq('a dot file keeps its name whole', freeNameOf(S('.bashrc'), '.bashrc'), '.bashrc 2');

// long names: the copy fits 255 bytes, the extension and the number kept
const long = `${'a'.repeat(251)}.jpg`;                    // 255 bytes
eq('long: fits as is', freeNameOf(S(), long), long);
const dup = freeNameOf(S(long), long);
eq('long duplicate: within 255 bytes', nameBytes(dup), NAME_MAX);
eq('long duplicate: number and extension', dup.endsWith(' 2.jpg'), true);
eq('long duplicate: not taken', dup !== long, true);
const dup3 = freeNameOf(S(long, dup), long);
eq('long, again: 3', dup3.endsWith(' 3.jpg') && nameBytes(dup3) <= NAME_MAX, true);

// multi-byte letters are never cut in half
const jp = `${'写'.repeat(84)}.png`;                      // 84 * 3 + 4 = 256 bytes: one too many
const fitted = freeNameOf(S(), jp);
eq('CJK: fitted', nameBytes(fitted) <= NAME_MAX && fitted.endsWith('.png'), true);
eq('CJK: whole letters', [...fitted.slice(0, -4)].every((c) => c === '写'), true);
const jpDup = freeNameOf(S(`${'写'.repeat(80)}.png`), `${'写'.repeat(80)}.png`);
eq('CJK duplicate: within', nameBytes(jpDup) <= NAME_MAX && jpDup.endsWith(' 2.png'), true);
const emo = freeNameOf(S(), `${'😀'.repeat(70)}.txt`);
eq('emoji: within and whole', nameBytes(emo) <= NAME_MAX && [...emo.slice(0, -4)].every((c) => c === '😀'), true);

// an archive's name built from a long name (Compress: "<name>.zip")
const zip = freeNameOf(S(), `${'b'.repeat(253)}.zip`);
eq('a built name past the limit: shortened', nameBytes(zip), NAME_MAX);
eq('…keeping .zip', zip.endsWith('.zip'), true);
// a cut that ends in spaces or dots drops them
eq('no trailing spaces at the cut', /[ .] 2\.txt$/.test(freeNameOf(S(`${'x'.repeat(240)}${' '.repeat(11)}.txt`), `${'x'.repeat(240)}${' '.repeat(11)}.txt`)), false);
// an "extension" that is most of the name is part of the stem
eq('a huge extension', nameBytes(freeNameOf(S(`a.${'e'.repeat(250)}`), `a.${'e'.repeat(250)}`)) <= NAME_MAX, true);

console.log(`files-names-test: ${ok} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
