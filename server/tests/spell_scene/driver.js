// driver.js — goes after the theme bundle in tests/app_scene_headless.sh spell_scene (docs/SPELLING.md "Screenshots"):
// Settings → Keyboard's test field (a TextInput) with misspelled words underlined, the right-click menu with
// suggestions, Add to dictionary and Ignore, a suggestion replacing the word, then the whole text selected and
// ✨ AI ▸ Correct spelling and grammar: the shell's panel streaming the answer, done, and Replace. The Spelling sections
// of the page are in the pictures. Lines start with "APP-SCENE".
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__appScene;
  const log = (s) => lord.log(`APP-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const ev = (type, id, x, y, b = 0) => globalThis.__lordEvent(type, id, x, y, b);
  const textId = (label) => { for (const [raw, s] of P.texts) if (s === label) { const id = P.parent.get(raw); if (id && box(id)) return id; } return null; };
  const shownTexts = () => { const out = []; for (const [raw, s] of P.texts) { const id = P.parent.get(raw); if (id && box(id)) out.push(s); } return out; };
  const step = (what, ok, extra) => log(`${what} ${ok ? 'done' : 'missing'}${extra ? ` (${extra})` : ''}`);
  const click = (id, b = 0) => { const l = box(id); if (!l) return false; const x = l[0] + l[2] / 2, y = l[1] + l[3] / 2; ev('move', id, x, y); ev('down', id, x, y, b); ev('up', id, x, y, b); return true; };
  const at = (id, x, y, b = 0) => { ev('move', id, x, y); ev('down', id, x, y, b); ev('up', id, x, y, b); };
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(900);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  const type = async (s) => { for (const ch of s) { globalThis.__lordKey(30, 1, ch, ch, ch.toLowerCase(), 0); await sleep(8); globalThis.__lordKey(30, 0, ch, '', '', 0); await sleep(8); } };
  const key = (name, chr = '', flags = 0) => { globalThis.__lordKey(30, 1, name, chr, (chr || name).toLowerCase(), flags); globalThis.__lordKey(30, 0, name, '', '', flags); };
  const tr = sdk.t;
  (async () => {
    await sleep(2500);
    sdk.settings.set((s) => ({ ...s, writing: { on: true }, sentinela: { ...(s.sentinela || {}), model: 'qwen3-1.7b' } }));
    sdk.wm.open('settings', { args: { section: 'keyboard' } });
    await sleep(2500);
    const w = (sdk.windows.get() || []).find((x) => x.app === 'settings');
    if (w) { sdk.wm.resize(w.id, 980, 760); sdk.wm.center(w.id); }
    await sleep(1500);
    // the test field is further down the page: scroll to it
    const anchor = textId(tr('settings.kb.test'));
    if (anchor) { const b = box(anchor); for (let i = 0; i < 30; i++) { ev('wheel', anchor, b[0] + 5, b[1] + 5, 3); await sleep(40); } }
    await sleep(800);
    const ph = textId(tr('settings.kb.placeholder'));
    step('the test field', !!ph);
    if (!ph) { log('done'); return; }
    const field = P.parent.get(P.parent.get(ph)) || P.parent.get(ph);
    click(ph);
    await sleep(300);
    const sentence = 'I recieve teh wierd letter untill today, and I definately need to answer it.';
    await type(sentence);
    await sleep(400);
    key('End');
    await sleep(1500);
    step('the words were asked about (in a batch, after the pause)', ['recieve', 'teh', 'wierd'].every((x) => P.asked.includes(x)), P.asked.join(' '));
    step('the Spelling section is on the page', shownTexts().includes(tr('spell.settings.languages')) && shownTexts().includes(tr('spell.settings.check')));
    await shot('underlined');
    // a right click on "recieve": the suggestions first
    const fb = box(field);
    const tx = fb[0] + 11 + lord.measureText('I rec', 13, 400);
    at(field, tx, fb[1] + fb[3] / 2, 2);
    await sleep(900);
    const menu = shownTexts();
    step('suggestions first in the menu', menu.includes('receive') && menu.includes('relieve'));
    step('Add to dictionary and Ignore', menu.includes(tr('spell.menu.add', { word: 'recieve' })) && menu.includes(tr('spell.menu.ignore')));
    step('the field\'s own rows follow', menu.includes(tr('shell.textMenu.paste')));
    await shot('suggestions');
    const rec = textId('receive');
    if (rec) click(rec);
    await sleep(900);
    // the whole text selected, then ✨ AI
    click(field);
    await sleep(200);
    key('a', 'a', 2);
    await sleep(300);
    at(field, fb[0] + 60, fb[1] + fb[3] / 2, 2);
    await sleep(900);
    const aiLabel = `✨ ${tr('writing.menu.ai')}`;
    const ai = textId(aiLabel);
    step('✨ AI in the menu with a selection', !!ai);
    if (ai) { const b = box(ai); ev('move', ai, b[0] + 10, b[1] + b[3] / 2); await sleep(150); ev('move', ai, b[0] + 30, b[1] + b[3] / 2); }
    await sleep(1200);
    const sub = shownTexts();
    step('its submenu: the actions and Translate to', sub.includes(tr('writing.action.fix')) && sub.includes(tr('writing.action.shorter')) && sub.includes(tr('writing.menu.translate')));
    await shot('ai-menu');
    const fix = textId(tr('writing.action.fix'));
    if (fix) click(fix);
    await sleep(500);
    await shot('panel-streaming');
    await sleep(2500);
    const asked = globalThis.__writingAsked || {};
    step('only the selection and a fixed action went to lord.writing', asked.action === 'fix' && asked.text && asked.text.startsWith('I receive teh') && asked.model === 'qwen3-1.7b', JSON.stringify(asked));
    const panel = shownTexts();
    step('the panel: the answer, Replace, Copy, Discard', panel.some((s) => s.includes('definitely need to answer')) && panel.includes(tr('writing.panel.replace')) && panel.includes(tr('writing.panel.discard')));
    await shot('panel-done');
    const rep = textId(tr('writing.panel.replace'));
    if (rep) click(rep);
    await sleep(1200);
    const after = shownTexts();
    step('Replace put the answer in the field', after.some((s) => s.startsWith('I received the weird letter only today')));
    await shot('replaced');
    log('done');
  })().catch((e) => { log(`error ${e && e.stack}`); log('done'); });
})();
