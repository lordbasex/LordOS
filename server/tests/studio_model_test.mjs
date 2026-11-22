// studio_model_test.mjs — sdk/studiomodel.js (LordOS Studio's popover logic, docs/CAMERA-STUDIO-PLAN.md 9.3).
// Run from server/: node tests/studio_model_test.mjs
import assert from 'node:assert/strict';
import {
  normalizeStudio, STUDIO_DEFAULTS, realCameras, hasStudioCamera, deviceFor, chosenCamera, serverConfig, portraitOn, backgroundOn,
  withPortrait, withBackground, withPicture, withMode, previewSize, usersOf, previewSource, perfLine,
  loopbackState, studioUseOf, studioWhat, MATCH_MAX, STUDIO_FILTERS, effectsOn, filterOf, isVideoPath, isPicturePath, activeBackground,
  BG_CATEGORIES, bgItems, bgThumb, bgVariant, bgPick, bgSrc, bgReady, bgItemOf, bgCredit, VOICE_SOURCE, VOICE_CAPTURE, voiceAvailable, hardwareMics,
  micModeOf, micPlan, withoutVoiceInternals,
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
    { ...STUDIO_DEFAULTS, blur: 100 });
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
  const looksOff = { key: 'none', similarity: 40, smoothness: 20, spill: 50, match: 0, framing: 0, light: 0, filter: 'none', filterStrength: 100, retouch: 0 };
  assert.deepEqual(serverConfig({ camera: 'usb-0000:00:14.0-3', mode: 'none', blur: 20, mirror: true }, cams),
    { on: true, camera: '/dev/video0', mode: 'none', blur: 20, mirror: true, ...looksOff });
  assert.deepEqual(serverConfig({}, cams), { on: true, camera: '', mode: 'blur', blur: 50, mirror: false, ...looksOff });
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

t('phase 3: the looks reach lord.studio.set only as numbers and names it knows', () => {
  const c = normalizeStudio({ green: true, greenKey: 'pink', similarity: 140, match: 99, stage: true, stageAmount: 0, light: true,
    lightAmount: 70.4, filter: 'lomo', filterStrength: -3, retouch: 'yes', retouchAmount: 20 });
  assert.equal(c.greenKey, 'green');
  assert.equal(c.similarity, 100);
  assert.equal(c.match, MATCH_MAX);
  assert.equal(c.filter, 'none');
  assert.equal(c.filterStrength, 0);
  assert.equal(c.retouch, false);                                  // only true turns it on
  const s = serverConfig({ ...c, mode: 'blur' }, cams);
  assert.equal(s.key, 'none');                                     // a green screen only with a picture or a video
  assert.equal(s.match, 0);                                        // the same for the colour match
  assert.equal(s.framing, 1);                                      // on at its widest is still on
  assert.equal(s.light, 70);
  assert.equal(s.retouch, 0);
  const p = serverConfig({ mode: 'picture', background: '/home/a/p.jpg', green: true, greenKey: 'blue', match: 25, filter: 'film', filterStrength: 60 }, cams);
  assert.deepEqual([p.mode, p.key, p.match, p.filter, p.filterStrength], ['picture', 'blue', 25, 'film', 60]);
  for (const f of STUDIO_FILTERS) assert.equal(normalizeStudio({ filter: f }).filter, f);
  assert.equal(effectsOn({ mode: 'none' }), false);
  assert.equal(effectsOn({ mode: 'none', filter: 'mono' }), true);
  assert.equal(effectsOn({ mode: 'none', filter: 'mono', filterStrength: 0 }), false);
  assert.equal(effectsOn({ mode: 'none', stage: true }), true);
  assert.equal(filterOf({ filter: 'warm' }), 'warm');
  assert.equal(filterOf({}), null);
});

t('phase 3: pictures and videos as the background', () => {
  const v = '/var/lib/lordos/studio/' + 'a'.repeat(64) + '.mp4';
  assert.equal(isVideoPath(v), true);
  assert.equal(isVideoPath('/home/a/clip.webm'), true);
  assert.equal(isVideoPath('/home/a/../clip.mp4'), false);
  assert.equal(isVideoPath('/home/a/clip.jpg'), false);
  assert.equal(isPicturePath('asset:studio/backgrounds/office-loft.jpg'), true);
  assert.equal(isPicturePath('asset:studio/models/x.jpg'), false);
  assert.equal(isPicturePath('asset:wallpapers/a/b/c.jpg'), false);
  // a video chosen: the Video mode, the picture remembered; and back
  let c = withPicture({ mode: 'picture', background: '/home/a/p.jpg' }, v);
  assert.deepEqual([c.mode, c.video, c.background], ['video', v, '/home/a/p.jpg']);
  assert.equal(activeBackground(c), v);
  c = withMode(c, 'picture');
  assert.equal(activeBackground(c), '/home/a/p.jpg');
  assert.equal(withMode({ mode: 'blur' }, 'video').mode, 'blur');               // no video yet
  assert.equal(normalizeStudio({ mode: 'video', background: '/home/a/p.jpg' }).mode, 'picture');
  assert.equal(normalizeStudio({ mode: 'video' }).mode, 'blur');
  assert.equal(activeBackground({ mode: 'blur', background: '/home/a/p.jpg' }), '/home/a/p.jpg');   // kept ready for the switch back
  assert.equal(backgroundOn({ mode: 'video', video: v }), true);
  // Background on again: what was chosen last
  assert.equal(withBackground({ mode: 'none', video: v }, true, []).mode, 'video');
  assert.equal(withBackground({ mode: 'video', video: v }, false, []).mode, 'none');
});

