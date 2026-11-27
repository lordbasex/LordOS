// hands_names_scene.mjs — the shell's glyph-only buttons have accessible names (ws170 QA BUG-11: Hands' find "Trash" didn't
// find the Dock's Trash), run by tests/workspaces_test.mjs: node tests/workspaces_test.mjs hands_names_scene.mjs
// <default|workspaces>. Over the theme's real host tree (runtime hostRoots, first/next, as sdk/handsmodel.js walks it):
// Flow's Dock (every app icon and the Trash) and its menu bar's glyph items, Workspaces' column (the spine) and its bar
// buttons each carry role "button" and an accessibilityLabel. The stub has no catalogs: a label is its locale key.
// Prints "ok <name>" or "FAIL <name> <detail>".
const React = (await import('react')).default;
const rt = await import('@lordos/runtime');
const sdk = await import('@lordos/sdk');
const theme = globalThis.__themeUnderTest;
if (theme === 'default') await import('../../themes/default/index.jsx');
else await import('../../themes/workspaces/index.jsx');
const flush = async () => { for (let i = 0; i < 30; i++) await new Promise((res) => setImmediate(res)); await new Promise((res) => setTimeout(res, 5)); for (let i = 0; i < 10; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const D = { id: 1, name: 'LordOS', x: 0, y: 0, w: 1920, h: 1080, scale: 100, main: true };
lord.screen.displays = [D]; lord.screen.width = 1920; lord.screen.height = 1080; globalThis.__lordScreen();
sdk.settings.set({ welcomeShown: true });
rt.render(React.createElement(() => globalThis.__shellElement));
await flush();

const all = [];
const walk = (n, d) => { if (!n || d > 200) return; if (n.props) all.push(n); for (let c = n.first; c; c = c.next) walk(c, d + 1); };
for (const r of rt.hostRoots()) walk(r.node, 0);
const labelled = (pred) => all.filter((n) => pred(n.props));
const named = (n) => typeof n.props.accessibilityLabel === 'string' && n.props.accessibilityLabel.length > 0 && n.props.accessibilityRole === 'button';
if (theme === 'default') {
  const trash = labelled((p) => p.dropTrash);
  check('Flow: the Dock\'s Trash is a button named Trash', trash.length === 1 && named(trash[0]) && trash[0].props.accessibilityLabel === 'shell.trash.name',
    JSON.stringify(trash.map((n) => n.props.accessibilityLabel)));
  const names = labelled((p) => p.accessibilityLabel === 'shell.shortcuts.search' || p.accessibilityLabel === 'shell.shortcuts.quickSettings' || p.accessibilityLabel === 'LordOS');
  check('Flow: the menu bar\'s logo, search and quick settings are named', names.length >= 3 && names.every(named), String(names.length));
  const dockIcons = all.filter((n) => typeof n.props.onContextMenu === 'function' && typeof n.props.onMouseDown === 'function' && n.props.style?.position === 'absolute' && !n.props.dropTrash);
  check('Flow: every Dock icon is a named button', dockIcons.length > 0 && dockIcons.every(named), JSON.stringify(dockIcons.map((n) => n.props.accessibilityLabel)));
} else {
  const spine = labelled((p) => typeof p.accessibilityLabel === 'string' && p.accessibilityLabel.startsWith('shell.ws.spine.'));
  check('Workspaces: the column\'s buttons are named', spine.length >= 8 && spine.every(named), String(spine.length));
}
process.exit(0);
