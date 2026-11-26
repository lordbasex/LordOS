// pre.js — goes before the theme bundle in tests/app_scene_headless.sh ws_topbar_scene (a test tool, never shipped):
// keeps which nodes are one of Workspaces' top-band surfaces (44 px high, a 22 px radius: the activity strip and the
// status cluster, components/TopBar.jsx), the activities' pills and the accent under the one in view, so driver.js can compare where they are drawn.
(function () {
  const seen = new Map();
  { const real = lord.createNode; lord.createNode = (...a) => { const id = real(...a); seen.delete(id); return id; }; }
  const real = lord.setStyle;
  lord.setStyle = (id, k, v) => {
    if (k === 'height' || k === 'borderRadius' || k === 'maxWidth' || k === 'pointerEvents') { const s = seen.get(id) || {}; s[k] = v; seen.set(id, s); }
    return real(id, k, v);
  };
  { const d = lord.destroy; lord.destroy = (id, ...a) => { seen.delete(id); return d(id, ...a); }; }
  // the accent under the activity in view, and the activities' pills
  globalThis.__wsGlider = () => [...seen].filter(([, s]) => s.height === 36 && s.borderRadius === 18 && s.pointerEvents === 'none').map(([id]) => id);
  globalThis.__wsPills = () => [...seen].filter(([, s]) => s.height === 36 && s.borderRadius === 18 && s.maxWidth === 190).map(([id]) => id);
  globalThis.__wsBand = () => [...seen].filter(([, s]) => s.height === 44 && s.borderRadius === 22).map(([id]) => id);
})();
