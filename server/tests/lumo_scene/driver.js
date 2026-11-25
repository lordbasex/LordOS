// driver.js — goes after the theme bundle in tests/app_scene_headless.sh lumo_scene: the 3D Lumo where LordOS shows it
// (sdk/mascot.js, docs/LORDOS-BRAND.md "Lumo"): the first-login tour's wave (120 px), an empty state's confused pose
// (Help with an article that does not exist, EmptyState at 112 px) and About's avatar (24 px, the small crop). Checks that
// each picture is on screen with the render lumoSource picks, and keeps a screenshot of each, plus Calendar without its
// service (run it with build/lord-maild not executable) and the installer's welcome for docs/brand/mascot/screens. A test tool, never shipped. Lines start with "APP-SCENE".
(function () {
  const sdk = globalThis.__lordos.require('@lordos/sdk');
  const P = globalThis.__appScene;
  const log = (s) => lord.log(`APP-SCENE ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const box = (id) => { const l = lord.getLayout(id); return l && l[2] > 0 && l[3] > 0 ? l : null; };
  const step = (what, ok, extra) => log(`${what} ${ok ? 'done' : 'missing'}${extra ? ` (${extra})` : ''}`);
  // the image nodes on screen whose source is a Lumo render, with their size
  const lumos = () => {
    const out = [];
    for (const [id, src] of P.images) if (/^asset:mascot\/lumo-/.test(src) && box(id)) out.push(`${src.slice(13)}@${Math.round(box(id)[2])}`);
    return out;
  };
  let n = 0;
  const shot = async (name) => {
    n++;
    await sleep(800);
    const id = await sdk.media.screenshot('screenshots');
    const r = id ? await sdk.media.saveImage(id, `${lord.fs.home()}/shot-${n}.png`, { format: 'png' }) : null;
    log(`shot ${n} ${name} ${r && r.ok !== false ? 'saved' : 'failed'}`);
  };
  const front = async (app, args, w, h) => {
    for (const x of sdk.windows.get() || []) if (x.app !== app) sdk.wm.close?.(x.id);
    sdk.wm.open(app, { args });
    await sleep(2500);
    const win = (sdk.windows.get() || []).find((x) => x.app === app);
    if (win) { sdk.wm.resize(win.id, w, h); sdk.wm.center(win.id); }
    await sleep(1500);
  };
  (async () => {
    await sleep(2500);
    await front('help', { tour: true, nonce: 1 }, 760, 560);
    let seen = lumos();
    step('the tour shows Lumo waving', seen.some((s) => s.startsWith('lumo-wave-128.png@120')), seen.join(' '));
    await shot('tour-wave');
    await front('help', { article: 'no-such-article', nonce: 2 }, 760, 560);
    seen = lumos();
    step('an empty state shows Lumo confused', seen.some((s) => s.startsWith('lumo-confused-128.png@112')), seen.join(' '));
    await shot('empty-confused');
    // the brand kit's pictures (docs/brand/mascot/screens): Calendar without its service and the installer's welcome;
    // reported, not checked (the service and the installer depend on the machine)
    await front('calendar', {}, 960, 600);
    log(`calendar: ${lumos().join(' ') || 'no Lumo'}`);
    await shot('calendar-unavailable');
    await front('installer', {}, 900, 640);
    log(`installer: ${lumos().join(' ') || 'no Lumo'}`);
    await shot('installer-welcome');
    await front('about', {}, 640, Math.min(820, lord.screen.height - 110));   // the avatar is at the bottom: SCENE_SIZE=1280x1000 shows it whole
    seen = lumos();
    step('About shows the small avatar', seen.some((s) => s.startsWith('lumo-avatar-small-32.png@24')), seen.join(' '));
    await shot('about-avatar');
    log('done');
  })();
})();
