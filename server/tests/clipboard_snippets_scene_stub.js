// clipboard_snippets_scene_stub.js — imported first by clipboard_snippets_scene.mjs: a home folder in memory with a
// history and two snippets saved, lord.clipboard.saveStore keeping what it is given, and the clipboard's state.
export const files = new Map([
  ['.local/share/lordos/clipboard/snippets.json', JSON.stringify({ version: 1, items: [
    { id: 'a', name: 'Greeting', text: 'Hello, how can we help?', created: Date.now(), updated: Date.now() },
    { id: 'b', name: 'Address', text: 'Main Street 1\nSpringfield', created: Date.now(), updated: Date.now() },
  ] })],
  ['.local/share/lordos/clipboard/history.json', JSON.stringify({ version: 1, items: [
    { id: 'h1', kind: 'text', text: 'a line copied earlier', origin: 'lordos', time: Date.now(), pinned: false, type: 'text' },
  ] })],
]);
export const saved = {};
export const clipState = { kind: 'text', sensitive: false };
lord.fs = {
  read: (p) => { if (!files.has(p)) throw new Error('ENOENT'); return files.get(p); },
  write: (p, d) => { files.set(p, d); return true; },
  list: () => [], mkdir: () => true, home: () => '/home/test',
};
lord.clipboard.saveStore = (name, text) => { saved[name] = text; return true; };
lord.clipboard.state = () => ({ ...clipState, origin: 'lordos' });
