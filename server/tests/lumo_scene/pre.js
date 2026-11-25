// pre.js — goes before the theme bundle in tests/app_scene_headless.sh lumo_scene (a test tool, never shipped): keeps
// which text and which picture each node shows and where it hangs, so driver.js can find them.
(function () {
  const texts = new Map(), parent = new Map(), images = new Map();
  const wrap = (name, fn) => { const real = lord[name]; lord[name] = (...a) => { fn(...a); return real(...a); }; };
  wrap('setText', (id, s) => texts.set(id, String(s)));
  wrap('setProp', (id, k, v) => { if (k === 'src') { if (v) images.set(id, String(v)); else images.delete(id); } });
  wrap('appendChild', (p, c) => parent.set(c, p));
  wrap('insertBefore', (p, c) => parent.set(c, p));
  wrap('destroy', (id) => { texts.delete(id); parent.delete(id); images.delete(id); });
  // About shows the computer's and the person's names: the screenshots go in the brand kit, so neutral ones
  if (lord.system && lord.system.info) {
    const info = lord.system.info.bind(lord.system);
    lord.system.info = (...a) => { const r = info(...a); return r && typeof r === 'object' ? { ...r, hostname: 'lordos', user: 'lumo' } : r; };
  }
  globalThis.__appScene = { texts, parent, images };
})();
