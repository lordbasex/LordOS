// pre.js — goes before the theme bundle in tests/app_scene_headless.sh spell_scene (a test tool, never shipped): keeps
// which text each node shows, and stands in for lord.spell and lord.writing (docs/SPELLING.md "Screenshots") so the
// pictures don't depend on which dictionaries or models the machine has: a few known misspellings with their
// suggestions, and a "model" that streams a corrected sentence in pieces. lord.sentinela says a model is installed.
(function () {
  const texts = new Map(), parent = new Map();
  const wrap = (name, fn) => { const real = lord[name]; lord[name] = (...a) => { fn(...a); return real(...a); }; };
  wrap('setText', (id, s) => texts.set(id, String(s)));
  wrap('appendChild', (p, c) => parent.set(c, p));
  wrap('insertBefore', (p, c) => parent.set(c, p));
  wrap('destroy', (id) => { texts.delete(id); parent.delete(id); });
  const BAD = { recieve: ['receive', 'relieve', 'recede'], teh: ['the', 'tech', 'ten', 'eh'], wierd: ['weird', 'wired', 'wield'],
    untill: ['until', 'untie', 'unfill'], definately: ['definitely', 'defiantly'] };
  const results = new Map(), asked = [];
  let n = 0;
  const words = [];
  lord.spell = {
    installed: () => ['en_US', 'es_ES', 'pt_BR'], defaults: () => ['en_US'], setLanguages: (l) => l.length,
    state: () => ({ langs: ['en_US'], loaded: [1], state: 'ready', queued: 0 }),
    check: (ws) => { const id = ++n; asked.push(...ws); results.set(id, { ok: true, good: ws.map((w) => (BAD[String(w).toLowerCase()] ? 0 : 1)) }); return id; },
    suggest: (w) => { const id = ++n; results.set(id, { ok: true, suggestions: BAD[String(w).toLowerCase()] || [] }); return id; },
    take: (id) => { const r = results.get(id) || null; results.delete(id); return r; },
    words: () => words.slice(), addWord: (w) => { words.push(w); return true; }, removeWord: () => true,
  };
  const FIXED = ['I ', 'received ', 'the weird ', 'letter ', 'only today, ', 'and I definitely ', 'need to answer it.'];
  const jobs = new Map();
  let jn = 0;
  lord.writing = {
    start: (model, action, target, text) => { const id = ++jn; jobs.set(id, { i: 0, action, target, text, model }); globalThis.__writingAsked = { model, action, target, text }; return id; },
    peek: (id) => { const j = jobs.get(id); if (!j || j.i >= FIXED.length) return null; j.i++; return { state: 'writing', text: FIXED.slice(0, j.i).join('') }; },
    take: (id) => { const j = jobs.get(id); if (!j || j.i < FIXED.length) return null; jobs.delete(id); return { ok: true, content: FIXED.join(''), truncated: false }; },
    cancel: () => true,
  };
  if (lord.sentinela) { lord.sentinela.available = () => true; lord.sentinela.models = () => [{ id: 'qwen3-1.7b', bytes: 1280000000 }]; }
  globalThis.__appScene = { texts, parent, asked };
})();
