// driver.js — goes after the theme bundle in tests/app_scene_headless.sh about_labels_scene: About's list of facts in
// the language the scene runs in (run it in es, de, ar and fi, the long ones), at its own size and in a narrow window.
// Each label must be drawn whole: on one line it is no wider than its box ("Pausas de más de 50 ms" was cut to
// "sas de más de 50 ms" in a fixed 118 px column), wrapped its longest word fits, and the box stays inside its row and
// the row inside the list. Lines start with "APP-SCENE" in the shell log; a screenshot of each size.
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__appScene;
  const log = (s) => lord.log(`APP-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { try { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; } catch (e) { return null; } };
  const step = (what, ok, extra) => log(`${what} ${ok ? 'done' : 'missing'}${extra ? ` (${extra})` : ''}`);
  const KEYS = ['os', 'kernel', 'host', 'cpu', 'memory', 'case', 'uptime', 'bootTime', 'server', 'engine', 'codeLabel', 'display', 'ram', 'render', 'pauses'];
  const SIZE = 12;   // typeScale.callout
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(800);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  const front = async (w, h) => {
    for (const x of sdk.windows.get() || []) if (x.app !== 'about') sdk.wm.close?.(x.id);
    if (!(sdk.windows.get() || []).some((x) => x.app === 'about')) { sdk.wm.open('about'); await sleep(2500); }
    const win = (sdk.windows.get() || []).find((x) => x.app === 'about');
    if (win) { sdk.wm.resize(win.id, w, h); sdk.wm.center(win.id); }
    await sleep(2000);
  };
  const inside = (a, b) => a && b && a[0] >= b[0] - 0.5 && a[0] + a[2] <= b[0] + b[2] + 0.5;
  const check = (label) => {
    let seen = 0, bad = [];
    for (const k of KEYS) {
      const s = sdk.t(`about.${k}`);
      // the string is a raw node inside the Text node, which has the box; then the row and the list
      const ids = [...P.texts].filter(([id, x]) => x === s && box(P.parent.get(id))).map(([id]) => P.parent.get(id));
      if (!ids.length) continue;
      seen++;
      for (const id of ids) {
        const b = box(id), row = box(P.parent.get(id)), list = box(P.parent.get(P.parent.get(id)));
        const whole = lord.measureText(s, SIZE, 400);
        const oneLine = b[3] < SIZE * 2;
        const longestWord = Math.max(...s.split(/\s+/).map((w) => lord.measureText(w, SIZE, 400)));
        const fits = oneLine ? whole <= b[2] + 0.5 : longestWord <= b[2] + 0.5;
        if (!fits || !inside(b, row) || !inside(row, list)) bad.push(`${k} "${s}" text ${Math.round(whole)} box ${b.map(Math.round).join(',')} row ${row ? row.map(Math.round).join(',') : '-'}`);
      }
    }
    step(`${label}: the labels are on screen`, seen >= 12, `${seen} labels`);
    step(`${label}: no label is cut`, !bad.length, bad.join('; '));
  };
  (async () => {
    await sleep(2500);
    log(`language ${sdk.t('about.pauses')} (${sdk.direction()})`);
    await front(490, Math.min(730, lord.screen.height - 70));
    check('its own size');
    await shot('about');
    await front(360, Math.min(730, lord.screen.height - 70));
    check('narrow');
    await shot('about-narrow');
    log('done');
  })();
})();
