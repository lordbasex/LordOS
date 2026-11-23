// classic_startmenu_categories_scene.mjs — a scene of tests/classic_startmenu_power_test.mjs (not run by itself;
// `node tests/classic_startmenu_power_test.mjs classic_startmenu_categories_scene.mjs`): Lord Classic's start menu by
// category (#210). The real StartMenu, SDK and runtime over the stub `lord` of that test, with Linux and Flatpak apps
// (lord.apps.linux / lord.apps.flatpak, their .desktop Categories). It opens All apps (A–Z by default), switches to
// Categories (remembered in settings.startMenuView), checks the groups and their counts, opens and closes one with
// the mouse and the keyboard (→, ←, Enter launches), checks that search stays global, mirrors the arrows right to
// left, and that the menu opens again on the view chosen last. Prints "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import { refreshApps, settings, locale } from '@lordos/sdk';
import { StartMenu } from '../../themes/classic/components/StartMenu.jsx';
import { makeTheme } from '../../themes/classic/tokens.js';

const { texts, parents, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, got, want) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  console.log(g === w ? `ok ${name}` : `FAIL ${name} got ${g}, want ${w}`);
};
const click = async (id) => {
  globalThis.__lordEvent('down', id, 10, 10, 0); await flush();
  globalThis.__lordEvent('up', id, 10, 10, 0); await flush();
};
const key = async (name) => { globalThis.__lordKey(0, 1, name, '', name.toLowerCase(), 0); globalThis.__lordKey(0, 0, name, '', '', 0); await flush(); };
// a node still in the tree: neither it nor any ancestor was removed
const live = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
// the texts on screen, in the order they were made
const shown = () => [...texts].filter(([id]) => live(id)).map(([, v]) => v);
const has = (s) => shown().includes(s);
// the <Text> that shows s now (the newest live one)
const textNode = (s) => { let r = 0; for (const [id, v] of texts) if (v === s && live(id)) r = parents.get(id) || 0; return r; };

// the installed apps: Linux ones with their .desktop Categories, and a Flatpak one
const launched = [];
lord.apps = {
  linux: () => [
    { id: 'chess.desktop', name: 'Chess', icon: '', categories: 'GNOME;GTK;Game;BoardGame;' },
    { id: 'tetris.desktop', name: 'Blocks', icon: '', categories: 'Game;BlocksGame;' },
    { id: 'browser.desktop', name: 'Web Browser', icon: '', categories: 'WebBrowser;' },
    { id: 'odd.desktop', name: 'Oddity', icon: '', categories: 'X-Vendor-Thing;' },
  ],
  flatpak: () => [{ id: 'org.example.Paint', name: 'Paint', icon: '', categories: 'Graphics;RasterGraphics;' }],
  launch: (id) => { launched.push(id); return true; },
  launchFlatpak: (id) => { launched.push(`flatpak:${id}`); return true; },
  lastError: () => '',
};
lord.settings.save = () => true;
refreshApps();

enableShellLayers();
const t = makeTheme({ mode: 'light' });
let open = true, rerender = null;
function Scene() {
  const [, set] = React.useState(0);
  rerender = () => set((v) => v + 1);
  return open ? h(StartMenu, { t, box: { left: 100, top: 100, width: 640, height: 700 },
    onClose: () => { open = false; rerender(); }, isOnBar: () => false }) : null;
}
rt.render(h(Scene));
await flush();

// ---- All apps opens on A–Z ----
await click(textNode('shell.classic.allApps'));
check('All apps opens', has('shell.classic.back'), true);
check('the view switch shows A–Z and Categories', [has('shell.classic.viewAz'), has('shell.classic.viewCategories')], [true, true]);
check('A–Z by default: every app by name, no groups', [has('Chess'), has('Paint'), has('shell.category.Game')], [true, true, false]);

// ---- Categories ----
await click(textNode('shell.classic.viewCategories'));
check('the choice is remembered', settings.get().startMenuView, 'categories');
const groups = shown().filter((s) => s.startsWith('shell.category.'));
check('only the groups that hold apps, Other last', groups,
  ['shell.category.Game', 'shell.category.Graphics', 'shell.category.Network', 'shell.category.Other']);
check('the counts', shown().filter((s) => /^\d+$/.test(s)), ['2', '1', '1', '1']);
check('the groups start closed', has('Chess'), false);

// a click opens a group: its apps, by name, under it
await click(textNode('shell.category.Game'));
const after = shown();
check('a click opens Games', [after.includes('Blocks'), after.includes('Chess'), after.includes('Paint')], [true, true, false]);
await click(textNode('shell.category.Game'));
check('a second click closes it', has('Chess'), false);

// ---- the keyboard (the search field keeps the focus) ----
await key('Home');                          // the first row: Games (the click left the keyboard on it)
await key('ArrowRight');                    // opens it
check('→ opens the selected group', has('Blocks'), true);
await key('ArrowRight');                    // to its first app
await key('ArrowLeft');                     // back to the group
await key('ArrowLeft');                     // closes it
check('← closes it', has('Blocks'), false);
await key('ArrowDown');                     // Graphics
await key('Enter');                         // opens it
check('Enter opens a group', has('Paint'), true);
await key('ArrowDown');                     // Paint
await key('Enter');                         // launches it
check('Enter on an app launches it (a Flatpak app)', launched, ['flatpak:org.example.Paint']);
check('and closes the menu', open, false);

// ---- the menu opens again on the view chosen last ----
open = true; rerender(); await flush();
await click(textNode('shell.classic.allApps'));
check('the last view comes back', [has('shell.category.Game'), has('shell.classic.viewCategories')], [true, true]);
check('groups start closed again', has('Paint'), false);

// ---- search stays global: typing on Categories finds an app of a closed group ----
for (const ch of 'blo') { globalThis.__lordKey(0, 1, ch, ch, ch, 0); globalThis.__lordKey(0, 0, ch, '', '', 0); }
await flush();
check('typing searches every app (Blocks, in the closed Games)', [has('Blocks'), has('shell.category.Game')], [true, false]);
for (let i = 0; i < 3; i++) await key('Backspace');
check('clearing the search shows the categories again', [has('shell.category.Game'), has('Blocks')], [true, false]);

// ---- right to left: ← opens and → closes ----
locale.set({ ...locale.get(), dir: 'rtl' });
await flush();
await key('Home');
await key('ArrowLeft');
check('right to left: ← opens the group', has('Blocks'), true);
await key('ArrowRight');
check('right to left: → closes it', has('Blocks'), false);

// ---- back to A–Z: remembered too ----
await click(textNode('shell.classic.viewAz'));
check('A–Z is remembered', settings.get().startMenuView, 'az');
check('A–Z lists every app again', [has('Blocks'), has('Paint'), has('shell.category.Game')], [true, true, false]);
process.exit(0);
