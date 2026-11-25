// driver.js — goes after the theme bundle in tests/app_scene_headless.sh notes_pick_scene (0.8.9.6.2): a note written
// into ~/Notes by another program shows on the desktop by itself (lord.fs.watch, no window open); the widget catalog's
// sticky note offers New Note or a note already made (searchable) and puts the one picked where the widget is added;
// On desktop brings a note saved off every display onto the window's display; the round checklist circles in every
// paper colour, ticked with a click. Lines start with "APP-SCENE" in the shell log.
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__appScene;
  const T = globalThis.L_notes || {}, SH = globalThis.L_shell || {};
  const log = (s) => lord.log(`APP-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { let l = null; try { l = lord.getLayout(id); } catch (e) { l = null; } return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const ev = (type, id, x, y, b = 0) => globalThis.__lordEvent(type, id, x, y, b);
  const at = (id) => { const l = box(id); return l ? [l[0] + l[2] / 2, l[1] + l[3] / 2] : null; };
  const click = (id, b = 0) => { const p = id && at(id); if (!p) return false; ev('move', id, ...p, b); ev('down', id, ...p, b); ev('up', id, ...p, b); return true; };
  const all = (text) => { const out = []; for (const [raw, s] of P.texts) if (s.trim() === text) { const id = P.parent.get(raw); if (id && box(id)) out.push(id); } return out.sort((a, b) => b - a); };
  const byText = (text) => all(text)[0] || null;
  const has = (text) => all(text).length > 0;
  const type = async (s) => { for (const ch of s) { globalThis.__lordKey(30, 1, ch, ch, ch.toLowerCase(), 0); await sleep(8); globalThis.__lordKey(30, 0, ch, '', '', 0); await sleep(8); } };
  const step = (what, ok, extra) => log(`${what} ${ok ? 'done' : 'missing'}${extra ? ` (${extra})` : ''}`);
  const home = lord.fs.home();
  const read = (p) => { try { return lord.fs.read(p); } catch (e) { return null; } };
  const winOf = (app) => (sdk.windows.get() || []).filter((w) => w.app === app);
  const desk = () => sdk.desktopWidgets.get().note || [];
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(700);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${home}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };

  (async () => {
    await sleep(3000);
    step('the desktop notes of the setup are shown', desk().some((w) => w.id === 'check-yellow') && desk().some((w) => w.id === 'check-blue'));
    // ---- another program writes a note with desktop: true; no window open ----
    step('no Notes window is open', winOf('notes').length === 0);
    lord.fs.mkdir(`${home}/Notes/from-assistant`);
    await sleep(120);
    lord.fs.write(`${home}/Notes/from-assistant/nota.md`, '---\ntitle: From the assistant\ncolor: blue\nx: 820\ny: 80\nw: 300\nh: 200\ndesktop: true\n---\n# From the assistant\n- [ ] Buy yerba\n- [x] Call Ana\n');
    const t0 = Date.now();
    let seen = false;
    while (Date.now() - t0 < 3000) { if (desk().some((w) => w.id === 'from-assistant')) { seen = true; break; } await sleep(50); }
    step('a note written elsewhere shows on the desktop by itself', seen, `${Date.now() - t0} ms`);
    step('… within a second', seen && Date.now() - t0 < 1000);
    await shot('desktop-new-note-by-itself');
    // ---- the catalog: Sticky note → New Note or a note already made ----
    sdk.widgetPicker.set({ display: sdk.mainDisplay(), x: 60, y: 90 });
    await sleep(1200);
    click(byText(SH['widgets.note.name'] || 'Sticky note'));
    await sleep(1500);
    step('the sticky note asks: New Note first', has(T.new || 'New Note'));
    step('… and lists the notes, the newest first', has('From the assistant') && has('List gray') && has('Far away'));
    step('… marking the ones on the desktop', all(T.onDesktop || 'on the desktop').length >= 3);
    await shot('catalog-pick-list');
    await type('purple');
    await sleep(800);
    step('search keeps the matching note', has('List purple') && !has('List gray'));
    await shot('catalog-pick-search');
    click(byText('List purple'));
    await sleep(1500);
    const pw = desk().find((w) => w.id === 'check-purple');
    // (at the place of the add, moved inside the display's work area: Lord Workspaces' rail and top bar)
    step('the note picked goes on the desktop where the widget is added', !!pw && pw.x >= 60 && pw.x < 160 && pw.y >= 90 && pw.y < 160, JSON.stringify(pw));

    step('… keeping its text', (read(`${home}/Notes/check-purple/nota.md`) || '').includes('Open item') && (read(`${home}/Notes/check-purple/nota.md`) || '').includes('desktop: true'));
    await shot('desktop-picked-note');
    // ---- the round checklist: tick an open item on a desktop note ----
    const yw = desk().find((w) => w.id === 'check-yellow');
    // the circles: their 28 px hit targets inside the note, top to bottom (the second one is "Open item")
    const hits = [...P.parent.keys()].map((id) => [id, box(id)]).filter(([, b]) => b && yw && b[2] === 28 && b[3] === 28 && b[0] >= yw.x && b[0] < yw.x + yw.w && b[1] > yw.y && b[1] < yw.y + yw.h)
      .sort((p, q) => p[1][1] - q[1][1]);
    const hit = hits[1] && hits[1][0];
    step('each item has a 28 px hit target', hits.length === 3, JSON.stringify(hits.map((h) => h[1])));
    if (hit) click(hit);
    await sleep(1500);

    step('a click on the circle ticks the item', /- \[x\] Open item/.test(read(`${home}/Notes/check-yellow/nota.md`) || ''));
    step('… and only that line', /- \[x\] Done item/.test(read(`${home}/Notes/check-yellow/nota.md`) || '') && /- \[ \] A longer/.test(read(`${home}/Notes/check-yellow/nota.md`) || ''));
    // ---- On desktop for a note saved off every display ----
    sdk.wm.open('notes', { args: { slug: 'far-away', nonce: 1 } });
    await sleep(2000);
    const nw = winOf('notes')[0];
    if (nw) { sdk.wm.resize(nw.id, 900, 560); sdk.wm.move?.(nw.id, 320, 120); }
    await sleep(1200);
    const fileMenu = (sdk.appMenus.get()[nw?.id] || [])[0];
    const onDesk = fileMenu && fileMenu.items.find((it) => it && it.label === (T.showDesktop || 'On desktop'));
    step('the File menu has Import… after New Note', !!fileMenu && fileMenu.items[1]?.label === (T.import || 'Import…'));
    if (onDesk) onDesk.onClick();
    await sleep(1500);
    const fw = desk().find((w) => w.id === 'far-away');
    step('a note saved off every display comes onto the window\'s display', !!fw && fw.x >= 0 && fw.x < 1280 && fw.y >= 0 && fw.y < 800, JSON.stringify(fw));
    // the window on a checklist: round circles in the reading view
    click(all('List green').pop());
    await sleep(1500);
    await shot('window-checklist');
    click(all('قائمة').pop());
    await sleep(1500);
    await shot('window-checklist-arabic');
    // the toolbar's checklist button: the editor opens and the caret's line becomes "- [ ] "
    click(all('List pink').pop());
    await sleep(1200);
    const before = read(`${home}/Notes/check-pink/nota.md`) || '';
    const tb = all('✓').map((id) => [id, box(id)]).filter(([, b]) => b && b[3] === 14);   // the toolbar glyph's 11 px tick (the circles' ticks are 13 px)
    log(`checklist buttons ${JSON.stringify(tb.map((x) => x[1]))}`);
    click(tb.length ? tb[0][0] : null);
    await sleep(1500);
    await type('typed');
    await sleep(1500);
    const after = read(`${home}/Notes/check-pink/nota.md`) || '';
    step('the checklist button opens the editor and toggles the line', after !== before && after.includes('- [ ] typed'), JSON.stringify(after.split('\n').slice(-6)));
    await shot('window-checklist-button');
    log('done');
  })().catch((e) => { log(`error ${e && e.message} ${e && e.stack}`); log('done'); });
})();
