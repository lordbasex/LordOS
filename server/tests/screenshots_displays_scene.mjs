// screenshots_displays_scene.mjs — a scene of tests/textfield_menu_test.mjs (GitHub #252; not run by itself): the
// Screenshots app (apps/screenshots/index.jsx) with several displays, over the stub lord and the whole SDK. Print Screen
// (capture.full) takes the main display first, so its picture is the one on the clipboard, and each display's capture
// gets its own floating thumbnail inside that display's work area; with Save to: Markup editor the editor opens inside
// the main display, and a click on the other display's thumbnail opens the editor inside that display. Run on the
// reporter's layout (two displays side by side, the main one listed second) and on one where the other display is
// above and to the left of the main one at 200 % (the main one is then away from the space's origin).
// Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import { AppWindowContext } from '../../sdk/components.jsx';
import { windows, configureWindows } from '../../sdk/windows.js';
import { settings } from '../../sdk/settings.js';
import Screenshots, { capture } from '../../apps/screenshots/index.jsx';
import shotsEn from '../../assets/locales/en/screenshots.json';

const { texts, parents, removed } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const sleep = (ms) => new Promise((res) => setTimeout(res, ms));
const check = (name, got, want) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  console.log(g === w ? `ok ${name}` : `FAIL ${name} got ${g}, want ${w}`);
};

const shellCatalog = lord.locale.catalog;
lord.locale.catalog = (lang, ns) => (lang === 'en' && ns === 'screenshots' ? JSON.stringify(shotsEn) : shellCatalog(lang, ns));
lord.perms = { get: () => 'allow', names: () => [] };
// a home where no file exists yet (fs.stat throws): the captures' names are free
lord.fs = new Proxy({ home: () => '/home/test', stat: () => { throw new Error('ENOENT'); } }, { get: (o, k) => (k in o ? o[k] : () => undefined) });
// lord.media as lord-server answers it: a capture handle per display asked for, jobs that end at once
const grabbed = [], copied = [], byHandle = new Map();
let nextHandle = 100, nextJob = 1;
const done = (r) => { const id = nextJob++; setImmediate(() => globalThis.__lordMedia(0, 'job', { id, ...r })); return id; };
lord.media = new Proxy({
  screenshot: (app, rect) => { const hd = nextHandle++; grabbed.push(rect.display ?? null); byHandle.set(hd, rect.display ?? null); return hd; },
  copyImage: (hd) => { copied.push(byHandle.get(hd)); return done({ ok: true }); },
  saveImage: (hd, p) => done({ ok: true, path: p }),
  info: () => ({ width: 1920, height: 1080 }),
}, { get: (o, k) => (k in o ? o[k] : () => undefined) });

const plug = (list) => {
  lord.screen.displays = list;
  lord.screen.width = Math.max(...list.map((d) => d.x + d.w));
  lord.screen.height = Math.max(...list.map((d) => d.y + d.h));
  lord.screen.mirror = false;
  globalThis.__lordScreen();
};
// the theme's work areas: a 28 px menu bar and a 72 px Dock on the main display, all of the others
const areaOf = (d) => (d.main ? { x: d.x, y: d.y + 28, w: d.w, h: d.h - 100 } : { x: d.x, y: d.y, w: d.w, h: d.h });
configureWindows({ apps: [{ id: 'screenshots', name: 'Screenshots', size: [440, 420] }], workArea: areaOf });
const inside = (r, d) => { const a = areaOf(d); return !!r && r.x >= a.x && r.y >= a.y && r.x + r.w <= a.x + a.w && r.y + r.h <= a.y + a.h; };
const win = (id) => windows.get().find((w) => w.id === id) || null;
const frameOf = (w) => (w ? w.pip || { x: w.x, y: w.y, w: w.w, h: w.h } : null);
const reset = () => { windows.set([]); grabbed.length = 0; copied.length = 0; };
const printScreen = async () => { capture.full(); await sleep(400); await flush(); };

// one root for the whole scene: the thumbnail window shown in it changes per layout (null: none)
let showWin = null;
function Host() {
  const [w, set] = React.useState(null);
  showWin = set;
  return w ? h(AppWindowContext.Provider, { key: w.id + w.args.handle, value: { id: w.id, focused: false, args: w.args } }, h(Screenshots)) : null;
}
enableShellLayers();
rt.render(h(Host));
await flush();

