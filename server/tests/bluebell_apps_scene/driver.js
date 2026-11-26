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
  // what the real account, computer and hardware are (pre.js) must never be on screen: checked on each app's screen
  const leaks = [];
  const leaksIn = (where) => {
    const R = globalThis.__sceneReal || {};
    let author = ''; try { author = String(sdk.themes.info()?.author || ''); } catch (e) { author = ''; }
    const screens = []; try { for (const id of lord.term.list() || []) screens.push(String(lord.term.text(id, 'screen') || '')); } catch (e) { /* none */ }
    for (const x of [...shownTexts(), ...screens]) {
      if ([R.user, R.fullName, R.hostname, ...(R.disks || []), ...(R.hardware || [])].some((v) => v && v.length > 2 && v !== author && x.includes(v))) leaks.push(`${where}: ${x}`);
    }
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
    ['disks', {}, (sdk) => sdk.t('disks.title')],
    ['about', {}, 'LordOS'],
    ['help', {}, (sdk) => sdk.t('help.tour.open')],
    ['characters', {}, (sdk) => sdk.t('characters.tab.symbols')],
    ['fonts', {}, (sdk) => sdk.t('fonts.coll.mono')],
    ['logs', {}, (sdk) => sdk.t('logs.src.kernel')],
    ['sysinfo', {}, (sdk) => sdk.t('sysinfo.tab.hardware')],
    ['backup', {}, (sdk) => sdk.t('backup.tabSettings')],
    ['pdf', {}, (sdk) => sdk.t('pdf.tool.ink')],
    ['forcequit', {}, (sdk) => sdk.t('forcequit.quit')],
    ['appabout', { key: 'files' }, (sdk) => sdk.t('apps.files')],
    ['scanner', {}, (sdk) => sdk.t('apps.scanner')],
    ['recorder', {}, (sdk) => sdk.t('apps.recorder')],
    ['imager', {}, (sdk) => sdk.t('apps.imager')],
    ['databases', {}, (sdk) => sdk.t('apps.databases')],
    ['machines', {}, (sdk) => sdk.t('apps.machines')],
    ['cleaner', {}, (sdk) => sdk.t('apps.cleaner')],
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
      leaksIn(app);                     // checked on every app's own screen, not only at the end
      await shot(app);
      if (w) sdk.wm.close(w.id, true); else if (id) sdk.wm.close(id, true);
      await sleep(600);
    }
    leaksIn('end');
    step('privacy: no real account or host name on screen', !leaks.length, leaks.length ? `leaked in: ${leaks.slice(0, 4).map((x) => x.slice(0, 70)).join(' | ')}` : '');
    log('done');
  })();
})();
