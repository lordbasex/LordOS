// notes_store_test.mjs — Notes' storage and what a note's links may reach (apps/notes/store.js, links.js; GitHub
// #238), over a real temporary home: store.js is bundled with esbuild and '@lordos/sdk' replaced by a stub whose fs
// works on that folder the way lord.fs does (a link is listed and stated as a link; a copy copies the link itself), so
// links, folders and odd names are real files here. Nothing outside the temporary folder is read or written.
//
// The format (header, round trip, odd values), the title, sane geometry, new notes and attachments (names with ../,
// slashes, control and direction-override characters, brackets, spaces, a huge file, a folder, a link), and the link
// rules: file://, javascript:, data:, mailto:, an absolute path, ~, .., a protocol-relative address, percent-encoded
// ways out, a link inside the note's folder pointing out, nota.md itself, remote images; and that every write stays
// under ~/Notes.
import { createRequire } from 'node:module';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
let esbuild = null;
for (const p of [path.join(root, 'node_modules'), ...(process.env.NODE_PATH || '').split(':').filter(Boolean)]) {
  try { esbuild = createRequire(path.join(p, 'x.js'))('esbuild'); break; } catch (e) { /* next */ }
}
if (!esbuild) { console.log('skipped: needs node_modules (esbuild)'); process.exit(77); }

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'notes-store-'));
const home = path.join(tmp, 'home');
const outside = path.join(tmp, 'outside');
fs.mkdirSync(home); fs.mkdirSync(outside);
fs.writeFileSync(path.join(outside, 'secret.txt'), 'top secret');
fs.writeFileSync(path.join(outside, 'pic.png'), 'png');

// The stub: what store.js uses of '@lordos/sdk'. Every write is recorded with its real path.
const STUB = `
import nfs from 'node:fs';
import npath from 'node:path';
export const HOME = ${JSON.stringify(home)};
export const writes = [];
const abs = (p) => (String(p).startsWith('/') ? String(p) : npath.join(HOME, String(p)));
const rec = (p) => { writes.push(npath.resolve(abs(p))); };
export const fs = {
  home: () => HOME,
  list(p, { hidden = false } = {}) {
    return nfs.readdirSync(abs(p), { withFileTypes: true }).filter((d) => hidden || !d.name.startsWith('.')).map((d) => {
      const full = npath.join(abs(p), d.name);
      let dir = false; try { dir = nfs.statSync(full).isDirectory(); } catch (e) { dir = false; }
      const st = nfs.lstatSync(full);
      return { name: d.name, link: d.isSymbolicLink(), dir, size: st.size, mtime: st.mtimeMs };
    });
  },
  stat(p) {
    const st = nfs.lstatSync(abs(p));
    let dir = st.isDirectory();
    if (st.isSymbolicLink()) { try { dir = nfs.statSync(abs(p)).isDirectory(); } catch (e) { dir = false; } }
    return { name: npath.basename(abs(p)), path: abs(p), dir, link: st.isSymbolicLink(), size: st.size, mtime: st.mtimeMs };
  },
  read: (p) => nfs.readFileSync(abs(p), 'utf8'),
  write(p, data) { rec(p); nfs.writeFileSync(abs(p), data); },
  mkdir(p) { rec(p); nfs.mkdirSync(abs(p)); },
  rename(a, b) { rec(b); nfs.renameSync(abs(a), abs(b)); },
  copy(a, b) {
    rec(b);
    const st = nfs.lstatSync(abs(a));
    if (st.isSymbolicLink()) nfs.symlinkSync(nfs.readlinkSync(abs(a)), abs(b));
    else nfs.copyFileSync(abs(a), abs(b));
    return 1;
  },
  trash(p) { rec(p); nfs.rmSync(abs(p), { recursive: true }); return true; },
};
export function createStore(v) {
  let s = v; const subs = new Set();
  return { get: () => s, set(n) { s = typeof n === 'function' ? n(s) : n; for (const f of subs) f(s); }, subscribe(f) { subs.add(f); return () => subs.delete(f); } };
}
export const screen = { width: 1920, height: 1080 };
export const workArea = () => ({ x: 0, y: 24, w: 1920, h: 1000 });
export const registerWidgetType = () => {};
export const widgets = [];
export const setWidgets = (type, list) => { widgets.splice(0, widgets.length, ...list); };
`;
const stubFile = path.join(tmp, 'sdk-stub.mjs');
fs.writeFileSync(stubFile, STUB);
const entry = path.join(tmp, 'entry.mjs');
fs.writeFileSync(entry, `export * from ${JSON.stringify(path.join(root, 'apps/notes/store.js'))};
export * as links from ${JSON.stringify(path.join(root, 'apps/notes/links.js'))};
export { writes, HOME, widgets } from ${JSON.stringify(stubFile)};`);
const out = path.join(tmp, 'bundle.mjs');
await esbuild.build({
  entryPoints: [entry], bundle: true, format: 'esm', platform: 'node', outfile: out, logLevel: 'silent',
  plugins: [{ name: 'sdk', setup(b) { b.onResolve({ filter: /^@lordos\/sdk$/ }, () => ({ path: stubFile })); } }],
});
globalThis.lord = { clipboard: {} };
const S = await import(pathToFileURL(out).href);
const L = S.links;

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

