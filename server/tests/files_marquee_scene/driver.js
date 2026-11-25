// driver.js — goes after the theme bundle in tests/app_scene_headless.sh files_marquee_scene (GitHub #233): Files'
// rubber-band selection driven the way a mouse does it (a press on the empty space, pointer moves, a release), with
// Shift and Ctrl held through lord-server's key path, in the icon view, the list view and a big folder that scrolls
// by itself near the edge. Every count is read from the status bar ("N of M selected"); screenshots are taken with the
// rectangle on the screen. Lines start with "APP-SCENE" in the shell log.
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__appScene;
  const F = globalThis.L_files || {};
  const log = (s) => lord.log(`APP-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const ev = (type, id, x, y, b = 0) => globalThis.__lordEvent(type, id, x, y, b);
  const textId = (label) => { for (const [raw, s] of P.texts) if (s === label) { const id = P.parent.get(raw); if (id && box(id)) return id; } return null; };
  const nameId = (base) => textId(`${base}.txt`) || textId(base);
  const up = (id, n) => { let x = id; for (let i = 0; i < n && x; i++) x = P.parent.get(x); return x; };
  const step = (what, ok, extra) => log(`${what} ${ok ? 'done' : 'missing'}${extra ? ` (${extra})` : ''}`);
  const shift = (on) => globalThis.__lordKey(42, on ? 1 : 0, 'Shift', '', '', on ? 1 : 0);
  const ctrl = (on) => globalThis.__lordKey(29, on ? 1 : 0, 'Control', '', '', on ? 2 : 0);
  // "{count} of {total} selected" in the status bar, as a number (0 when the bar shows the item count instead)
  const tpl = String(F['status.selected'] || '{count} of {total} selected');
  const re = new RegExp(`^${tpl.replace(/[.*+?^$()|[\]\\]/g, '\\$&').replace('\\{count\\}', '(\\d+)').replace('{count}', '(\\d+)').replace('\\{total\\}', '\\d+').replace('{total}', '\\d+')}`);
  // (of the texts on the screen: a closed window's nodes may keep theirs)
  const selected = () => {
    let best = null;
    for (const [raw, s] of P.texts) { const m = re.exec(s.split(',')[0].trim()); const id = P.parent.get(raw); if (m && id && box(id) && (!best || id > best[0])) best = [id, Number(m[1])]; }
    return best ? best[1] : 0;
  };
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(500);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  const center = (id) => { const b = box(id); return [b[0] + b[2] / 2, b[1] + b[3] / 2]; };
  // a drag: press at a, moves to b in steps (one per frame-ish), and optionally hold before the release
  const drag = async (target, a, b, { hold = 0, shotName = null, steps = 8 } = {}) => {
    ev('move', target, ...a); ev('down', target, ...a);
    for (let i = 1; i <= steps; i++) { ev('move', target, a[0] + (b[0] - a[0]) * i / steps, a[1] + (b[1] - a[1]) * i / steps); await sleep(30); }
    if (hold) await sleep(hold);
    await sleep(120);
    if (shotName) await shot(shotName);
    ev('up', 0, ...b);
    await sleep(500);
  };
  const win = () => (sdk.windows.get() || []).find((w) => w.app === 'files' || String(w.id).startsWith('files'));
  (async () => {
    await sleep(2500);
    const home = lord.fs.home();
    // ---- the icon view ----
    sdk.wm.open('files', { new: true, args: { path: `${home}/Marquee`, view: 'icons', nonce: Date.now() } });
    await sleep(2500);
    const w = win();
    if (w) { sdk.wm.resize(w.id, 980, 680); sdk.wm.center(w.id); }
    await sleep(1500);
    const pad = (i) => String(i).padStart(2, '0');
    const f1 = nameId('file-01'), f2 = nameId('file-02'), f6 = nameId('file-06');
    step('icon view: the cards are drawn', f1 && f2 && f6, [f1, f2, f6].join(','));
    // the card: the name's Text → its pill → the card; the content (with the press handler) is a few levels up
    const card1 = up(f1, 2), grid = up(card1, 1);
    const content = up(grid, 2);
    const c1 = box(card1), cols = Math.round(box(grid)[2] / (c1[2] + 8));
    log(`cards per row ${cols}, card ${c1.map(Math.round).join(',')}`);
    // an empty spot: in the content's top padding, beside the first card
    const start = [c1[0] + 2, c1[1] - 6];
    // the rectangle from beside the first card to the middle of the second card of the second row: four cards
    const r2c2 = center(up(nameId(`file-${pad(cols + 2)}`), 2));
    await drag(content, start, r2c2, { shotName: 'icons-dragging' });
    const two = selected();
    step('icon view: a drag selects the cards its rectangle touches', two === 4, `selected ${two}`);
    await shot('icons-selected');
    // Shift: the first two cards of the third row are added (from the gap above them)
    const r3c1 = box(up(nameId(`file-${pad(2 * cols + 1)}`), 2)), r3c2 = box(up(nameId(`file-${pad(2 * cols + 2)}`), 2));
    shift(true);
    await drag(content, [r3c1[0] + 4, r3c1[1] - 3], [r3c2[0] + r3c2[2] / 2, r3c2[1] + 10], { shotName: 'icons-shift' });
    shift(false);
    const added = selected();
    step('Shift adds to the selection', added === two + 2, `selected ${added}, before ${two}`);
    // Ctrl: toggles file-01, -02 (selected: out) and -03 (not: in)
    const c3 = box(up(nameId('file-03'), 2));
    ctrl(true);
    await drag(content, start, [c3[0] + 10, c3[1] + 10], { shotName: 'icons-ctrl' });
    ctrl(false);
    const toggled = selected();
    step('Ctrl toggles what the rectangle touches', toggled === added - 1, `selected ${toggled}, before ${added}`);
    // a plain click on the empty space: nothing selected (as before)
    ev('move', content, ...start); ev('down', content, ...start); ev('up', content, ...start);
    await sleep(600);
    step('a click without a drag clears the selection', selected() === 0, `selected ${selected()}`);
    // a press on a card still selects (and drags) it, never starts a rectangle
    const p6 = center(up(f6, 2));
    ev('move', f6, ...p6); ev('down', f6, ...p6); ev('up', f6, ...p6);
    await sleep(600);
    step('a press on a card selects that card only', selected() === 1, `selected ${selected()}`);

    // ---- the list view ----
    const closeAll = () => { for (const x of sdk.windows.get() || []) if (x.app === 'files' || String(x.id).startsWith('files')) sdk.wm.close(x.id); };
    closeAll();
    await sleep(800);
    sdk.wm.open('files', { new: true, args: { path: `${home}/Few`, view: 'list', nonce: Date.now() } });
    await sleep(2500);
    const w2 = (sdk.windows.get() || []).filter((x) => x.app === 'files' || String(x.id).startsWith('files')).pop();
    if (w2) { sdk.wm.resize(w2.id, 980, 760); sdk.wm.center(w2.id); }
    await sleep(1500);
    const all = (label) => { const out = []; for (const [raw, s] of P.texts) if (s === label || s === label.replace(/\.txt$/, '')) { const id = P.parent.get(raw); if (id && box(id)) out.push(id); } return out.sort((a, b) => b - a); };
    const r24 = all('note-8.txt')[0], r20 = all('note-4.txt')[0];
    step('list view: the rows are drawn', !!r24 && !!r20);
    if (r24 && r20) {
      // below the last row is empty: the drag goes up from there to file-20's row
      const last = box(r24), row20 = box(r20);
      const lcontent = up(r24, 7);
      const from = [last[0] + 200, last[1] + last[3] + 24];
      await drag(lcontent, from, [last[0] + 260, row20[1] + 4], { shotName: 'list-dragging' });
      const rows = selected();
      step('list view: the rows from the last one up to note-4 are selected', rows === 5, `selected ${rows}`);
      await shot('list-selected');
    }

    // ---- a big folder: the view scrolls by itself while the pointer stays near the bottom edge ----
    closeAll();
    await sleep(800);
    sdk.wm.open('files', { new: true, args: { path: `${home}/Many`, view: 'icons', nonce: Date.now() } });
    await sleep(3000);
    const w3 = (sdk.windows.get() || []).filter((x) => x.app === 'files' || String(x.id).startsWith('files')).pop();
    if (w3) { sdk.wm.resize(w3.id, 980, 600); sdk.wm.center(w3.id); }
    await sleep(1500);
    const m1 = nameId('item-001');
    if (m1) {
      const mc = box(up(m1, 2)), mcontent = up(m1, 5);
      const wb = w3 ? [w3.x, w3.y, w3.w, w3.h] : [0, 0, 980, 600];
      const bottom = wb[1] + wb[3] - 60;          // just above the status bars: inside the view's bottom band
      ev('move', mcontent, mc[0] + 2, mc[1] - 6); ev('down', mcontent, mc[0] + 2, mc[1] - 6);
      for (let i = 1; i <= 8; i++) { ev('move', mcontent, mc[0] + 200, mc[1] + (bottom - mc[1]) * i / 8); await sleep(30); }
      await sleep(300);
      const early = selected();
      await sleep(1800);
      const later = selected();
      await shot('autoscroll');
      ev('up', 0, mc[0] + 200, bottom);
      await sleep(600);
      step('near the bottom edge the view scrolls and the selection grows', later > early && early > 0, `selected ${early} then ${later}`);
      const firstGone = !box(up(m1, 2)) || box(up(m1, 2))[1] < wb[1];
      step('the first cards scrolled out of sight and stay selected', firstGone && selected() >= later, `selected ${selected()}`);
    } else step('big folder drawn', false);
    log('done');
  })();
})();
