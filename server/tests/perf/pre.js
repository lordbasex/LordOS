// pre.js — goes before the theme bundle in perf_headless.sh: it keeps which text each node shows and where it hangs,
// so driver.js can click a control by its label (headless, no input devices).
(function () {
  const texts = new Map();      // node id → text (raw nodes)
  const parent = new Map();     // node id → parent node id
  const wrap = (name, fn) => { const real = lord[name]; lord[name] = (...a) => { fn(...a); return real(...a); }; };
  wrap('setText', (id, s) => texts.set(id, String(s)));
  wrap('appendChild', (p, c) => parent.set(c, p));
  wrap('insertBefore', (p, c) => parent.set(c, p));
  wrap('destroy', (id) => { texts.delete(id); parent.delete(id); });
  globalThis.__perf = { texts, parent };
})();
