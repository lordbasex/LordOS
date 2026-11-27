// shortcuts_list_scene.mjs — Settings → Keyboard's list of shortcuts (sdk/shortcuts.js shortcutList) over a whole theme,
// run by tests/workspaces_test.mjs: node tests/workspaces_test.mjs shortcuts_list_scene.mjs <default|classic>. A modifier
// shown by itself (❖ pressed alone for Flow's Apps, ⌥ alone for the menu bar) is labelled "<key> alone"
// (shell.shortcuts.alone), never a bare "❖" next to the other key, which read as ❖+F4 (ws170 QA BUG-21).
// Prints "ok <name>" or "FAIL <name> <detail>".
const React = (await import('react')).default;
const rt = await import('@lordos/runtime');
const sdk = await import('@lordos/sdk');
const theme = globalThis.__themeUnderTest;
if (theme === 'default') await import('../../themes/default/index.jsx');
else if (theme === 'classic') await import('../../themes/classic/index.jsx');
else await import('../../themes/workspaces/index.jsx');
const flush = async () => { for (let i = 0; i < 30; i++) await new Promise((res) => setImmediate(res)); await new Promise((res) => setTimeout(res, 5)); };
const check = (name, got, want) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  console.log(g === w ? `ok ${name}` : `FAIL ${name} got ${g}, want ${w}`);
};
const D = { id: 1, name: 'LordOS', x: 0, y: 0, w: 1920, h: 1080, scale: 100, main: true };
lord.screen.displays = [D]; lord.screen.width = 1920; lord.screen.height = 1080; globalThis.__lordScreen();
sdk.settings.set({ welcomeShown: true });
rt.render(React.createElement(() => globalThis.__shellElement));
await flush();
const rows = sdk.shortcutList().flatMap((g) => g.items);
const lone = new Set(['❖', '⌥', '⌃', '⇧']);
check(`${theme}: no row shows a bare modifier`, rows.filter((r) => r.combos.some((c) => lone.has(c))).map((r) => r.id), []);
const alone = sdk.t('shell.shortcuts.alone', { key: '❖' });
if (theme === 'default') check('default: Apps is "❖ alone" and F4', rows.find((r) => r.id === 'launcher')?.combos, [alone, 'F4']);
if (theme === 'classic') check('classic: the Start menu is "❖ alone"', rows.find((r) => r.id === 'start')?.combos, [alone]);
process.exit(0);
