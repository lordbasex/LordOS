// pre.js — goes before the theme bundle in tests/app_scene_headless.sh files_renamelayout_scene (a test tool, never shipped): keeps
// which text each node shows and where it hangs, so driver.js can find and click them.
(function () {
  const texts = new Map(), parent = new Map();
  const wrap = (name, fn) => { const real = lord[name]; lord[name] = (...a) => { fn(...a); return real(...a); }; };
  // lord-server hands a freed node's id out again, and lord.destroy frees a whole subtree while only its root is told
  // here: a new node starts with nothing known about it (else a stale name, under a node now in another window, wins)
  { const real = lord.createNode; lord.createNode = (...a) => { const id = real(...a); texts.delete(id); parent.delete(id); return id; }; }
  wrap('setText', (id, s) => texts.set(id, String(s)));
  wrap('appendChild', (p, c) => parent.set(c, p));
  wrap('insertBefore', (p, c) => parent.set(c, p));
  wrap('destroy', (id) => { texts.delete(id); parent.delete(id); });
  wrap('removeChild', (p, c) => { if (parent.get(c) === p) parent.delete(c); });
  globalThis.__appScene = { texts, parent };
})();
