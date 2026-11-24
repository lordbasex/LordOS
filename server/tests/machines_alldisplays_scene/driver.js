// driver.js — goes after the theme bundle in tests/machines_alldisplays_headless.sh: a running machine's window, then View →
// All Displays: the machine's window full screen on the left display, output 1's window full screen on the right one,
// lord.vm.allDisplays given both displays' sizes and output 1 attached; then output 1's Back to One Display: its window
// goes, the machine's window leaves full screen and lord.vm.allDisplays(…, null) puts one display back. Screenshots in
// the home folder (shot-1.png …); lines start with "AD-SCENE" in the shell log.
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__appScene, S = globalThis.__adScene;
  const L = globalThis.APP_L || {};
  const log = (s) => lord.log(`AD-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const ev = (type, id, x, y, b = 0) => globalThis.__lordEvent(type, id, x, y, b);
  const at = (id) => { const l = box(id); return l ? [l[0] + l[2] / 2, l[1] + l[3] / 2] : null; };
  const click = (id, b = 0) => { const p = id && at(id); if (!p) return false; ev('move', id, ...p, b); ev('down', id, ...p, b); ev('up', id, ...p, b); return true; };
  const byText = (text) => { for (const [raw, s] of P.texts) if (s.trim() === text) { const id = P.parent.get(raw); if (id && box(id)) return id; } return null; };
  const menuItem = (winId, label) => {
    for (const m of sdk.appMenus.get()[winId] || []) for (const it of m.items || []) if (it && it.label === label) return it;
    return null;
  };
  const win = (id) => (sdk.windows.get() || []).find((x) => x.id === id) || null;
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(900);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  const step = (what, ok, more = '') => log(`${what} ${ok ? 'done' : 'missing'}${more ? ` (${more})` : ''}`);
  const MID = '11111111-2222-4333-8444-555555555555';
  const MAIN = `machines#vm-${MID}`, HEAD = `machines#vmhead-${MID}-1`;
  (async () => {
    await sleep(2500);
    step('two displays', sdk.screenDisplays().length === 2, `${sdk.screenDisplays().length}`);
    // the guests' picture: a still of the desktop
    S.picture = await sdk.media.screenshot('screenshots');
    sdk.wm.open('machines', { instance: `vm-${MID}`, args: { machine: '/home/scene/Machines/Debian 13.lordvm', name: 'Debian 13' } });
    await sleep(800);
    sdk.wm.resize(MAIN, 640, 520);
    sdk.wm.moveToDisplay(MAIN, 11);
    await sleep(3500);
    const it = menuItem(MAIN, L.item);
    step('All Displays in the View menu', !!it && !it.disabled, it ? `disabled=${!!it.disabled} hint=${it.hint || ''}` : 'no item');
    await shot('window');
    if (it) it.onClick();
    await sleep(3000);
    step('lord.vm.allDisplays got both displays', S.calls[0] === `${L.half}x${L.h},${L.half}x${L.h}`, S.calls.join(' | '));
    step('the machine\'s window full screen on the left', !!win(MAIN)?.fullscreen && sdk.displayFor(sdk.wm.frame(win(MAIN))).id === 11);
    step('output 1\'s window full screen on the right', !!win(HEAD)?.fullscreen && sdk.displayFor(sdk.wm.frame(win(HEAD))).id === 12);
    step('output 1 attached', S.attached.includes(1), S.attached.join(','));
    step('View says All Displays', !!menuItem(MAIN, L.item)?.checked);
    await shot('all-displays');
    // output 1's bar, at its top edge: Back to One Display
    const hf = sdk.screenDisplays().find((d) => d.id === 12);    // full screen: the window is its display
    // the node that covers output 1's window (its picture's box): the newest one with the window's whole frame
    // (the deepest of them: the picture inside the window's frame)
    let pic = 0, best = -1;
    const depth = (id) => { let d = 0; for (let p = P.parent.get(id); p && d < 200; p = P.parent.get(p)) d++; return d; };
    for (const id of P.parent.keys()) {
      const l = box(id);
      if (!l || Math.abs(l[0] - hf.x) > 1 || Math.abs(l[1] - hf.y) > 1 || Math.abs(l[2] - hf.w) > 1 || Math.abs(l[3] - hf.h) > 1) continue;
      const d = depth(id);
      if (d > best) { best = d; pic = id; }
    }
    log(`head frame ${JSON.stringify(hf)} node ${pic} ${JSON.stringify(box(pic))}`);
    ev('move', pic, hf.x + hf.w / 2, hf.y + 2);
    await sleep(800);
    await shot('head-bar');
    step('Back to One Display', click(byText(L.back)));
    await sleep(2500);
    step('output 1\'s window gone', !win(HEAD));
    step('one display again (allDisplays null)', S.calls[S.calls.length - 1] === 'off', S.calls.join(' | '));
    step('the machine\'s window out of full screen', !win(MAIN)?.fullscreen);
    step('output 1 let go of', S.closed.includes(1));
    await shot('one-display');
    log('done');
  })();
})();
