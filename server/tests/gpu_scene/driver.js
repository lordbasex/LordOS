// driver.js — goes after the theme bundle in tests/gpu_headless.sh: opens Monitor on GPU (pre.js's made-up cards) and
// saves screenshots in the home folder: the RX 580 alone ("One"), then Side by side. Lines start with "GPU-SCENE".
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__gpuScene;
  const L = globalThis.GPU_L || {};
  const log = (s) => lord.log(`GPU-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const ev = (type, id, x, y) => globalThis.__lordEvent(type, id, x, y, 0);
  const click = (id) => { const l = id && box(id); if (!l) return false; const p = [l[0] + l[2] / 2, l[1] + l[3] / 2]; ev('move', id, ...p); ev('down', id, ...p); ev('up', id, ...p); return true; };
  const byText = (text) => {
    const out = [];
    for (const [raw, s] of P.texts) if (s.trim() === text) { const id = P.parent.get(raw); if (id && box(id)) out.push(id); }
    out.sort((a, b) => b - a);
    return out[0] || null;
  };
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(1200);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  (async () => {
    await sleep(2500);
    const id = sdk.wm.open('monitor', { args: { tab: 'gpu', nonce: Date.now() } });
    await sleep(800);
    // the mockup's size: 1280×800
    const w = (sdk.windows.get() || []).find((x) => x.id === id || String(x.app || x.appId || '').startsWith('monitor'));
    if (w) { sdk.wm.move(w.id, 80, 40); sdk.wm.resize(w.id, 1280, 800); }
    log(`window ${w ? 'done' : 'missing'}`);
    await sleep(6000);
    await shot('one');
    log(`side ${click(byText(L.side)) ? 'done' : 'missing'}`);
    await sleep(1500);
    await shot('side');
    log('done');
  })();
})();
