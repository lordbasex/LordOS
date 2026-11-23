// pre.js — goes before the theme bundle in tests/store_rankings_headless.sh (a test tool, never shipped): it keeps
// which text each node shows and where it hangs, so driver.js can click a control by its label. When the script found
// no Flathub appstream that lord-appstream can read, it sets STORE_FIXTURE (the recorded catalog,
// tests/data/flathub/catalog-2026-11-22.json, without its icons: their files aren't there) and lord.flatpak's catalog
// job answers with it; every other call (the rankings, the installed list, the settings) is the real code.
(function () {
  const texts = new Map(), parent = new Map();
  const wrap = (name, fn) => { const real = lord[name]; lord[name] = (...a) => { fn(...a); return real(...a); }; };
  wrap('setText', (id, s) => texts.set(id, String(s)));
  wrap('appendChild', (p, c) => parent.set(c, p));
  wrap('insertBefore', (p, c) => parent.set(c, p));
  wrap('destroy', (id) => { texts.delete(id); parent.delete(id); });
  globalThis.__storeScene = { texts, parent };
  const fixture = globalThis.STORE_FIXTURE;
  if (!fixture || !lord.flatpak) return;
  const F = lord.flatpak, realTake = F.take;
  const done = [];
  let next = 900000;
  F.loadCatalog = () => { const id = ++next; done.push({ id, done: true, code: 0 }); return id; };
  F.take = () => [...realTake(), ...done.splice(0)];
  F.catalog = () => fixture.map((a) => ({ ...a, icon: '' }));
  F.description = () => '';
})();
