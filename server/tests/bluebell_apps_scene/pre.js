// pre.js — goes before the theme bundle in tests/app_scene_headless.sh bluebell_apps_scene (a test tool, never shipped): keeps
// which text each node shows and where it hangs, so driver.js can find and click them.
(function () {
  const texts = new Map(), parent = new Map();
  const wrap = (name, fn) => { const real = lord[name]; lord[name] = (...a) => { fn(...a); return real(...a); }; };
  wrap('setText', (id, s) => texts.set(id, String(s)));
  wrap('appendChild', (p, c) => parent.set(c, p));
  wrap('insertBefore', (p, c) => parent.set(c, p));
  wrap('destroy', (id) => { texts.delete(id); parent.delete(id); });
  globalThis.__appScene = { texts, parent };
})();
// A demo identity and computer name (the lead's rule for screenshots that may go into docs or the site): the scene never
// shows the real account or host. The real ones are kept only so driver.js can check they appear nowhere on screen.
(function () {
  const sys = lord.system;
  const real = sys.info.bind(sys);
  try { const r = real(); globalThis.__sceneReal = { user: String(r.user || ''), fullName: String(r.fullName || ''), hostname: String(r.hostname || '') }; } catch (e) { globalThis.__sceneReal = {}; }
  const demo = () => ({ ...real(), user: 'demo', fullName: 'Alex Demo', hostname: 'demo-pc' });
  try { sys.info = demo; } catch (e) { /* below */ }
  if (sys.info !== demo) { try { Object.defineProperty(sys, 'info', { value: demo, configurable: true, writable: true }); } catch (e) { /* stays real */ } }
})();
