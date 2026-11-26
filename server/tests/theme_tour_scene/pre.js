// pre.js — goes before the theme bundle in tests/app_scene_headless.sh theme_tour_scene (a test tool, never shipped).
// A demo identity and computer name (the lead's rule for screenshots that may go into docs, the site or LordOS-themes'
// README): a theme's start menu, session menu or bar never shows the real account or host. Same identity as luna_scene.
(function () {
  const sys = lord.system;
  if (!sys || typeof sys.info !== 'function') return;
  const real = sys.info.bind(sys);
  const demo = () => ({ ...real(), user: 'demo', fullName: 'Alex Demo', hostname: 'demo-pc', cpu: 'Demo CPU @ 2.0GHz', machine: 'Demo PC', memTotalKB: 16 * 1024 * 1024, memAvailableKB: 9 * 1024 * 1024 });
  try { sys.info = demo; } catch (e) { /* below */ }
  if (sys.info !== demo) { try { Object.defineProperty(sys, 'info', { value: demo, configurable: true, writable: true }); } catch (e) { /* stays real */ } }
})();
