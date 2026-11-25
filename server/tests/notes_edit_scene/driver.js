// driver.js — goes after the theme bundle in tests/app_scene_headless.sh notes_edit_scene: Notes' editor on a long note
// (setup.sh): screenshots of Edit mode (rows that wrap must not overlap: each row's text is drawn on its own row and
// within the editor's width), and the main thread's busy time per keystroke (lord.stats(1): JS, layout and paint) while
// typing in the middle of the note, printed as "APP-SCENE perf …".
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__appScene;
  const T = globalThis.L_notes || {};
  const log = (s) => lord.log(`APP-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { let l = null; try { l = lord.getLayout(id); } catch (e) { l = null; } return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const ev = (type, id, x, y, b = 0) => globalThis.__lordEvent(type, id, x, y, b);
  const at = (id) => { const l = box(id); return l ? [l[0] + l[2] / 2, l[1] + l[3] / 2] : null; };
  const click = (id, b = 0) => { const p = id && at(id); if (!p) return false; ev('move', id, ...p, b); ev('down', id, ...p, b); ev('up', id, ...p, b); return true; };
  const all = (text) => { const out = []; for (const [raw, s] of P.texts) if (s.trim() === text) { const id = P.parent.get(raw); if (id && box(id)) out.push(id); } return out.sort((a, b) => b - a); };
  const key = async (name, chr = '', flags = 0) => { globalThis.__lordKey(30, 1, name, chr, (chr || name).toLowerCase(), flags); globalThis.__lordKey(30, 0, name, '', '', flags); };
  const step = (what, ok, extra) => log(`${what} ${ok ? 'done' : 'missing'}${extra ? ` (${extra})` : ''}`);
  const home = lord.fs.home();
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(700);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${home}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  // the editor's rows: every Text in the editor area, with its box; two rows overlap when their boxes cross vertically
  const rowsIn = (area) => {
    const out = [];
    for (const [raw, s] of P.texts) {
      if (!s) continue;
      const b = box(P.parent.get(raw)) || box(raw);
      if (!b || b[0] < area[0] || b[0] > area[0] + area[2] || b[1] < area[1] || b[1] + b[3] > area[1] + area[3]) continue;
      out.push({ s, b });
    }
    return out.sort((a, b) => a.b[1] - b.b[1]);
  };

  (async () => {
    await sleep(2500);
    sdk.wm.open('notes', { args: { slug: 'long', nonce: 1 } });
    await sleep(2000);
    const nw = (sdk.windows.get() || []).find((w) => w.app === 'notes');
    if (nw) { sdk.wm.resize(nw.id, 900, 640); sdk.wm.center(nw.id); }
    await sleep(1200);
    click(all(T.edit || 'Edit').pop());
    await sleep(1500);
    const w = (sdk.windows.get() || []).find((x) => x.app === 'notes');
    const f = w ? sdk.wm.frame(w) : null;
    // the note's paper: right of the list (240) and below the toolbar
    const area = f ? [f.x + 250, f.y + 90, f.w - 260, f.h - 100] : [0, 0, 1280, 800];
    const rows = rowsIn(area);
    let overlaps = 0, wide = 0;
    for (let i = 1; i < rows.length; i++) if (rows[i].b[1] < rows[i - 1].b[1] + rows[i - 1].b[3] - 1 && Math.abs(rows[i].b[0] - rows[i - 1].b[0]) < 4) overlaps++;
    for (const r of rows) if (r.b[0] + r.b[2] > area[0] + area[2] + 2) wide++;
    step('edit mode: the rows are found', rows.length > 20, `${rows.length}`);
    step('edit mode: no two rows overlap', overlaps === 0, `${overlaps} of ${rows.length}`);
    step('edit mode: no row is drawn past the editor\'s width', wide === 0, `${wide}`);
    await shot('edit-top');
    // the middle of the note: click in it, then type
    const mid = rows[Math.floor(rows.length / 2)];
    if (mid) { ev('move', 0, mid.b[0] + 30, mid.b[1] + 4); ev('down', 0, mid.b[0] + 30, mid.b[1] + 4); ev('up', 0, mid.b[0] + 30, mid.b[1] + 4); }
    await sleep(500);
    for (let i = 0; i < 15; i++) { await key('PageDown'); await sleep(30); }
    await sleep(800);
    // the same time without typing first: what the desktop does anyway (clocks, the caret's blink, timers)
    const K = 60;
    lord.stats(1);
    const i0 = Date.now();
    for (let i = 0; i < K; i++) await sleep(40);
    const idle = lord.stats(1), idleMs = Date.now() - i0;
    const idleBusy = (idle.busyJsMs || 0) + (idle.busyLayoutMs || 0) + (idle.busyPaintMs || 0);
    log(`perf idle wallMs=${idleMs} busyMs=${idleBusy.toFixed(1)} maxRunMs=${(idle.maxRunMs || 0).toFixed(1)}`);
    lord.stats(1);
    const t0 = Date.now();
    let worstCall = 0;
    for (let i = 0; i < K; i++) {
      const ch = 'abcdefghij'[i % 10];
      const a = Date.now();
      await key(ch, ch);
      worstCall = Math.max(worstCall, Date.now() - a);
      await sleep(40);
    }
    await sleep(1500);   // the save after the last key
    const s = lord.stats(1);
    const busy = (s.busyJsMs || 0) + (s.busyLayoutMs || 0) + (s.busyPaintMs || 0);
    const wall = Date.now() - t0;
    log(`perf netBusyPerKeyMs=${((busy - idleBusy * (wall / idleMs)) / K).toFixed(2)} (typing minus idle for the same time)`);
    log(`perf keys=${K} wallMs=${wall} busyPerKeyMs=${(busy / K).toFixed(2)} jsPerKeyMs=${((s.busyJsMs || 0) / K).toFixed(2)} layoutPerKeyMs=${((s.busyLayoutMs || 0) / K).toFixed(2)} paintPerKeyMs=${((s.busyPaintMs || 0) / K).toFixed(2)} maxRunMs=${(s.maxRunMs || 0).toFixed(1)} worstKeyCallMs=${worstCall} nodes=${s.nodes}`);
    await shot('edit-typed');
    const saved = lord.fs.read(`${home}/Notes/long/nota.md`) || '';
    step('what was typed is saved', saved.includes('abcdefghij'));
    log('done');
  })().catch((e) => { log(`error ${e && e.message} ${e && e.stack}`); log('done'); });
})();
