// driver.js — goes after the theme bundle in tests/app_scene_headless.sh settings_region_scene (GitHub #239): Settings →
// Language & Region → Region → Change… lists each glibc region with its language and its country ("Basque · France
// (eu_FR)"), so the regions of one country (br, ca, eu, fr, oc in France) can be told apart; a search by the language's
// name finds them. Screenshots of the list (searched for "France", then for the language). Lines start with "APP-SCENE".
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
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(500);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  const type = async (s) => { for (const ch of s) { globalThis.__lordKey(0, 1, ch, ch, ch, 0); globalThis.__lordKey(0, 0, ch, '', '', 0); await sleep(20); } };
  (async () => {
    await sleep(2500);
    sdk.wm.open('settings', { args: { section: 'language' } });
    await sleep(2500);
    const w = (sdk.windows.get() || []).find((x) => x.app === 'settings');
    if (w) { sdk.wm.resize(w.id, 900, 700); sdk.wm.center(w.id); }
    await sleep(1200);
    const change = textId(sdk.t('settings.change'));
    step('the Region row has Change…', !!change);
    if (!change) { log('done'); return; }
    click(change);
    await sleep(1500);
    const all = shownTexts();
    const label = (r) => sdk.regionLabel(r, sdk.tOptional);
    const fr = ['br_FR', 'ca_FR', 'eu_FR', 'fr_FR', 'oc_FR'].map(label);
    log(`labels: ${fr.join(' | ')}`);
    step('every France region names its language', new Set(fr.map((x) => x.split(' (')[0])).size === 5 && fr.every((x) => / · /.test(x)));
    step('the list is open', all.some((s) => / \([a-z]{2,3}_[A-Z]{2}/.test(s)), all.filter((s) => /_/.test(s)).slice(0, 3).join(' | '));
    const country = sdk.t('countries.FR');
    await type(country);
    await sleep(1200);
    const seen = shownTexts().filter((s) => /_FR\)/.test(s));
    log(`France rows: ${seen.join(' | ')}`);
    step('a search for the country lists its regions by language', fr.every((x) => seen.includes(x)), `${seen.length} rows`);
    await shot('region-france');
    for (let i = 0; i < country.length; i++) { globalThis.__lordKey(14, 1, 'Backspace', '', '', 0); globalThis.__lordKey(14, 0, 'Backspace', '', '', 0); await sleep(20); }
    const basque = sdk.t('languages.eu');
    await type(basque);
    await sleep(1200);
    const bq = shownTexts().filter((s) => s.startsWith(`${basque} · `));
    log(`${basque} rows: ${bq.join(' | ')}`);
    step('a search for the language finds its regions', bq.length >= 2);
    await shot('region-language');
    log('done');
  })();
})();
