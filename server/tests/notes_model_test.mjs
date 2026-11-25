// notes_model_test.mjs — Notes' pure logic (apps/notes/notesmodel.js): the header round trip and the title's cleaning
// (GitHub #238), the merge of a fresh read of ~/Notes with the notes in memory (made, edited and deleted elsewhere, a
// change not saved yet, a save that failed), the widget catalog's "pick a note" list (search, order, on the desktop),
// where "Show on desktop" puts a note whose place is off every display, and what an imported .md/.txt becomes.
//
//   node tests/notes_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const M = await import(path.join(here, '../../apps/notes/notesmodel.js'));
const { parseNote, serialize, titleOf, noteFrom, mergeScan, pickList, visibleOn, clampInto, placeOnDesktop, importNote, importable, MAX_NOTE } = M;

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`FAIL ${name}\n  got  ${g}\n  want ${w}`); }
};

// ---- the header round trip: values that would read back as something else ----
for (const title of ['a: b', 'say "hi"', "it's", 'true', 'false', '123', '-4.5', ' leading', 'trailing ', '#tag', '']) {
  const note = { title, color: 'blue', x: 1, y: 2, w: 300, h: 200, desktop: true, pinned: false, created: '2026-11-01T00:00:00.000Z', updated: '2026-11-02T00:00:00.000Z', body: 'x' };
  eq(`round trip: title ${JSON.stringify(title)}`, parseNote(serialize(note)).meta.title, title);
}
eq('round trip: a line break in a title becomes a space', parseNote(serialize({ title: 'one\ntwo', body: '' })).meta.title, 'one two');
eq('round trip: the body after the header, whole', parseNote(serialize({ title: 't', body: '# A\n---\nb\n' })).body, '# A\n---\nb\n');
eq('round trip: numbers and booleans keep their type', (({ x, desktop }) => [x, desktop])(parseNote(serialize({ x: 12, desktop: true, body: '' })).meta), [12, true]);

// ---- titles: no control characters, no direction overrides (#238) ----
eq('title: markdown marks out', titleOf('## **Groceries** `list`'), 'Groceries list');
eq('title: direction overrides and isolates removed', titleOf('a\u202eb\u202ac\u2066d\u2069e'), 'abcde');
eq('title: control characters and line separators become spaces', titleOf('a\u0007b\u2028c\u2029d\u0085e').replace(/ +/g, ' '), 'a b c d e');
eq('title: at most 60 characters', titleOf('x'.repeat(100)).length, 60);
eq('a note from its header: a bidi title is cleaned', noteFrom('s', '---\ntitle: "\u202eevil"\n---\nbody', 0).title, 'evil');

// ---- the merge of a fresh read with the notes in memory ----
const mk = (slug, extra = {}) => ({ slug, body: `# ${slug}`, title: slug, color: 'yellow', x: 80, y: 80, w: 260, h: 240, desktop: false, pinned: false,
  created: '2026-11-01T00:00:00.000Z', updated: '2026-11-01T00:00:00.000Z', ...extra });
{
  const a = mk('a'), b = mk('b');
  const mem = [a, b];
  const same = mergeScan(mem, [mk('a'), mk('b')]);
  eq('nothing changed: the same list (no redraw)', [same.changed, same.list === mem], [false, true]);
  const added = mergeScan(mem, [mk('a'), mk('b'), mk('c', { updated: '2026-11-03T00:00:00.000Z', desktop: true }), mk('d', { updated: '2026-11-02T00:00:00.000Z' })]);
  eq('notes made elsewhere come first, newest first; the others keep their order', added.list.map((x) => x.slug), ['c', 'd', 'a', 'b']);
  eq('a note made elsewhere with desktop: true keeps it', added.list[0].desktop, true);
  eq('a note that did not change keeps its object', added.list[2] === a, true);
  const gone = mergeScan(mem, [mk('b')]);
  eq('a folder deleted elsewhere goes', gone.list.map((x) => x.slug), ['b']);
  const edited = mergeScan(mem, [mk('a', { body: 'edited elsewhere' }), mk('b')]);
  eq('an edit made elsewhere comes in', [edited.changed, edited.list[0].body], [true, 'edited elsewhere']);
  const typing = { ...a, body: 'typed, not saved yet' };
  const kept = mergeScan([typing, b], [mk('a', { body: 'old on disk' }), mk('b')], new Set(['a']));
  eq('a change not saved yet wins over the disk', kept.list[0].body, 'typed, not saved yet');
  eq('… and is the same object', kept.list[0] === typing, true);
  const failed = mergeScan([typing, b], [mk('b')], new Set(['a']));
  eq('a note whose save failed stays even when its file is missing', failed.list.map((x) => x.slug), ['a', 'b']);
  eq('an empty folder read changes nothing when the list is empty', mergeScan([], []).changed, false);
}