t('phase 3: the background catalog', () => {
  const sha = 'b'.repeat(64);
  const cat = { items: [
    { id: 'office-a', kind: 'picture', category: 'office', builtin: true, name: 'A', author: 'X', license: 'CC0', source: 'https://polyhaven.com/a/x',
      variants: [{ name: '2160', h: 2160, sha256: 'e'.repeat(64) }, { name: '1080', h: 1080, sha256: 'c'.repeat(64) }] },
    { id: 'sky-b', kind: 'video', category: 'nature', name: 'B', author: 'Y', license: 'CC BY 4.0', credit: 'Video: Y, CC BY 4.0', source: 'https://commons.wikimedia.org/x',
      variants: [{ name: '2160.h264', h: 2160, sha256: 'f'.repeat(64) }, { name: '1080.h264', h: 1080, sha256: sha }] },
    { id: 'old-c', kind: 'video', category: 'nature', name: 'C', author: 'Z', license: 'CC0', source: 'https://commons.wikimedia.org/y',
      variants: [{ name: '720.h264', h: 720, sha256: 'a'.repeat(64) }] },
  ] };
  assert.deepEqual(bgItems(cat, 'video').map((x) => x.id), ['sky-b', 'old-c']);
  assert.deepEqual(bgItems(null, 'video'), []);
  assert.equal(bgThumb(cat.items[0]), 'asset:studio/backgrounds/office-a.thumb.jpg');
  // a picture at 3840×2160 on the GPU path, 1920×1080 on the CPU path; a loop always 1920×1080 (a 4K decode on every
  // camera picture doesn't fit the frame); an old single-variant entry still has its one
  assert.equal(bgVariant(cat.items[1]).name, '1080.h264');
  assert.equal(bgVariant(cat.items[1], { big: true }).name, '1080.h264');
  assert.equal(bgVariant(cat.items[0]).name, '1080');
  assert.equal(bgVariant(cat.items[0], { big: true }).name, '2160');
  assert.equal(bgVariant(cat.items[2]).name, '720.h264');
  assert.equal(bgVariant(cat.items[2], { big: true }).name, '720.h264');
  assert.equal(bgVariant({ variants: [] }), null);
  assert.equal(bgSrc(cat.items[0]), 'asset:studio/backgrounds/office-a.jpg');                  // built in
  assert.equal(bgSrc(cat.items[0], '1080'), 'asset:studio/backgrounds/office-a.jpg');
  assert.equal(bgSrc(cat.items[0], '2160'), `/var/lib/lordos/studio/${'e'.repeat(64)}.jpg`);   // its 4K, downloaded
  assert.equal(bgSrc(cat.items[1], '1080.h264'), `/var/lib/lordos/studio/${sha}.mp4`);
  assert.equal(bgSrc(cat.items[1], 'nope'), null);
  assert.equal(bgReady(cat.items[0], {}), true);
  assert.equal(bgReady(cat.items[1], {}), false);
  assert.equal(bgReady(cat.items[1], { 'sky-b': '1080.h264' }), true);
  // what choosing does: a built-in picture as it is on the CPU path, its 4K fetched on the GPU path (the built-in if that fails)
  assert.deepEqual(bgPick(cat.items[0], {}), { src: 'asset:studio/backgrounds/office-a.jpg', fetch: null, fallback: null });
  const p4 = bgPick(cat.items[0], {}, { big: true });
  assert.equal(p4.fetch.name, '2160');
  assert.equal(p4.fallback, 'asset:studio/backgrounds/office-a.jpg');
  assert.equal(bgPick(cat.items[0], { 'office-a': '2160' }).src, `/var/lib/lordos/studio/${'e'.repeat(64)}.jpg`);  // a bigger picture serves
  // a loop: the exact variant (a bigger one would cost its decode on every picture); one no longer in the catalog is fetched again
  assert.equal(bgPick(cat.items[1], {}).fetch.name, '1080.h264');
  assert.equal(bgPick(cat.items[1], { 'sky-b': '1080.h264' }).src, `/var/lib/lordos/studio/${sha}.mp4`);
  assert.equal(bgPick(cat.items[1], { 'sky-b': '1080.h264' }, { big: true }).src, `/var/lib/lordos/studio/${sha}.mp4`);
  const up = bgPick(cat.items[1], { 'sky-b': '2160.h264' }, { big: true });
  assert.equal(up.fetch.name, '1080.h264');
  assert.equal(up.fallback, `/var/lib/lordos/studio/${'f'.repeat(64)}.mp4`);
  assert.equal(bgPick(cat.items[1], { 'sky-b': '2160.h264' }).fetch.name, '1080.h264');
  assert.equal(bgPick(cat.items[1], { 'sky-b': '720.h264' }).fetch.name, '1080.h264');
  assert.equal(bgPick(null, {}).src, null);
  assert.equal(bgItemOf(cat, `/var/lib/lordos/studio/${sha}.mp4`).id, 'sky-b');
  assert.equal(bgItemOf(cat, 'asset:studio/backgrounds/office-a.jpg').id, 'office-a');
  assert.equal(bgItemOf(cat, '/home/a/me.jpg'), null);
  assert.equal(bgCredit(cat.items[1]), 'B — Video: Y, CC BY 4.0 — CC BY 4.0 — https://commons.wikimedia.org/x');
  assert.equal(bgCredit(cat.items[0]), 'A — X — CC0 — https://polyhaven.com/a/x');
  assert.deepEqual(normalizeStudio({ bgFiles: { 'sky-b': '720.h264', '../x': 'y', ok: 'a b' } }).bgFiles, { 'sky-b': '720.h264' });
  assert.ok(BG_CATEGORIES.includes('office') && BG_CATEGORIES.includes('space'));
});

