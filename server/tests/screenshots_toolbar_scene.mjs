// screenshots_toolbar_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): the Screenshots toolbar
// (apps/screenshots/toolbar.jsx) in its bare floating window. One mode is lit and a click on another chooses it (and
// Capture becomes Record for a recording mode); Options opens a menu with Save to, Timer, Microphone and Options sections,
// a choice there is saved in settings.screenshots and checked, Recents is a page of its own; Color, Text, Close and the
// primary button run their actions. Prints one line per check: "ok <name>" or "FAIL <name> <detail>".
import React from 'react';
import * as rt from '@lordos/runtime';
import { enableShellLayers } from '../../sdk/layers.js';
import { AppWindowContext } from '../../sdk/components.jsx';
import { windows } from '../../sdk/windows.js';
import { settings } from '../../sdk/settings.js';
import { Toolbar, menuRows } from '../../apps/screenshots/toolbar.jsx';
import { bar, recent } from '../../apps/screenshots/state.js';
import shotsEn from '../../assets/locales/en/screenshots.json';

const { texts, parents, removed, styles } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);

const shellCatalog = lord.locale.catalog;
lord.locale.catalog = (lang, ns) => (lang === 'en' && ns === 'screenshots' ? JSON.stringify(shotsEn) : shellCatalog(lang, ns));

const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const textNode = (s) => [...texts].find(([id, x]) => x === s && inTree(id))?.[0] || null;
const shown = (s) => !!textNode(s);
const click = async (id) => { __lordEvent('down', id, 2, 2, 0); __lordEvent('up', id, 2, 2, 0); await flush(); };
// a press on a text, else on what holds it, until `done` (the row or button with the handler is one or two levels up)
const clickText = async (s, done) => { let n = textNode(s); for (let i = 0; n && i < 4 && !done(); i++, n = parents.get(n)) await click(n); };
const live = () => [...styles].filter(([id]) => inTree(id) && parents.has(id));
const litCount = () => modeButtons().filter((id) => styles.get(id).backgroundColor).length;
// the five mode buttons, start to end (34 × 34; Close, Color and Text are 30 × 30)
const modeButtons = () => live().filter(([, s]) => s.width === 34 && s.height === 34).map(([id]) => id).sort((a, b) => a - b);

const ran = [];
const actions = {
  hasMail: true, close: () => ran.push('close'), primary: () => ran.push('primary'), color: () => ran.push('color'), text: () => ran.push('text'),
  protectFront: () => ran.push('protect'), openRecent: (r) => ran.push(`recent:${r.path}`), chooseFolder: () => ran.push('folder'),
};
windows.set([{ id: 'screenshots', app: 'screenshots', title: null, x: 0, y: 0, w: 640, h: 50, pip: { x: 300, y: 900, w: 640, h: 50, bare: true } }]);
settings.set({ screenshots: { clipboard: true } });     // the old panel's "only copy to the clipboard"
recent.set([{ path: '/home/a/Pictures/Screenshots/Shot 1.png', time: 1 }]);

enableShellLayers();
rt.render(h(AppWindowContext.Provider, { value: { id: 'screenshots', focused: false, args: null } }, h(rt.View, { style: { width: 640, height: 50 } }, h(Toolbar, { actions }))));
await flush();

check('the bar shows Options and Capture', shown('Options') && shown('Capture'));
check('five mode buttons', modeButtons().length === 5, `${modeButtons().length}`);
check('one mode is lit', litCount() === 1, `${litCount()}`);
await click(modeButtons()[3]);
check('a click on Record Entire Screen chooses it', settings.get().screenshots.mode === 'recordFull', settings.get().screenshots.mode);
check('the primary button says Record', shown('Record') && !shown('Capture'));
check('still one mode lit', litCount() === 1, `${litCount()}`);
await click(modeButtons()[0]);
check('back to Entire Screen: Capture', settings.get().screenshots.mode === 'full' && shown('Capture'));

await clickText('Capture', () => ran.includes('primary'));
check('Capture runs the primary action', ran.includes('primary'), ran.join());

await clickText('Options', () => shown('Save to'));
check('Options opens its menu', shown('Save to') && shown('Timer') && shown('Microphone') && shown('Remember Last Selection'));
check('Save to lists the places', ['Desktop', 'Documents', 'Pictures', 'Clipboard', 'Markup Editor', 'Other Location…'].every(shown));
check('the microphones are listed', shown('Built-in Microphone') && shown('USB Headset') && !shown('Monitor'));
const rows = menuRows({ ...settings.get().screenshots, saveTo: 'clipboard', timer: 0, mic: false, thumbnail: true, remember: false, pointer: true, otherDir: '' }, 'main',
  { mics: [], recents: [], front: null, protectedCount: 0, actions });
check('the old "only copy" reads as Clipboard, checked', rows.find((r) => r.key === 'save.clipboard')?.checked === true);
check('sections are apart: a separator before Timer', rows[rows.findIndex((r) => r.header === 'Timer') - 1]?.sep === true);
await clickText('Desktop', () => settings.get().screenshots.saveTo === 'desktop');
check('Desktop is saved', settings.get().screenshots.saveTo === 'desktop' && settings.get().screenshots.clipboard === false, JSON.stringify(settings.get().screenshots));
check('the menu stays open after a choice', shown('Save to'));
await clickText('10 Seconds', () => settings.get().screenshots.timer === 10);
check('the timer is saved', settings.get().screenshots.timer === 10);
await clickText('USB Headset', () => settings.get().screenshots.mic === true);
check('the microphone is on, that one', settings.get().screenshots.mic === true && settings.get().screenshots.micSource === 'alsa_input.usb');
await clickText('Remember Last Selection', () => settings.get().screenshots.remember === true);
check('Remember Last Selection is saved', settings.get().screenshots.remember === true);
await clickText('Other Location…', () => ran.includes('folder'));
check('Other Location asks for a folder', ran.includes('folder'));
check('and the menu closes for the dialog', !shown('Save to'));
await clickText('Options', () => shown('Save to'));
await clickText('Recent', () => shown('Shot 1.png'));
check('Recents is a page of its own', shown('Shot 1.png') && !shown('Save to'));
await clickText('Shot 1.png', () => ran.some((x) => x.startsWith('recent:')));
check('a recent capture opens', ran.includes('recent:/home/a/Pictures/Screenshots/Shot 1.png'));
bar.set({ menu: false, page: 'main', lit: -1 });
await flush();
check('the menu closes', !shown('Save to') && !shown('Shot 1.png'));

const icons30 = live().filter(([, s]) => s.width === 30 && s.height === 30).map(([id]) => id).sort((a, b) => a - b);
check('close, color and text buttons', icons30.length === 3, `${icons30.length}`);
for (const id of icons30) await click(id);
check('they run close, color and text', ['close', 'color', 'text'].every((x) => ran.includes(x)), ran.join());
process.exit(0);
