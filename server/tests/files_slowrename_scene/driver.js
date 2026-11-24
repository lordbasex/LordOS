// driver.js — goes after the theme bundle in tests/app_scene_headless.sh files_slowrename_scene: Files' slow second
// click on a selected item's name renames it in place (sdk/slowclick.js), driven the way a mouse does it, in the icon,
// list and column views: the slow click (the field opens; a letter and Enter rename the file on disk), a double click,
// a click on the icon instead of the name, a drag, Ctrl held, two items selected, too late, and a first click that only
// brought the window forward. "Is the rename field open" is sdk.hasTextFocus() (the field takes the keys). Lines start
// with "APP-SCENE" in the shell log.
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__appScene;
  const log = (s) => lord.log(`APP-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const ev = (type, id, x, y, b = 0) => globalThis.__lordEvent(type, id, x, y, b);
  const textId = (label) => { let best = null; for (const [raw, s] of P.texts) if (s === label) { const id = P.parent.get(raw); if (id && box(id) && (!best || id > best)) best = id; } return best; };
  const nameId = (base) => textId(`${base}.txt`) || textId(base);
  const up = (id, n) => { let x = id; for (let i = 0; i < n && x; i++) x = P.parent.get(x); return x; };
  const step = (what, ok, extra) => log(`${what} ${ok ? 'done' : 'missing'}${extra ? ` (${extra})` : ''}`);
  const ctrl = (on) => globalThis.__lordKey(29, on ? 1 : 0, 'Control', '', '', on ? 2 : 0);
  const key = (code, k, chr) => { globalThis.__lordKey(code, 1, k, chr, chr || k, 0); globalThis.__lordKey(code, 0, k, '', '', 0); };
  // "z.txt": the whole name is selected, and a new extension would ask first (Files → Settings → Advanced)
  const type = async (s) => { for (const ch of s) { globalThis.__lordKey(30, 1, ch, ch, ch, 0); await sleep(8); globalThis.__lordKey(30, 0, ch, '', '', 0); await sleep(8); } };
  const editing = () => !!sdk.hasTextFocus();
  const exists = (p) => { try { lord.fs.stat(p); return true; } catch (e) { return false; } };
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(400);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  const center = (id) => { const b = box(id); return [b[0] + b[2] / 2, b[1] + b[3] / 2]; };
  const click = (id, at) => { const p = at || center(id); ev('move', id, ...p); ev('down', id, ...p); ev('up', id, ...p); return p; };
  const files = () => (sdk.windows.get() || []).filter((w) => w.app === 'files' || String(w.id).startsWith('files'));
  const closeAll = () => { for (const x of sdk.windows.get() || []) sdk.wm.close(x.id); };      // the Editor a double click opened too
  const open = async (view) => {
    await escape();
    closeAll();
    await sleep(800);
    sdk.wm.open('files', { new: true, args: { path: `${lord.fs.home()}/Slow`, view, nonce: Date.now() } });
    await sleep(2500);
    const w = files().pop();
    if (w) { sdk.wm.resize(w.id, 980, 640); sdk.wm.center(w.id); sdk.wm.focus(w.id); }
    await sleep(1500);
    return w;
  };
  const escape = async () => { if (editing()) { key(1, 'Escape', ''); await sleep(400); } };
  // the slow click on a name: a first click, a pause past the double-click interval, a second click; then the wait
  const slowClick = async (base, gap = 800) => { const id = nameId(base); if (!id) return false; click(id); await sleep(gap); click(nameId(base)); await sleep(800); return true; };
  // the slow click renames: the field opens, "z" + Enter renames the file on disk to <view>-z
  const renames = async (view, base, shotName) => {
    const drawn = await slowClick(base);
    const open1 = editing();
    if (shotName) await shot(shotName);
    if (open1) { await type('z.txt'); await sleep(150); key(28, 'Enter', ''); await sleep(900); }
    try { log(`listing: ${lord.fs.list(`${lord.fs.home()}/Slow`).map((x) => x.name).join(' ')}`); } catch (e) { log(`list ${e.message}`); }
    const done = exists(`${lord.fs.home()}/Slow/z.txt`) || exists(`${lord.fs.home()}/Slow/z`);
    step(`${view}: the slow second click on ${base}'s name opens the rename field and renames it`, drawn && open1 && done, `drawn ${drawn}, field ${open1}, renamed ${done}`);
    try { lord.fs.rename(`${lord.fs.home()}/Slow/${exists(`${lord.fs.home()}/Slow/z.txt`) ? 'z.txt' : 'z'}`, `${lord.fs.home()}/Slow/${view}-z.txt`); } catch (e) { /* not renamed */ }
    await sleep(600);
  };
  (async () => {
    await sleep(2500);
    for (const view of (globalThis.SCENE_VIEWS || ['icons', 'list', 'columns'])) {
      await open(view);
      await renames(view, view === 'icons' ? 'alpha' : view === 'list' ? 'bravo' : 'charlie', `${view}-renaming`);
      if (view !== 'icons') continue;
      // a real double click: the second press within the interval opens (the text file opens in its app); no field
      let id = nameId('delta');
      click(id); await sleep(800); click(nameId('delta')); await sleep(90); click(nameId('delta')); await sleep(900);
      step('icons: a double click on a selected name opens it, no rename', !editing());
      await escape();
      await open('icons');
      // the icon, not the name
      id = nameId('echo');
      const card = up(id, 2), cb = box(card);
      const iconAt = [cb[0] + cb[2] / 2, cb[1] + 20];
      click(card, iconAt); await sleep(800); click(card, iconAt); await sleep(800);
      step('icons: a slow second click on the icon (not the name) doesn\'t rename', !editing());
      await escape();
      // too late
      await slowClick('foxtrot', 1900);
      step('icons: a second click after 1.5 s doesn\'t rename', !editing());
      await escape();
      // a drag: the second press moves 40 px before the release
      id = nameId('golf');
      click(id); await sleep(800);
      { const p = center(nameId('golf')); ev('move', id, ...p); ev('down', id, ...p); for (let i = 1; i <= 6; i++) { ev('move', id, p[0] + i * 7, p[1] + i * 3); await sleep(30); } ev('up', 0, p[0] + 42, p[1] + 18); }
      await sleep(900);
      step('icons: a drag from a selected name drags, no rename', !editing());
      await escape();
      // Ctrl held on the second click
      id = nameId('hotel');
      click(id); await sleep(800); ctrl(true); click(nameId('hotel')); ctrl(false); await sleep(800);
      step('icons: the second click with Ctrl held doesn\'t rename', !editing());
      await escape();
      // two items selected: hotel, then Ctrl+india, then a slow click on india's name
      click(nameId('hotel')); await sleep(600); ctrl(true); click(nameId('india')); ctrl(false); await sleep(800); click(nameId('india')); await sleep(800);
      step('icons: with two items selected a slow click doesn\'t rename', !editing());
      await shot('icons-two-selected');
      await escape();
      // the first click only focused the window: another window on top, then two clicks; a third click renames
      await open('icons');
      const fw = files().pop();
      sdk.wm.open('calculator', { new: true });
      await sleep(2000);
      step('icons: another window has the focus', sdk.wm.focused() !== fw.id, `focused ${sdk.wm.focused()}`);
      click(nameId('delta')); await sleep(800);
      log(`focused after the first click: ${sdk.wm.focused()} (files ${fw.id})`);
      click(nameId('delta')); await sleep(800);
      step('icons: a first click that only focused the window doesn\'t count', !editing());
      click(nameId('delta')); await sleep(800);
      const third = editing();
      step('icons: then the next slow click renames', third);
      await shot('icons-after-focus-click');
      await escape();
      for (const x of sdk.windows.get() || []) if (x.app === 'calculator') sdk.wm.close(x.id);
      await sleep(500);
    }
    log('done');
  })();
})();