t('phase 3: Mic Mode through the PulseAudio client', () => {
  const st = {
    defaultSource: 'alsa_input.usb-mic',
    sources: [
      { index: 1, name: 'alsa_input.usb-mic', monitorOf: -1, default: true },
      { index: 2, name: 'alsa_output.hdmi.monitor', monitorOf: 5 },
      { index: 3, name: 'alsa_input.pci-builtin', monitorOf: -1 },
      { index: 9, name: VOICE_SOURCE, monitorOf: -1 },
    ],
    sinks: [{ index: 5, name: 'alsa_output.hdmi' }, { index: 6, name: 'lordos_voice_reference' }],
    inputs: [{ index: 40, name: 'lordos_voice_playback' }, { index: 41, name: 'Firefox' }],
    outputs: [{ index: 30, name: VOICE_CAPTURE, source: 3 }, { index: 31, name: 'Meet', source: 1 }],
  };
  assert.equal(voiceAvailable(st), true);
  assert.deepEqual(hardwareMics(st).map((m) => m.name), ['alsa_input.usb-mic', 'alsa_input.pci-builtin']);
  // Voice Isolation: the filter listens to the microphone in use, which stays remembered; then it is the default
  let p = micPlan(st, 'voice');
  assert.deepEqual(p, { ok: true, mic: 'alsa_input.usb-mic', calls: [['move', 'output', 30, 'alsa_input.usb-mic'], ['setDefault', 'source', VOICE_SOURCE]] });
  const on = { ...st, defaultSource: VOICE_SOURCE, outputs: [{ index: 30, name: VOICE_CAPTURE, source: 1 }] };
  assert.equal(micModeOf(on, {}), 'voice');
  assert.deepEqual(micPlan(on, 'voice', 'alsa_input.usb-mic').calls, []);                  // already so
  // back to Standard: the remembered microphone is the default again
  p = micPlan(on, 'standard', 'alsa_input.usb-mic');
  assert.deepEqual(p.calls, [['setDefault', 'source', 'alsa_input.usb-mic']]);
  assert.equal(micModeOf(st, { mic: 'wide' }), 'wide');
  assert.equal(micModeOf(st, { mic: 'voice' }), 'standard');                               // the default says otherwise
  assert.deepEqual(micPlan(st, 'wide').calls, []);
  // without the drop-in's source, or without a microphone: nothing to do
  const plain = { ...st, sources: st.sources.filter((s) => s.name !== VOICE_SOURCE) };
  assert.equal(micPlan(plain, 'voice').ok, false);
  assert.equal(micPlan({ sources: [] }, 'standard').ok, false);
  assert.equal(micPlan(st, 'loud').ok, false);
  // the sound lists never show the filter's own echo reference or its streams
  const shown = withoutVoiceInternals(st);
  assert.deepEqual(shown.sinks.map((x) => x.name), ['alsa_output.hdmi']);
  assert.deepEqual(shown.inputs.map((x) => x.name), ['Firefox']);
  assert.deepEqual(shown.outputs.map((x) => x.name), ['Meet']);
  assert.equal(shown.sources.length, 4);                                                    // Voice Isolation is a microphone
  assert.equal(withoutVoiceInternals(null), null);
});

console.log(`studio model: ${n} groups ok`);
