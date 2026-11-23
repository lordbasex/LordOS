// driver.js — goes after the theme bundle in tests/store_rankings_headless.sh: it opens the Store, goes to its Flathub
// section, waits for the catalog and Flathub's rankings (E10), and saves a screenshot after each step in the home
// folder (shot-1.png …): the home's top, the home scrolled to the next rows (twice), Popular's "See all", a category
// with its subcategory chips, the rankings' switch turned off, and the home without the rankings. Nodes are found the
// way tests/studio_scene/driver.js finds them: by the text they show (pre.js keeps it), in the scene's language.
// Lines start with "STORE-SCENE" in the shell log.
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
    const l = box(id);
    if (!l) return false;
    const x = l[0] + l[2] / 2, y = l[1] + l[3] / 2;
    ev('move', id, x, y); ev('down', id, x, y); ev('up', id, x, y);
    return true;
  };
  // the shown nodes with this text (the text's own node: the one that has the onClick), top to bottom
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
  const byText = (text, minX = 0) => allByText(text).find((id) => box(id)[0] >= minX) || null;
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(900);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  // the main column: right of the Store's two side columns
  const CONTENT_X = FRAME.x + 250 + 190 + 10;
  // a node inside the main column's scroll view: Flathub's subtitle, at the top of every Flathub page (its box may be
  // scrolled out of the window; the wheel goes to the node, and up its parents to the scroll view)
  const content = () => {
    for (const [raw, s] of P.texts) {
      if (s.trim() !== t('flathub.subtitle')) continue;
      const id = P.parent.get(raw);
      if (id && box(id)) return id;
    }
    return null;
  };
  const wheel = (dy) => {
    const id = content();
    if (!id) return false;
    ev('wheel', id, CONTENT_X + 40, FRAME.y + 300, dy);
    return true;
  };
  // scrolls until the node with this text is near the main column's top (or the list ends)
  const scrollTo = async (text) => {
    for (let i = 0; i < 120; i++) {
      const id = byText(text, CONTENT_X);
      if (id && box(id)[1] > FRAME.y + 95 && box(id)[1] < FRAME.y + 230) return true;
      if (!wheel(-1)) return false;
      await sleep(30);
    }
    return false;
  };
  // the switch on the line of this text (its sibling before it), clicked; false when not there
  const toggleBy = (text) => {
    const lbl = byText(text, CONTENT_X);
    const row = lbl && P.parent.get(lbl);
    const tog = row && [...P.parent.keys()].find((id) => P.parent.get(id) === row && id !== lbl && box(id) && box(id)[0] < box(lbl)[0]);
    return tog ? click(tog) : false;
  };
  const toTop = async () => { for (let i = 0; i < 80; i++) { wheel(6); await sleep(10); } await sleep(300); };
  const rowsShown = () => sdk.flathubModel.COLLECTIONS.filter((k) => byText(t(`flathub.coll.${k}`), CONTENT_X)).join(',');

  (async () => {
    await sleep(2500);
    sdk.wm.open('store', { frame: FRAME });
    await sleep(2500);
    const fh = byText(t('store.page.flathub'));
    log(`flathub item ${fh ? 'found' : 'missing'}`);
    if (fh) click(fh);
    // the catalog, then the rankings (a live fetch takes longer)
    const live = !!globalThis.STORE_LIVE;
    for (let i = 0; i < 240 && !(F.state.get().loaded && F.state.get().catalog.length); i++) await sleep(250);
    const s0 = F.state.get();
    log(`catalog ${s0.loaded ? 'loaded' : 'NOT loaded'}: ${s0.catalog.length} apps, busy=${s0.busy || ''} msg=${s0.msg ? s0.msg.text : ''}`);
    for (let i = 0; i < (live ? 400 : 40) && Object.keys(F.state.get().ranks).length < 6; i++) await sleep(250);
    let rk = F.state.get().ranks;
    log(`ranks ${Object.keys(rk).map((k) => `${k}=${rk[k].length}`).join(' ') || 'none'} (${live ? 'live fetch' : 'seeded cache'})`);
    if (live) {
      // the raw answer, then nothing else to shoot: the cache files tell the rest
      log(`live raw ${JSON.stringify(lord.flatpak.ranks()).slice(0, 300)}`);
      await shot('live-home');
      log('done');
      return;
    }
    await sleep(1500);
    log(`rows ${rowsShown()} banner=${(sdk.flathubModel.banner(s0.catalog, { now: Date.now(), ranks: rk }) || {}).kind}`);
    const rowsOf = (showAll) => sdk.flathubModel.homeRows(F.state.get().catalog, { now: Date.now(), ranks: F.state.get().ranks, showAll })
      .map((r) => `${r.key}=${r.apps.length}[${r.apps.slice(0, 3).map((a) => a.id).join(' ')}]`).join(' ');
    log(`home rows: ${rowsOf(false)}`);
    await shot('home-top');
    const trending = await scrollTo(t('flathub.coll.trending'));
    log(`scrolled to trending: ${trending} rows ${rowsShown()}`);
    await shot('home-trending');
    const updated = await scrollTo(t('flathub.coll.updated'));
    log(`scrolled to recently updated: ${updated} rows ${rowsShown()}`);
    await shot('home-updated');
    // Verified is a row only while every app is shown (the switch over the rankings' one)
    await toTop();
    const all = toggleBy(t('flathub.showAllOff', { count: F.state.get().catalog.filter((a) => !a.verified).length }));
    await sleep(1200);
    log(`show every app ${all ? 'on' : 'switch missing'}: ${rowsOf(true)}`);
    const verified = await scrollTo(t('flathub.coll.verified'));
    log(`scrolled to verified: ${verified} rows ${rowsShown()}`);
    await shot('home-verified');
    await toTop();
    toggleBy(t('flathub.showAllOn'));
    await sleep(1200);
    // Popular's "See all": the accent text on its title's line
    await toTop();
    await scrollTo(t('flathub.coll.popular'));
    const pt = byText(t('flathub.coll.popular'), CONTENT_X);
    let more = null;
    if (pt) {
      const py = box(pt)[1] + box(pt)[3] / 2;
      for (const [raw, s] of P.texts) {
        const id = P.parent.get(raw), l = id && box(id);
        if (l && inWin(l) && id !== pt && Math.abs(l[1] + l[3] / 2 - py) < 8 && l[0] > box(pt)[0] && /\d/.test(s)) more = id;
      }
    }
    log(`popular see-all ${more ? `found "${P.texts.get([...P.texts.keys()].find((r) => P.parent.get(r) === more))}"` : 'missing'}`);
    if (more) click(more);
    await sleep(1200);
    await shot('popular-all');
    // a category with its subcategories: Graphics in the second column (the chips are over its apps)
    const cat = byText(t('store.cat.Graphics'));
    log(`category ${cat ? 'found' : 'missing'}`);
    if (cat) click(cat);
    await sleep(1200);
    await shot('category');
    // back to the home (the sidebar's Flathub item), then the rankings' switch: the toggle on its text's line
    if (fh) click(byText(t('store.page.flathub')) || fh);
    await sleep(1500);
    await toTop();
    const tog = toggleBy(t('flathub.rankingsOn'));
    log(`rankings switch ${tog ? 'found' : 'missing'}`);
    await sleep(1200);
    rk = F.state.get().ranks;
    log(`after the switch: setting rankings=${sdk.flathubModel.prefsOf(sdk.settings.get().flathub).rankings} ranks=${Object.keys(rk).length} rows ${rowsShown()}`);
    await shot('switch-off');
    await scrollTo(t('flathub.home.categories'));
    log(`rankings off, rows ${rowsShown()}`);
    await shot('home-off');
    log('done');
  })();
})();
