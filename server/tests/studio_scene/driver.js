// driver.js — goes after the theme bundle in tests/studio_headless.sh: an app "uses" the camera (as the Camera app does
// when it opens LordOS Studio) so the capture item shows; then it clicks the item, "Video effects", and the Background
// row, saving a screenshot after each step in the home folder (shot-1.png …). Nodes are found the way
// tests/perf/driver.js finds them: by the text they show (pre.js keeps it) or, for the item, by its size.
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__studioScene;
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const click = (id) => {
    const l = box(id);
    if (!l) return false;
    const x = l[0] + l[2] / 2, y = l[1] + l[3] / 2;
    globalThis.__lordEvent('move', id, x, y, 0);
    globalThis.__lordEvent('down', id, x, y, 0);
    globalThis.__lordEvent('up', id, x, y, 0);
    return true;
  };
  const byText = (text) => {
    for (const [raw, s] of P.texts) if (s.trim() === text) { const id = P.parent.get(raw); if (id && box(id)) return id; }
    return null;
  };
  let n = 0;
  const shot = async () => {
    n++;
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    lord.log(`STUDIO-SCENE shot ${n} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  (async () => {
    await sleep(2500);
    sdk.markDeviceUse('camera', 'camera', true);
    await sleep(1500);
    // the item's pill: 30 × 18 in the top bar
    const pill = [...P.parent.keys()].find((id) => { const l = box(id); return l && Math.abs(l[2] - 30) < 1 && Math.abs(l[3] - 18) < 1 && l[1] < 40; });
    lord.log(`STUDIO-SCENE item ${pill ? 'found' : 'missing'}`);
    if (pill) click(pill);
    await sleep(1200);
    await shot();
    const fx = byText(globalThis.STUDIO_OPEN || 'Video effects');
    lord.log(`STUDIO-SCENE effects ${fx ? 'found' : 'missing'}`);
    if (fx) click(fx);
    await sleep(2500);
    await shot();
    const bg = byText(globalThis.STUDIO_BG || 'Background');
    if (bg) click(bg);
    await sleep(1500);
    await shot();
    lord.log('STUDIO-SCENE done');
  })();
})();