// ---- the format ----
{
  const n = { title: 'Groceries: "weekend"', color: 'green', x: 10, y: 20, w: 300, h: 200, desktop: true, pinned: false,
    created: '2026-10-16T10:12:00.000Z', updated: '2026-10-16T10:20:31.000Z', body: '# Groceries\n- [ ] yerba\n---\nnot a header\n' };
  const back = S.parseNote(S.serialize(n));
  eq('round trip: title with a colon and quotes', back.meta.title, n.title);
  eq('round trip: numbers and booleans', [back.meta.x, back.meta.w, back.meta.desktop, back.meta.pinned], [10, 300, true, false]);
  eq('round trip: a --- line in the body stays in the body', back.body, n.body);
  eq('CRLF files read the same', S.parseNote('---\r\ntitle: a\r\n---\r\nbody').body, 'body');
  eq('no header: all body', S.parseNote('just text').body, 'just text');
  eq('a title of digits stays text', S.parseNote(S.serialize({ ...n, title: '2026' })).meta.title, '2026');
}
// ---- the title ----
eq('title: heading marks off', S.titleOf('## Plan *now*\nmore'), 'Plan now');
eq('title: task marks off', S.titleOf('- [ ] buy milk'), 'buy milk');
eq('title: control characters become spaces', S.titleOf('a\u0007b\u0000c'), 'a b c');
eq('title: direction overrides dropped', S.titleOf('invoice‮gpj.exe'), 'invoicegpj.exe');
eq('title: line separators', S.titleOf('one two'), 'one two');
eq('title: at most 60', S.titleOf('x'.repeat(100)).length, 60);
// ---- geometry from a header edited by hand ----
eq('geometry: defaults', S.geometryOf({}), { x: 80, y: 80, w: 260, h: 240 });
eq('geometry: clamped', S.geometryOf({ x: -1e9, y: 1e12, w: 5, h: 1e7 }), { x: -2000, y: 20000, w: 160, h: 4000 });
eq('geometry: not numbers', S.geometryOf({ x: 'NaN', w: true }), { x: 80, y: 80, w: 260, h: 240 });
// ---- folder names ----
eq('slug: a plain name', S.validSlug('note-20261124-101010'), true);
for (const bad of ['', '.', '..', '.trash', 'a/b', '../x', 'a\\b', 'a\nb', 'x'.repeat(201), null, 5]) eq(`slug refused: ${JSON.stringify(bad)}`, S.validSlug(bad), false);
eq('folder() refuses a way out', (() => { try { S.folder('../../etc'); return 'made'; } catch (e) { return 'refused'; } })(), 'refused');

