// firststart_scene.mjs — the first-start desktop chooser (sdk/firststart.jsx, 0.8.9) over each theme, run by
// tests/workspaces_test.mjs: node tests/workspaces_test.mjs firststart_scene.mjs <default|classic|workspaces>. One
// 1920×1080 display, a stub lord.themes with the three desktops. It walks the whole wizard: step 1 with no choice (the
// main button does nothing), a card, See it live (the theme switch is asked for, then the JS context "reloads" and
// resumes on step 2 from the saved settings), the floating box dragged and kept inside the work area, Escape back to
// step 1, forward again without a second switch, step 3's replica of Settings → Appearance, Back, Start: the flag is set,
// the state cleared, nothing drawn and no timer left. Prints "ok <name>" or "FAIL <name> <detail>".
const live = new Map();
const realSet = globalThis.setInterval, realClear = globalThis.clearInterval;
globalThis.setInterval = (fn, ms, ...a) => { const id = realSet(fn, ms, ...a); live.set(id, ms); return id; };
globalThis.clearInterval = (id) => { live.delete(id); realClear(id); };

const React = (await import('react')).default;
const rt = await import('@lordos/runtime');
const sdk = await import('@lordos/sdk');
const theme = globalThis.__themeUnderTest;
if (theme === 'default') await import('../../themes/default/index.jsx');
else if (theme === 'classic') await import('../../themes/classic/index.jsx');
else await import('../../themes/workspaces/index.jsx');
const { texts, styles, parents, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 30; i++) await new Promise((res) => setImmediate(res)); await new Promise((res) => setTimeout(res, 5)); for (let i = 0; i < 10; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, got, want) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  console.log(g === w ? `ok ${name}` : `FAIL ${name} got ${g}, want ${w}`);
};
const attached = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return parents.has(id); };
const shown = (v) => [...texts].filter(([id, x]) => x === v && attached(id)).map(([id]) => id);
const click = async (id) => { const p = parents.get(id) ?? id; globalThis.__lordEvent('down', p, 0, 0, 0); globalThis.__lordEvent('up', p, 0, 0, 0); await flush(); };
const key = async (name) => { globalThis.__lordKey(0, 1, name, '', name.toLowerCase(), 0); globalThis.__lordKey(0, 0, name, '', '', 0); await flush(); };

const D = { id: 1, name: 'LordOS', connector: 'DP-1', x: 0, y: 0, w: 1920, h: 1080, scale: 100, main: true };
lord.screen.displays = [D]; lord.screen.width = 1920; lord.screen.height = 1080; globalThis.__lordScreen();

// the three desktops; use() records the switch (lord-server would discard this context and load the new theme)
let current = theme;
const switched = [];
const NAMES = { default: 'Flow', classic: 'Classic', workspaces: 'Workspaces' };
lord.themes = {
  list: () => ['default', 'classic', 'workspaces', 'greeter'].map((id) => ({ id, current: id === current,
    meta: JSON.stringify(id === 'greeter' ? { hidden: true } : { name: NAMES[id], description: `${id} description`, ...(id === 'workspaces' ? { sketch: { rail: true, pills: true, frame: true } } : null) }) })),
  use: (id) => { switched.push(id); return true; },
};
let done = 0;
const ctx = { list: sdk.themes.list, current: sdk.themes.current, use: (id) => sdk.themes.use(id), onDone: () => { done++; } };

function Shell() {
  return h(React.Fragment, null, globalThis.__shellElement, h(sdk.FirstStart));
}
rt.render(h(Shell));
await flush();
const timersBefore = live.size;
check('a new account sees it', sdk.startFirstStart(ctx), true);
await flush();
check('step 1: the title', shown('shell.firstStart.title').length, 1);
check('step 1: three cards with their names and descriptions', ['Flow', 'Classic', 'Workspaces', 'default description', 'classic description', 'workspaces description'].map((v) => shown(v).length), [1, 1, 1, 1, 1, 1]);
check('step 1: the crown and the three pictures', ['asset:icons/lordos-crown-512.png', 'asset:desktops/default.jpg', 'asset:desktops/classic.jpg', 'asset:desktops/workspaces.jpg']
  .map((src) => [...globalThis.__lordTest.props].some(([id, p]) => p.src === src && attached(id))), [true, true, true, true]);
const order = sdk.firstStartState().order;
check('step 1: nothing chosen', sdk.firstStartState().pick, null);
await click(shown('shell.firstStart.seeLive')[0]);
check('See it live does nothing before a choice', [sdk.firstStartState().step, switched.length], [1, 0]);
await key('Enter');
check('neither does Enter', sdk.firstStartState().step, 1);

