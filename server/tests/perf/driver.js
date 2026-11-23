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
  const SETTINGS_PAGES = ['general', 'appearance', 'wallpaper', 'dock', 'displays', 'network', 'bluetooth', 'sound', 'notifications',
    'printers', 'clipboard', 'sharing', 'cloud', 'language', 'datetime', 'keyboard', 'trackpad', 'defaultapps', 'webapps', 'flatpak',
    'ai', 'battery', 'power', 'cooling', 'firmware', 'gpus', 'privacy', 'users', 'updates', 'session'];
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
    // every Settings page opened by its id (apps/settings/index.jsx SECTIONS, also those its sidebar hides on this
    // machine), each followed by a wait step: what opening it costs, and what it does in the 1.5 s after
    settingsPanes() {
      begin('Settings: open');
      step('close the others', closeAll);
      step('open', () => sdk.wm.open('settings', { args: { section: 'general', nonce: 1 } }));
      step('wait', () => {});
      end();
      begin('Settings: each page by id');
      let n = 2;
      for (const id of SETTINGS_PAGES) {
        step(`page ${id}`, () => sdk.wm.open('settings', { args: { section: id, nonce: n++ } }));
        step(`after ${id}`, () => {});
      }
      end();
    },
    // Settings → Flatpak opened, left, and opened again (the maintainer's freeze, 2026-11-23): perf_headless.sh fills the
    // test home's ~/.local/share/flatpak and ~/.var/app first, so measuring them takes a while
    flatpak() {
      begin('Settings → Flatpak');
      step('close the others', closeAll);
      step('open settings', () => sdk.wm.open('settings', { args: { section: 'general', nonce: 1 } }));
      step('wait', () => {});
      // what the page shows for the sizes (a size once measured; "calculating…" before)
      const sizes = () => log({ flatpakSizes: labels(frameOf('settings') || { x: 0, y: 0, w: 0, h: 0 }).map((x) => x.text)
        .filter((t) => /^[\d.,]+ ?[KMGT]?B\+?$|calculating/.test(t)) });
      for (let i = 0; i < 2; i++) {
        step(`open flatpak ${i}`, () => { sdk.wm.open('settings', { args: { section: 'flatpak', nonce: 10 + i * 2 } }); });
        step('wait', sizes);
        step('wait', sizes);
        step('general', () => sdk.wm.open('settings', { args: { section: 'general', nonce: 11 + i * 2 } }));
      }
      end();
    },
    // Files → Get Info on a big folder, twice (2026-11-23): its size was lord.fs.du on the main thread, up to 900 ms.
    // perf_headless.sh fills the test home first (PERF_FLATPAK_FILES files under ~/.local/share/flatpak and ~/.var/app);
    // Files opens the home folder, and ❖I with nothing selected shows the folder open
    filesInfo() {
      begin('Files → Get Info on a big folder');
      step('close the others', closeAll);
      step('open files', () => sdk.wm.open('files', { args: { path: lord.fs.home() } }));
      step('wait', () => {});
      const sizes = () => log({ infoSizes: labels(frameOf('files') || { x: 0, y: 0, w: 0, h: 0 }).map((x) => x.text)
        .filter((t) => /\d ?[KMGT]?B\+? · |calculating/.test(t)) });
      const info = () => {          // Ctrl+I in the Files window (flag 2: Ctrl)
        const w = sdk.windows.get().find((x) => x.id === 'files' || x.id.startsWith('files#'));
        if (!w) return false;
        sdk.wm.focus(w.id);
        globalThis.__lordKey(23, 1, 'i', '', 'i', 2);
        globalThis.__lordKey(23, 0, 'i', '', 'i', 2);
        return true;
      };
      for (let i = 0; i < 2; i++) {
        step(`open info ${i}`, info);
        step('wait', sizes);
        step('wait', sizes);
        step('close info', info);
      }
      end();
    },
    // the camera list from a worker (sdk/cameras.js), then lord.media.cameras on the main thread, each timed after 4.5 s
    // of nothing (a USB camera suspends after 2 s and wakes up to answer: the cost the Camera app's 3 s poll paid each
    // time). Only VIDIOC_QUERYCAP: no camera is opened for a picture
    cameraNatives() {
      begin('Camera list');
      for (let i = 0; i < 2; i++) {
        step(`camerasAsync ${i}`, () => {
          if (!sdk.refreshCameras || typeof lord.media.camerasAsync !== 'function') return false;
          const t0 = lord.now();
          sdk.refreshCameras().then((r) => log({ native: `refreshCameras answered (${r ? r.length : 'none'}) after`, ms: +(lord.now() - t0).toFixed(1) }));
          log({ native: 'media.camerasAsync (the call)', ms: +(lord.now() - t0).toFixed(1) });
          return true;
        });
        for (let k = 0; k < 3; k++) step('wait', () => {});
        step(`media.cameras ${i}`, () => {
          const t0 = lord.now();
          const r = lord.media.cameras();
          log({ native: `media.cameras (${r.length})`, ms: +(lord.now() - t0).toFixed(1) });
        });
        for (let k = 0; k < 3; k++) step('wait', () => {});
      }
      end();
    },
    // each native app opened alone (PERF_APPS, or the list below), then two quiet steps: the worst run on opening it and
    // in the 3 s after; with PERF_MARK=1 and PERF_OUT the stall lines fall under each step. Not Camera and LordOS Studio:
    // they would turn a real webcam on (cameraNatives times their list instead)
    appsOpen() {
      const apps = globalThis.PERF_APPS || ['files', 'store', 'mail', 'photos', 'player', 'notes', 'terminal', 'machines', 'monitor',
        'recorder', 'imager', 'settings', 'calendar', 'contacts', 'editor', 'databases', 'passwords', 'logs', 'sysinfo', 'disks'];
      for (const a of apps) {
        begin(`open ${a}`);
        step('close the others', closeAll);
        step(`open ${a}`, () => { if (!sdk.getApp || sdk.getApp(a)) sdk.wm.open(a); });
        step(`after ${a}`, () => {});
        step(`after ${a} 2`, () => {});
        end();
      }
    },
    files() {
      begin('Files: open');
      step('close the others', closeAll);
      step('open', () => sdk.wm.open('files', { args: { path: globalThis.PERF_DIR || '/usr/share/doc' } }));
      step('wait', () => {});
      step('wait', () => {});
      end();
      begin('Files: scroll the big folder');
      for (let i = 0; i < 6; i++) step(`scroll ${i}`, () => {
        const f = frameOf('files');
        const it = f && labels({ x: f.x + 260, y: f.y + 120, w: f.w - 300, h: f.h - 200 })[0];
        if (!it) return false;
        for (let k = 0; k < 8; k++) globalThis.__lordEvent('wheel', it.id, it.x, it.y, -3);
        return true;
      });
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
    // Monitor → Processes on screen (the list refreshes once a second): its native call, lord.monitor.processes, was
    // ~20 ms on the main thread and a third of a session's stall lines (docs/MACHINES-GPU.md, Part 1 step 6)
    monitorProcs() {
      begin('Monitor: Processes open');
      step('close the others', closeAll);
      step('open', () => sdk.wm.open('monitor'));
      step('wait', () => {});
      step('Processes', () => clickText('monitor', sdk.t('monitor.processes')));
      for (let i = 0; i < 10; i++) step('wait', () => {});
      end();
      begin('monitor.processes, timed alone');
      step('close the others', closeAll);
      step('start', () => { lord.monitor.start(); lord.monitor.want(true, false, false); });
      for (let i = 0; i < 6; i++) step('call', () => {
        const t0 = lord.now();
        const r = lord.monitor.processes('en');
        if (r) log({ native: `monitor.processes (${r.list.length}, ${r.list.filter((p) => p.app).length} with an app, self ${r.list.find((p) => p.self)?.app})`, ms: +(lord.now() - t0).toFixed(2) });
      });
      end();
    },
    // the maintainer's case: several apps open, then a setting changes and windows are focused in turn
    mixed() {
      // (not Monitor: its charts redraw four times a second and would be in every step; idleMonitor measures it)
      begin('Three apps open: open them');
      step('close the others', closeAll);
      for (const a of ['store', 'settings', 'files']) step(`open ${a}`, () => sdk.wm.open(a));
      step('wait', () => {});
      step('wait', () => {});
      end();
      begin('Three apps open: settings.set (6 times)');
      for (let i = 0; i < 6; i++) step(`windowSnap ${i % 2 ? 'on' : 'off'}`, () => sdk.settings.set({ windowSnap: i % 2 === 1 }));
      end();
      begin('Three apps open: focus each (2 rounds)');
      for (let r = 0; r < 2; r++) for (const a of ['store', 'settings', 'files']) step(`focus ${a}`, () => sdk.wm.focus(a));
      end();
      begin('Three apps open: idle');
      for (let i = 0; i < 4; i++) step('idle', () => {});
      end();
    },
    // the native calls apps make while rendering, each timed alone (ms; the first call may fill a cache)
    natives() {
      begin('native calls');
      const calls = [
        ['pkg.upgrades', () => lord.pkg.upgrades()], ['pkg.upgrades again', () => lord.pkg.upgrades()],
        ['pkg.info bash', () => lord.pkg.info('bash', { lang: 'en' })], ['pkg.lastUpdate', () => lord.pkg.lastUpdate()],
        ['pkg.categoryCounts', () => lord.pkg.categories && lord.pkg.categories()],
        ['pkg.apps Graphics', () => lord.pkg.apps({ category: 'Graphics', limit: 2000, lang: 'en' })],
        ['pkg.search editor', () => lord.pkg.search('editor', { limit: 60, lang: 'en' })],
        ['pkg.plan vim', () => lord.pkg.plan({ install: ['vim'] })],
        ['fs.list /usr/bin', () => lord.fs.list('/usr/bin')], ['fs.list /usr/share/doc', () => lord.fs.list('/usr/share/doc')],
        ['fonts.list', () => lord.fonts && lord.fonts.list()], ['apps.list', () => lord.apps && lord.apps.list && lord.apps.list()],
        ['system.info', () => lord.system.info()], ['monitor.sample', () => lord.monitor && lord.monitor.sample()],
      ];
      for (const [name, fn] of calls) step(name, () => {
        const t0 = lord.now();
        let n = '';
        try { const r = fn(); n = Array.isArray(r) ? ` (${r.length})` : ''; } catch (e) { n = ` (${e.message})`; }
        log({ native: name + n, ms: +(lord.now() - t0).toFixed(1) });
      });
      end();
    },
    // the native calls Settings' pages make while rendering or for their buttons, each timed alone (read-only ones only;
    // a call missing on this lord-server is noted). settings.set: one key, with whatever runs on every settings change.
    settingsNatives() {
      begin('Settings: native calls');
      const home = lord.fs.home();
      const zones = () => lord.time.zones();
      const calls = [
        ['pkg.available', () => lord.pkg.available()], ['pkg.upgrades', () => lord.pkg.upgrades()], ['pkg.lastUpdate', () => lord.pkg.lastUpdate()],
        ['pkg.info flatpak', () => lord.pkg.info('flatpak', {})], ['pkg.plan remove flatpak', () => lord.pkg.plan({ remove: ['flatpak'] })],
        ['time.zones', zones], ['time.zoneOffset x400', () => { const now = Math.floor(Date.now() / 1000); return zones().slice(0, 400).map((z) => lord.time.zoneOffset(z, now)); }],
        ['net.interfaces', () => lord.net.interfaces()], ['net.routes', () => lord.net.routes()], ['net.dns', () => lord.net.dns()],
        ['net.wifiKnown', () => lord.net.wifiKnown()], ['net.vpnList', () => lord.net.vpnList()], ['net.vpnTypes', () => lord.net.vpnTypes()],
        ['users.list', () => lord.users.list()], ['power.history', () => lord.power.history()], ['power.buttons', () => lord.power.buttons()],
        ['vm.gpus', () => lord.vm.gpus()], ['sysinfo.gpuReport', () => lord.sysinfo.gpuReport()], ['locale.available', () => lord.locale.available()],
        ['media.cameras', () => lord.media.cameras()], ['webapps.running', () => lord.webapps.running()], ['apps.flatpak', () => lord.apps.flatpak('en')],
        ['flatpak.available', () => lord.flatpak.available()], ['fs.du ~/.local/share/flatpak (sync)', () => lord.fs.du(`${home}/.local/share/flatpak`, 2000)],
        ['system.info', () => lord.system.info()], ['fs.volumes', () => lord.fs.volumes()], ['fonts.list', () => lord.fonts && lord.fonts.list()],
        ['settings.set (one key)', () => sdk.settings.set({ windowSnap: !sdk.settings.get().windowSnap })],
      ];
      for (const [name, fn] of calls) step(name, () => {
        const t0 = lord.now();
        let n = '';
        try { const r = fn(); n = Array.isArray(r) ? ` (${r.length})` : ''; } catch (e) { n = ` (${e.message})`; }
        log({ native: name + n, ms: +(lord.now() - t0).toFixed(1) });
      });
      end();
    },
    // the Terminal while a program prints fast (`yes | head -c 50M`): the __lordTerm events and what React does for them
    // (docs/MAIN-THREAD.md); terminalBg: the same with another window in front (the tab marks activity)
    terminal() { termPlan(false); },
    terminalBg() { termPlan(true); },
    // a window's title changing (what a terminal program's title escapes end in): what the theme re-renders for it
    titles() {
      begin('Terminal open: wm.setTitle 4 times a step');
      step('close the others', closeAll);
      step('open', () => sdk.wm.open('terminal'));
      step('wait', () => {});
      for (let i = 0; i < 6; i++) step(`titles ${i}`, () => { const w = sdk.windows.get().find((x) => x.id.startsWith('terminal')); for (let k = 0; k < 4; k++) sdk.wm.setTitle(w.id, `t${i}.${k}`); });
      end();
    },
    launcher() {
      begin('Launcher F4 (4 times)');
      for (let i = 0; i < 4; i++) step(`F4 ${i}`, () => key('F4', 62));
      end();
    },
  };
  const termPlan = (bg) => {
    const name = bg ? 'Terminal behind a window: yes | head -c 50M' : 'Terminal: yes | head -c 50M';
    begin(name + ' (open)');
    step('close the others', closeAll);
    step('open', () => sdk.wm.open('terminal'));
    step('wait', () => {});
    step('wait', () => {});
    if (bg) step('open settings in front', () => { sdk.wm.open('settings'); sdk.wm.focus('settings'); });
    end();
    begin(name);
    step('run', () => {
      const sids = lord.term.list();
      if (!sids.length) return false;
      for (const sid of sids) lord.term.write(sid, 'yes | head -c 50M; for i in $(seq 1 20); do printf "\\033]0;t$i\\007"; yes | head -c 2M; done; for i in $(seq 1 300); do printf "\\033]0;s$i\\007"; sleep 0.01; done\r');
      return true;
    });
    for (let i = 0; i < 18; i++) step('printing', () => { log({ termEvents: { ...termCounts } }); for (const k in termCounts) delete termCounts[k]; });
    end();
  };
  const termCounts = {};
  sdk.term.onAny((sid, type) => { termCounts[type] = (termCounts[type] || 0) + 1; });
  // idleX: app X alone, then 20 idle steps (30 s): what it does in the background, and whether its heap grows
  const idleApp = (app) => {
    begin(`${app} alone: open`);
    step('close the others', closeAll);
    step('open', () => sdk.wm.open(app));
    step('wait', () => {});
    step('wait', () => {});
    end();
    begin(`${app} alone: idle 30 s`);
    for (let i = 0; i < 20; i++) step('idle', () => {});
    end();
  };
  for (const p of (globalThis.PERF_PLAN || ['idle', 'settingsSet', 'launcher', 'store', 'settings', 'files', 'monitor'])) {
    if (/^idle[A-Za-z]+$/.test(p) && !plans[p]) idleApp(p.slice(4).toLowerCase());
    else plans[p]?.();
  }
  steps.push({ begin: 'after all' });
  steps.push({ name: 'settle', fn: () => {}, scenario: 'after all' });
  steps.push({ end: 'after all' });

  let cursor = -1;
  const heap = () => { const s = lord.stats(2); return { usedKB: s.jsUsedKB, objects: s.jsObjects, nodes: s.nodes }; };
  let last = lord.stats(1), lastOps = 0;
  function next() {
    cursor++;
    if (cursor >= steps.length) { log({ done: true, stalls: lord.stats().stalls, worst: lord.stats().stallWorstMs }); lord.quit?.(); return; }
    const s = steps[cursor];
    if (s.begin) { log({ begin: s.begin, heap: heap() }); setTimeout(() => { last = lord.stats(1); next(); }, 300); return; }
    if (s.end) { log({ end: s.end, heap: heap() }); setTimeout(() => { last = lord.stats(1); next(); }, 300); return; }
    let ok = true;
    if (globalThis.PERF_MARK) log({ at: s.name });
    try { ok = s.fn() !== false; } catch (e) { log({ error: String(e && e.message || e), step: s.name }); }
    setTimeout(() => {
      const now = lord.stats(1);
      const ops = globalThis.__lordReactOps ? globalThis.__lordReactOps() : 0;
      log({ scenario: s.scenario, step: s.name, ok, updates: ops - lastOps, js: +(now.busyJsMs - last.busyJsMs).toFixed(1), layout: +(now.busyLayoutMs - last.busyLayoutMs).toFixed(1),
        paint: +(now.busyPaintMs - last.busyPaintMs).toFixed(1), max: +now.maxRunMs.toFixed(1), nodes: now.nodes });
      last = now;
      lastOps = ops;
      next();
    }, STEP);
  }
  setTimeout(() => { last = lord.stats(1); next(); }, globalThis.PERF_START || 4000);
})();
