// freeze.js — goes after the theme bundle in tests/monitor_freeze_headless.sh (a test tool): Monitor → Processes,
// Grouped, over pre.js's list with MONITOR_BIG (a Terminal with 240 programs). It measures, with lord.stats(1), the
// longest run of the main thread (JS, layout and paint: what keeps the pointer from moving) when the Terminal's group
// opens, while it stays open (the list is redrawn every second) and when it closes.
// Lines start with "MONITOR-FREEZE" in the shell log.
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__monitorScene;
  const L = globalThis.MONITOR_L || {};
  const log = (s) => lord.log(`MONITOR-FREEZE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const ev = (type, id, x, y) => globalThis.__lordEvent(type, id, x, y, 0);
  const at = (id) => { const l = box(id); return l ? [l[0] + l[2] / 2, l[1] + l[3] / 2] : null; };
  const click = (id) => { const p = id && at(id); if (!p) return false; ev('move', id, ...p); ev('down', id, ...p); ev('up', id, ...p); return true; };
  const key = (name, flags = 0) => { globalThis.__lordKey(0, 1, name, '', name.toLowerCase(), flags); globalThis.__lordKey(0, 0, name, '', '', flags); };
  const byText = (text) => {
    const out = [];
    for (const [raw, s] of P.texts) if (s.trim() === text) { const id = P.parent.get(raw); if (id && box(id)) out.push(id); }
    out.sort((a, b) => b - a);
    return out[0] || null;
  };
  const measure = async (what, fn, ms) => {
    const s0 = lord.stats(1);
    const t0 = lord.now();
    fn();
    const sync = lord.now() - t0;
    await sleep(ms);
    const s = lord.stats(1);
    log(`${what}: sync ${sync.toFixed(1)} ms, longest main-thread run ${s.maxRunMs.toFixed(1)} ms (busy: JS ${(s.busyJsMs - s0.busyJsMs).toFixed(0)}, layout ${(s.busyLayoutMs - s0.busyLayoutMs).toFixed(0)}, paint ${(s.busyPaintMs - s0.busyPaintMs).toFixed(0)} ms), nodes ${s.nodes}`);
  };
  (async () => {
    await sleep(2500);
    sdk.wm.open('monitor', { args: { tab: 'processes', nonce: Date.now() } });
    await sleep(3500);
    if (!(sdk.settings.get().monitor || {}).procGrouped) click(byText(L.grouped));
    await sleep(2500);
    await measure('closed, redrawn every second', () => {}, 3000);
    log(`select ${click(byText(L.terminal)) ? 'ok' : 'missing'}`);
    await sleep(500);
    await measure('open the Terminal', () => key('ArrowRight'), 1500);
    await measure('open, redrawn every second', () => {}, 4000);
    await measure('close the Terminal', () => key('ArrowLeft'), 1500);
    log('done');
  })();
})();
