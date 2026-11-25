// driver.js — goes after the theme bundle in tests/monitor_headless.sh: it opens Monitor on Processes (pre.js's made-up
// list) and saves a screenshot after each step in the home folder (shot-1.png …): the flat list with its origin badges
// and filters; Grouped; Chrome and the Terminal open, with who started each program; a group's hover summary; an origin
// badge's tooltip without the row's summary; Quit
// All's sheet; a Terminal program's Get Info (Statistics: "Started by"); the Flatpak filter with the program picker.
// Nodes are found by the text they show (pre.js keeps it), in the scene's language (MONITOR_L: the labels).
// Lines start with "MONITOR-SCENE" in the shell log.
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__monitorScene;
  const L = globalThis.MONITOR_L || {};
  const log = (s) => lord.log(`MONITOR-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const ev = (type, id, x, y) => globalThis.__lordEvent(type, id, x, y, 0);
  const at = (id) => { const l = box(id); return l ? [l[0] + l[2] / 2, l[1] + l[3] / 2] : null; };
  const click = (id) => { const p = id && at(id); if (!p) return false; ev('move', id, ...p); ev('down', id, ...p); ev('up', id, ...p); return true; };
  const move = (id) => { const p = id && at(id); if (p) ev('move', id, ...p); return !!p; };
  const key = (name, flags = 0) => { globalThis.__lordKey(0, 1, name, '', name.toLowerCase(), flags); globalThis.__lordKey(0, 0, name, '', '', flags); };
  // the shown node with this text (the text's own node) inside Monitor's window
  const FRAME = { x: 180, y: 60, w: 1180, h: 700 };
  const inWin = (l) => l[0] + l[2] / 2 >= FRAME.x && l[0] + l[2] / 2 <= FRAME.x + FRAME.w && l[1] + l[3] / 2 >= FRAME.y && l[1] + l[3] / 2 <= FRAME.y + FRAME.h;
  const byText = (text) => {
    const out = [];
    for (const [raw, s] of P.texts) if (s.trim() === text) { const id = P.parent.get(raw); const l = id && box(id); if (l && inWin(l)) out.push(id); }
    // a node taken out of the tree keeps its last box: the newest one with the text is the one on screen
    out.sort((a, b) => b - a);
    return out[0] || null;
  };
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(900);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  const step = (what, ok, extra) => log(`${what} ${ok ? 'done' : 'missing'}${extra ? ` (${extra})` : ''}`);
  (async () => {
    await sleep(2500);
    sdk.wm.open('monitor', { args: { tab: 'processes', nonce: Date.now() } });
    await sleep(3500);
    await shot('flat');
    step('grouped', click(byText(L.grouped)));
    await sleep(1500);
    // Chrome and the Terminal open; the pointer leaves the list so the order is released
    step('chrome', click(byText('Google Chrome')));
    await sleep(300);                                   // the selection is drawn before the key
    key('ArrowRight');
    await sleep(300);
    step('terminal', click(byText(L.terminal)));
    await sleep(300);
    key('ArrowRight');
    await sleep(300);
    step('select sleep', click(byText('3021')));
    move(byText(L.processes));
    await sleep(2600);
    await shot('grouped-open');
    // the hover summary of Chrome's row
    step('hover', move(byText('Google Chrome')));
    await sleep(1200);
    await shot('summary');
    // a group row's origin badge (a verified seal, or a pill such as Chrome's "Google LLC"): its own tooltip, never the
    // row's summary with it (they overlapped); back on the row's name, the summary comes again
    {
      const summaryShown = () => [...P.summaries].some((id) => box(id));
      const rowAt = (name) => { const b = box(byText(name)); return b ? b[1] + b[3] / 2 : -1; };
      const onRow = (y) => (b) => b && inWin(b) && Math.abs(b[1] + b[3] / 2 - y) < 8;
      const badgeOf = (name) => {
        const y = rowAt(name);
        const seal = [...P.seals].find((id) => onRow(y)(box(id)));
        if (seal) return seal;
        for (const [raw, txt] of P.texts) if (txt === 'Google LLC') { const id = P.parent.get(raw); if (id && onRow(y)(box(id))) return id; }
        return null;
      };
      const badge = badgeOf('Google Chrome');
      step('badge found', !!badge);
      move(byText(L.processes));
      await sleep(300);
      if (badge) { move(byText('Google Chrome')); await sleep(100); move(badge); }
      await sleep(1500);
      step('on the badge: no row summary', !!badge && !summaryShown());
      await shot('badge-tooltip');
      move(byText('Google Chrome'));
      await sleep(1300);
      step('back on the row: the summary', summaryShown());
      move(byText(L.processes));
      await sleep(400);
    }
    // Quit All's sheet, from the selected row's own button
    step('select chrome', click(byText('Google Chrome')));
    await sleep(300);
    step('quit all', click(byText(L.quitAll)));
    await sleep(800);
    await shot('quit-all');
    key('Escape');
    await sleep(400);
    // a Terminal program's Get Info: Statistics, "Started by"
    step('select sleep again', click(byText('3021')));
    await sleep(300);
    key('i', 2);
    await sleep(1200);
    step('stats', click(byText(L.stats)));
    await sleep(1200);
    await shot('get-info');
    key('Escape');
    await sleep(400);
    // the Flatpak filter: only Flatpak apps
    step('flatpak filter', click(byText(L.flatpak)));
    move(byText(L.processes));
    await sleep(2600);
    await shot('filter');
    click(byText(L.flatpak));
    // the rolling statistics: their columns over 1 minute, sorted by the CPU peak, and a group's summary with its sparklines
    const cur = sdk.settings.get().monitor || {};
    sdk.settings.set({ monitor: { ...cur, procStats: 1, procSort: { key: 'cpuPeak', dir: -1 } } });
    move(byText(L.processes));
    await sleep(3500);
    step('hover stats', move(byText('Google Chrome')));
    await sleep(1400);
    await shot('stats');
    sdk.settings.set({ monitor: { ...cur } });
    log('done');
  })();
})();
