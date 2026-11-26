// driver.js — goes after the theme bundle in tests/app_scene_headless.sh bluebell_apps_scene (run it with the bluebell
// theme): each app Bluebell skins with the shared shapes (a command bar, a list pane), opened alone, checked by a text its
// skin draws and screenshot. The same demo identity as bluebell_scene (pre.js), and the same check that the real account
// and computer never show. Lines start with "APP-SCENE".
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__appScene;
  const log = (s) => lord.log(`APP-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const shownTexts = () => { const out = []; for (const [raw, s] of P.texts) { const id = P.parent.get(raw); if (id && box(id)) out.push(s); } return out; };
  const step = (what, ok, extra) => log(`${what} ${ok ? 'done' : 'missing'}${extra ? ` (${extra})` : ''}`);
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(900);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  // [app, args, a text its skin draws (or (sdk) => it)]: one row per app skinned with the shared shapes
  const APPS = [
    ['notes', {}, (sdk) => sdk.t('notes.new')],
    ['calculator', {}, '÷'],
    ['photos', {}, (sdk) => sdk.t('photos.refresh')],
    ['player', {}, (sdk) => sdk.t('player.nav.songs')],
    ['monitor', {}, (sdk) => sdk.t('monitor.tab.overview')],
    ['clock', {}, (sdk) => sdk.t('clock.tab.stopwatch')],
    ['screenshots', { shortcut: 'capture.panel' }, (sdk) => sdk.t('screenshots.capture')],
  ];
  (async () => {
    await sleep(2500);
    for (const [app, args, text] of APPS) {
      // an app shown its own way (Screenshots' capture bar) is opened by its shortcut and keeps its own frame
      const own = args && args.shortcut;
      const id = own ? (sdk.runShortcut(args.shortcut), null) : sdk.wm.open(app, { new: true, args: args || {} });
      await sleep(2800);
      const w = (sdk.windows.get() || []).find((x) => x.app === app);
      if (w && !own) sdk.wm.setFrame?.(w.id, { x: 120, y: 40, w: 900, h: 600 });
      await sleep(1500);
      const texts = shownTexts();
      step(`${app}: its skin is drawn`, texts.includes(typeof text === 'function' ? text(sdk) : text));
      await shot(app);
      if (w) sdk.wm.close(w.id, true); else if (id) sdk.wm.close(id, true);
      await sleep(600);
    }
    const R = globalThis.__sceneReal || {};
    let author = ''; try { author = String(sdk.themes.info()?.author || ''); } catch (e) { author = ''; }
    const screens = []; try { for (const id of lord.term.list() || []) screens.push(String(lord.term.text(id, 'screen') || '')); } catch (e) { /* none */ }
    const leaks = [...shownTexts(), ...screens].filter((x) => [R.user, R.fullName, R.hostname].some((v) => v && v.length > 2 && v !== author && x.includes(v)));
    step('privacy: no real account or host name on screen', !leaks.length, leaks.length ? `leaked in: ${leaks.slice(0, 3).map((x) => x.slice(0, 60)).join(' | ')}` : '');
    log('done');
  })();
})();
