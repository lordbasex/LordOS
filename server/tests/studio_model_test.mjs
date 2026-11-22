// studio_model_test.mjs — sdk/studiomodel.js (LordOS Studio's popover logic, docs/CAMERA-STUDIO-PLAN.md 9.3).
// Run from server/: node tests/studio_model_test.mjs
import assert from 'node:assert/strict';
import {
  normalizeStudio, STUDIO_DEFAULTS, realCameras, hasStudioCamera, deviceFor, chosenCamera, serverConfig, portraitOn, backgroundOn,
  withPortrait, withBackground, withPicture, withMode, previewSize, usersOf, previewSource, perfLine,
  loopbackState, studioUseOf, studioWhat,
} from '../../sdk/studiomodel.js';
import { captureRows, rowStops, captureGlyph } from '../../sdk/capturemodel.js';

let n = 0;
const t = (name, fn) => { fn(); n++; };

const cams = [
  { device: '/dev/video0', name: 'Logitech Webcam C930e', bus: 'usb-0000:00:14.0-3' },
  { device: '/dev/video2', name: 'Integrated Camera', bus: 'usb-0000:00:14.0-7' },
  { device: 'studio', name: 'LordOS Studio', bus: 'studio', studio: true },
];

t('defaults and validation', () => {
  assert.deepEqual(normalizeStudio(undefined), { ...STUDIO_DEFAULTS });
  assert.deepEqual(normalizeStudio({ mode: 'sparkles', blur: 900, mirror: 'yes', camera: 7, extra: 1 }),
    { on: true, camera: '', mode: 'blur', blur: 100, mirror: false, background: null });
  assert.equal(normalizeStudio({ on: false }).on, false);
  assert.equal(normalizeStudio({ on: 'no' }).on, true);           // only false turns it off
  assert.equal(normalizeStudio({ blur: -4 }).blur, 0);
  assert.equal(normalizeStudio({ blur: 33.6 }).blur, 34);
  assert.equal(normalizeStudio({ blur: NaN }).blur, 50);
  // a picture mode without a picture falls back to the blur
  assert.equal(normalizeStudio({ mode: 'picture' }).mode, 'blur');
  // pictures: LordOS's wallpapers or an absolute path to a picture; nothing else
  assert.equal(normalizeStudio({ mode: 'picture', background: 'asset:wallpapers/cosmos/lordos-event-horizon.jpg' }).mode, 'picture');
  assert.equal(normalizeStudio({ background: '/home/ana/Pictures/office.PNG' }).background, '/home/ana/Pictures/office.PNG');
  for (const bad of ['asset:wallpapers/../../etc/passwd.jpg', 'asset:icons/x.png', 'relative.jpg', '/home/a/notes.txt', '/home/a/b\n.jpg', 'https://x/y.jpg'])
    assert.equal(normalizeStudio({ background: bad }).background, null, bad);
});

t('cameras: the studio entry is not a real camera, and a camera is kept by its bus', () => {
  assert.deepEqual(realCameras(cams).map((c) => c.device), ['/dev/video0', '/dev/video2']);
  assert.equal(hasStudioCamera(cams), true);
  assert.equal(hasStudioCamera(cams.slice(0, 2)), false);
  assert.equal(deviceFor('usb-0000:00:14.0-7', cams), '/dev/video2');
  // renumbered after a reboot: the same bus, another node
  assert.equal(deviceFor('usb-0000:00:14.0-7', [{ device: '/dev/video4', bus: 'usb-0000:00:14.0-7' }]), '/dev/video4');
  assert.equal(deviceFor('usb-gone', cams), '');
  assert.equal(deviceFor('', cams), '');
  assert.equal(deviceFor('studio', cams), '');            // never itself
  assert.equal(chosenCamera('', cams).device, '/dev/video0');
  assert.equal(chosenCamera('usb-gone', cams).device, '/dev/video0');
  assert.equal(chosenCamera('x', []), null);
});

t('what lord.studio.set gets', () => {
  assert.deepEqual(serverConfig({ camera: 'usb-0000:00:14.0-3', mode: 'none', blur: 20, mirror: true }, cams),
    { on: true, camera: '/dev/video0', mode: 'none', blur: 20, mirror: true });
  assert.deepEqual(serverConfig({}, cams), { on: true, camera: '', mode: 'blur', blur: 50, mirror: false });
  assert.equal(serverConfig({ on: false }, cams).on, false);
});

t('Portrait and Background exclude each other', () => {
  const pics = [{ src: 'asset:wallpapers/signature/lordos-royal-flow.jpg' }];
  let c = normalizeStudio({ mode: 'none' });
  assert.equal(portraitOn(c), false);
  c = withPortrait(c, true);
  assert.equal(c.mode, 'blur');
  c = withBackground(c, true, pics);
  assert.equal(c.mode, 'picture');
  assert.equal(c.background, pics[0].src);
  assert.equal(portraitOn(c), false);
  assert.equal(backgroundOn(c), true);
  c = withPortrait(c, true);
  assert.equal(c.mode, 'blur');
  assert.equal(c.background, pics[0].src);                // the picture is remembered
  c = withPortrait(c, false);
  assert.equal(c.mode, 'none');
  c = withBackground(c, false, pics);
  assert.equal(c.mode, 'none');
  // Background on with no picture anywhere: nothing changes
  assert.equal(withBackground({ mode: 'none' }, true, []).mode, 'none');
  // a picture chosen turns the Background on
  c = withPicture({ mode: 'none' }, '/home/ana/Pictures/beach.jpg');
  assert.equal(c.mode, 'picture');
  assert.equal(withPicture({ mode: 'none' }, 'asset:../x.jpg').mode, 'none');
  // the page's three choices
  assert.equal(withMode({ mode: 'blur' }, 'picture').mode, 'blur');     // no picture yet
  assert.equal(withMode({ mode: 'blur', background: pics[0].src }, 'picture').mode, 'picture');
  assert.equal(withMode({ mode: 'blur' }, 'none').mode, 'none');
  assert.equal(withMode({ mode: 'blur' }, 'bokeh').mode, 'blur');
});

