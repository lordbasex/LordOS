// notes_breaks_scene.mjs — the scene of tests/textfield_menu_test.mjs notes_breaks_scene.mjs (GitHub #169; not run by
// itself): Notes' reading view breaks a paragraph's lines where the editor does. sdk/markdown.jsx with `breaks` keeps a
// single line break inside a paragraph (one row per typed line); without it, plain markdown joins the lines, as Help
// and Mail expect. Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { Markdown, parseMarkdown } from '../../sdk/markdown.jsx';

const { parents, texts, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

// the note of the issue: a title, a paragraph typed on two lines, a blank line, a paragraph typed on three lines
const NOTE = '# New note\n\nFiles : montage partisions\nmode double pane\n\nCase #124 :\nSome questions :\n1/ On this picture\n\n> quoted line one\n> quoted line two\n';

// the parser
const plain = parseMarkdown(NOTE), kept = parseMarkdown(NOTE, { breaks: true });
const paras = (bs) => bs.filter((b) => b.type === 'paragraph');
check('plain markdown joins the lines of a paragraph', paras(plain)[0].text === 'Files : montage partisions mode double pane' && !paras(plain)[0].lines,
  JSON.stringify(paras(plain)[0]));
check('breaks: the paragraph keeps its typed lines', JSON.stringify(paras(kept)[0].lines) === JSON.stringify(['Files : montage partisions', 'mode double pane']),
  JSON.stringify(paras(kept)[0]));
check('breaks: three lines stay three', paras(kept)[1].lines?.length === 3, JSON.stringify(paras(kept)[1]));
check('breaks: "#124" inside a line is no heading', kept.filter((b) => b.type === 'heading').length === 1);
check('breaks: a quote keeps its lines too', kept.find((b) => b.type === 'quote')?.blocks[0]?.lines?.length === 2);
check('breaks: a one-line paragraph is unchanged', !parseMarkdown('one line', { breaks: true })[0].lines);
check('breaks: the blank line still separates paragraphs', paras(kept).length === 2);

// the drawing: each word is a <Text> in its line's wrapping row
const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
// a word: its raw text node, in its <Text>, in its line's row
const rowOf = (word) => { const id = [...texts].find(([n, s]) => s.trim() === word && inTree(n))?.[0]; return id ? parents.get(parents.get(id)) : null; };
let breaks = false;
let setBreaks = null;
function Scene() {
  const [b, set] = React.useState(breaks);
  setBreaks = set;
  return h(rt.View, { style: { width: 600, height: 400 } }, h(Markdown, { source: NOTE, breaks: b, width: 560 }));
}
rt.render(h(Scene));
await flush();
check('plain: "partisions" and "mode" share a row', rowOf('partisions') && rowOf('partisions') === rowOf('mode'));
setBreaks(true);
await flush();
check('breaks: "mode" starts its own row', rowOf('partisions') && rowOf('mode') && rowOf('partisions') !== rowOf('mode'));
check('breaks: words of one line share a row', rowOf('montage') === rowOf('partisions') && rowOf('mode') === rowOf('double'));
check('breaks: "Some" starts a row after "Case"', rowOf('Case') && rowOf('Some') && rowOf('Case') !== rowOf('Some'));
