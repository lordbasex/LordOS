// pre.js — goes before the theme bundle in tests/app_scene_headless.sh about_labels_scene (a test tool, never shipped):
// keeps which text each node shows and where it hangs, so driver.js can find About's labels.
(function () {
  const texts = new Map(), parent = new Map();
  const wrap = (name, fn) => { const real = lord[name]; lord[name] = (...a) => { fn(...a); return real(...a); }; };
  wrap('setText', (id, s) => texts.set(id, String(s)));
  wrap('appendChild', (p, c) => parent.set(c, p));
  wrap('insertBefore', (p, c) => parent.set(c, p));
  wrap('destroy', (id) => { texts.delete(id); parent.delete(id); });
  // neutral names in the screenshots
  if (lord.system && lord.system.info) {
    const info = lord.system.info.bind(lord.system);
    lord.system.info = (...a) => { const r = info(...a); return r && typeof r === 'object' ? { ...r, hostname: 'lordos', user: 'lumo' } : r; };
  }
  globalThis.__appScene = { texts, parent };
})();
