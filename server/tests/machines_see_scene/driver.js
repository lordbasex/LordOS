// driver.js — goes after the theme bundle in tests/machines_see_headless.sh: Configure → Security's "Guest can see" list
// (every channel's row, its state and switch), Configure → Boot's Start Linux directly (a machine with a chosen kernel, and
// one with Secure Boot that refuses it), the generic Requirements page and a chosen system's own (from the assistant's
// Review, What's Missing), each as a screenshot in the home folder (shot-1.png …). Texts are found in the scene's
// language (APP_M: machines.json). Lines start with "APP-SCENE" in the shell log.
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__appScene;
  const M = globalThis.APP_M || {};
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
  const shown = (text) => all(text).length > 0;
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
  const step = (what, ok, extra) => log(`${what} ${ok ? 'done' : 'missing'}${extra ? ` (${extra})` : ''}`);
  const win = (id) => (sdk.windows.get() || []).find((x) => x.id === id);
  const cardMenu = async (name, entry) => {
    const id = byText(name);
    const p = id && at(id);
    if (!p) return false;
    ev('move', id, ...p, 2); ev('down', id, ...p, 2); ev('up', id, ...p, 2);
    await sleep(700);
    const ok = click(byText(entry));
    await sleep(1200);
    return ok;
  };
  // the wheel over a node, to bring what is under it into view
  const wheel = async (text, times = 6, dy = -3) => { const id = byText(text); const q = id && at(id); if (q) for (let i = 0; i < times; i++) { ev('wheel', id, ...q, dy); await sleep(60); } await sleep(500); };
  (async () => {
    await sleep(2500);
    sdk.wm.open('machines', { args: { nonce: Date.now() } });
    await sleep(1500);
    const w = win('machines');
    if (w) { sdk.wm.resize(w.id, 1100, 760); sdk.wm.center(w.id); }
    await sleep(2500);
    // Configure → Security: Guest can see
    step('configure', await cardMenu('Ubuntu 24.04', M['cmd.configure']));
    step('security section', click(byText(M['cfg.sec.title.security'])));
    await sleep(1200);
    step('guest can see: its title', shown(M['cfg.see.title']));
    const rows = ['network', 'display', 'input', 'sound', 'microphone', 'clipIn', 'clipOut', 'folders', 'transfer', 'agent', 'automation', 'files', 'links', 'notify'];
    const missing = rows.filter((r) => !has(M[`cfg.see.row.${r}`]));
    step('guest can see: every channel listed', !missing.length, missing.join(', '));
    step('guest can see: the network mode and the 3D line', has(f('cfg.see.network', { mode: M['cfg.net.shared'] })) && has(M['cfg.see.display.2d']));
    step('guest can see: the folder count', has(f('cfg.see.folders', { n: 1 })));
    await shot('guest-can-see');
    await wheel(M['cfg.see.row.transfer'], 8);
    await shot('guest-can-see-scrolled');
    // Show: a row with a list opens its own section
    const show = all(M['cfg.see.show']);
    step('guest can see: Show opens the section', show.length > 0 && click(show[show.length - 1]));
    await sleep(1000);
    await shot('guest-can-see-show');
    // Configure → Boot: Start Linux directly with a kernel kept
    step('boot section', click(byText(M['cfg.sec.title.boot'])));
    await sleep(1200);
    await wheel(M['cfg.bootMenu'], 8);
    step('start linux directly: its group', has(M['cfg.kboot.title']) && has(M['cfg.kboot.on']));
    step('start linux directly: kernel and initrd kept, the command line', all(M['cfg.kboot.kept']).length >= 2 && has(M['cfg.kboot.cmdline']));
    await shot('kboot');
    click(byText(M.cancel));
    await sleep(900);
    // Base Debian: Secure Boot refuses it, with why
    step('configure (secure boot)', await cardMenu('Base Debian', M['cmd.configure']));
    step('boot section (secure boot)', click(byText(M['cfg.sec.title.boot'])));
    await sleep(1200);
    await wheel(M['cfg.bootMenu'], 8);
    step('start linux directly: refused with Secure Boot, said why', has(M['cfg.kboot.no.secureBoot']) && !has(M['cfg.kboot.kept']));
    await shot('kboot-secureboot');
    click(byText(M.cancel));
    await sleep(900);
    // the generic Requirements page (the Machine menu)
    step('requirements', run(menuItem('machines', M['req.menu'])));
    await sleep(1500);
    step('requirements: the generic page', has(M['req.page']) && has(M['req.row.tpm']) && !has(M['req.row.answers']));
    await shot('requirements');
    click(byText(M.back));
    await sleep(900);
    // a chosen system's page: New Machine → the virtio-win card (its image is pinned in the signed catalog: no list to fetch) → Continue to the Review → What's Missing
    step('new machine', click(byText(M.newMachine)) || click(byText(M['new.title'])));
    await sleep(1500);
    step('assistant: the virtio-win card', click(byText('Windows 11')));
    await sleep(1200);
    await wheel(M['as.title.choose'], 10);
    step('assistant: the 90-day image', click(byText(M['win.eval'])));
    await sleep(1000);
    for (let i = 0; i < 8 && !has(M['as.title.review']); i++) { click(byText(M['as.continue'])); await sleep(1300); }
    step('assistant: the review', has(M['as.title.review']));
    await shot('review');
    step('assistant: What\'s Missing', click(byText(M['req.button'])));
    await sleep(1500);
    const title = [...P.texts.values()].find((s) => s.startsWith(f('req.pageFor', { name: '' }).trim().split(' ')[0]) && s !== M['req.page']) || '';
    step('requirements: the system\'s own page', has(f('req.pageFor', { name: 'Windows 11' })), title);
    step('requirements: its rows (the TPM, drivers, WinFsp, its tools, the network missing)', has(M['req.row.tpm']) && has(M['req.row.drivers']) && has(M['req.row.winfsp']) && has(M['req.row.toolsWin']) && has(M['req.st.download']));
    await shot('requirements-system');
    log('done');
  })().catch((e) => log(`error missing ${e && e.stack}`));
})();
