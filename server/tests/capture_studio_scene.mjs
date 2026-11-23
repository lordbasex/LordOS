// capture_studio_scene.mjs — a scene of tests/textfield_menu_test.mjs (not run by itself): LordOS Studio's ways in
// (docs/CAMERA-STUDIO-PLAN.md 9.1, 9.2). The capture item by the clock (sdk/capturestatus.jsx) while a Linux app uses
// only the microphone shows a Mic Mode entry and no "Video effects"; opened, Mic Mode alone, with no camera preview
// asked for, says which apps it reaches, and choosing Noise Reduction makes it the default microphone; while an app
// uses the camera "Video effects" shows as before and opens the popover with its preview. The LordOS Studio app
// (apps/studio) is in the launcher's list only where LordOS Studio is installed, and its window renders the effects,
// Mic Mode and the way to the Store's LordOS Studio Camera. Prints "ok <name>" or "FAIL <name> <detail>".
import './capture_studio_scene_stubs.js';
import React from 'react';
import * as rt from '@lordos/runtime';
import { PrivacyIndicators } from '../../sdk/capturestatus.jsx';
import { enableShellLayers } from '../../sdk/layers.js';
import { linuxCapture, brokerDevices } from '../../sdk/privacy.js';
import { configureWindows, listApps } from '../../sdk/windows.js';
import * as StudioApp from '../../apps/studio/index.jsx';
import studioManifest from '../../apps/studio/manifest.json';
import studioEn from '../../assets/locales/en/studio.json';
import appsEn from '../../assets/locales/en/apps.json';

const { texts, parents, removed, styles } = globalThis.__lordTest;
const h = React.createElement;
const flush = async () => { for (let i = 0; i < 20; i++) await new Promise((res) => setImmediate(res)); };
const check = (name, ok, detail = '') => console.log(ok ? `ok ${name}` : `FAIL ${name} ${detail}`);
const calls = globalThis.__studioCalls;

const shellCatalog = lord.locale.catalog;
const CAT = { studio: studioEn, apps: appsEn };
lord.locale.catalog = (lang, ns) => (lang === 'en' && CAT[ns] ? JSON.stringify(CAT[ns]) : shellCatalog(lang, ns));

const inTree = (id) => { for (let n = id; n; n = parents.get(n)) if (removed.has(n)) return false; return true; };
const textId = (s) => [...texts].find(([id, x]) => String(x) === s && inTree(id))?.[0];
const shows = (s) => !!textId(s);
const click = async (id) => { const t = parents.get(id) || id; __lordEvent('down', t, 5, 5, 0); __lordEvent('up', t, 5, 5, 0); await flush(); };
const shown = () => [...texts].filter(([id]) => inTree(id)).map(([, x]) => x).join(' | ');
// the item's pill: 30 × 18
const pill = () => [...styles].find(([id, s]) => s.width === 30 && s.height === 18 && inTree(id))?.[0];

const app = { ...studioManifest, system: true, Component: StudioApp.default, Icon: StudioApp.Icon, available: StudioApp.available };
configureWindows({ apps: [app] });
check('the app is listed where LordOS Studio is installed', listApps().some((a) => a.id === 'studio'));

enableShellLayers();
rt.render(h(rt.View, null, h(PrivacyIndicators)));
await flush();
check('no item while nothing is captured', !pill());

// a Linux app, the microphone only (the maintainer's case: Google Chrome — Microphone)
linuxCapture.set([{ app: 'desktop:google-chrome', name: 'Google Chrome', sys: '', exe: '/opt/google/chrome/chrome', mic: true, camera: false, screen: false }]);
await flush();
check('the item shows for the microphone', !!pill());
await click(pill());
check('the panel names the app', shows('google-chrome'), shown());
check('microphone only: a Mic Mode entry', shows(studioEn.mic), shown());
check('its detail is the current mode', shows(studioEn.micStandard), shown());
check('microphone only: no Video effects', !shows(studioEn.open), shown());
await click(textId(studioEn.mic));
check('Mic Mode opens in the same popover', shows(studioEn.micVoice) && shows(studioEn.micWide), shown());
check('it says which apps it reaches', shows(studioEn.micApps), shown());
check('no camera preview for Mic Mode', calls.preview === 0, `preview asked ${calls.preview} times`);
check('no camera rows in it', !shows(studioEn.portrait), shown());
check('a link to the LordOS Studio app', shows(studioEn.openApp), shown());
await click(textId(studioEn.micVoice));
check('Noise Reduction becomes the default microphone', calls.setDefault.some(([k, n]) => k === 'source' && n === 'lordos_voice_isolation'),
  JSON.stringify(calls.setDefault));
await click(textId(studioEn.back) || parents.get(textId('‹')));
check('Back returns to the panel', shows('google-chrome') && shows(studioEn.mic), shown());

// the camera too (a native app): Video effects as before, and Mic Mode still for the microphone
brokerDevices.set([{ app: 'camera', kind: 'camera' }]);
await flush();
check('camera in use: Video effects', shows(studioEn.open), shown());
check('camera and microphone: Mic Mode too', shows(studioEn.mic), shown());
linuxCapture.set([]);
await flush();
check('camera only: Video effects', shows(studioEn.open), shown());
check('camera only: no Mic Mode entry', !shows(studioEn.mic), shown());
await click(textId(studioEn.open));
check('Video effects opens the popover', shows(studioEn.portrait) && shows(studioEn.background), shown());
check('with its preview', calls.preview === 1, `preview asked ${calls.preview} times`);
check('and a link to the app', shows(studioEn.openApp), shown());
brokerDevices.set([]);
await flush();
check('the item goes when nothing is captured', !pill());
rt.render(h(rt.View, null));
await flush();

// the app's window
rt.render(h(rt.View, { style: { width: 860, height: 560 } }, h(StudioApp.default)));
await flush();
check('the app opens: its effects', shows(studioEn.effects) && shows(studioEn.portrait) && shows(studioEn.faceLight) && shows(studioEn.filters), shown());
check('the app: Mic Mode', shows(studioEn.mic) && shows(studioEn.micVoice), shown());
check('the app: the camera it uses', shows('Test camera'), shown());
check('the app: the Store link for LordOS Studio Camera', shows(studioEn.getCamera), shown());
check('the app: its preview, opened again after the popover closed it', calls.preview === 2, `preview asked ${calls.preview} times`);
await click(textId(studioEn.portrait));
check('the app: a row opens its page in place', shows(studioEn.blurAmount) && shows(studioEn.portraitHint), shown());
rt.render(h(rt.View, null));
await flush();

// not installed: kept out of the launcher
lord.studio.status = () => ({ available: false });
configureWindows({ apps: [app] });
check('the app is not listed without LordOS Studio', !listApps().some((a) => a.id === 'studio'));
process.exit(0);
