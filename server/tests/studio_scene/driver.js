// driver.js — goes after the theme bundle in tests/studio_headless.sh, saving a screenshot after each step in the home
// folder (shot-1.png …). STUDIO_SCENE picks what it shows:
//   effects (the default): an app "uses" the camera (as the Camera app does when it opens LordOS Studio) so the capture
//            item shows; then it clicks the item, "Video effects", and the Background row;
//   mic:     an app uses only the microphone (the Voice Recorder); it clicks the item, then Mic Mode;
//   app:     it opens the LordOS Studio app (apps/studio), then its Portrait row.
// Nodes are found the way tests/perf/driver.js finds them: by the text they show (pre.js keeps it) or, for the item,
// by its size.
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__studioScene;
  const kind = globalThis.STUDIO_SCENE || 'effects';
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
  const openItem = async () => {
    // the item's pill: 30 × 18 in the top bar
    const pill = [...P.parent.keys()].find((id) => { const l = box(id); return l && Math.abs(l[2] - 30) < 1 && Math.abs(l[3] - 18) < 1 && l[1] < 40; });
    lord.log(`STUDIO-SCENE item ${pill ? 'found' : 'missing'}`);
    if (pill) click(pill);
    await sleep(1200);
    await shot();
  };
  const step = async (label, name, wait = 1500) => {
    const id = byText(label);
    lord.log(`STUDIO-SCENE ${name} ${id ? 'found' : 'missing'}`);
    if (id) click(id);
    await sleep(wait);
    await shot();
  };
  (async () => {
    await sleep(2500);
    if (kind === 'mic') {
      sdk.markDeviceUse('recorder', 'microphone', true);
      await sleep(1500);
      await openItem();
      await step(globalThis.STUDIO_MIC || 'Mic Mode', 'mic');
    } else if (kind === 'app') {
      sdk.wm.open('studio');
      await sleep(3000);
      await shot();
      await step(globalThis.STUDIO_PORTRAIT || 'Portrait', 'portrait');
    } else {
      sdk.markDeviceUse('camera', 'camera', true);
      await sleep(1500);
      await openItem();
      await step(globalThis.STUDIO_OPEN || 'Video effects', 'effects', 2500);
      await step(globalThis.STUDIO_BG || 'Background', 'background');
    }
    lord.log('STUDIO-SCENE done');
  })();
})();
