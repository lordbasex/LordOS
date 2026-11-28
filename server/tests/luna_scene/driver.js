// driver.js — goes after the theme bundle in tests/app_scene_headless.sh luna_scene (run it with the luna theme):
// Luna, the demo of style packs and app skins. Files as the explorer of its era (toolbar, address bar, task
// pane, icons and details, the context menu), the start menu, Settings as a page of categories with a task pane, and
// Settings → Appearance's "From this style" switches;
// then layouts turned off there brings Files' own regions back. Lines start with "APP-SCENE".
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__appScene;
  const log = (s) => lord.log(`APP-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const ev = (type, id, x, y, b = 0) => globalThis.__lordEvent(type, id, x, y, b);
  const textId = (label) => { for (const [raw, s] of P.texts) if (s === label) { const id = P.parent.get(raw); if (id && box(id)) return id; } return null; };
  const shownTexts = () => { const out = []; for (const [raw, s] of P.texts) { const id = P.parent.get(raw); if (id && box(id)) out.push(s); } return out; };
  const step = (what, ok, extra) => log(`${what} ${ok ? 'done' : 'missing'}${extra ? ` (${extra})` : ''}`);
  const click = (id, b = 0) => { const r = box(id); const x = r[0] + r[2] / 2, y = r[1] + r[3] / 2; ev('move', id, x, y); ev('down', id, x, y, b); ev('up', id, x, y, b); };
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(900);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  const win = (app) => (sdk.windows.get() || []).find((w) => w.app === app);
  (async () => {
    await sleep(2500);
    const home = lord.fs.home();
    step('the start button', !!textId(sdk.t('shell.luna.start')));
    sdk.wm.open('files', { new: true, args: { path: `${home}/Documents`, view: 'icons', nonce: Date.now() } });
    await sleep(2500);
    const w = win('files');
    if (w) { sdk.wm.resize(w.id, 900, 600); sdk.wm.setFrame?.(w.id, { x: 60, y: 40, w: 900, h: 600 }); }
    await sleep(1500);
    const all = shownTexts();
    step('Files: the task pane', all.includes(sdk.t('files.skin.tasks')) && all.includes(sdk.t('files.skin.otherPlaces')));
    step('Files: the address bar', all.includes(sdk.t('files.skin.address')) && all.includes(sdk.t('files.menu.go')));
    step('Files: the items', all.includes('list.txt') && all.includes('Letters'));
    const item = textId('manual.pdf');
    if (item) { const r = box(item); ev('move', item, r[0] + 4, r[1] + 4); ev('down', item, r[0] + 4, r[1] + 4); ev('up', item, r[0] + 4, r[1] + 4); }
    await sleep(600);
    step('Files: Details shows the selected file', shownTexts().filter((s) => s === 'manual.pdf').length >= 2);
    await shot('files-icons');
    const views = textId(sdk.t('files.view.icons'));
    if (views) click(views);
    await sleep(1200);
    step('Files: the details view', !!textId(sdk.t('files.col.size')));
    await shot('files-details');
    const row = textId('notes.md');
    if (row) { const r = box(row); ev('move', row, r[0] + 4, r[1] + 4); ev('down', row, r[0] + 4, r[1] + 4, 2); ev('up', row, r[0] + 4, r[1] + 4, 2); globalThis.__lordEvent('contextmenu', row, r[0] + 4, r[1] + 4, 2); }
    await sleep(800);
    step('Files: the context menu', !!textId(sdk.t('files.menu.trash')));
    await shot('files-menu');
    globalThis.__lordKey(1, 1, 'Escape', '', '', 0); globalThis.__lordKey(1, 0, 'Escape', '', '', 0);
    await sleep(500);
    // the start menu
    const start = textId(sdk.t('shell.luna.start'));
    if (start) click(start);
    await sleep(1200);
    step('start menu: the two columns and the footer', !!textId('Alex Demo') && !!textId(sdk.t('files.place.documents')) && !!textId(sdk.t('shell.menu.logout'))
      && !!textId(sdk.t('shell.menu.shutdown')) && !!textId(sdk.t('shell.classic.allApps')));
    await shot('start-menu');
    globalThis.__lordKey(1, 1, 'Escape', '', '', 0); globalThis.__lordKey(1, 0, 'Escape', '', '', 0);
    await sleep(600);
    // Settings: the categories first (the skin's home), then Appearance from its link: the parts this style brings
    sdk.wm.open('settings');
    await sleep(2500);
    const sw = win('settings');
    if (sw) { sdk.wm.resize(sw.id, 900, 700); sdk.wm.setFrame?.(sw.id, { x: 320, y: 20, w: 900, h: 700 }); }
    await sleep(1500);
    step('Settings: the categories', !!textId(sdk.t('settings.skin.group.look')) && shownTexts().filter((x) => x === sdk.t('settings.skin.pick')).length >= 2);
    await shot('settings-home');
    const look = textId(sdk.t('settings.section.appearance'));
    if (look) click(look);
    await sleep(2000);
    const title = textId(sdk.t('settings.themeParts.title'));
    if (title) { const r = box(title); for (let i = 0; i < 12 && r[1] > 500; i++) { ev('wheel', title, r[0] + 5, 300, 3); await sleep(60); } }
    await sleep(600);
    const parts = shownTexts();
    step('Settings: From this style', !!textId(sdk.t('settings.themeParts.title')));
    step('Settings: every part', ['icons', 'cursors', 'sounds', 'fonts', 'layouts'].every((p) => parts.includes(sdk.t(`settings.themeParts.${p}`))));
    await shot('settings-parts');
    // layouts off: Files' own regions come back
    sdk.settings.set({ themeParts: { ...(sdk.settings.get().themeParts || {}), layouts: false } });
    if (w) sdk.wm.focus(w.id);
    await sleep(1500);
    const own = shownTexts();
    step('layouts off: Files draws its own sidebar', own.includes(sdk.t('files.place.recent')) || own.includes(sdk.t('files.favorites')), sdk.t('files.favorites'));
    await shot('files-own-layout');
    sdk.settings.set({ themeParts: { ...(sdk.settings.get().themeParts || {}), layouts: true } });
    await sleep(500);
    // the Terminal: the command row and the property-sheet tabs
    sdk.wm.open('terminal', { new: true });
    await sleep(2500);
    const tw = win('terminal');
    if (tw) sdk.wm.setFrame?.(tw.id, { x: 200, y: 120, w: 760, h: 460 });
    await sleep(1200);
    step('Terminal: the command row', !!textId(sdk.t('terminal.action.newTab')) && !!textId(sdk.t('terminal.action.find')));
    await shot('terminal');
    // the Text Editor: the command row, the tabs and the status panels
    sdk.wm.open('editor', { new: true, args: { path: `${home}/Documents/notes.md` } });
    await sleep(2500);
    const ew = win('editor');
    if (ew) sdk.wm.setFrame?.(ew.id, { x: 260, y: 60, w: 760, h: 480 });
    await sleep(1200);
    step('Text Editor: the command row and the status', !!textId(sdk.t('editor.cmd.save')) && !!textId('LF'));
    await shot('editor');
    // the Store: the column of big buttons and the toolbar
    sdk.wm.open('store', { new: true });
    await sleep(3000);
    const stw = win('store');
    if (stw) sdk.wm.setFrame?.(stw.id, { x: 140, y: 40, w: 900, h: 600 });
    await sleep(1500);
    step('Store: the pages column', !!textId(sdk.t('store.page.discover')) && !!textId(sdk.t('store.src.debian')));
    await shot('store');
    // the demo identity is the only one on screen (pre.js)
    const R = globalThis.__sceneReal || {};
    // what React shows, and what every terminal session's screen shows (its prompt is the shell's)
    const screens = []; try { for (const id of lord.term.list() || []) screens.push(String(lord.term.text(id, 'screen') || '')); } catch (e) { /* no sessions */ }
    // the theme's own credited author (theme.json, public on purpose) is not the account: a name equal to it is left out
    let author = ''; try { author = String(sdk.themes.info()?.author || ''); } catch (e) { author = ''; }
    const leaks = [...shownTexts(), ...screens].filter((x) => [R.user, R.fullName, R.hostname, ...(R.disks || []), ...(R.hardware || [])].some((v) => v && v.length > 2 && v !== author && x.includes(v)));
    step('privacy: no real account or host name on screen', !leaks.length && lord.system.info().hostname === 'demo-pc', leaks.length ? `leaked in: ${leaks.slice(0, 3).map((x) => x.slice(0, 60)).join(' | ')}` : '');
    log('done');
  })();
})();
