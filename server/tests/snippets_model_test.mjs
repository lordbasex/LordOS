// snippets_model_test.mjs — the clipboard's Snippets (sdk/snippetsmodel.js; GitHub #205): add, edit and rename, delete,
// reorder, search, the file read back, and the limits on count and size.
//
//   node tests/snippets_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const M = await import(path.join(here, '../../sdk/snippetsmodel.js'));
const { addSnippet, updateSnippet, removeSnippet, moveSnippet, matchSnippet, parseSnippets, serializeSnippets, nameFor, cleanText, cleanName,
  utf8Bytes, SNIPPET_MAX_COUNT, SNIPPET_MAX_BYTES, SNIPPET_MAX_NAME } = M;

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};
const ids = (list) => list.map((s) => s.id);

// ---- add ----
let list = [];
let r = addSnippet(list, { name: 'Customer response', text: 'Hello, thank you for contacting us.\nHow can we help you today?' }, { id: 'a', time: 10 });
eq('added', r.snippet, { id: 'a', name: 'Customer response', text: 'Hello, thank you for contacting us.\nHow can we help you today?', created: 10, updated: 10 });
list = r.list;
r = addSnippet(list, { text: '  \n  Regards,\r\nThe team  ' }, { id: 'b', time: 20 });
eq('no name: its first line, and \\r\\n kept as \\n', [r.snippet.name, r.snippet.text], ['Regards,', '  \n  Regards,\nThe team  ']);
list = r.list;
eq('new ones go first', ids(list), ['b', 'a']);
eq('no text', addSnippet(list, { name: 'x', text: ' \n\t ' }, { id: 'c' }).error, 'empty');
eq('the same text twice', [addSnippet(list, { name: 'again', text: list[1].text }, { id: 'c' }).error, addSnippet(list, { text: list[1].text }, { id: 'c' }).snippet.id], ['exists', 'a']);
eq('a long first line as the name', nameFor(`${'word '.repeat(20)}\nmore`), `${'word '.repeat(8).trimEnd()}…`);
eq('a name is one line, without controls, capped', cleanName(`  a\tb\n\u0007c${'x'.repeat(200)}`).length, SNIPPET_MAX_NAME);
eq('control characters are dropped, tabs and breaks kept', cleanText('a\u0000b\u001b[31mc\td\re\u007f'), 'ab[31mc\td\ne');

// ---- edit and rename ----
r = updateSnippet(list, 'a', { name: 'Greeting' }, 30);
eq('renamed only', [r.snippet.name, r.snippet.text === list[1].text, r.snippet.created, r.snippet.updated], ['Greeting', true, 10, 30]);
list = r.list;
r = updateSnippet(list, 'a', { name: '', text: 'Hi there' }, 40);
eq('edited, with the name left empty: from the text', [r.snippet.name, r.snippet.text], ['Hi there', 'Hi there']);
list = r.list;
eq('edited to empty', updateSnippet(list, 'a', { text: '' }).error, 'empty');
eq('edited to another snippet\'s text', updateSnippet(list, 'a', { text: list[0].text }).error, 'exists');
eq('kept its own text', updateSnippet(list, 'a', { name: 'Hi', text: 'Hi there' }).snippet.name, 'Hi');
eq('no such snippet', updateSnippet(list, 'zz', { name: 'q' }).error, 'missing');
eq('the order stays after an edit', ids(list), ['b', 'a']);

// ---- reorder and delete ----
list = addSnippet(list, { text: 'third' }, { id: 'c' }).list;
eq('three', ids(list), ['c', 'b', 'a']);
eq('down', ids(moveSnippet(list, 'c', 1)), ['b', 'c', 'a']);
eq('up', ids(moveSnippet(list, 'a', -1)), ['c', 'a', 'b']);
eq('not above the first', moveSnippet(list, 'c', -1), list);
eq('not below the last', moveSnippet(list, 'a', 1), list);
eq('unknown id', moveSnippet(list, 'q', 1), list);
eq('deleted', ids(removeSnippet(list, 'b')), ['c', 'a']);

