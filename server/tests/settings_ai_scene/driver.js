// driver.js — goes after the theme bundle in tests/app_scene_headless.sh settings_ai_scene (0.8.9.6.2): Settings →
// Artificial intelligence, the store around Lumo. Lumo with its five pieces and "Talk to Lumo" disabled, the search and
// its filters with counts, "What Lumo can do" (Sentinela's card opens its page), the capabilities as cards, the Voice
// capability page (reading aloud, the language's models and voices, own voices), a model's sheet, the Brain page and
// a coming-soon sheet (Image: the before and after), a search. Screenshots of each; run it in Arabic too for right to
// left. Lines start with "APP-SCENE".
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__appScene;
  const log = (s) => lord.log(`APP-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const ev = (type, id, x, y, b = 0) => globalThis.__lordEvent(type, id, x, y, b);
  const textId = (label) => { for (const [raw, s] of P.texts) if (s === label) { const id = P.parent.get(raw); if (id && box(id)) return id; } return null; };
  const textIds = (label) => { const out = []; for (const [raw, s] of P.texts) if (s === label) { const id = P.parent.get(raw); if (id && box(id)) out.push(id); } return out; };
  const shownTexts = () => { const out = []; for (const [raw, s] of P.texts) { const id = P.parent.get(raw); if (id && box(id)) out.push(s); } return out; };
  const step = (what, ok, extra) => log(`${what} ${ok ? 'done' : 'missing'}${extra ? ` (${extra})` : ''}`);
  const click = (id) => { const b = box(id); const x = b[0] + b[2] / 2, y = b[1] + b[3] / 2; ev('move', id, x, y); ev('down', id, x, y); ev('up', id, x, y); };
  const t = (k, v) => sdk.t(`settings.aiStore.${k}`, v);
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(900);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  const type = async (s) => { for (const ch of s) { globalThis.__lordKey(0, 1, ch, ch, ch, 0); globalThis.__lordKey(0, 0, ch, '', '', 0); await sleep(20); } };
  const key = (name, code) => { globalThis.__lordKey(code, 1, name, '', '', 0); globalThis.__lordKey(code, 0, name, '', '', 0); };
  const scrollTo = async (label) => { const id = textId(label); if (!id) return; const b = box(id); for (let i = 0; i < 14; i++) { ev('wheel', id, b[0] + 5, b[1] + 5, -3); await sleep(50); } };
  (async () => {
    await sleep(2500);
    sdk.wm.open('settings', { args: { section: 'ai' } });
    await sleep(2500);
    const w = (sdk.windows.get() || []).find((x) => x.app === 'settings');
    if (w) { sdk.wm.resize(w.id, 1180, 860); sdk.wm.center(w.id); }
    await sleep(2000);
    const all = shownTexts();
    step('Lumo: its title', all.includes(t('lumo.title')));
    step('Lumo: the five pieces', ['ear', 'brain', 'voice', 'eyes', 'hands'].every((p) => all.includes(t(`piece.${p}`))));
    step('Lumo: Talk to Lumo, coming soon', all.includes(t('lumo.talk')) && all.includes(t('lumo.talkSoon')));
    log(`pieces ready: ${all.find((s) => /\d/.test(s) && s.includes(String(5))) || '?'}`);
    step('the filters with counts', all.includes(t('filter.installed')) && all.includes(t('cap.eye')));
    step('What Lumo can do', all.includes(t('functions')) && all.includes(sdk.t('sentinela.name')) && all.includes(t('fn.machines')));
    step('levels', all.includes(t('level.read')) && all.includes(t('level.ask')) && all.includes(t('level.admin')));
    step('the capabilities', all.includes(t('capabilities')) && all.includes(t('tech.img')));
    step('Whisper is listed', all.includes('Whisper Small'));
    await shot('home');
    // a search (then cleared)
    const any = textId(t('foot.privacy')) || textId(t('cap.img'));
    if (any) { const b = box(any); for (let i = 0; i < 30; i++) { ev('wheel', any, b[0] + 5, b[1] + 5, 3); await sleep(40); } }
    await sleep(800);
    // (0.8.9.6.2) where they run: the Brain's Automatic / Processor / Graphics card and what the engine uses
    {
      const hint = textId(t('foot.brainHint'));
      if (hint) { const b = box(hint); for (let i = 0; i < 40; i++) { ev('wheel', hint, b[0] + 5, b[1] + 5, -3); await sleep(30); } }
      await sleep(600);
      const foot = shownTexts();
      const whereLine = foot.find((s) => /^(Brain|Cerebro|Cérebro|الدماغ)/.test(s) && s !== t('cap.brain')) || '';
      // (0.8.9.6.2) the Brain's cores: the slider's label and its hint, "Automatic (N)" or a count
      const [pre, post] = t('foot.brainCoresAuto', { n: '@' }).split('@');
      const coresLabel = foot.find((s) => s.startsWith(pre) && s.endsWith(post) && /^\d+$/.test(s.slice(pre.length, s.length - post.length))) || '';
      step('the Brain\'s cores', foot.includes(t('foot.brainCores')) && foot.includes(t('foot.brainCoresHint')) && !!coresLabel, coresLabel);
      step('the Brain\'s choice and where it runs', foot.includes(t('foot.brainHint')) && foot.filter((s) => s === sdk.t('settings.ai.backend.gpu')).length >= 2 && !!whereLine, whereLine);
      await shot('where they run');
      if (hint) { const b = box(hint); for (let i = 0; i < 40; i++) { ev('wheel', hint, b[0] + 5, b[1] + 5, 3); await sleep(30); } }
      await sleep(400);
    }
    const search = textId(t('search'));
    if (search) { click(search); await sleep(300); click(search); }
    await sleep(600);
    await type('whisper');
    await sleep(1500);
    const found = shownTexts();
    log(`search: field ${search ? 'found' : 'not found'}, typed ${found.includes('whisper') ? 'yes' : 'no'}`);
    step('a search finds Whisper only', found.includes('Whisper Base') && !found.includes('Qwen3 1.7B'));
    await shot('search');
    for (let i = 0; i < 8; i++) { globalThis.__lordKey(14, 1, 'Backspace', '', '', 0); globalThis.__lordKey(14, 0, 'Backspace', '', '', 0); await sleep(20); }
    await sleep(1000);
    await scrollTo(t('functions'));
    await sleep(800);
    await shot('home-rows');
    // the bottom: space, where they run, privacy
    await scrollTo(t('cap.img'));
    await sleep(500);
    await scrollTo(t('cap.img'));
    await sleep(800);
    const bottom = shownTexts();
    step('the bottom: space, where, privacy', bottom.includes(t('foot.space')) && bottom.includes(t('foot.where')) && bottom.includes(t('foot.privacy')));
    await shot('footer');

    // the Voice capability page, from its card
    const voiceCards = textIds(t('cap.voice'));
    if (!voiceCards.length) { step('the Voice card', false); log('done'); return; }
    sdk.wm.open('settings', { args: { section: 'ai', page: 'voice', nonce: Date.now() } });
    await sleep(2000);
    const vp = shownTexts();
    step('Voice page: the breadcrumb', vp.filter((s) => s === sdk.t('settings.section.ai')).length >= 1 && vp.includes(t('cap.voice')));
    step('Voice page: reading aloud and quality', vp.includes(sdk.t('settings.ai.readAloud')) && vp.includes(sdk.t('settings.ai.quality')));
    step('Voice page: the models', vp.includes(sdk.t('settings.ai.standard')) && vp.includes(sdk.t('settings.ai.hd')));
    step('Voice page: Lola', vp.includes('Lola'));
    await shot('voice-page');
    // a voice's sheet
    const lola = textId('Lola');
    if (lola) {
      click(lola);
      await sleep(1500);
      const sh = shownTexts();
      step('a voice sheet: its facts and Listen', sh.includes(t('fact.size')) && sh.includes(t('try.listen')));
      await shot('voice-sheet');
      key('Escape', 1);
      await sleep(800);
    } else step('Lola card', false);
    // back: Esc on the page
    key('Escape', 1);
    await sleep(1200);
    step('Esc goes back to the store', shownTexts().includes(t('lumo.title')));

    // the Brain page and a model's sheet
    sdk.wm.open('settings', { args: { section: 'ai', page: 'brain', nonce: Date.now() } });
    await sleep(2000);
    const qwen = textId('Qwen3 1.7B');
    step('Brain page: Qwen3', !!qwen && shownTexts().includes(t('brainNote')));
    await shot('brain-page');
    if (qwen) {
      click(qwen);
      await sleep(1500);
      const sh = shownTexts();
      step('Qwen3 sheet: license, try it', sh.includes('Apache-2.0') && sh.includes(t('try.title')));
      await shot('brain-sheet');
      key('Escape', 1);
      await sleep(800);
    }
    // Image: coming soon, the before and after
    sdk.wm.open('settings', { args: { section: 'ai', page: 'img', nonce: Date.now() } });
    await sleep(2000);
    const esr = textId('Real-ESRGAN x4');
    if (esr) {
      click(esr);
      await sleep(1800);
      const sh = shownTexts();
      step('Real-ESRGAN sheet: coming soon, before and after, no install', sh.includes(t('state.soon')) && sh.includes(t('try.compare')) && !sh.includes(t('install', { size: '64 MB' })) && !sh.includes(t('install', { size: '64.0 MB' })));
      await shot('image-sheet');
      key('Escape', 1);
      await sleep(800);
    } else step('Real-ESRGAN card', false);
    // Sentinela: its own page, from the function card
    sdk.wm.open('settings', { args: { section: 'ai', nonce: Date.now() } });
    await sleep(1500);
    key('Escape', 1);
    await sleep(800);
    const sent = textIds(sdk.t('sentinela.name'))[0];
    if (sent) {
      click(sent);
      await sleep(1500);
      step('Sentinela page: its switch', shownTexts().includes(sdk.t('sentinela.settings.on')));
      await shot('sentinela-page');
      key('Escape', 1);
      await sleep(1000);
    } else step('Sentinela card', false);
    // (0.8.9.6.2) Hands: each command has Copy, which puts exactly that command on the clipboard and says Copied
    sdk.wm.open('settings', { args: { section: 'ai', page: 'hands', nonce: Date.now() } });
    await sleep(2000);
    {
      const CMD = 'claude mcp add -s user lordos-mcp -- lordos-mcp';
      // to the bottom of the page, where the commands are
      const title = textId(sdk.t('hands.settings.howTitle'));
      if (title) { const b = box(title); for (let i = 0; i < 40; i++) { ev('wheel', title, b[0] + 5, b[1] + 5, -3); await sleep(30); } }
      await sleep(600);
      const cmd = textId(CMD);
      step('Hands: the command with -s user is shown', !!cmd);
      step('Hands: the QA command is shown too', !!textId('lordos-mcp --headless notes'));
      const under = (id, top) => { for (let n = id; n; n = P.parent.get(n)) if (n === top) return true; return false; };
      const row = cmd ? P.parent.get(P.parent.get(cmd)) : null;   // the text, its column, the block's row
      const copy = row ? textIds(sdk.t('shell.code.copy')).find((id) => under(id, row)) : null;
      step('Hands: the command has a Copy button', !!copy);
      if (copy) {
        sdk.clipboard.set('something else');
        click(copy);
        await sleep(400);
        const got = String(sdk.clipboard.get() || '');
        step('Hands: Copy puts the exact command on the clipboard', got === CMD, JSON.stringify(got));
        step('Hands: the button says Copied', textIds(sdk.t('shell.code.copied')).some((id) => under(id, row)));
        await shot('hands-copied');
      }
    }
    log('done');
  })();
})();
