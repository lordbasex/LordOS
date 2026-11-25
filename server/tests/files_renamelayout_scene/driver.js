// driver.js — goes after the theme bundle in tests/app_scene_headless.sh files_renamelayout_scene: Files' rename field
// takes the name's place without moving anything (the icon grid moved down 6 px while it was open: the 22 px field in
// a 16 px line). In the icon, list and column views it selects a name, notes where every item's name and the renamed
// item itself are, opens the field (F2, and in the icon view the slow second click too) and compares, with screenshots
// before and while renaming. Lines start with "APP-SCENE" in the shell log.
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__appScene;
  const log = (s) => lord.log(`APP-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const ev = (type, id, x, y, b = 0) => globalThis.__lordEvent(type, id, x, y, b);
  const textId = (label) => { let best = null; for (const [raw, s] of P.texts) if (s === label) { const id = P.parent.get(raw); const b = id && box(id); if (b && b[3] <= 40 && (!best || id > best)) best = id; } return best; };
  const NAMES = ['alpha', 'bravo', 'charlie', 'delta', 'echo', 'foxtrot', 'golf', 'hotel', 'india', 'juliet', 'kilo', 'lima', 'mike-with-a-much-longer-name'];
  const nameId = (base) => textId(`${base}.txt`) || textId(base);
  const up = (id, n) => { let x = id; for (let i = 0; i < n && x; i++) x = P.parent.get(x); return x; };
  const step = (what, ok, extra) => log(`${what} ${ok ? 'done' : 'missing'}${extra ? ` (${extra})` : ''}`);
  const key = (code, k, chr) => { globalThis.__lordKey(code, 1, k, chr, chr || k, 0); globalThis.__lordKey(code, 0, k, '', '', 0); };
  const editing = () => !!sdk.hasTextFocus();
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(400);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  const center = (id) => { const b = box(id); return [b[0] + b[2] / 2, b[1] + b[3] / 2]; };
  const click = (id) => { const p = center(id); ev('move', id, ...p); ev('down', id, ...p); ev('up', id, ...p); };
  const files = () => (sdk.windows.get() || []).filter((w) => w.app === 'files' || String(w.id).startsWith('files'));
  const escape = async () => { if (editing()) { key(1, 'Escape', ''); await sleep(500); } };
  const open = async (view) => {
    await escape();
    for (const x of sdk.windows.get() || []) sdk.wm.close(x.id);
    await sleep(800);
    sdk.wm.open('files', { new: true, args: { path: `${lord.fs.home()}/Slow`, view, nonce: Date.now() } });
    await sleep(2500);
    const w = files().pop();
    if (w) { sdk.wm.resize(w.id, 980, 640); sdk.wm.center(w.id); sdk.wm.focus(w.id); }
    await sleep(1500);
  };
  // where each name is drawn, and the renamed item's own box (its card or row: `lift` levels above its name)
  const where = (skip, lift) => {
    const out = {};
    for (const b of NAMES) if (b !== skip) { const id = nameId(b); out[b] = id && box(id) ? box(id).map(Math.round).join(',') : null; }
    return out;
  };
  const same = (a, b) => Object.keys(a).filter((k) => a[k] !== b[k]);
  // LIFT: how far the item's card or row is above its name, per view
  // (the column view: the name is in the preview too, so only the other names are compared)
  const LIFT = { icons: 2, list: 3, columns: 0 };
  const check = async (view, base, how, start) => {
    const id = nameId(base);
    const item = up(id, LIFT[view]);
    const before = where(base), itemBefore = box(item)?.map(Math.round).join(',');
    await start();
    await sleep(900);
    const open1 = editing();
    const after = where(base), itemAfter = box(item)?.map(Math.round).join(',');
    const moved = same(before, after);
    step(`${view}, ${how}: the rename field is open`, open1);
    step(`${view}, ${how}: no other name moved`, !moved.length, moved.map((k) => `${k} ${before[k]} → ${after[k]}`).join('; '));
    if (LIFT[view]) step(`${view}, ${how}: the renamed item keeps its box`, !!itemBefore && itemBefore === itemAfter, `${itemBefore} → ${itemAfter}`);
    return open1;
  };
  (async () => {
    await sleep(2500);
    for (const view of ['icons', 'list', 'columns']) {
      await open(view);
      const base = view === 'columns' ? 'bravo' : 'echo';
      click(nameId(base)); await sleep(900);
      await shot(`${view}-selected`);
      await check(view, base, 'F2', async () => { key(60, 'F2', ''); });
      await shot(`${view}-renaming`);
      await escape();
      if (view === 'icons') {
        // the slow second click on the selected name
        click(nameId(base)); await sleep(1700);           // a fresh first click (the last one is too old for a second)
        await check(view, base, 'slow click', async () => { click(nameId(base)); await sleep(800); click(nameId(base)); });
        await escape();
        // a name on two lines: the field keeps both lines' height
        const long = 'mike-with-a-much-longer-name';
        click(nameId(long)); await sleep(900);
        await check(view, long, 'F2 on a two-line name', async () => { key(60, 'F2', ''); });
        await shot('icons-renaming-two-lines');
        await escape();
      }
    }
    log('done');
  })();
})();
