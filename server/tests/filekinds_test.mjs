// filekinds_test.mjs — a file's MIME types for "Open With" (sdk/filekinds.js): every extension of a
// kind has a type, the system's guess comes first, and an app that declares either type opens it
// (issue #49: .xlsx and the other office documents had no type, so no app was ever offered).
//
//   node tests/filekinds_test.mjs        (from server/; run_suites.sh runs it when node exists)
import { kindOf, mimeOf, mimesOf, appOpens } from '../../sdk/filekinds.js';

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};
const f = (name) => ({ name, path: `/home/u/${name}`, dir: false });
const XLSX = 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet';

// ---- the table ----
eq('xlsx kind', kindOf(f('a.xlsx')), 'doc');
eq('xlsx type', mimeOf(f('Book 2.XLSX')), XLSX);
eq('docx type', mimeOf(f('a.docx')), 'application/vnd.openxmlformats-officedocument.wordprocessingml.document');
eq('ods type', mimeOf(f('a.ods')), 'application/vnd.oasis.opendocument.spreadsheet');
eq('epub type', mimeOf(f('a.epub')), 'application/epub+zip');
eq('folder', mimeOf({ name: 'x', dir: true }), 'inode/directory');
eq('code falls back to text', mimeOf(f('a.rs')), 'text/plain');
eq('unknown', mimeOf(f('a.qqq')), 'application/octet-stream');
// every extension of a kind that has no text fallback resolves to a real type
const EXTS = {
  doc: ['odt', 'doc', 'docx', 'rtf', 'ods', 'xls', 'xlsx', 'odp', 'ppt', 'pptx', 'epub'],
  image: ['png', 'jpg', 'jpeg', 'gif', 'webp', 'bmp', 'ico', 'svg', 'tif', 'tiff', 'heic', 'avif'],
  audio: ['mp3', 'ogg', 'oga', 'opus', 'flac', 'wav', 'm4a', 'aac', 'wma', 'mid', 'midi'],
  video: ['mp4', 'mkv', 'webm', 'avi', 'mov', 'm4v', 'wmv', 'flv', 'mpg', 'mpeg', 'ogv'],
  archive: ['zip', 'gz', 'tgz', 'xz', 'bz2', 'zst', '7z', 'rar', 'tar', 'iso', 'img', 'dmg', 'lz', 'lzma'],
  pdf: ['pdf'], deb: ['deb'],
};
for (const [kind, list] of Object.entries(EXTS)) for (const ext of list) {
  eq(`.${ext} is ${kind}`, kindOf(f(`x.${ext}`)), kind);
  const m = mimeOf(f(`x.${ext}`));
  eq(`.${ext} has a type`, m !== 'application/octet-stream' && /^[a-z]+\/[\w.+-]+$/.test(m), true);
}

// ---- the system's guess first ----
eq('no guess: the table', mimesOf(f('a.xlsx'), null), [XLSX]);
eq('same guess once', mimesOf(f('a.xlsx'), () => XLSX), [XLSX]);
eq('guess first', mimesOf(f('a.xlsx'), () => 'application/x-wps-et'), ['application/x-wps-et', XLSX]);
eq('guess fills an unknown name', mimesOf(f('a.numbers'), () => 'application/x-iwork-numbers-sffnumbers'),
  ['application/x-iwork-numbers-sffnumbers', 'application/octet-stream']);
eq('guess null', mimesOf(f('a.xlsx'), () => null), [XLSX]);
eq('guess throws', mimesOf(f('a.xlsx'), () => { throw new Error('x'); }), [XLSX]);
eq('guess junk ignored', mimesOf(f('a.xlsx'), () => 'not a type'), [XLSX]);
eq('guess octet-stream ignored', mimesOf(f('a.bin'), () => 'application/octet-stream'), ['application/octet-stream']);
eq('folder: no guess', mimesOf({ name: 'x.xlsx', dir: true }, () => XLSX), ['inode/directory']);

// ---- which apps open it ----
const wps = `application/wps-office.et;${XLSX};application/vnd.ms-excel;`;
const gridcraft = `${XLSX};text/csv;`;
const opens = (decl, e, guess) => mimesOf(e, guess).some((m) => appOpens(decl, m));
eq('WPS opens .xlsx', opens(wps, f('a.xlsx'), null), true);
eq('GridCraft opens .xlsx', opens(gridcraft, f('a.xlsx'), null), true);
eq('WPS opens .xls', opens(wps, f('a.xls'), null), true);
eq('GridCraft does not open .docx', opens(gridcraft, f('a.docx'), null), false);
eq('found through the guess only', opens('application/x-custom;', f('a.custom'), () => 'application/x-custom'), true);
eq('text app opens code', opens('text/plain;', f('a.py'), () => 'text/x-python'), true);
eq('image/* opens heic', opens('image/*;', f('a.heic'), null), true);

console.log(bad ? `filekinds: ${bad} of ${n} failed` : `filekinds: ${n}/${n} ok`);
process.exit(bad ? 1 : 0);
