// driver.js — goes after the theme bundle in tests/app_scene_headless.sh theme_tour_scene (a test tool, never shipped).
// The theme tour of lordos-theme-kit (its scenes/theme_tour/driver.js, approved for LordOS 2026-11-26): it shows any
// theme in themes/<id> (a community one too) the way a reviewer looks at it, and keeps a screenshot of each step:
//   1 desktop          the empty desktop (bars, Dock, wallpaper)
//   2 windows          Text Editor (in front: its menus) and Calculator: a focused and an unfocused frame
//   3 sidebar          Characters: sidebarBg, a search field, selection (the token contract with the apps)
// With globalThis.THEME_TOUR_FULL === true (never set here), also, because they show the person's name, user, host name
// and disks (review those before sharing):
//   4 settings         Settings → Appearance (the theme's card, "Community · unofficial")
//   5 files-terminal   Files and the Terminal (a transparent app's frame)
// Lines start with "APP-SCENE" in the shell log: each step says "done" or "missing" (an app that didn't open, a
// screenshot that wasn't saved), and "done" ends the tour. Same screenshot calls as the other scenes.
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const FULL = globalThis.THEME_TOUR_FULL === true;
  const log = (s) => lord.log(`APP-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const step = (what, ok) => log(`${what} ${ok ? 'done' : 'missing'}`);
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(900);
    let ok = false;
    try {
      const id = await sdk.media.screenshot('screenshots');
      const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
      ok = !!(r && r.ok !== false);
    } catch (e) { log(`shot ${n} ${name} error ${e && e.message}`); }
    step(`shot ${n} ${name}`, ok);
  };
  const win = (app) => (sdk.windows.get() || []).find((x) => x.app === app);
  const open = async (app, args) => {
    sdk.wm.open(app, args ? { args } : undefined);
    for (let i = 0; i < 20 && !win(app); i++) await sleep(250);
    await sleep(1200);
    const w = win(app);
    step(`open ${app}`, !!w);
    return w;
  };
  const place = (w, fx, fy, fw, fh) => {
    const W = lord.screen.width, H = lord.screen.height;
    if (w) sdk.wm.setFrame(w.id, { x: Math.round(W * fx), y: Math.round(H * fy), w: Math.round(W * fw), h: Math.round(H * fh) });
  };
  const closeAll = async () => { for (const x of sdk.windows.get() || []) sdk.wm.close(x.id); await sleep(700); };
  (async () => {
    try {
      await sleep(3500);
      log(`screen ${lord.screen.width}x${lord.screen.height} direction ${sdk.direction()} full ${FULL}`);
      await shot('desktop');
      // apps that show nothing about the person or the machine
      const ed = await open('editor');
      const calc = await open('calculator');
      place(ed, 0.05, 0.1, 0.55, 0.62);
      place(calc, 0.64, 0.16, 0.26, 0.56);
      if (ed) sdk.wm.focus(ed.id);
      await shot('windows');
      await closeAll();
      const ch = await open('characters');
      place(ch, 0.12, 0.1, 0.76, 0.7);
      await shot('sidebar');
      if (FULL) {
        await closeAll();
        const s = await open('settings', { section: 'appearance', nonce: 1 });
        place(s, 0.1, 0.08, 0.8, 0.78);
        await shot('settings');
        await closeAll();
        const f = await open('files');
        const t = await open('terminal');
        place(f, 0.06, 0.12, 0.5, 0.62);
        place(t, 0.48, 0.22, 0.44, 0.5);
        await shot('files-terminal');
      }
    } catch (e) {
      log(`error missing (${e && e.message})`);
    }
    log('done');
  })();
})();