// ---- search ----
eq('by name', list.filter((s) => matchSnippet(s, 'REGARDS')).map((s) => s.id), ['b']);
eq('by text', list.filter((s) => matchSnippet(s, 'team')).map((s) => s.id), ['b']);
eq('empty search: all', list.filter((s) => matchSnippet(s, '')).length, 3);
eq('nothing found', list.filter((s) => matchSnippet(s, 'nope')).length, 0);

// ---- the file ----
eq('round trip', parseSnippets(serializeSnippets(list)), list);
eq('only the known fields are written', Object.keys(JSON.parse(serializeSnippets([{ ...list[0], extra: 1 }])).items[0]), ['id', 'name', 'text', 'created', 'updated']);
eq('not JSON', parseSnippets('{oops'), []);
eq('no items', parseSnippets('{"version":1}'), []);
eq('bad entries are skipped, the rest repaired', parseSnippets(JSON.stringify({ items: [null, 5, { name: 'n' }, { text: '' }, { id: 'x y', text: 't1', created: -3 },
  { id: 'k', text: 't2', name: 'a\nb' }, { id: 'k', text: 't3' }, { id: 'm', text: 't2' }] })),
[{ id: 's0', name: 't1', text: 't1', created: 0, updated: 0 }, { id: 'k', name: 'a b', text: 't2', created: 0, updated: 0 }, { id: 's2', name: 't3', text: 't3', created: 0, updated: 0 }]);

// ---- limits ----
eq('bytes, not characters', [utf8Bytes('aé€😀'), utf8Bytes('')], [10, 0]);
const full = Array.from({ length: SNIPPET_MAX_COUNT }, (_, i) => ({ id: `i${i}`, name: `n${i}`, text: `t${i}`, created: 0, updated: 0 }));
eq('no more than the count', addSnippet(full, { text: 'one more' }, { id: 'z' }).error, 'tooMany');
eq('an existing text is still found at the count', addSnippet(full, { text: 't7' }, { id: 'z' }).error, 'exists');
eq('the most text a snippet may hold', addSnippet([], { text: 'x'.repeat(SNIPPET_MAX_BYTES) }, { id: 'z' }).snippet.text.length, SNIPPET_MAX_BYTES);
eq('one byte more', addSnippet([], { text: 'x'.repeat(SNIPPET_MAX_BYTES + 1) }, { id: 'z' }).error, 'tooLong');
eq('counted in UTF-8', addSnippet([], { text: '€'.repeat(Math.floor(SNIPPET_MAX_BYTES / 3) + 1) }, { id: 'z' }).error, 'tooLong');
eq('edited past the size', updateSnippet(list, 'a', { text: 'x'.repeat(SNIPPET_MAX_BYTES + 1) }).error, 'tooLong');
{
  const big = JSON.stringify({ items: [...full, { id: 'over', text: 'past the count' }, ...[0].map(() => ({ id: 'q', text: 'y' }))] });
  eq('a file with too many: cut at the count', parseSnippets(big).length, SNIPPET_MAX_COUNT);
  const huge = parseSnippets(JSON.stringify({ items: [{ id: 'h', text: `${'€'.repeat(SNIPPET_MAX_BYTES)}` }] }))[0].text;
  eq('a file with a text too long: cut on a character', [utf8Bytes(huge) <= SNIPPET_MAX_BYTES, utf8Bytes(huge) > SNIPPET_MAX_BYTES - 3, [...huge].every((c) => c === '€')], [true, true, true]);
  const worst = Array.from({ length: SNIPPET_MAX_COUNT }, (_, i) => ({ id: `w${i}`, name: '"'.repeat(SNIPPET_MAX_NAME), text: `${i}${'\n'.repeat(SNIPPET_MAX_BYTES - 4)}`, created: 0, updated: 0 }));
  eq('the file stays below 32 MB with every snippet full and escaped', Buffer.byteLength(serializeSnippets(worst)) < 32 * 1048576, true);
}

console.log(bad ? `snippets-model: ${bad} of ${n} failed` : `snippets-model: ${n} checks passed`);
process.exit(bad ? 1 : 0);
