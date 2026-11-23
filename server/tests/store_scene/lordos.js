// lordos.js — goes after the theme bundle in tests/store_rankings_headless.sh with STORE_SCENE=lordos: the Store's
// package-system sections after 2026-11-23 — each shop straight on its content, its second column with Installed,
// Settings (what sat above the shop) and the categories, the title bar's update icon and the section's own search — and
// LordOS apps, LordOS's own store (apps/store/lordospages.js). It saves a screenshot after each step (shot-1.png …):
// Flathub's shop, its Settings, its own search, GNU/Linux apps (Debian) → Settings, Web apps' shop and Settings, LordOS
// apps' shop (its cards, then the .lpk packages in the home folder), its Installed, LordOS Studio Camera's page (from its
// card), the general Installed page with its LordOS apps filter, and Discover's search over every system. What is
// installed is this machine's own (the package index, the voice models; Flatpak's apps are pre.js's when seeded).
// Nodes are found by the text they show (pre.js keeps it), in the scene's language; lines start with "STORE-SCENE".
// Run it in a left-to-right language (columns are found by x).
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__storeScene;
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
  const byText = (text, minX = 0, maxX = 1e9) => allByText(text).find((id) => box(id)[0] >= minX && box(id)[0] < maxX) || null;
  const SIDE_END = FRAME.x + 250;                  // the Store's sidebar
  const COL_END = SIDE_END + 190;                  // an origin's second column
  const has = (text, minX, maxX) => !!byText(text, minX, maxX);
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
  const seen = (what, names, minX, maxX) => log(`${what}: ${names.map((s) => `${s}=${has(s, minX, maxX) ? 'shown' : 'absent'}`).join(' ')}`);
  const NAMES = ['studioCamera', 'diskFormats', 'webRuntime', 'voices'].map((id) => t(`store.native.${id}.name`));
  // the second column: Installed, Settings, then All (and Categories when the section has them)
  const column = () => {
    const y = (k) => { const id = byText(t(k), SIDE_END, COL_END); return id ? box(id)[1] : null; };
    const a = y('store.page.installed'), s = y('store.sub.settings'), b = y('store.sub.all'), c = y('store.section.categories');
    const order = a != null && s != null && b != null && a < s && s < b && (c == null || s < c);
    return `column ${order ? 'Installed, Settings, then the shop' : `WRONG installed=${a} settings=${s} all=${b}`}, Categories ${c != null ? 'shown' : 'absent'}`;
  };
  // the title bar's update icon (↻, right of the column) and the search field's placeholder
  const icon = () => (has('↻', COL_END) ? 'update icon shown' : 'no update icon');
  const ph = (key) => `placeholder ${has(t(key), SIDE_END) ? `"${t(key)}"` : `"${t(key)}" ABSENT`}`;
  const open = async (args) => { sdk.wm.open('store', { frame: FRAME, args: { ...args, nonce: Date.now() } }); await sleep(2000); };

  (async () => {
    await sleep(2500);
    sdk.wm.open('store', { frame: FRAME });
    await sleep(2500);
    // Flathub apps: the shop straight on its banner, its Settings with what sat above it
    side(t('store.page.flathub'));
    for (let i = 0; i < 240 && !(sdk.flathub.state.get().loaded && sdk.flathub.state.get().catalog.length); i++) await sleep(250);
    await sleep(1000);
    log(`flathub: ${column()}; ${icon()}; ${ph('store.search.in.flathub')}`);
    seen('flathub shop (the header must be absent)', [t('flathub.subtitle'), t('flathub.turnOff'), t('flathub.refresh')], COL_END);
    await shot('flathub-shop');
    second(t('store.sub.settings'));
    await sleep(1200);
    seen('flathub settings', [t('flathub.subtitle'), t('flathub.turnOff'), t('flathub.refresh'), t('store.refresh.flathub'), t('store.settings.flathubOff')], COL_END);
    await shot('flathub-settings');
    // Flathub's own search
    await open({ page: 'flathub', search: 'gimp' });
    await sleep(1500);
    seen('flathub search', [t('store.search.catalog'), t('store.debian.otherPackages')], COL_END);
    await shot('flathub-search');
    // GNU/Linux apps (Debian) → Settings
    side(t('store.src.debian'));
    await sleep(1500);
    log(`debian: ${column()}; ${icon()}; ${ph('store.search.in.debian')}`);
    second(t('store.sub.settings'));
    await sleep(1200);
    seen('debian settings', [t('store.settings.debianAbout'), t('store.settings.listsTitle'), t('store.updates.auto')], COL_END);
    await shot('debian-settings');
    // Web apps (PWA): the shop, then its Settings
    side(t('store.page.webapps'));
    await sleep(1500);
    log(`web: ${column()}; ${icon()}; ${ph('store.search.in.web')}`);
    seen('web shop (the header must be absent)', [t('webapps.store.subtitle'), t('webapps.store.catalogTitle')], COL_END);
    await shot('web-shop');
    second(t('store.sub.settings'));
    await sleep(1000);
    seen('web settings', [t('webapps.store.subtitle'), t('store.settings.webManage')], COL_END);
    await shot('web-settings');
    // LordOS apps: the shop, its Installed, Studio Camera's page
    side(t('store.page.lordos'));
    await sleep(1500);
    log(`lordos: ${column()}; ${icon()}; ${ph('store.search.in.lordos')}`);
    seen('lordos shop', [...NAMES, t('store.lordos.lpkTitle'), t('store.lordos.filesTitle'), t('store.lordos.intro')], COL_END);
    await shot('lordos-shop');
    second(t('store.page.installed'));
    await sleep(1200);
    seen('lordos installed', [t('store.lordos.catalogTitle'), ...NAMES, t('store.lordos.lpkTitle')], COL_END);
    await shot('lordos-installed');
    second(t('store.sub.all'));
    await sleep(1000);
    log(`studio camera card ${click(byText(NAMES[0], COL_END)) ? 'clicked' : 'MISSING'}`);
    await sleep(1500);
    seen('studio camera page', [NAMES[0], t('store.native.studioCamera.summary'), t('store.open'), t('store.install'), t('store.remove'), t('store.installedBadge')], SIDE_END);
    await shot('studio-camera');
    // the general Installed page, LordOS apps filter
    side(t('store.page.installed'));
    await sleep(1500);
    click(byText(t('store.page.lordos'), SIDE_END));
    await sleep(1000);
    seen('general installed, LordOS apps filter', [...NAMES, t('store.kind.native')], SIDE_END);
    await shot('installed-lordos');
    // the global search (Discover: every system)
    await open({ search: 'studio' });
    await sleep(2500);
    log(`discover: ${icon()}`);
    seen('global search', [t('store.page.lordos'), NAMES[0], 'OBS Studio'], SIDE_END);
    await shot('global-search');
    log('done');
  })();
})();
