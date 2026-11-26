// terminal_skin_scene_lord.js — imported first by tests/terminal_skin_scene.mjs: lord.term over menubar_test.mjs's stub lord,
// with sessions that only answer info() and record every write, key and paste, so the scene can tell nothing a skin does
// reaches a session.
export const termLog = [];
let next = 1;
const live = new Set();
const api = {
  create: () => { const id = next++; live.add(id); return id; },
  list: () => [...live],
  close: (id) => { live.delete(id); termLog.push(`close ${id}`); },
  info: (id) => (live.has(id) ? { process: 'bash', busy: false, shell: 'bash', cwd: '/home/test', cols: 80, rows: 24, hasSelection: false } : null),
  write: (id, s) => { termLog.push(`write ${id} ${s}`); },
  key: (id, k, c) => { termLog.push(`key ${id} ${k}${c}`); },
  paste: (id, s) => { termLog.push(`paste ${id} ${s}`); },
};
globalThis.lord.term = new Proxy(api, { get: (o, k) => (k in o ? o[k] : () => undefined) });
globalThis.lord.getLayout = () => [0, 0, 900, 600];
const fsBase = globalThis.lord.fs;
globalThis.lord.fs = new Proxy({ home: () => '/home/test' }, { get: (o, k) => (k in o ? o[k] : (fsBase && fsBase[k]) || (() => undefined)) });
