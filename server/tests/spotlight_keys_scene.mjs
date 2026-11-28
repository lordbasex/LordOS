// spotlight_keys_scene.mjs — the menu bar's search (sdk/spotlight.js) over a whole theme, run by tests/workspaces_test.mjs:
// node tests/workspaces_test.mjs spotlight_keys_scene.mjs <default|classic|workspaces|celeste>. Every key typed in its
// field lands in it, as lord-server sends them (code, key, text): "documento-qa" and "c t ca to" (ws170 QA BUG-28: c,
// t and Space were lost in Flow), and with nothing typed it lists the recent files. Screenshots' selection that kept c, t
// and Space after its overlays were closed is tests/screenshots_overlay_close_scene.mjs. Prints "ok <name>" or "FAIL <name> <detail>".
const React = (await import('react')).default;
const rt = await import('@lordos/runtime');
const sdk = await import('@lordos/sdk');
const { SpotlightPanel } = await import('../../sdk/spotlight.js');
const { Layer } = await import('../../sdk/layers.js');
const theme = globalThis.__themeUnderTest;
if (theme === 'default') await import('../../themes/default/index.jsx');
else if (theme === 'classic') await import('../../themes/classic/index.jsx');
else if (theme === 'celeste') await import('../../themes/celeste/index.jsx');
else await import('../../themes/workspaces/index.jsx');
const { texts, parents, removed } = globalThis.__lordTest;
const flush = async () => { for (let i = 0; i < 30; i++) await new Promise((res) => setImmediate(res)); await new Promise((res) => setTimeout(res, 5)); for (let i = 0; i < 10; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, got, want) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  console.log(g === w ? `ok ${name}` : `FAIL ${name} got ${g}, want ${w}`);
};
const attached = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return parents.has(id); };
const CODES = { a: 30, b: 48, c: 46, d: 32, e: 18, f: 33, g: 34, h: 35, i: 23, j: 36, k: 37, l: 38, m: 50, n: 49, o: 24, p: 25,
  q: 16, r: 19, s: 31, t: 20, u: 22, v: 47, w: 17, x: 45, y: 21, z: 44, ' ': 57, '-': 12 };
const type = async (s) => {
  for (const ch of s) {
    globalThis.__lordKey(CODES[ch], 1, ch === ' ' ? ' ' : ch, ch, ch, 0);
    globalThis.__lordKey(CODES[ch], 0, ch === ' ' ? ' ' : ch, '', ch, 0);
    await flush();
  }
};
const shownText = (v) => [...texts].some(([id, x]) => x === v && attached(id));

// a home with two files changed lately (the empty search lists them as recent)
const NOW = Date.now();
lord.fs = { home: () => '/home/qa', search: () => [], listAsync: async () => [], list: () => [], read: () => '',
  recent: () => [{ name: 'documento-qa.odt', path: '/home/qa/Descargas/documento-qa.odt', mtime: NOW - 60000 },
    { name: 'informe.txt', path: '/home/qa/informe.txt', mtime: NOW - 3600000 }] };
const D = { id: 1, name: 'LordOS', x: 0, y: 0, w: 1920, h: 1080, scale: 100, main: true };
lord.screen.displays = [D]; lord.screen.width = 1920; lord.screen.height = 1080; globalThis.__lordScreen();
sdk.settings.set({ welcomeShown: true });
// the shim keeps the theme's element only: the search panel goes in its layer as startShell's LocaleRoot puts it
rt.render(React.createElement(() => React.createElement(React.Fragment, null, globalThis.__shellElement,
  React.createElement(Layer, { name: 'overlay' }, React.createElement(SpotlightPanel)))));
await flush();

// nothing typed: the recent files (ws170 QA item 180), newest first
sdk.spotlight.open();
await flush();
check(`${theme}: empty search lists the recent files`, shownText('documento-qa.odt') && shownText('informe.txt'), true);
sdk.spotlight.close();
await flush();

for (const words of ['documento-qa', 'c t ca to']) {
  sdk.spotlight.open();
  await flush();
  await type(words);
  check(`${theme}: "${words}" typed in the search`, shownText(words), true);
  sdk.spotlight.close();
  await flush();
}
process.exit(0);