async function layout(name, list) {
  plug(list);
  const main = list.find((d) => d.main), other = list.find((d) => !d.main);
  settings.set({ screenshots: { saveTo: 'pictures', thumbnail: true } });
  reset();
  await printScreen();
  check(`${name}: Print Screen takes every display, the main one first`, grabbed, [main.id, other.id]);
  check(`${name}: the main display's capture is the one copied`, copied, [main.id]);
  const tm = win(`screenshots#thumb-${main.id}`), to = win(`screenshots#thumb-${other.id}`);
  check(`${name}: a thumbnail per display`, [!!tm, !!to], [true, true]);
  check(`${name}: the main display's thumbnail inside its work area`, inside(frameOf(tm), main), true);
  check(`${name}: the other display's thumbnail inside that display`, inside(frameOf(to), other), true);

  // Save to: Markup editor — the editor on the main display's capture, inside the main display
  settings.set({ screenshots: { saveTo: 'editor', thumbnail: true } });
  reset();
  await printScreen();
  const eds = windows.get().filter((w) => String(w.id).startsWith('screenshots#edit'));
  check(`${name}: Save to editor opens one editor`, eds.length, 1);
  check(`${name}: … inside the main display, not across both`, inside(frameOf(eds[0]), main), true);

  // a click on the other display's thumbnail: the editor opens on that display
  settings.set({ screenshots: { saveTo: 'pictures', thumbnail: true } });
  reset();
  await printScreen();
  const t = win(`screenshots#thumb-${other.id}`);
  showWin(t);
  await flush();
  const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
  const label = [...texts].find(([id, s]) => /Screenshots/.test(String(s)) && inTree(id))?.[0];
  const opened = () => windows.get().find((w) => String(w.id).startsWith('screenshots#edit'));
  for (let n = label, i = 0; n && i < 4 && !opened(); n = parents.get(n), i++) {
    globalThis.__lordEvent('down', n, 2, 2, 0); globalThis.__lordEvent('up', n, 2, 2, 0); await flush();
  }
  check(`${name}: a click on the other display's thumbnail opens the editor`, !!opened(), true);
  check(`${name}: … inside that display`, inside(frameOf(opened()), other), true);
  showWin(null);
  await flush();
}

// GitHub #252: two external displays side by side, the laptop's lid closed; lord-server lists the main one second
await layout('#252 side by side', [
  { id: 71, name: 'DP-2', x: 1920, y: 0, w: 1920, h: 1080, pw: 1920, ph: 1080, scale: 100, main: false },
  { id: 64, name: 'DP-1', x: 0, y: 0, w: 1920, h: 1080, pw: 1920, ph: 1080, scale: 100, main: true },
]);
// the other display above and to the left of the main one, a 4K panel at 200 % (logical 1920 × 1080): the space starts
// at the other display, so the main one is at (1920, 1080)
await layout('other display above-left at 200 %', [
  { id: 5, name: 'eDP-1', x: 0, y: 0, w: 1920, h: 1080, pw: 3840, ph: 2160, scale: 200, main: false },
  { id: 6, name: 'HDMI-A-1', x: 1920, y: 1080, w: 2560, h: 1440, pw: 2560, ph: 1440, scale: 100, main: true },
]);
// mirrored displays: one capture, of Settings' choice, and one thumbnail
plug([
  { id: 1, x: 0, y: 0, w: 1920, h: 1080, scale: 100, main: true },
  { id: 2, x: 0, y: 0, w: 1920, h: 1080, scale: 100, main: false },
]);
lord.screen.mirror = true; globalThis.__lordScreen();
settings.set({ screenshots: { saveTo: 'pictures', thumbnail: true } });
reset();
await printScreen();
check('mirrored: one capture, of Settings\' choice', grabbed, [null]);
check('mirrored: one thumbnail, inside the main display', inside(frameOf(win('screenshots#thumb')), { x: 0, y: 0, w: 1920, h: 1080, main: true }), true);
process.exit(0);
