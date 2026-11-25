// driver.js — goes after the theme bundle in tests/machines_group_headless.sh: it opens Machines on pre.js's made-up
// library and saves a screenshot after each step in the home folder (shot-1.png …): the library with the group's one
// card; its menu; Destroy and Start Fresh's dialog, then with "Also reset the Gateway" (the entry guards warning);
// Configure's group view; the New Machine assistant with the Whonix (Tor) card chosen; the About Whonix step.
// Nodes are found by the text they show (pre.js keeps it), in the scene's language (GROUP_L: the labels). Lines start
// with "GROUP-SCENE" in the shell log.
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__groupScene;
  const L = globalThis.GROUP_L || {};
  const log = (s) => lord.log(`GROUP-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const ev = (type, id, x, y, b = 0) => globalThis.__lordEvent(type, id, x, y, b);
  const at = (id) => { const l = box(id); return l ? [l[0] + l[2] / 2, l[1] + l[3] / 2] : null; };
  const click = (id, b = 0) => { const p = id && at(id); if (!p) return false; ev('move', id, ...p, b); ev('down', id, ...p, b); ev('up', id, ...p, b); return true; };
  const key = (name, flags = 0) => { globalThis.__lordKey(0, 1, name, '', name.toLowerCase(), flags); globalThis.__lordKey(0, 0, name, '', '', flags); };
  // the shown nodes with this text (the text's own node), newest first
  const all = (text) => {
    const out = [];
    for (const [raw, s] of P.texts) if (s.trim() === text) { const id = P.parent.get(raw); if (id && box(id)) out.push(id); }
    return out.sort((a, b) => b - a);
  };
  const byText = (text) => all(text)[0] || null;
  // the smallest shown node with this text: a dialog's own label, never a stale entry of a node id used again since
  const label = (text) => all(text).map((id) => [id, box(id)]).sort((a, b) => a[1][2] * a[1][3] - b[1][2] * b[1][3]).map((x) => x[0])[0] || null;
  // the "···" button nearest to a text, on its right and about its height (the card's own menu button)
  const moreNear = (text) => {
    const t = byText(text), tl = t && box(t);
    if (!tl) return null;
    let best = null, bd = 1e9;
    for (const id of all('···')) { const l = box(id); const d = Math.abs(l[1] - tl[1]) + Math.abs(l[0] - tl[0]) / 4; if (l[0] > tl[0] && d < bd) { bd = d; best = id; } }
    return best;
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
    // a roomy window, in the middle of the screen
    const w = (sdk.windows.get() || []).find((x) => String(x.id).startsWith('machines'));
    if (w) { sdk.wm.resize(w.id, 1280, 800); sdk.wm.center(w.id); }
    await sleep(2500);
    await shot('library');
    // the assistant: the Whonix (Tor) card chosen, then About Whonix
    step('new', click(byText('+')));
    await sleep(1800);
    step('whonix card', click(byText(L.card)));
    await sleep(2000);
    await shot('assistant-card');
    step('continue', click(byText(L.continue)));
    await sleep(1500);
    await shot('about');
    step('close', click(byText('✕')));
    await sleep(900);
    // the group's menu, Destroy and Start Fresh, Configure
    step('menu', click(moreNear('Whonix')));
    await sleep(900);
    await shot('menu');
    step('fresh', click(byText(L.fresh)));
    await sleep(900);
    await shot('fresh');
    step('also gateway', click(byText(L.alsoGateway)));
    await sleep(700);
    await shot('fresh-gateway');
    step('cancel', click(label(L.cancel)));
    await sleep(600);
    // Rename… and Delete… on Whonix (19.8)
    step('menu for rename', click(moreNear('Whonix')));
    await sleep(700);
    step('rename', click(byText(L.rename)));
    await sleep(1000);
    await shot('rename');
    step('cancel rename', click(label(L.cancel)));
    await sleep(600);
    step('menu for delete', click(moreNear('Whonix')));
    await sleep(700);
    step('delete', click(byText(L.delete)));
    await sleep(1000);
    await shot('delete');
    step('cancel delete', click(label(L.cancel)));
    await sleep(600);
    // the damaged group (19.9): its card, its menu, Repair…
    await shot('damaged-card');
    step('damaged menu', click(moreNear('Tor Work')));
    await sleep(800);
    await shot('damaged-menu');
    step('repair', click(byText(L.repair)));
    await sleep(1000);
    await shot('repair');
    step('cancel repair', click(label(L.cancel)));
    await sleep(600);
    step('menu again', click(moreNear('Whonix')));
    await sleep(700);
    step('configure', click(byText(L.configure)));
    await sleep(1800);
    await shot('configure');
    log('done');
  })().catch((e) => log(`error ${e && e.stack}`));
})();
