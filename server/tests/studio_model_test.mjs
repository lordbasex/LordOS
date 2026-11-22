// studio_model_test.mjs — sdk/studiomodel.js (LordOS Studio's popover logic, docs/CAMERA-STUDIO-PLAN.md 9.3).
// Run from server/: node tests/studio_model_test.mjs
import assert from 'node:assert/strict';
import {
  normalizeStudio, STUDIO_DEFAULTS, realCameras, hasStudioCamera, deviceFor, chosenCamera, serverConfig, portraitOn, backgroundOn,
  withPortrait, withBackground, withPicture, withMode, previewSize, usersOf, previewSource, perfLine,
} from '../../sdk/studiomodel.js';

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
    { camera: '', mode: 'blur', blur: 100, mirror: false, background: null });
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
    { camera: '/dev/video0', mode: 'none', blur: 20, mirror: true });
  assert.deepEqual(serverConfig({}, cams), { camera: '', mode: 'blur', blur: 50, mirror: false });
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

console.log(`studio model: ${n} groups ok`);
