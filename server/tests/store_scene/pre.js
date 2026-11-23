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
  // STORE_SEED_INSTALLED (the "installed" scene, driver installed.js): a few installed apps of each system without
  // installing anything — two Flatpak apps (`flatpak list`'s job answers with them, lord.apps.flatpak exports them)
  // and two web apps of LordOS's catalog (lord.webapps.list); LordOS's and Debian's are this machine's own.
  if (globalThis.STORE_SEED_INSTALLED) {
    const FP = [['org.libreoffice.LibreOffice', 'LibreOffice', '25.8.3.2'], ['org.gimp.GIMP', 'GNU Image Manipulation Program', '3.0.6']];
    if (lord.flatpak) {
      const F = lord.flatpak, realStart = F.start, realTake = F.take, out = [];
      let next = 800000;
      F.start = (name, ...rest) => {
        if (name !== 'list') return realStart(name, ...rest);
        const id = ++next;
        for (const [app, , v] of FP) out.push({ id, line: `${app}\tstable\t${v}\tflathub` });
        out.push({ id, done: true, code: 0 });
        return id;
      };
      F.take = () => [...realTake(), ...out.splice(0)];
    }
    if (lord.apps) lord.apps.flatpak = () => FP.map(([id, name]) => ({ id, name, comment: '', icon: '', categories: '' }));
    const WEB = [
      { id: 'telegram', name: 'Telegram', origin: 'https://web.telegram.org', startUrl: 'https://web.telegram.org/', source: 'catalog', profiles: [{ id: 'default', name: 'Personal' }], iconPath: '' },
      { id: 'whatsapp', name: 'WhatsApp', origin: 'https://web.whatsapp.com', startUrl: 'https://web.whatsapp.com/', source: 'catalog', profiles: [{ id: 'default', name: 'Personal' }], iconPath: '' },
    ];
    if (!lord.webapps) lord.webapps = {};
    lord.webapps.list = () => WEB.map((e) => ({ ...e }));
    lord.webapps.get = (app) => WEB.find((e) => e.id === app) || null;
  }
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
