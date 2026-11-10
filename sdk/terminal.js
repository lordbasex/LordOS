// terminal.js — terminal sessions for apps (lord.term) and the session clipboard.
// Emulation and drawing are in C; here sessions are only created, configured and
// their events listened to: 'title', 'cwd', 'bell', 'badge', 'notify', 'profile', 'attention',
// 'copy', 'command' (exit code), 'output' (activity, ~1/s) and 'exit'.
import { createStore } from './store.js';

const subs = new Map();
const anySubs = new Set();

globalThis.__lordTerm = (id, type, data) => {
  for (const fn of [...(subs.get(id) || [])]) { try { fn(type, data); } catch (e) { console.error('term:', e); } }
  for (const fn of [...anySubs]) { try { fn(id, type, data); } catch (e) { console.error('term:', e); } }
};

const pass = (name) => (...a) => lord.term[name](...a);

export const term = {
  create: (opts = {}) => lord.term.create(opts),
  list: () => lord.term.list(),
  close(id) { lord.term.close(id); subs.delete(id); },
  on(id, fn) {
    if (!subs.has(id)) subs.set(id, new Set());
    subs.get(id).add(fn);
    return () => subs.get(id)?.delete(fn);
  },
  onAny(fn) { anySubs.add(fn); return () => anySubs.delete(fn); },
  configure: pass('configure'), write: pass('write'), key: pass('key'), paste: pass('paste'),
  mouse: pass('mouse'), wheel: pass('wheel'), focus: pass('focus'), scroll: pass('scroll'),
  select: pass('select'), text: pass('text'), copy: pass('copy'), clear: pass('clear'),
  search: pass('search'), searchMove: pass('searchMove'), mark: pass('mark'), urlAt: pass('urlAt'), lineAt: pass('lineAt'),
  words: pass('words'), cursorWord: pass('cursorWord'), info: pass('info'),
  logicalText: pass('logicalText'), setMatches: pass('setMatches'), copyMode: pass('copyMode'), copyKey: pass('copyKey'),
  replay: pass('replay'), log: pass('log'), export: pass('export'), highlight: pass('highlight'),
  tmuxPane: pass('tmuxPane'), tmuxCommand: pass('tmuxCommand'), feed: pass('feed'), tmuxReady: pass('tmuxReady'), ackRestored: pass('ackRestored'),
};

export { clipboard, clipboardHistory } from './clipboard.js';
