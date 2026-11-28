// driver.js — goes after the theme bundle in tests/app_scene_headless.sh notes_scene (GitHub #238): Notes checked the
// way a person uses it, with lord-server's own pointer and key paths, on the notes setup.sh made in the scene's home.
// The window: the list and search, the reading view (markdown, the image drawn, the table of contents), what each link
// opens (an attachment alone in its app, never Files on the note's folder; file://, javascript:, absolute paths, ..,
// a link planted in the folder and nota.md open nothing; a remote image is never drawn), ⊕ Emoji… from the reading
// view (the editor opens first), the + New Note button, typing, saving, On Desktop. The sticky note on the desktop: the
// Add widget catalog, typing, the emoji picker whole on the screen beside it, moving, resizing, pinning; and after a
// restart (RESTART=1) that it is where it was. Lines start with "APP-SCENE" in the shell log.
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
  const wins = () => sdk.windows.get() || [];
  const winOf = (app) => wins().filter((w) => w.app === app);
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(700);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${home}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  const W = 'note-20261101-120000', B = 'note-20261101-110000';
  const listNotes = () => { try { return lord.fs.list(`${home}/Notes`).map((e) => e.name); } catch (e) { return []; } };
  const notesOnDesk = () => sdk.desktopWidgets.get().note || [];
  // a click on a link: the windows it opened (closed again afterwards)
  const opens = async (label) => {
    const before = new Set(wins().map((w) => w.id));
    const ok = click(byText(label));
    await sleep(900);
    const fresh = wins().filter((w) => !before.has(w.id));
    for (const w of fresh) sdk.wm.close(w.id, true);
    await sleep(300);
    return { ok, fresh: fresh.map((w) => ({ app: w.app, path: w.args?.path || null, single: !!w.args?.single })) };
  };

  async function restartPart() {
    await sleep(3500);
    const w = notesOnDesk().find((x) => x.w === 360);
    step('after a restart the sticky note is on the desktop where it was', !!w && w.h === 340, JSON.stringify(notesOnDesk()));
    step('after a restart it is still pinned in front', !!w && w.pinned === true);
    await shot('restart');
    log('done');
  }

  async function firstPart() {
    await sleep(2500);
    // ---- the window ----
    sdk.wm.open('notes', { args: { slug: W } });
    await sleep(2500);
    const nw = winOf('notes')[0];
    if (nw) { sdk.wm.resize(nw.id, 1000, 640); sdk.wm.center(nw.id); }
    await sleep(1500);
    const nw2 = winOf('notes')[0];
    step('the list shows the notes, a linked note folder left out', has('Welcome') && has('Hostile links') && has('ملاحظة') && !has('Linked'));
    step('reading view: headings, the table, the task list', has('Lists') && has(T.toc || 'Contents') && has('first'));
    step('the PNG attachment is drawn in the note (no [alt] placeholder)', !has('[lumo]'));
    await shot('window-reading');
    // links: each attachment opens alone, in its app; Files never opens on the note's folder
    const txt = await opens('notes.txt');
    step('a text attachment opens that file alone in Text Editor', txt.fresh.length === 1 && txt.fresh[0].app === 'editor' && txt.fresh[0].path === `${home}/Notes/${W}/notes.txt`, JSON.stringify(txt));
    const pdf = await opens('report.pdf');
    step('a PDF opens in its app with that file', pdf.fresh.length === 1 && pdf.fresh[0].app !== 'files' && pdf.fresh[0].path === `${home}/Notes/${W}/report.pdf`, JSON.stringify(pdf));
    const webp = await opens('photo.webp');
    step('a WebP opens in Photos alone (not its folder)', webp.fresh.length === 1 && webp.fresh[0].app === 'photos' && webp.fresh[0].single, JSON.stringify(webp));
    step('Files never opened', winOf('files').length === 0);
    // search
    click(byText(T.search || 'Search notes'));
    await sleep(300);
    await type('hostile');
    await sleep(800);
    step('search keeps only the matching note', has('Hostile links') && !has('ملاحظة'));
    for (let i = 0; i < 7; i++) { globalThis.__lordKey(14, 1, 'Backspace', '', '', 0); await sleep(10); globalThis.__lordKey(14, 0, 'Backspace', '', '', 0); await sleep(10); }
    await sleep(500);
    // the hostile note: nothing opens, nothing outside is drawn
    click(all('Hostile links').pop());
    await sleep(1200);
    await shot('window-hostile');
    for (const label of ['fileurl', 'abspath', 'dotdot', 'jsurl', 'planted', 'notafile', 'web']) {
      const r = await opens(label);
      step(`hostile link "${label}" opens nothing`, r.ok && r.fresh.length === 0, JSON.stringify(r));
    }
    step('a remote image is not fetched (its [alt] is shown)', has('[remoteimg]'));
    step('an image by absolute path is not drawn', has('[absimg]'));
    step('an image linked out of the folder is not drawn', has('[plantedimg]'));
    // ⊕ → Emoji… from the reading view: the editor opens first, the emoji goes in at its caret
    click(byText('⊕'));
    await sleep(700);
    click(byText(T.insertEmoji || 'Emoji…'));
    await sleep(1200);
    step('⊕ Emoji… from the reading view opens the editor', has('# Hostile links'));
    await shot('window-emoji');
    click(all('😀').pop());
    await sleep(1500);
    step('the emoji is saved in the note', (read(`${home}/Notes/${B}/nota.md`) || '').includes('😀'));
    // + New Note (the + in the sidebar, beside the search field)
    const before = listNotes().length;
    // (beside the search field: at its left in English, at its right in Arabic)
    const sfb = box(byText(T.search || 'Search notes'));
    const plus = all('+').find((id) => { const b = box(id); return b && sfb && Math.abs(b[1] + b[3] / 2 - (sfb[1] + sfb[3] / 2)) < 16; });
    step('the sidebar has a + New Note button', !!plus);
    click(plus);
    await sleep(1200);
    await type('Typed in the window');
    await sleep(1500);
    step('the + button makes a new note', listNotes().length === before + 1, `${before} → ${listNotes().length}`);
    const newest = listNotes().filter((x) => x.startsWith('note-')).sort().pop();
    step('what was typed is saved', (read(`${home}/Notes/${newest}/nota.md`) || '').includes('Typed in the window'));
    await shot('window-new');
    // Duplicate, then Delete the copy (File menu; Delete asks first)
    const fileItem = (label) => ((sdk.appMenus.get()[nw2?.id] || [])[0]?.items || []).find((it) => it && it.label === label);
    fileItem(T.duplicate || 'Duplicate')?.onClick();
    await sleep(1200);
    step('Duplicate makes a copy', listNotes().length === before + 2, `${listNotes().length}`);
    fileItem(T.delete || 'Delete')?.onClick();
    await sleep(800);
    await shot('window-delete-ask');
    click(byText(T.delete || 'Delete'));
    await sleep(1200);
    step('Delete (confirmed) takes the copy away', listNotes().length === before + 1, `${listNotes().length}`);
    step('a link in the list shows its words, not its address', !has('[fileurl](file:') && ![...P.texts].some(([raw, s]) => s.includes('file:///') && box(P.parent.get(raw) || raw)));
    // On desktop, from the File menu
    const fileMenu = (sdk.appMenus.get()[nw2?.id] || [])[0];
    const onDesk = fileMenu && fileMenu.items.find((it) => it && it.label === (T.showDesktop || 'On desktop'));
    if (onDesk) onDesk.onClick();
    await sleep(1200);
    step('On desktop puts the note on the desktop', notesOnDesk().some((w) => w.id === newest));
    for (const w of winOf('notes')) sdk.wm.close(w.id, true);
    await sleep(1000);
    await shot('desktop-from-window');

    // ---- the sticky note on the desktop, from the Add widget catalog ----
    sdk.widgetPicker.set({ display: sdk.mainDisplay() });
    await sleep(1200);
    click(byText(SH['widgets.note.name'] || 'Sticky note'));
    await sleep(1200);
    // the catalog's sticky note asks first: a new note or one already made (NoteChooser)
    click(all(T.new || 'New Note').pop());
    await sleep(1500);
    const w = notesOnDesk().find((x) => x.id !== newest);
    step('the catalog puts a sticky note on the desktop', !!w);
    if (!w) { log('done'); return; }
    const emptyText = all(T.empty).find((id) => { const b = box(id); return b && b[0] >= w.x && b[0] < w.x + w.w; });
    if (emptyText) ev('move', emptyText, w.x + 100, w.y + 70);
    await sleep(500);
    const pen = all('✎').find((id) => { const b = box(id); return b && b[0] >= w.x && b[0] < w.x + w.w; });
    click(pen);
    await sleep(800);
    await type('Hello widget');
    await sleep(1500);
    step('typing in the sticky note saves it', (read(`${home}/Notes/${w.id}/nota.md`) || '').includes('Hello widget'));
    await shot('widget-typed');
    // the emoji picker: whole on the screen, beside the note, not cut by it
    const smileBtn = all('😀').find((id) => { const b = box(id); return b && b[1] < w.y + 26; });
    click(smileBtn);
    await sleep(1000);
    const close = byText(T.close || 'Close');
    const cb = close && box(close);
    const inScreen = (b) => b && b[0] >= 0 && b[1] >= 0 && b[0] + b[2] <= 1280 && b[1] + b[3] <= 800;
    step('the widget\'s emoji picker is drawn whole (its Close button on the screen)', !!cb && inScreen(cb), JSON.stringify({ cb, w }));
    step('the picker is not cut by the note (it is drawn past the note\'s edges)', !!cb && (cb[1] + cb[3] > w.y + w.h || cb[0] < w.x), JSON.stringify({ cb }));
    await shot('widget-emoji');
    click(all('😀').filter((id) => id !== smileBtn).pop());
    await sleep(1200);
    step('the picked emoji is in the note', (read(`${home}/Notes/${w.id}/nota.md`) || '').includes('Hello widget😀'));
    // move by the header, resize by the corner
    ev('move', 0, 10, 400);
    await sleep(500);
    // the header: the ✕ button's row (while editing the header shows the buttons, not the title)
    const xBtn = all('✕').find((id) => { const b = box(id); return b && b[0] >= w.x && b[0] < w.x + w.w && b[1] < w.y + 26; });
    const head = xBtn && P.parent.get(P.parent.get(xBtn));
    if (head) {
      ev('move', head, w.x + 120, w.y + 12); ev('down', head, w.x + 120, w.y + 12);
      for (let i = 1; i <= 10; i++) { ev('move', head, w.x + 120 - i * 40, w.y + 12 + i * 20); await sleep(20); }
      ev('up', 0, w.x - 280, w.y + 212);
      await sleep(1200);
    }
    const moved = notesOnDesk().find((x) => x.id === w.id);
    step('the sticky note moves by its header', !!moved && moved.x === w.x - 400 && moved.y === w.y + 200, JSON.stringify(moved));
    const grip = all('◢').find((id) => { const b = box(id); return b && moved && b[0] > moved.x && b[0] < moved.x + moved.w; });
    const g = moved;
    ev('move', grip, g.x + g.w - 6, g.y + g.h - 6); ev('down', grip, g.x + g.w - 6, g.y + g.h - 6);
    for (let i = 1; i <= 10; i++) { ev('move', grip, g.x + g.w - 6 + i * 10, g.y + g.h - 6 + i * 10); await sleep(20); }
    ev('up', 0, g.x + g.w + 94, g.y + g.h + 94);
    await sleep(1200);
    const sized = notesOnDesk().find((x) => x.id === w.id);
    step('the sticky note resizes by its corner', !!sized && sized.w === 360 && sized.h === 340, JSON.stringify(sized));
    const hdr = read(`${home}/Notes/${w.id}/nota.md`) || '';
    step('its place and size are saved in the note', hdr.includes('w: 360') && hdr.includes('h: 340'));
    // pin it in front of the windows
    const anyText = all('Hello widget😀').pop();
    if (anyText) ev('move', anyText, sized.x + 100, sized.y + 80);
    await sleep(500);
    click(all('📍').find((id) => { const b = box(id); return b && b[0] >= sized.x && b[0] < sized.x + sized.w; }));
    await sleep(900);
    step('📍 pins it in front of the windows', notesOnDesk().find((x) => x.id === w.id)?.pinned === true);
    sdk.wm.open('notes', {});
    await sleep(1500);
    await shot('widget-pinned');
    log('done');
  }

  (globalThis.SCENE_RESTART ? restartPart : firstPart)().catch((e) => { log(`error ${e && e.stack}`); log('done'); });
})();
