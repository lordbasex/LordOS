// driver.js — goes after the theme bundle in tests/app_scene_headless.sh screenshots_menu_scene (GitHub #237): the
// Screenshots toolbar floats just above the Dock, so its Options menu has no room below it. On the very first open the
// menu must already be above the bar and inside the work area (it used to open downward, cut off above the Dock, and
// move up only on the next render). The menu's box is read on each frame after the click, from the first frame it is
// drawn (visible) on. Lines start with "APP-SCENE" in the shell log; screenshots are taken with the menu open.
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__appScene;
  const log = (s) => lord.log(`APP-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const ev = (type, id, x, y, b = 0) => globalThis.__lordEvent(type, id, x, y, b);
  const textIds = (label) => { const out = []; for (const [raw, s] of P.texts) if (s === label) { const id = P.parent.get(raw); if (id) out.push(id); } return out; };
  const textId = (label) => textIds(label).find((id) => box(id)) || null;
  const up = (id, n) => { let x = id; for (let i = 0; i < n && x; i++) x = P.parent.get(x); return x; };
  const step = (what, ok, extra) => log(`${what} ${ok ? 'done' : 'missing'}${extra ? ` (${extra})` : ''}`);
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(400);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  // the popover: the nearest ancestor of a menu row's label whose box holds every section (it has the menu's border)
  const menuBox = (label) => {
    const t = textId(label);
    if (!t) return null;
    let best = null;
    for (let x = t, i = 0; x && i < 12; x = P.parent.get(x), i++) {
      const b = box(x);
      if (b && b[2] >= 200 && b[2] <= 320) best = [x, b];
    }
    return best;
  };
  const opacityOf = (id) => (P.opacity.has(id) ? P.opacity.get(id) : null);
  (async () => {
    await sleep(3000);
    const work = sdk.workAreaOf ? sdk.workAreaOf(sdk.mainDisplay ? sdk.mainDisplay() : null) : null;
    // the bar where Screenshots puts it by default: centred, just above the Dock (Meta+Shift+5)
    globalThis.__lordKey(125, 1, 'Meta', '', '', 16);
    globalThis.__lordKey(42, 1, 'Shift', '', '', 17);
    globalThis.__lordKey(6, 1, '5', '%', '5', 17);
    globalThis.__lordKey(6, 0, '5', '', '', 17);
    globalThis.__lordKey(42, 0, 'Shift', '', '', 16);
    globalThis.__lordKey(125, 0, 'Meta', '', '', 0);
    await sleep(2000);
    const opts = textId(sdk.t('screenshots.options'));
    step('the toolbar shows', !!opts);
    if (!opts) { log('done'); return; }
    const anchor = box(up(opts, 1)) || box(opts);
    log(`bar options at ${anchor.map(Math.round).join(',')}, work area ${work ? [work.x, work.y, work.w, work.h].join(',') : '?'}`);
    // the first open: the box on every frame until it is drawn
    const target = up(opts, 1);
    const [cx, cy] = [anchor[0] + anchor[2] / 2, anchor[1] + anchor[3] / 2];
    ev('move', target, cx, cy); ev('down', target, cx, cy); ev('up', target, cx, cy);
    const timer = sdk.t('screenshots.opt.saveTo');
    const seen = [];
    for (let i = 0; i < 40; i++) {
      await sleep(16);
      const m = menuBox(timer);
      if (m) seen.push({ i, b: m[1].map(Math.round), op: opacityOf(m[0]) });
    }
    log(`menu boxes: ${seen.slice(0, 6).map((s) => `${s.i}:${s.b.join(',')}${s.op != null ? `/o${s.op}` : ''}`).join(' ')}`);
    const last = seen[seen.length - 1];
    step('the menu opens', !!last);
    if (last) {
      const [x, y, w, h] = last.b;
      step('the menu is above the bar', y + h <= anchor[1] + 1, `menu bottom ${y + h}, bar top ${Math.round(anchor[1])}`);
      const bottom = work ? work.y + work.h : (lord.screen ? lord.screen.height : 800);
      step('the menu is inside the work area (not under the Dock)', y + h <= bottom && y >= (work ? work.y : 0) - 1, `menu ${y}..${y + h}, area bottom ${bottom}`);
      // every frame where it was laid out below the bar was invisible (opacity 0), so nothing was drawn there
      const below = seen.filter((s) => s.b[1] > anchor[1] && s.op !== 0);
      step('it never showed below the bar first', below.length === 0, below.map((s) => `${s.i}:${s.b.join(',')}/o${s.op}`).join(' '));
      log(`menu at ${x},${y} ${w}x${h}`);
    }
    await shot('menu-first-open');
    // closed and opened again: the same place
    ev('move', target, cx, cy); ev('down', target, cx, cy); ev('up', target, cx, cy);
    await sleep(500);
    ev('move', target, cx, cy); ev('down', target, cx, cy); ev('up', target, cx, cy);
    await sleep(800);
    const again = menuBox(timer);
    step('opened again, the same place', !!again && last && again[1].map(Math.round).join(',') === last.b.join(','), again ? again[1].map(Math.round).join(',') : '');
    await shot('menu-second-open');
    log('done');
  })();
})();