// ---- links ----
const kind = (u) => L.linkTarget(u).kind;
eq('link: a plain attachment', L.linkTarget('report.pdf'), { kind: 'file', name: 'report.pdf' });
eq('link: ./ prefix', L.linkTarget('./report.pdf'), { kind: 'file', name: 'report.pdf' });
eq('link: percent-encoded spaces', L.linkTarget('my%20file%20%281%29.pdf'), { kind: 'file', name: 'my file (1).pdf' });
eq('link: https is web (shown, not opened)', kind('https://example.com/x'), 'web');
for (const u of ['file:///etc/passwd', 'FILE:///home/u/.ssh/id_rsa', 'javascript:alert(1)', 'JaVaScRiPt:x', 'data:text/html,x', 'mailto:a@b.c',
  'vbscript:x', 'lord:apps/launch', 'smb://host/share', 'http:/x', '//evil.example/x.png', '/etc/passwd', '/home/u/.ssh/id_rsa', '~/x',
  '../x', '..', '.', 'a/b.txt', '..%2F..%2Fetc%2Fpasswd', '%2Fetc%2Fpasswd', 'a\\b', 'x%00.png', 'a\u0000b', 'a\nb', '', '   ', 'x'.repeat(2000)]) {
  eq(`link blocked: ${JSON.stringify(u).slice(0, 40)}`, kind(u), 'blocked');
}
eq('link: a bad escape is read as typed', L.linkTarget('100%.txt'), { kind: 'file', name: '100%.txt' });
// ---- attachment names ----
eq('name: slashes', L.safeAttachName('a/b\\c.txt'), 'a_b_c.txt');
eq('name: dot-dot', L.safeAttachName('..'), 'attachment');
eq('name: control characters', L.safeAttachName('a\nb\u0007.txt'), 'a_b_.txt');
eq('name: direction override dropped', L.safeAttachName('photo‮gpj.exe'), 'photogpj.exe');
eq('name: ~ at the start', L.safeAttachName('~x'), '_x');
eq('name: long, extension kept', (() => { const s = L.safeAttachName(`${'n'.repeat(300)}.pdf`); return [s.length, s.endsWith('.pdf')]; })(), [200, true]);
eq('ref: spaces and parentheses encoded', L.attachmentRef('my file (1).pdf'), 'my%20file%20%281%29.pdf');
eq('ref → link round trip', L.linkTarget(L.attachmentRef('a b(c)%[d].png')).name, 'a b(c)%[d].png');
eq('label: brackets', L.linkLabel('a[b]'), 'a(b)');
eq('image kinds', ['a.PNG', 'b.jpeg', 'c.webp', 'd.heic', 'e.avif', 'f.gif', 'g.txt'].map(L.imageKind), ['inline', 'inline', 'photo', 'photo', 'photo', 'photo', null]);
eq('textual', ['a.md', 'b.TXT', 'c.pdf', 'd.png'].map(L.textual), [true, true, false, false]);

// ---- notes on disk ----
S.loadNotes();
eq('the Notes folder is made', fs.existsSync(path.join(home, 'Notes')), true);
const slug = S.createNote({ body: '# First\n\nhello' });
eq('a new note', !!slug && fs.existsSync(path.join(home, 'Notes', slug, 'nota.md')), true);
const nd = path.join(home, 'Notes', slug);
// attachments
fs.writeFileSync(path.join(home, 'my report (v2).pdf'), 'pdf');
fs.writeFileSync(path.join(home, 'shot.png'), 'png');
const mdPdf = S.attachFile(slug, path.join(home, 'my report (v2).pdf'));
eq('attach: a document is a 📎 link, its name encoded', mdPdf, '[📎 my report (v2).pdf](my%20report%20%28v2%29.pdf)');
eq('attach: copied into the note', fs.existsSync(path.join(nd, 'my report (v2).pdf')), true);
eq('attach: an image is drawn', S.attachFile(slug, path.join(home, 'shot.png')), '![shot.png](shot.png)');
eq('attach: the same name again gets -2', S.attachFile(slug, path.join(home, 'shot.png')), '![shot-2.png](shot-2.png)');
fs.writeFileSync(path.join(home, 'nota.md'), 'x');
eq('attach: a file named nota.md never replaces the note', S.attachFile(slug, path.join(home, 'nota.md')), '[📎 nota-2.md](nota-2.md)');
fs.symlinkSync(path.join(outside, 'secret.txt'), path.join(home, 'link.txt'));
eq('attach: a link is refused', S.attachFile(slug, path.join(home, 'link.txt')), null);
fs.mkdirSync(path.join(home, 'Folder'));
eq('attach: a folder is refused', S.attachFile(slug, path.join(home, 'Folder')), null);
fs.writeFileSync(path.join(home, 'odd‮gnp.exe'), 'x');
eq('attach: a direction override is dropped from the name', S.attachFile(slug, path.join(home, 'odd‮gnp.exe')), '[📎 oddgnp.exe](oddgnp.exe)');
// a huge file: attached by a copy (lord.fs.copy runs in the background in lord-server), never read into memory here
{
  const big = path.join(home, 'huge.bin');
  const fd = fs.openSync(big, 'w'); fs.ftruncateSync(fd, 3 * 1024 * 1024 * 1024); fs.closeSync(fd);
  const t0 = Date.now();
  const md = S.attachFile(slug, big);
  eq('attach: a 3 GB file is a link, at once', [md, Date.now() - t0 < 5000], ['[📎 huge.bin](huge.bin)', true]);
  fs.rmSync(path.join(nd, 'huge.bin')); fs.rmSync(big);
}
// what a link opens
eq('open: an attachment', S.attachmentPath(slug, 'my%20report%20%28v2%29.pdf')?.path, path.join(nd, 'my report (v2).pdf'));
eq('open: nota.md itself is not opened', S.attachmentPath(slug, 'nota.md'), null);
eq('open: a missing file', S.attachmentPath(slug, 'nope.pdf'), null);
eq('open: a way out', S.attachmentPath(slug, '../../outside/secret.txt'), null);
eq('open: an absolute path', S.attachmentPath(slug, path.join(outside, 'secret.txt')), null);
eq('open: file://', S.attachmentPath(slug, `file://${path.join(outside, 'secret.txt')}`), null);
fs.symlinkSync(path.join(outside, 'secret.txt'), path.join(nd, 'planted.txt'));
fs.symlinkSync(path.join(outside, 'pic.png'), path.join(nd, 'planted.png'));
eq('open: a link inside the note pointing out', S.attachmentPath(slug, 'planted.txt'), null);
eq('image: a link inside the note pointing out is not drawn', S.noteImageSrc(slug, 'planted.png'), null);
eq('image: the note\'s own PNG', S.noteImageSrc(slug, 'shot.png'), path.join(nd, 'shot.png'));
eq('image: remote', S.noteImageSrc(slug, 'https://example.com/a.png'), null);
eq('image: absolute', S.noteImageSrc(slug, path.join(outside, 'pic.png')), null);
eq('image: a PDF is not drawn', S.noteImageSrc(slug, 'my%20report%20%28v2%29.pdf'), null);
fs.mkdirSync(path.join(nd, 'sub.png'));
eq('image: a folder named .png is not drawn', S.noteImageSrc(slug, 'sub.png'), null);
eq('duplicate: links and folders are not copied', (() => {
  const c = S.duplicateNote(slug);
  const names = fs.readdirSync(path.join(home, 'Notes', c)).sort();
  return [names.includes('planted.txt'), names.includes('sub.png'), names.includes('shot.png'), names.includes('nota.md')];
})(), [false, false, true, true]);

