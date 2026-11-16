// theme_cost_scene.mjs — what a change costs in a theme, in native calls (createNode, setStyle, setText, appendChild,
// insertBefore, removeChild) and in timers, for Lord Flow, Lord Classic and Lord Workspaces alike: node
// tests/workspaces_test.mjs theme_cost_scene.mjs <default|classic|workspaces>. One 1920×1080 display, three windows.
// Prints "ok cost <name> <value>" lines (numbers, not pass/fail: docs/WORKSPACES.md compares them), and fails only if
// the theme doesn't start. Not in make test: a measurement.
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
const { counts } = globalThis.__lordTest;
const flush = async () => { for (let i = 0; i < 30; i++) await new Promise((res) => setImmediate(res)); await new Promise((res) => setTimeout(res, 5)); for (let i = 0; i < 10; i++) await new Promise((res) => setImmediate(res)); };
const out = (name, v) => console.log(`ok cost ${name} ${v}`);
const D = { id: 1, name: 'LordOS', x: 0, y: 0, w: 1920, h: 1080, scale: 100, main: true };
lord.screen.displays = [D]; lord.screen.width = 1920; lord.screen.height = 1080; globalThis.__lordScreen();
sdk.settings.set({ welcomeShown: true });
const t0 = counts.ops;
rt.render(React.createElement(() => globalThis.__shellElement));
await flush();
out('first-render-ops', counts.ops - t0);
const ids = ['files', 'terminal', 'calculator'].map((a, i) => sdk.wm.open(a, { frame: { x: 200 + 60 * i, y: 150 + 40 * i, w: 700, h: 480 } }));
await flush();
const cost = async (name, fn) => { const a = counts.ops; await fn(); await flush(); out(name, counts.ops - a); };
await cost('title-change-ops', async () => { for (let i = 0; i < 10; i++) { sdk.wm.setTitle(ids[0], `t${i}`); await flush(); } });
await cost('focus-change-ops', async () => { for (let i = 0; i < 10; i++) { sdk.wm.focus(ids[i % 3]); await flush(); } });
await cost('open-close-ops', async () => { const id = sdk.wm.open('store'); await flush(); sdk.wm.close(id); });
const d2 = sdk.addDesktopAndShow(); await flush();
await cost('switch-ops', async () => { for (let i = 0; i < 4; i++) { sdk.switchDesktop(i % 2 ? d2 : sdk.desktopList.get()[0]); await flush(); } });
const per = {};
for (const ms of live.values()) per[ms] = (per[ms] || 0) + 1;
out('idle-intervals', JSON.stringify(per));
out('idle-wakeups-per-minute', Object.entries(per).reduce((n, [ms, k]) => n + k * Math.round(60000 / +ms), 0));
process.exit(0);
