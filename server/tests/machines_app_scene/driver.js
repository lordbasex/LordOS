// driver.js — goes after the theme bundle in tests/machines_app_headless.sh: it opens Machines on pre.js's made-up library
// and saves a screenshot after each step in the home folder (shot-1.png …): the library (the agent and disk lights, the
// "Weaker isolation on this kernel" notice, memory for machines and "Short of memory", Pause All on battery); What this
// computer can run (from the Help menu); Windows 11's window refusing its start beside the two running (Suspend ‹another›,
// Start with less memory); Configure with its search; the same search narrowed to one section. Nodes are found by the
// text they show (pre.js keeps it), in the scene's language (APP_L: the labels). Lines start with "APP-SCENE" in the
// shell log.
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__appScene;
  const L = globalThis.APP_L || {};
  const log = (s) => lord.log(`APP-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const ev = (type, id, x, y, b = 0) => globalThis.__lordEvent(type, id, x, y, b);
  const at = (id) => { const l = box(id); return l ? [l[0] + l[2] / 2, l[1] + l[3] / 2] : null; };
  const click = (id, b = 0) => { const p = id && at(id); if (!p) return false; ev('move', id, ...p, b); ev('down', id, ...p, b); ev('up', id, ...p, b); return true; };
  const all = (text) => {
    const out = [];
    for (const [raw, s] of P.texts) if (s.trim() === text) { const id = P.parent.get(raw); if (id && box(id)) out.push(id); }
    return out.sort((a, b) => b - a);
  };
  const byText = (text) => all(text)[0] || null;
  const has = (text) => [...P.texts.values()].some((s) => s.includes(text));
  // a menu item of a window by its label (the menus are the window's own, as the theme's menu bar shows them)
  const menuItem = (winId, label) => {
    for (const m of sdk.appMenus.get()[winId] || []) for (const it of m.items || []) if (it && it.label === label) return it;
    return null;
  };
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(900);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  const step = (what, ok) => log(`${what} ${ok ? 'done' : 'missing'}`);
  (async () => {
    await sleep(2500);
    sdk.wm.open('machines', { args: { nonce: Date.now() } });
    await sleep(1500);
    const w = (sdk.windows.get() || []).find((x) => x.id === 'machines');
    if (w) { sdk.wm.resize(w.id, 1280, 800); sdk.wm.center(w.id); }
    await sleep(4000);                                  // two stats reads: the disk light is lit
    step('lights', has(L.toolsOn) && has(L.diskBusy));
    step('weak notice', has(L.weak));
    step('budget line', has(L.budget) && has(L.pressure));
    await shot('library');
    const it = menuItem('machines', L.canRun);
    step('can run item', !!it);
    if (it) { if (it.onClick) it.onClick(); else sdk.appMenuClick(it.id); }
    await sleep(2500);
    step('can run page', has(L.nested) && has(L.sensors));
    await shot('can-run');
    step('back', click(byText(L.back)));
    await sleep(900);
    // Windows 11: its window, refused beside the two running
    sdk.wm.open('machines', { instance: 'vm-33333333-2222-4333-8444-555555555555', args: { machine: '/home/scene/Machines/Windows 11.lordvm', name: 'Windows 11' } });
    await sleep(2500);
    step('budget card', has(L.budgetTitle) && has(L.suspendDebian) && has(L.less));
    await shot('budget');
    step('cancel', click(byText(L.cancel)));
    await sleep(700);
    // Configure: the search
    sdk.wm.open('machines', { instance: 'cfg-11111111-2222-4333-8444-555555555555', args: { configure: '/home/scene/Machines/Debian 13.lordvm', name: 'Debian 13', section: 'startup' } });
    await sleep(2500);
    step('at login and away', has(L.atLogin) && has(L.autoPause));
    await shot('configure');
    log('done');
  })();
})();
