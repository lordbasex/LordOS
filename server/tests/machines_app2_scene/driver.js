// driver.js — goes after the theme bundle in tests/machines_app2_headless.sh: the app's last parts (docs/VIRTUALIZATION.md
// 5.1, 5.2, 5.5), a screenshot after each step in the home folder (shot-1.png …): the library with a template (its badge
// and the Templates chip) and a machine with its own color; the Templates chip; the New Machine assistant's Detect step
// on an arm64 disc (lord-vm --iso-detect for real: "it will run emulated") and its Name step (where it goes, the space
// there); Configure's General (icon, tags), CPU and memory (processor, huge pages), Disks (Add Disk), Network (New
// Address) and Sharing (printers). Nodes are found by the text they show (pre.js keeps it), in the scene's language (APP_M:
// machines.json, APP_S: shell.json). Lines start with "APP-SCENE" in the shell log.
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__appScene;
  const M = globalThis.APP_M || {}, SH = globalThis.APP_S || {};
  const f = (k, v) => Object.entries(v || {}).reduce((s, [a, b]) => s.split(`{${a}}`).join(String(b)), M[k] || k);
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
  const menuItem = (winId, label) => {
    for (const m of sdk.appMenus.get()[winId] || []) for (const it of m.items || []) if (it && it.label === label) return it;
    return null;
  };
  const run = (it) => { if (!it) return false; if (it.onClick) it.onClick(); else sdk.appMenuClick(it.id); return true; };
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(900);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  const step = (what, ok) => log(`${what} ${ok ? 'done' : 'missing'}`);
  const home = lord.fs.home();
  const cfg = async (section, check, name) => {
    sdk.wm.open('machines', { instance: `cfg-44444444-2222-4333-8444-555555555555-${section}`, args: { configure: `${home}/Machines/Ubuntu 24.04.lordvm`, name: 'Ubuntu 24.04', section, nonce: Date.now() } });
    await sleep(2200);
    step(`configure ${section}`, check.every((k) => has(M[k] || k)));
    await shot(name);
    const w = (sdk.windows.get() || []).filter((x) => String(x.id).includes(`cfg-44444444`)).pop();
    if (w) sdk.wm.close(w.id);
    await sleep(500);
  };
  (async () => {
    await sleep(2500);
    sdk.wm.open('machines', { args: { nonce: Date.now() } });
    await sleep(1500);
    const w = (sdk.windows.get() || []).find((x) => x.id === 'machines');
    if (w) { sdk.wm.resize(w.id, 1280, 800); sdk.wm.center(w.id); }
    await sleep(2500);
    step('template badge and chip', has(M['tpl.badge']) && has(f('tpl.chip', { n: 1 })));
    await shot('library');
    step('templates chip', click(byText(f('tpl.chip', { n: 1 }))));
    await sleep(900);
    step('only the template', has('Base Debian') && !all('Ubuntu 24.04').length);
    await shot('templates');
    click(byText(f('lib.chip.all', { n: 3 })));
    await sleep(600);
    // the assistant: Install from an ISO file, the scene's arm64 disc, Continue → Detect
    step('new machine', run(menuItem('machines', M.newMachine)));
    await sleep(1500);
    step('iso tile', click(byText(M['as.iso'])));
    await sleep(1500);
    const file = 'ubuntu-24.04.1-live-server-arm64.iso';
    const row = byText(file);
    step('file in the picker', !!row);
    if (row) { const p = at(row); ev('move', row, ...p); ev('down', row, ...p); ev('up', row, ...p); }
    await sleep(500);
    step('open', click(byText(SH['picker.open'] || 'Open')));
    await sleep(1500);
    step('continue to Detect', click(byText(M['as.continue'])));
    await sleep(3500);                                   // lord-vm --iso-detect, for real
    step('detect: emulated, arm64 from the boot files', has(f('as.det.emulated', { arch: 'arm64', host: 'amd64' })) && has(M['as.det.starts']) && has(M['as.title.detect']));
    await shot('detect');
    click(byText(M['as.continue']));
    await sleep(1200);
    click(byText(M['as.continue']));
    await sleep(1500);
    step('name: the place', has(M['as.place']) && has('~/Machines/'));
    await shot('name');
    click(byText(M.cancel));
    await sleep(800);
    await cfg('general', ['cfg.icon', 'cfg.tags'], 'cfg-general');
    await cfg('cpu', ['cfg.cpuModel', 'cfg.cpuHost', 'cfg.cpuCompat', 'cfg.hugePages'], 'cfg-cpu');
    await cfg('disks', ['cfg.addDisk', 'cfg.dataDisks'], 'cfg-disks');
    await cfg('network', ['cfg.macNew', 'cfg.mac'], 'cfg-network');
    await cfg('sharing', ['cfg.printers', 'cfg.printersNote'], 'cfg-sharing');
    log('done');
  })();
})();
