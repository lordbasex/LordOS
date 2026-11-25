// driver.js — goes after the theme bundle in tests/app_scene_headless.sh ws_topbar_scene (run with the workspaces
// theme; SCENE_SIZE sets the screen): four activities in Lord Workspaces' top band, then checks that the activity strip
// and the status cluster are both on screen and never overlap (Arabic at 1600 px wide put the strip over the clock),
// and the accent is under the activity in view, with a screenshot. Lines start with "APP-SCENE" in the shell log.
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const log = (s) => lord.log(`APP-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { try { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; } catch (e) { return null; } };
  const step = (what, ok, extra) => log(`${what} ${ok ? 'done' : 'missing'}${extra ? ` (${extra})` : ''}`);
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(600);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  const check = (label) => {
    const W = lord.screen.width;
    const b = globalThis.__wsBand().map(box).filter(Boolean).sort((p, q) => p[0] - q[0]);
    const txt = b.map((x) => x.map(Math.round).join(',')).join(' | ');
    step(`${label}: the activity strip and the status cluster are drawn`, b.length === 2, txt);
    if (b.length !== 2) return;
    const [a, c] = b;
    step(`${label}: both inside the screen`, a[0] >= 0 && c[0] + c[2] <= W + 0.5, txt);
    step(`${label}: they don't overlap`, a[0] + a[2] <= c[0] + 0.5, txt);
    // the accent lies under the activity in view (counted from the strip's start edge: the right one in RTL)
    const rtl = sdk.direction() === 'rtl';
    const pills = globalThis.__wsPills().map(box).filter(Boolean).sort((p, q) => (rtl ? q[0] - p[0] : p[0] - q[0]));
    const g = globalThis.__wsGlider().map(box).filter(Boolean)[0];
    const want = pills[sdk.desktopList.get().indexOf(sdk.activeDesktop.get())];
    step(`${label}: the accent is under the activity in view`, !!g && !!want && Math.abs(g[0] - want[0]) <= 1 && Math.abs(g[2] - want[2]) <= 1,
      `accent ${g ? g.map(Math.round).join(',') : '-'}, pill ${want ? want.map(Math.round).join(',') : '-'}`);
  };
  (async () => {
    await sleep(3000);
    sdk.setDesktopCount(4);
    await sleep(2500);
    check('four activities');
    await shot('four');
    sdk.setDesktopCount(9);
    await sleep(2500);
    check('nine activities');
    await shot('nine');
    log('done');
  })();
})();
