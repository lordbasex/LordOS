// driver.js — goes after the theme bundle in perf_headless.sh: runs the scenarios of PERF_PLAN (a global set by the
// script) one step every STEP ms and logs, per step, how long the main thread was busy (lord.stats(1): JavaScript,
// layout and paint, and the longest single run) and, per scenario, the JS heap after a full collection (lord.stats(2)).
// Lines start with "PERF " in the server log; perf_headless.sh prints them as a table.
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__perf;
  const STEP = globalThis.PERF_STEP || 1500;
  const log = (o) => lord.log('PERF ' + JSON.stringify(o));
  const frameOf = (app) => {
    const w = sdk.windows.get().find((x) => x.id === app || x.id.startsWith(app + '#'));
    return w ? sdk.wm.frame(w) : null;
  };
  const shown = (id) => {          // a text node's box, if it has one and every ancestor is laid out
    const l = lord.getLayout(id);
    return l && l[2] > 0 && l[3] > 0 ? l : null;
  };
  // visible labels inside a box: [{ text, id, x, y }], top to bottom
  function labels(box) {
    const out = [];
    for (const [raw, s] of P.texts) {
      const t = s.trim();
      if (!t) continue;
      const id = P.parent.get(raw);
      if (!id) continue;
      const l = shown(id);
      if (!l) continue;
      const cx = l[0] + l[2] / 2, cy = l[1] + l[3] / 2;
      if (cx < box.x || cx > box.x + box.w || cy < box.y || cy > box.y + box.h) continue;
      out.push({ text: t, id, x: cx, y: cy });
    }
    return out.sort((a, b) => a.y - b.y || a.x - b.x);
  }
  const click = (it) => {
    globalThis.__lordEvent('move', it.id, it.x, it.y, 0);
    globalThis.__lordEvent('down', it.id, it.x, it.y, 0);
    globalThis.__lordEvent('up', it.id, it.x, it.y, 0);
  };
  const clickText = (app, text) => {
    const f = frameOf(app);
    const it = f && labels(f).find((x) => x.text === text);
    if (it) click(it);
    return !!it;
  };
  // the sidebar's labels: the left `width` px of the window, under its title bar
  const sidebar = (app, width = 220) => {
    const f = frameOf(app);
    if (!f) return [];
    const seen = new Set();
    // words only (not glyphs), each label once, and not what another window in front shows there
    return labels({ x: f.x, y: f.y + 40, w: width, h: f.h - 40 })
      .filter((x) => /\p{L}{3}/u.test(x.text) && x.text.length < 40 && !seen.has(x.text) && seen.add(x.text));
  };

  // ---- the steps ----
  const steps = [];
  let scenario = '';
  const step = (name, fn) => steps.push({ name, fn, scenario });
  const begin = (name) => { scenario = name; steps.push({ begin: name }); };
  const closeAll = () => { for (const w of sdk.windows.get()) sdk.wm.close(w.id, true); };
  const end = () => steps.push({ end: scenario });
  const key = (name, code) => {
    globalThis.__lordKey(code, 1, name, '', name, 0);
    globalThis.__lordKey(code, 0, name, '', name, 0);
  };
  const plans = {
    idle() { begin('idle'); for (let i = 0; i < 3; i++) step('idle', () => {}); end(); },
    settingsSet() {
      begin('settings.set (one key, 6 times)');
      for (let i = 0; i < 6; i++) step(`windowSnap ${i % 2 ? 'on' : 'off'}`, () => sdk.settings.set({ windowSnap: i % 2 === 1 }));
      end();
    },
    store() {
      begin('Store: open');
      step('close the others', closeAll);
      step('open', () => sdk.wm.open('store'));
      step('wait', () => {});
      step('wait', () => {});
      end();
      begin('Store: sidebar (3 rounds)');
      for (let r = 0; r < 3; r++) step(`round ${r}`, () => {
        const items = sidebar('store');
        steps.splice(cursor + 1, 0, ...items.slice(0, 16).map((it) => ({ name: `store ${it.text}`, scenario: 'Store: sidebar (3 rounds)', fn: () => clickText('store', it.text) })));
      });
      end();
    },
    settings() {
      begin('Settings: open');
      step('close the others', closeAll);
      step('open', () => sdk.wm.open('settings'));
      step('wait', () => {});
      end();
      begin('Settings: every pane');
      step('list', () => {
        const items = sidebar('settings', 240);
        steps.splice(cursor + 1, 0, ...items.slice(0, 45).map((it) => ({ name: `pane ${it.text}`, scenario: 'Settings: every pane', fn: () => clickText('settings', it.text) })));
      });
      end();
    },
    files() {
      begin('Files: open');
      step('close the others', closeAll);
      step('open', () => sdk.wm.open('files', { args: { path: globalThis.PERF_DIR || '/usr/share/doc' } }));
      step('wait', () => {});
      step('wait', () => {});
      end();
      begin('Files: sidebar');
      step('list', () => {
        const items = sidebar('files', 200);
        steps.splice(cursor + 1, 0, ...items.slice(0, 10).map((it) => ({ name: `files ${it.text}`, scenario: 'Files: sidebar', fn: () => clickText('files', it.text) })));
      });
      end();
    },
    monitor() {
      begin('Monitor: open and wait');
      step('close the others', closeAll);
      step('open', () => sdk.wm.open('monitor'));
      for (let i = 0; i < 4; i++) step('wait', () => {});
      end();
    },
    launcher() {
      begin('Launcher F4 (4 times)');
      for (let i = 0; i < 4; i++) step(`F4 ${i}`, () => key('F4', 62));
      end();
    },
  };
  for (const p of (globalThis.PERF_PLAN || ['idle', 'settingsSet', 'launcher', 'store', 'settings', 'files', 'monitor'])) plans[p]?.();
  steps.push({ begin: 'after all' });
  steps.push({ name: 'settle', fn: () => {}, scenario: 'after all' });
  steps.push({ end: 'after all' });

  let cursor = -1;
  const heap = () => { const s = lord.stats(2); return { usedKB: s.jsUsedKB, objects: s.jsObjects, nodes: s.nodes }; };
  let last = lord.stats(1);
  function next() {
    cursor++;
    if (cursor >= steps.length) { log({ done: true, stalls: lord.stats().stalls, worst: lord.stats().stallWorstMs }); lord.quit?.(); return; }
    const s = steps[cursor];
    if (s.begin) { log({ begin: s.begin, heap: heap() }); setTimeout(() => { last = lord.stats(1); next(); }, 300); return; }
    if (s.end) { log({ end: s.end, heap: heap() }); setTimeout(() => { last = lord.stats(1); next(); }, 300); return; }
    let ok = true;
    try { ok = s.fn() !== false; } catch (e) { log({ error: String(e && e.message || e), step: s.name }); }
    setTimeout(() => {
      const now = lord.stats(1);
      log({ scenario: s.scenario, step: s.name, ok, js: +(now.busyJsMs - last.busyJsMs).toFixed(1), layout: +(now.busyLayoutMs - last.busyLayoutMs).toFixed(1),
        paint: +(now.busyPaintMs - last.busyPaintMs).toFixed(1), max: +now.maxRunMs.toFixed(1), nodes: now.nodes });
      last = now;
      next();
    }, STEP);
  }
  setTimeout(() => { last = lord.stats(1); next(); }, globalThis.PERF_START || 4000);
})();
