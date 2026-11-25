// driver.js — goes after the theme bundle in tests/app_scene_headless.sh settings_wallpaper_scene (0.8.9.6.2): Settings →
// Wallpaper's gallery. What shows now with For / Quality, the search and its filters with counts, the collections as
// cards (Abstract, Epic, Anime marked NEW), a collection's page (Download all, its tiles in the cloud state: nothing is
// downloaded in a fresh home), a wallpaper's sheet (HD and 4K with the catalog's sizes, Use on desktop / Login screen),
// a search, and the My photos filter. Screenshots of each; run it in Arabic too for right to left. Lines start with
// "APP-SCENE".
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__appScene;
  const log = (s) => lord.log(`APP-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const ev = (type, id, x, y, b = 0) => globalThis.__lordEvent(type, id, x, y, b);
  const textId = (label) => { for (const [raw, s] of P.texts) if (s === label) { const id = P.parent.get(raw); if (id && box(id)) return id; } return null; };
  const shownTexts = () => { const out = []; for (const [raw, s] of P.texts) { const id = P.parent.get(raw); if (id && box(id)) out.push(s); } return out; };
  const step = (what, ok, extra) => log(`${what} ${ok ? 'done' : 'missing'}${extra ? ` (${extra})` : ''}`);
  const click = (id) => { const b = box(id); const x = b[0] + b[2] / 2, y = b[1] + b[3] / 2; ev('move', id, x, y); ev('down', id, x, y); ev('up', id, x, y); };
  const t = (k, v) => sdk.t(`settings.${k}`, v);
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(700);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  const type = async (s) => { for (const ch of s) { globalThis.__lordKey(0, 1, ch, ch, ch, 0); globalThis.__lordKey(0, 0, ch, '', '', 0); await sleep(20); } };
  (async () => {
    await sleep(2500);
    sdk.wm.open('settings', { args: { section: 'wallpaper' } });
    await sleep(2500);
    const w = (sdk.windows.get() || []).find((x) => x.app === 'settings');
    if (w) { sdk.wm.resize(w.id, 1180, 860); sdk.wm.center(w.id); }
    await sleep(1500);
    const all = shownTexts();
    step('For: Desktop and Login screen', all.includes(t('wallpaperFor.desktop')) && all.includes(t('wallpaperFor.login')));
    step('Quality: Auto, HD, 4K', all.includes(t('wallpaperGallery.qualityAuto')) && all.includes(t('wallpaperGallery.quality4k')));
    log(`chips: ${all.filter((s) => / \d+$/.test(s)).slice(0, 6).join(' | ')}`);
    step('the filters with counts', all.some((s) => s.startsWith(t('wallpaperGallery.filter.here'))));
    step('the collections', all.includes(t('wallpaperGallery.collections')));
    step('Epic is a collection, marked NEW', all.includes(t('wallpaperCategory.epic')) && all.includes(t('wallpaperGallery.new')));
    await shot('home');
    // the Epic collection's page
    const epic = textId(t('wallpaperCategory.epic'));
    if (!epic) { step('Epic card', false); log('done'); return; }
    click(epic);
    await sleep(1500);
    const page = shownTexts();
    step('a collection page: Back and Download all', page.includes(t('wallpaperGallery.back')) && page.some((s) => s.startsWith(t('wallpaperGallery.downloadAll', { quality: t('wallpaperGallery.qualityHd'), size: '' }).trim().split('·')[0].trim())));
    await shot('collection-epic');
    // a wallpaper's sheet
    const first = sdk.stillItems.find((it) => it.section === 'epic');
    const tile = first ? textId(sdk.tOptional(`settings.wallpaperItem.${first.id}`) || first.name) : null;
    step('an Epic tile', !!tile, first && first.name);
    if (tile) {
      click(tile);
      await sleep(1500);
      const sh = shownTexts();
      step('the sheet: Download and use, Login screen', sh.includes(t('wallpaperGallery.downloadUse')) && sh.includes(t('wallpaperGallery.useLogin')));
      step('the sheet: the catalog\'s sizes', sh.some((s) => s.startsWith(sdk.t('settings.wallpaperGallery.option.4k', { size: '' }).slice(0, 4))));
      await shot('sheet');
      const close = textId(t('wallpaperGallery.close'));
      if (close) click(close);
      await sleep(800);
    }
    const back = textId(t('wallpaperGallery.back'));
    if (back) click(back);
    await sleep(1200);
    // a search
    const search = textId(t('wallpaperGallery.search'));
    if (search) click(search);
    await sleep(400);
    await type('sakura');
    await sleep(1500);
    await shot('search');
    step('a search finds by name', shownTexts().some((x) => /sakura/i.test(x)));
    for (let i = 0; i < 6; i++) { globalThis.__lordKey(14, 1, 'Backspace', '', '', 0); globalThis.__lordKey(14, 0, 'Backspace', '', '', 0); await sleep(20); }
    await sleep(1200);
    // one row of each collection, under the cards
    const cols = textId(t('wallpaperGallery.collections'));
    if (cols) { const b = box(cols); for (let i = 0; i < 12; i++) { ev('wheel', cols, b[0] + 5, b[1] + 5, -3); await sleep(60); } }
    await sleep(800);
    step('a collection row: See all', shownTexts().some((x) => x.startsWith(t('wallpaperGallery.seeAll', { count: 50 }).slice(0, 6))));
    await shot('rows');
    log('done');
  })();
})();