// ---- loading what is on disk ----
// a note folder that is a link (to a folder elsewhere) and a nota.md that is a link are not read
fs.mkdirSync(path.join(outside, 'elsewhere'));
fs.writeFileSync(path.join(outside, 'elsewhere', 'nota.md'), '---\ntitle: linked\n---\nfrom outside');
fs.symlinkSync(path.join(outside, 'elsewhere'), path.join(home, 'Notes', 'linked-folder'));
fs.mkdirSync(path.join(home, 'Notes', 'linked-file'));
fs.symlinkSync(path.join(outside, 'secret.txt'), path.join(home, 'Notes', 'linked-file', 'nota.md'));
// a header edited by hand with odd values
fs.mkdirSync(path.join(home, 'Notes', 'odd'));
fs.writeFileSync(path.join(home, 'Notes', 'odd', 'nota.md'), '---\ntitle: "a‮b"\ncolor: "red; x"\nw: 99999999\nh: -5\nx: NaN\ndesktop: "yes"\ncreated: never\n---\nbody');
// a nota.md too large to be a note
fs.mkdirSync(path.join(home, 'Notes', 'big'));
{ const fd = fs.openSync(path.join(home, 'Notes', 'big', 'nota.md'), 'w'); fs.ftruncateSync(fd, 9 * 1024 * 1024); fs.closeSync(fd); }
S.loadNotes();
const list = S.notesStore.get().list;
const by = (s) => list.find((n) => n.slug === s);
eq('load: a linked note folder is skipped', !!by('linked-folder'), false);
eq('load: a linked nota.md is skipped', !!by('linked-file'), false);
eq('load: a 9 MB nota.md is skipped', !!by('big'), false);
const odd = by('odd');
eq('load: odd header made sane', odd && [odd.title, odd.color, odd.w, odd.h, odd.x, odd.desktop, Number.isNaN(Date.parse(odd.created))], ['ab', 'yellow', 4000, 110, 80, false, false]);
eq('load: the real notes are there', !!by(slug), true);

// ---- every write stays under ~/Notes ----
S.updateNote(slug, { body: '# First\n\nchanged' });
S.flushAll();
S.createNote({ body: '../../escape', desktop: true });
await wait(700);
const notesDir = path.resolve(path.join(home, 'Notes'));
const outsideWrites = S.writes.filter((w) => w !== notesDir && !w.startsWith(`${notesDir}/`));
eq('every write is inside ~/Notes', outsideWrites, []);
eq('the outside folder is untouched', fs.readdirSync(outside).sort(), ['elsewhere', 'pic.png', 'secret.txt']);
eq('desktop notes are published as widgets', S.widgets.some((w) => w.type === 'note'), true);

fs.rmSync(tmp, { recursive: true, force: true });
console.log(`notes-store-test: ${ok} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
