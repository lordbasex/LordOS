// clipboard_snippets_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): the full clipboard panel
// (❖⇧V, sdk/clipboard.js) with its Snippets tab (GitHub #205), rendered through runtime/index.js over the stub `lord`
// and driven with the server's own events: the saved snippets show, the search filters them, Enter pastes one the way a
// history entry is pasted (and it stays out of the history), From clipboard and Save as snippet open the editor, a save
// is written with lord.clipboard.saveStore, Alt+↓ reorders, Del asks once more, a password on the clipboard is never
// kept, and Escape closes the editor before the panel. Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import { files, saved, clipState } from './clipboard_snippets_scene_stub.js';
import React from 'react';
import * as rt from '@lordos/runtime';
import { ClipboardPanel, toggleClipboardPanel, clipboardPanel, clipboardSnippets, clipboardHistory, toast } from '../../sdk/clipboard.js';
import { enableShellLayers } from '../../sdk/layers.js';

const { parents, texts, removed, clip } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 30; i++) await new Promise((res) => setImmediate(res)); };
const wait = (ms) => new Promise((res) => setTimeout(res, ms));
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return parents.has(id); };
const textId = (s) => [...texts].find(([id, x]) => x === s && inTree(id))?.[0];
const shows = (s) => !!textId(s);
const click = async (s, button = 0) => {
  const id = textId(s);
  if (!id) return false;
  const p = parents.get(id) ?? id;            // the Text's node: its string has no instance of its own
  __lordEvent('down', p, 5, 5, button); __lordEvent('up', p, 5, 5, button);
  await flush();
  return true;
};
// a key as lord-server sends it with libxkbcommon: flags 2 Ctrl, 4 Alt
const key = async (name, chr = '', flags = 0) => { __lordKey(0, 1, name, chr, (chr || name).toLowerCase(), flags); __lordKey(0, 0, name, '', '', flags); await flush(); };
const type = async (s) => { for (const ch of s) await key(ch, ch); };
const savedSnippets = () => JSON.parse(saved['snippets.json'] || '{"items":[]}').items;

enableShellLayers();
rt.render(h(ClipboardPanel));
await wait(5);
toggleClipboardPanel('full');
await flush();
check('the full panel has two tabs', shows('History') && shows('Snippets'));
check('it opens on the history', shows('a line copied earlier'));

await click('Snippets');
check('the saved snippets show, by name', shows('Greeting') && shows('Address'));
check('the first is selected: its text in the preview', shows('Hello, how can we help?'));
check('the snippets\' own hint', shows('Enter or click: paste · Alt+↑ ↓: reorder · Ctrl+Tab: switch tabs · Saved in your home folder, readable only by you.'));

await type('spring');
check('the search filters by text', shows('Address') && !shows('Greeting'));
check('and the tabs say how many match', shows('Snippets · 1') && shows('History · 0'));
for (let i = 0; i < 6; i++) await key('Backspace');
check('an empty search shows them all', shows('Greeting') && shows('Address'));

await key('ArrowDown');
check('↓ selects the next', shows('Main Street 1\nSpringfield'));
const before = clipboardHistory.get().length;
await key('Enter');
await wait(5);
await flush();
check('Enter pastes it: its text is on the clipboard', clip.text === 'Main Street 1\nSpringfield', JSON.stringify(clip.text));
check('the panel closed', clipboardPanel.get() === false);
check('and the history did not get it', clipboardHistory.get().length === before);

// From clipboard: the editor with the text and a name from its first line; Enter in the name saves it
toggleClipboardPanel('full');
await flush();
check('it opens again on Snippets', shows('Greeting'));
clip.text = 'Kind regards,\nThe team';
await click('+ From clipboard');
check('From clipboard opens the editor', shows('New snippet') && shows('Name') && shows('Kind regards,'));
await key('Enter');
await wait(400);
await flush();
check('saved: first in the list and in the file', clipboardSnippets.get()[0]?.text === 'Kind regards,\nThe team' && savedSnippets()[0]?.text === 'Kind regards,\nThe team',
  JSON.stringify(savedSnippets().map((s) => s.name)));
check('the editor closed and the list is back', !shows('New snippet') && shows('Greeting'));

// Alt+↓ moves the selected one down; the order is saved
await key('ArrowDown', '', 4);
await wait(400);
await flush();
check('Alt+↓ reorders', clipboardSnippets.get().map((s) => s.id).join() === `a,${savedSnippets()[1]?.id},b` && savedSnippets()[0]?.id === 'a',
  JSON.stringify(clipboardSnippets.get().map((s) => s.name)));

// Del asks once more, then deletes
await key('Delete');
check('Del asks first', shows('Delete this snippet?') && clipboardSnippets.get().length === 3);
await key('Delete');
await wait(400);
await flush();
check('a second Del deletes it', clipboardSnippets.get().length === 2 && savedSnippets().length === 2 && !savedSnippets().some((s) => s.name === 'Kind regards,'));

// a password on the clipboard is never kept
clipState.sensitive = true;
clip.text = 'hunter2';
await click('+ From clipboard');
check('a password is refused', !shows('New snippet') && toast.get() === 'What is on the clipboard is a password: it is never kept as a snippet.', toast.get());
clipState.sensitive = false;

// Save as snippet from a history entry; Escape closes the editor first, the panel next
await key('Tab', '', 2);
check('Ctrl+Tab goes back to the history', shows('a line copied earlier') && shows('Save as snippet'));
await click('Save as snippet');
check('Save as snippet opens the editor on the Snippets tab', shows('New snippet') && shows('Search snippets'));
await key('Escape');
check('Escape closes the editor and keeps the panel', !shows('New snippet') && clipboardPanel.get() === 'full');
await key('Escape');
check('a second Escape closes the panel', clipboardPanel.get() === false);
check('nothing else was written with lord.fs', ![...files.keys()].some((p) => p.endsWith('snippets.json') && files.get(p).includes('Kind regards')));
process.exit(0);