// a card that isn't the current theme
const pick = order.find((id) => id !== theme);
await click(shown(NAMES[pick])[0]);
check('a card is chosen', sdk.firstStartState().pick, pick);
await click(shown('shell.firstStart.seeLive')[0]);
check('See it live switches the theme for real', switched, [pick]);
const saved = JSON.parse(globalThis.__lordTest.saved());
check('the step and the pick are saved before the switch', [saved.theme, saved.firstStart?.step, saved.firstStart?.pick, saved.firstStart?.order], [pick, 2, pick, order]);

// the new theme's JS context: the settings come from the file and startShell starts it again
current = pick;
sdk.settings.set(saved);
check('the new context resumes it', sdk.startFirstStart(ctx), true);
await flush();
check('step 2 after the reload, same order', [sdk.firstStartState().step, sdk.firstStartState().pick, sdk.firstStartState().order], [2, pick, order]);
check('step 2: the floating box with Back and Use this', [shown('shell.firstStart.back').length, shown('shell.firstStart.useThis').length, shown('shell.firstStart.title').length], [1, 1, 0]);
check('step 2: a ✓ on the current one only', shown('✓').length, 1);
const box = () => [...styles].find(([id, s]) => s.width === 330 && s.position === 'absolute' && attached(id));
const area = sdk.workAreaOf(D);
const b0 = { ...box()?.[1] };
check('step 2: it opens inside the work area, on the end side', b0.left != null && b0.left + 330 <= area.x + area.w && b0.left > area.x + area.w / 2 && b0.top >= area.y, true);
// dragged by its grip far off the display: it stays inside the work area
const grip = [...styles].find(([id, s]) => s.cursor === 'grab' && s.height === 16 && attached(id))?.[0];
check('step 2: a grip', !!grip, true);
globalThis.__lordEvent('down', grip, 1000, 500, 0);
globalThis.__lordEvent('move', grip, 400, 450, 0);
await flush();
const b1 = { ...box()[1] };
check('the drag moves it', [b1.left, b1.top], [b0.left - 600, b0.top - 50]);
globalThis.__lordEvent('move', grip, -3000, -3000, 0);
globalThis.__lordEvent('up', grip, -3000, -3000, 0);
await flush();
const b2 = { ...box()[1] };
check('kept inside the work area', b2.left >= area.x && b2.top >= area.y, true);
check('where it was left is saved', sdk.settings.get().firstStart.pos, { x: b2.left, y: b2.top });
globalThis.__lordEvent('move', grip, 500, 500, 0);
await flush();
check('after the release, moving the pointer moves nothing', [box()[1].left, box()[1].top], [b2.left, b2.top]);

// Escape is Back: the dimmed chooser again, the pick kept, no switch back
await key('Escape');
check('Escape: back to step 1, the pick kept', [sdk.firstStartState().step, sdk.firstStartState().pick, shown('shell.firstStart.title').length], [1, pick, 1]);
await click(shown('shell.firstStart.seeLive')[0]);
check('forward again: no second switch (it is already the current one)', [sdk.firstStartState().step, switched.length], [2, 1]);
await click(shown('shell.firstStart.useThis')[0]);
check('step 3: the title and the path', [shown('shell.firstStart.laterTitle').length, shown('settings.style.title').length >= 2, shown('apps.settings').length >= 2], [1, true, true]);
check('step 3: the replica has the three desktop styles', ['Flow', 'Classic', 'Workspaces'].map((v) => shown(v).length), [1, 1, 1]);
check('step 3: the Desktop style group is ringed', [...styles].some(([id, s]) => s.borderWidth === 3 && (s.borderColor >>> 0) === 0xfff5b43c && attached(id)), true);
await click(shown('shell.firstStart.back')[0]);
check('Back on step 3: step 2', sdk.firstStartState().step, 2);
await key('Enter');
check('Enter: Use this', sdk.firstStartState().step, 3);
await click(shown('shell.firstStart.start')[0]);
check('Start: done, the state cleared', [sdk.settings.get().desktopChosen, sdk.settings.get().firstStart, sdk.firstStartState(), done], [true, null, null, 1]);
check('Start: the theme stays the chosen one', [sdk.settings.get().theme, switched.length], [pick, 1]);
check('nothing of it is drawn', ['shell.firstStart.laterTitle', 'shell.firstStart.start', 'shell.firstStart.title'].map((v) => shown(v).length), [0, 0, 0]);
await key('Escape');
await key('Enter');
check('its keys are gone', sdk.firstStartState(), null);
check('no timer left behind', live.size <= timersBefore, true);
check('a later session: not again', sdk.startFirstStart(ctx), false);
process.exit(0);
