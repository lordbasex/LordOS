// installed.js — goes after the theme bundle in tests/store_rankings_headless.sh with STORE_SCENE=installed: the
// Store's installed apps by package system (2026-11-23). pre.js seeded two Flatpak apps and two web apps
// (STORE_SEED_INSTALLED). It opens the Store and saves a screenshot after each step (shot-1.png …): Flathub's shop (the
// section opens on it), Flathub → Installed (the second column's first item), Web apps' shop, Web apps → Installed,
// Linux apps (Debian) → Installed, the general Installed page with every system, and its Flathub and Web apps filters.
// Nodes are found by the text they show (pre.js keeps it), in the scene's language. Lines start with "STORE-SCENE".
// The columns are found by their x, left to right: run it in a left-to-right language (a right-to-left one only shoots).
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__storeScene;
  const F = sdk.flathub;
  const t = sdk.t;
  const log = (s) => lord.log(`STORE-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const FRAME = { x: 30, y: 44, w: 1540, h: 770 };       // above the dock
  const inWin = (l) => l[0] + l[2] / 2 >= FRAME.x && l[0] + l[2] / 2 <= FRAME.x + FRAME.w && l[1] + l[3] / 2 >= FRAME.y && l[1] + l[3] / 2 <= FRAME.y + FRAME.h;
  const ev = (type, id, x, y, d = 0) => globalThis.__lordEvent(type, id, x, y, d);
  const click = (id) => {
    const l = id && box(id);
    if (!l) return false;
    const x = l[0] + l[2] / 2, y = l[1] + l[3] / 2;
    ev('move', id, x, y); ev('down', id, x, y); ev('up', id, x, y);
    return true;
  };
  const allByText = (text) => {
    const out = [];
    for (const [raw, s] of P.texts) {
      if (s.trim() !== text) continue;
      const id = P.parent.get(raw);
      const l = id && box(id);
      if (l && inWin(l)) out.push(id);
    }
    return out.sort((a, b) => box(a)[1] - box(b)[1] || box(a)[0] - box(b)[0]);
  };
  // the first node with this text whose left edge is between minX and maxX (a column of the window)
  const byText = (text, minX = 0, maxX = 1e9) => allByText(text).find((id) => box(id)[0] >= minX && box(id)[0] < maxX) || null;
  const SIDE_END = FRAME.x + 230;                  // the Store's sidebar
  const COL_END = SIDE_END + 190;                  // an origin's second column
  const has = (text, minX) => !!byText(text, minX);
  const gimp = () => (F.catalogApp('org.gimp.GIMP') || {}).name || 'org.gimp.GIMP';     // its name in the catalog's language
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(1200);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  const side = (label) => { const ok = click(byText(label, 0, SIDE_END)); log(`sidebar "${label}" ${ok ? 'clicked' : 'MISSING'}`); return ok; };
  const second = (label) => { const ok = click(byText(label, SIDE_END, COL_END)); log(`second column "${label}" ${ok ? 'clicked' : 'MISSING'}`); return ok; };
  const seen = (what, names, minX) => log(`${what}: ${names.map((s) => `${s}=${has(s, minX) ? 'shown' : 'absent'}`).join(' ')}`);
  // the second column's order: Installed over Categories
  const order = () => {
    const a = byText(t('store.page.installed'), SIDE_END, COL_END), b = byText(t('store.section.categories'), SIDE_END, COL_END);
    return a && b ? (box(a)[1] < box(b)[1] ? 'Installed above Categories' : 'Installed BELOW Categories') : `Installed ${a ? 'found' : 'missing'}, Categories ${b ? 'found' : 'missing'}`;
  };

  (async () => {
    await sleep(2500);
    sdk.wm.open('store', { frame: FRAME });
    await sleep(2500);
    side(t('store.page.flathub'));
    for (let i = 0; i < 240 && !(F.state.get().loaded && F.state.get().catalog.length); i++) await sleep(250);
    for (let i = 0; i < 40 && !F.state.get().installed.length; i++) await sleep(250);
    const s0 = F.state.get();
    log(`catalog ${s0.loaded ? 'loaded' : 'NOT loaded'}: ${s0.catalog.length} apps; installed ${s0.installed.map((x) => x.id).join(' ')}`);
    log(`flathub column: ${order()}`);
    seen('flathub shop', [t('flathub.home.categories'), 'LibreOffice'], COL_END);
    await shot('flathub-shop');
    second(t('store.page.installed'));
    await sleep(800);
    seen('flathub installed', ['LibreOffice', gimp(), t('flathub.updates.button'), t('flathub.home.categories')], COL_END);
    await shot('flathub-installed');
    side(t('store.page.webapps'));
    await sleep(800);
    log(`web column: ${order()}`);
    seen('web shop', [t('webapps.store.catalogTitle'), t('webapps.store.installedTitle')], COL_END);
    await shot('web-shop');
    second(t('store.page.installed'));
    await sleep(800);
    seen('web installed', ['Telegram', 'WhatsApp', t('webapps.store.manage'), t('webapps.store.catalogTitle')], COL_END);
    await shot('web-installed');
    side(t('store.src.debian'));
    await sleep(1500);
    log(`debian column: ${order()}`);
    second(t('store.page.installed'));
    await sleep(1200);
    await shot('debian-installed');
    side(t('store.page.installed'));
    await sleep(1500);
    seen('general installed filters', [t('store.off.filterAll'), t('store.page.lordos'), t('store.page.flathub'), t('store.page.webapps'), t('store.off.filterOff')], SIDE_END);
    seen('general installed rows', ['LibreOffice', 'Telegram', t('store.kind.flatpak'), t('store.kind.web')], SIDE_END);
    await shot('installed-all');
    click(byText(t('store.page.flathub'), SIDE_END));
    await sleep(1000);
    seen('filter Flathub', ['LibreOffice', gimp(), 'Telegram'], SIDE_END);
    await shot('installed-flathub');
    click(byText(t('store.page.webapps'), SIDE_END));
    await sleep(1000);
    seen('filter Web apps', ['Telegram', 'WhatsApp', 'LibreOffice'], SIDE_END);
    await shot('installed-web');
    log('done');
  })();
})();