t('the Pi policy: 640×360 on an ARM CPU without the GPU path', () => {
  assert.deepEqual(previewSize({ arch: 'x86_64', gpu: false }), { width: 1280, height: 720 });
  assert.deepEqual(previewSize({ arch: 'aarch64', gpu: true }), { width: 1280, height: 720 });
  assert.deepEqual(previewSize({ arch: 'aarch64', gpu: false }), { width: 640, height: 360 });
  assert.deepEqual(previewSize(), { width: 1280, height: 720 });
});

t('the indicator text and the preview source', () => {
  const status = { sources: [{ id: 4, app: 'studio' }, { id: 7, app: 'camera' }, { id: 9, app: 'golink', isolated: true }, { id: 11, app: 'camera' }] };
  const names = { camera: 'Camera', golink: 'go-link Desk' };
  assert.deepEqual(usersOf(status, (a) => names[a]), ['Camera', 'go-link Desk']);
  assert.deepEqual(usersOf(null), []);
  assert.equal(previewSource(status), 7);                 // a native app's source, never an isolated one's
  assert.equal(previewSource({ sources: [{ id: 9, app: 'golink', isolated: true }] }), null);
  assert.equal(previewSource({ sources: [{ id: 4, app: 'studio' }] }), null);
  assert.equal(perfLine({ fps: 0 }), null);
  assert.deepEqual(perfLine({ fps: 29.6, gpu: true, segMs: 4.21, fxMs: 6.04 }), { fps: 30, gpu: true, ms: 10.3 });
});

t('phase 2: the camera for every app, its readers and the indicator', () => {
  assert.equal(loopbackState(null, {}), 'missing');
  assert.equal(loopbackState({ loopback: 'missing' }, {}), 'missing');
  assert.equal(loopbackState({ loopback: 'running' }, { on: false }), 'off');
  assert.equal(loopbackState({ loopback: 'ready', stopped: true }, {}), 'stopped');
  assert.equal(loopbackState({ loopback: 'running' }, {}), 'ready');
  // "In use" names the Linux apps reading the device too; one LordOS can't name is said once
  const names = { camera: 'Camera', 'desktop:chromium': 'Chromium' };
  assert.deepEqual(usersOf({ sources: [{ id: 7, app: 'camera' }], apps: ['desktop:chromium', '', 'desktop:chromium', ''] }, (a) => names[a], 'An app'),
    ['Camera', 'Chromium', 'An app']);
  // the event, cleaned
  assert.deepEqual(studioUseOf({ apps: ['desktop:chromium', 3, 'desktop:chromium', ''], camera: 'Logitech C930e', effects: true }),
    { apps: ['desktop:chromium', ''], camera: 'Logitech C930e', effects: true });
  assert.deepEqual(studioUseOf(null), { apps: [], camera: '', effects: false });
  // "Camera via LordOS Studio (Logitech C930e) · effects on"
  assert.deepEqual(studioWhat({ camera: 'Logitech C930e', effects: true }).map((p) => p.key),
    ['shell.capture.what.viaStudio', 'shell.capture.what.effectsOn']);
  assert.equal(studioWhat({ camera: 'Logitech C930e', effects: true })[0].params.camera, 'Logitech C930e');
  assert.deepEqual(studioWhat({ camera: ' ', effects: false }).map((p) => p.key), ['shell.capture.what.viaStudioPlain', 'shell.capture.what.effectsOff']);
  assert.deepEqual(studioWhat(null), []);
  // the indicator's rows: a Linux app reading the device is a camera row with "via"; Turn off is its firewall's no-camera
  // (which blocks /dev/video50 too: lord-netperm's cgroup/dev, major 81) and only for an app the firewall can name
  const rows = captureRows({ studio: { apps: ['desktop:chromium', ''], camera: 'Logitech C930e', effects: true } },
    { name: (a) => names[a] || a, enforceable: (a) => a === 'desktop:chromium' });
  assert.equal(rows.length, 2);
  const chrome = rows.find((r) => r.app === 'desktop:chromium');
  assert.equal(chrome.camera, true);
  assert.deepEqual(chrome.via, { camera: 'Logitech C930e', effects: true });
  assert.deepEqual(rowStops(chrome).off.flags, ['no-camera']);
  assert.equal(rowStops(rows.find((r) => !r.app)).off, null);
  assert.equal(captureGlyph(rows), 'camera');
  // nobody reads it: no row, no glyph (the camera's light is off too: lord-studio closed the real camera)
  assert.deepEqual(captureRows({ studio: { apps: [], camera: '', effects: false } }), []);
  assert.equal(captureGlyph(captureRows({ studio: null })), null);
});

console.log(`studio model: ${n} groups ok`);