// ---- the "pick a note" list ----
{
  const list = [
    mk('old', { title: 'Groceries', body: 'yerba mate', updated: '2026-10-01T00:00:00.000Z' }),
    mk('new', { title: 'Café ideas', body: 'espresso', updated: '2026-11-20T00:00:00.000Z', desktop: true, color: 'pink' }),
    mk('mid', { title: 'Trip', body: 'Montevideo café', updated: '2026-11-10T00:00:00.000Z' }),
  ];
  eq('most recently updated first', pickList(list).map((x) => x.slug), ['new', 'mid', 'old']);
  eq('the ones on the desktop are marked', pickList(list).map((x) => x.desktop), [true, false, false]);
  eq('search: title or text, without accents or case', pickList(list, 'CAFE').map((x) => x.slug), ['new', 'mid']);
  eq('search: every word', pickList(list, 'cafe monte').map((x) => x.slug), ['mid']);
  eq('search: nothing matches', pickList(list, 'zzz'), []);
  eq('a row carries title, colour and date', pickList(list)[0], { slug: 'new', title: 'Café ideas', color: 'pink', updated: '2026-11-20T00:00:00.000Z', desktop: true });
  eq('at most max rows', pickList(list, '', 2).length, 2);
}

// ---- where a note goes on the desktop ----
{
  const displays = [{ x: 0, y: 0, w: 1920, h: 1080 }, { x: 1920, y: 0, w: 1280, h: 1024 }];
  const area = { x: 1920, y: 28, w: 1280, h: 996 };
  eq('visible on the first display', visibleOn({ x: 100, y: 100, w: 260, h: 240 }, displays), true);
  eq('visible on the second display', visibleOn({ x: 2500, y: 500, w: 260, h: 240 }, displays), true);
  eq('off every display (a monitor gone)', visibleOn({ x: 3400, y: 100, w: 260, h: 240 }, displays), false);
  eq('only a sliver of its bar on a display is not visible', visibleOn({ x: 3170, y: 100, w: 260, h: 240 }, displays), false);
  eq('below every display', visibleOn({ x: 100, y: 1100, w: 260, h: 240 }, displays), false);
  eq('Show on desktop: a visible note keeps its place', placeOnDesktop({ x: 100, y: 100, w: 260, h: 240 }, displays, area), { x: 100, y: 100, w: 260, h: 240 });
  eq('Show on desktop: a note off every display comes to the window\'s display', placeOnDesktop({ x: 5000, y: 3000, w: 260, h: 240 }, displays, area), { x: 2000, y: 68, w: 260, h: 240 });
  eq('a note larger than the area shrinks to it', placeOnDesktop({ x: 5000, y: 0, w: 4000, h: 4000 }, [], { x: 0, y: 0, w: 800, h: 600 }), { x: 0, y: 0, w: 800, h: 600 });
  eq('the pick: a place near the right edge is moved in', clampInto({ x: 3150, y: 900, w: 260, h: 240 }, area), { x: 2940, y: 784, w: 260, h: 240 });
  eq('the pick: a place inside stays', clampInto({ x: 2000, y: 100, w: 260, h: 240 }, area), { x: 2000, y: 100, w: 260, h: 240 });
}

// ---- import ----
{
  eq('importable: .md .markdown .txt, any case', ['a.md', 'b.MARKDOWN', 'c.Txt', 'd.pdf', 'e.md.exe', 'mdfile', ''].map(importable), [true, true, true, false, false, false, false]);
  eq('plain text: all of it is the note, title from its first line', importNote('# Plan\n- [ ] one\n', '/home/u/plan.md'), { body: '# Plan\n- [ ] one\n', title: 'Plan', color: 'yellow' });
  eq('a Notes header: its title and colour, nothing else', importNote('---\ntitle: Kept\ncolor: green\nx: 9999\ndesktop: true\ncreated: 2020-01-01\n---\ntext', 'n.md'),
    { body: 'text', title: 'Kept', color: 'green' });
  eq('a header colour that is not a Notes colour: yellow', importNote('---\ncolor: "red; x"\n---\nt', 'n.md').color, 'yellow');
  eq('another program\'s front matter stays in the text', importNote('---\nlayout: post\ntitle: Blog\n---\nhello', 'p.md').body, '---\nlayout: post\ntitle: Blog\n---\nhello');
  eq('an empty file: the file name is the title', importNote('', '/x/My list.txt'), { body: '', title: 'My list', color: 'yellow' });
  eq('a bidi override in the header title is cleaned', importNote('---\ntitle: "a\u202eb"\n---\n', 'n.md').title, 'ab');
  eq('a bidi override in the file name is cleaned', importNote('', '/x/\u202eevil.md').title, 'evil');
  eq('CRLF and a BOM are read as plain lines', importNote('\ufeff# Win\r\nline\r\n', 'w.txt'), { body: '# Win\nline\n', title: 'Win', color: 'yellow' });
  eq('a NUL byte: not a text file', importNote('a\u0000b', 'bin.txt'), null);
  eq('larger than MAX_NOTE: refused', importNote('x'.repeat(MAX_NOTE + 1), 'big.md'), null);
  eq('not a string: refused', importNote(null, 'n.md'), null);
  eq('a path in the text is only text (nothing is followed)', importNote('![x](/etc/passwd)\n[a](../../secret)', 'n.md').body, '![x](/etc/passwd)\n[a](../../secret)');
}

console.log(`notes-model-test: ${n - bad} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
