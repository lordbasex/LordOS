// pre.js — goes before the theme bundle in tests/app_scene_headless.sh screenshots_menu_scene (a test tool, never shipped): keeps
// which text each node shows and where it hangs, so driver.js can find and click them, and each node's opacity (the popover is drawn invisible until placed).
(function () {
  const texts = new Map(), parent = new Map(), opacity = new Map();
  const wrap = (name, fn) => { const real = lord[name]; lord[name] = (...a) => { fn(...a); return real(...a); }; };
  wrap('setText', (id, s) => texts.set(id, String(s)));
  wrap('appendChild', (p, c) => parent.set(c, p));
  wrap('insertBefore', (p, c) => parent.set(c, p));
  wrap('setStyle', (id, k, v) => { if (k === 'opacity') opacity.set(id, v); });
  wrap('destroy', (id) => { texts.delete(id); parent.delete(id); });
  globalThis.__appScene = { texts, parent, opacity };
})();
