// driver.js — goes after the theme bundle in tests/machines_sheets_headless.sh: every sheet and dialog of Machines' Control
// Center at its first size (the manifest's 760×500), each opened the way a person opens it (a card's menu by a right
// click, the Machine menu, Configure's sidebar), a screenshot of each in the home folder (shot-1.png …) and a check that
// its buttons are drawn whole inside the window: Clone or Duplicate…, New Machine from Template… (and the Templates chip),
// Export… (and its save dialog), Bring In an Exported Machine… (its folder picker), a group's Destroy and Start Fresh… (with its Gateway box
// ticked, the tallest), Configure → Disks (Add Disk) and the recovery key's window. Texts are found in the scene's
// language (APP_M: machines.json, APP_S: shell.json). Lines start with "APP-SCENE" in the shell log.
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
  const step = (what, ok, extra) => log(`${what} ${ok ? 'done' : 'missing'}${extra ? ` (${extra})` : ''}`);
  const win = (id) => (sdk.windows.get() || []).find((x) => x.id === id);
  // a text's node (the button it labels: the text's parent) drawn whole inside the window: [ok, where]
  // a text's button box: of the nodes showing that text, the one on the row of `near` (the dialog's own primary button,
  // whose text is unique) when there is one, else the smallest (a node taken out may keep its text and a larger parent)
  const boxes = (label) => all(label).map((id) => box(P.parent.get(id) || id)).filter(Boolean).sort((x, y) => x[2] * x[3] - y[2] * y[3]);
  const inside = (label, winId = 'machines', near = null) => {
    const w = win(winId);
    const c = boxes(label);
    const b = (near && c.find((x) => Math.abs(x[1] + x[3] / 2 - (near[1] + near[3] / 2)) < 4)) || c[0];
    if (!w || !b) return [false, `${label}: ${w ? 'not drawn' : 'no window'}`];
    const ok = b[0] >= w.x - 0.5 && b[1] >= w.y - 0.5 && b[0] + b[2] <= w.x + w.w + 0.5 && b[1] + b[3] <= w.y + w.h + 0.5;
    return [ok, `${label} at ${b.map(Math.round).join(',')} in ${[w.x, w.y, w.w, w.h].map(Math.round).join(',')}`];
  };
  // labels: the dialog's buttons, its primary (unique) one last
  const fits = (what, labels, winId) => {
    const near = boxes(labels[labels.length - 1])[0] || null;
    const r = labels.map((l) => inside(l, winId, near));
    step(`${what}: its buttons inside the 760x500 window`, r.every((x) => x[0]), r.map((x) => x[1]).join('; '));
  };
  // a card's menu: a right click on the machine's name in the library, then the entry
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
  const cancel = async () => { click(byText(M.cancel)); await sleep(800); };
  (async () => {
    await sleep(2500);
    sdk.wm.open('machines', { args: { nonce: Date.now() } });
    await sleep(1500);
    const w = win('machines');
    if (w) { sdk.wm.resize(w.id, 760, 500); sdk.wm.center(w.id); }
    await sleep(2500);
    const w2 = win('machines');
    step('the Control Center at 760x500', !!w2 && Math.round(w2.w) === 760 && Math.round(w2.h) === 500, w2 ? `${w2.w}x${w2.h}` : 'none');
    await shot('library');
    // Clone or Duplicate…
    step('clone sheet', await cardMenu('Ubuntu 24.04', M['cmd.clone']) && has(f('clone.title', { name: 'Ubuntu 24.04' })));
    fits('clone', [M.cancel, M['clone.clone']]);
    await shot('clone');
    // its body scrolls: the wheel over it brings the name inside the machine and the copy's note into the window
    const nm = byText(M['new.name']);
    const q = nm && at(nm);
    if (q) for (let i = 0; i < 6; i++) { ev('wheel', nm, ...q, -2); await sleep(60); }
    await sleep(600);
    fits('clone, scrolled', [M['clone.hostname'], M.cancel, M['clone.clone']]);  // the field's label: not on the buttons' row, the only one
    await shot('clone-scrolled');
    await cancel();
    // Templates: the chip, then New Machine from Template…
    step('templates chip', click(byText(f('tpl.chip', { n: 1 }))));
    await sleep(900);
    await shot('templates');
    step('new from template', await cardMenu('Base Debian', M['tpl.newFrom']) && has(f('clone.title', { name: 'Base Debian' })));
    fits('new from template', [M.cancel, M['clone.clone']]);
    await shot('template-clone');
    await cancel();
    click(byText(f('lib.chip.all', { n: 3 })));
    await sleep(700);
    // Export…
    step('export sheet', await cardMenu('Ubuntu 24.04', M['cmd.export']) && has(f('exp.title', { name: 'Ubuntu 24.04' })));
    fits('export', [M.cancel, M['exp.choose']]);
    await shot('export');
    // Choose Where…: the save dialog over the whole window, not inside the sheet
    step('export: choose where', click(byText(M['exp.choose'])));
    await sleep(1500);
    fits('export (the save dialog)', [SH['picker.cancel'] || 'Cancel', SH['picker.save'] || 'Save']);
    await shot('export-picker');
    click(all(SH['picker.cancel'] || 'Cancel')[0]);
    await sleep(800);
    await cancel();
    // Bring In an Exported Machine…: its sheet, with the folder picker over it
    step('bring in', run(menuItem('machines', M['imp.menu'])));
    await sleep(1800);
    step('bring in: the folder picker', has(M['imp.pick']));
    fits('bring in (the picker)', [SH['picker.cancel'] || 'Cancel', SH['picker.chooseFolder'] || 'Choose this folder']);
    await shot('bring-in');
    click(all(SH['picker.cancel'] || 'Cancel')[0]);
    await sleep(900);
    // a group's Destroy and Start Fresh…, then with "Also reset the Gateway" ticked (its warning: the tallest)
    step('start fresh', await cardMenu('Whonix', M['group.menu.fresh']) && has(f('group.fresh.title', { name: 'Whonix' })));
    fits('start fresh', [M.cancel, M['group.fresh.ok']]);
    await shot('fresh');
    click(byText(M['group.fresh.gateway']));
    await sleep(700);
    step('start fresh: the gateway warning', has(M['group.fresh.guards']));
    fits('start fresh with the gateway', [M.cancel, M['group.fresh.ok']]);
    await shot('fresh-gateway');
    await cancel();
    // Configure… → Disks: Add Disk
    step('configure', await cardMenu('Ubuntu 24.04', M['cmd.configure']));
    step('disks section', click(byText(M['cfg.sec.title.disks'])));
    await sleep(1200);
    step('add disk shown', has(M['cfg.addDisk']));
    fits('configure: disks', [M['cfg.addDisk']]);
    await shot('cfg-disks');
    await cancel();
    // the recovery key's window (a window of its own, at the app's size; opened here unprotected, only so its picture can be
    // taken: the app opens it protected, never captured)
    sdk.wm.open('machines', { instance: 'key-scene', args: { recoveryKey: 'scene', name: 'Ubuntu 24.04', nonce: Date.now() } });
    await sleep(2000);
    const kw = (sdk.windows.get() || []).find((x) => String(x.id).includes('key-scene'));
    step('recovery key window', !!kw && has(f('key.recoveryTitle', { name: 'Ubuntu 24.04' })), kw ? `${kw.w}x${kw.h}` : 'none');
    if (kw) fits('recovery key', [M.cancel, M['key.recoveryOk']], kw.id);
    await shot('recovery-key');
    if (kw) sdk.wm.close(kw.id);
    await sleep(500);
    log('done');
  })().catch((e) => log(`error missing ${e && e.stack}`));
})();
