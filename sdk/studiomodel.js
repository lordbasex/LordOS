// studiomodel.js — LordOS Studio's plain logic (sdk/studio.jsx; server/tests/studio_model_test.mjs; docs/CAMERA-STUDIO-PLAN.md 9.3).
//
// settings.studio = { on, camera, mode, blur, mirror, background }
//   on          LordOS Studio's camera for every app (phase 2): the "LordOS Studio" V4L2 device that Linux apps (browsers,
//               OBS) see when the lordos-studio-camera package is installed; off, apps reading it get the "camera off"
//               card and the real camera stays closed
//   camera      the real camera LordOS Studio uses, by its USB bus id (stable across reboots, unlike /dev/videoN); '' = the
//               first camera there is
//   mode        'none' | 'blur' (Portrait) | 'picture' (Background)
//   blur        0..100, the Portrait's strength
//   mirror      the video itself mirrored left to right (the preview is always shown mirrored, like a mirror)
//   background  'asset:wallpapers/…' (one of LordOS's wallpapers) or a picture's absolute path, or null
// These are new keys with defaults, so no migration (sdk/settings.js MIGRATIONS) is needed.

export const STUDIO_MODES = Object.freeze(['none', 'blur', 'picture']);
export const STUDIO_DEFAULTS = Object.freeze({ on: true, camera: '', mode: 'blur', blur: 50, mirror: false, background: null });
export const STUDIO_DEVICE = 'studio';

const BG_RE = /^(asset:wallpapers\/[A-Za-z0-9._\/-]+|\/[^\u0000-\u001f]+)\.(jpe?g|png|webp)$/i;

// Any stored value → a valid one (unknown keys dropped, numbers clamped)
export function normalizeStudio(s) {
  const o = s && typeof s === 'object' ? s : {};
  const blur = Number.isFinite(o.blur) ? Math.round(Math.min(100, Math.max(0, o.blur))) : STUDIO_DEFAULTS.blur;
  const bg = typeof o.background === 'string' && BG_RE.test(o.background) && !o.background.includes('..') ? o.background : null;
  let mode = STUDIO_MODES.includes(o.mode) ? o.mode : STUDIO_DEFAULTS.mode;
  if (mode === 'picture' && !bg) mode = 'blur';
  return {
    on: o.on !== false,
    camera: typeof o.camera === 'string' && o.camera.length < 128 ? o.camera : '',
    mode,
    blur,
    mirror: o.mirror === true,
    background: bg,
  };
}

// The cameras LordOS Studio can use: lord.media.cameras() without its own entry
export const realCameras = (list) => (Array.isArray(list) ? list.filter((c) => c && c.device !== STUDIO_DEVICE && !c.studio) : []);
export const hasStudioCamera = (list) => Array.isArray(list) && list.some((c) => c && c.device === STUDIO_DEVICE);

// The chosen camera's device now ('' when it is gone: lord-server then takes the first one)
export function deviceFor(camera, list) {
  const cams = realCameras(list);
  if (!camera) return '';
  const c = cams.find((x) => x.bus === camera) || cams.find((x) => x.device === camera);
  return c ? c.device : '';
}
// The camera to show as chosen in the picker: the saved one, else the first
export function chosenCamera(camera, list) {
  const cams = realCameras(list);
  return cams.find((x) => x.bus === camera) || cams.find((x) => x.device === camera) || cams[0] || null;
}
export const cameraKey = (c) => (c ? c.bus || c.device : '');

// What lord.studio.set receives
export function serverConfig(cfg, list) {
  const c = normalizeStudio(cfg);
  return { on: c.on, camera: deviceFor(c.camera, list), mode: c.mode, blur: c.blur, mirror: c.mirror };
}

// Portrait (the blur) and Background (a picture) are the two switches of the popover; only one can be on
export const portraitOn = (cfg) => normalizeStudio(cfg).mode === 'blur';
export const backgroundOn = (cfg) => normalizeStudio(cfg).mode === 'picture';
export function withPortrait(cfg, on) {
  const c = normalizeStudio(cfg);
  return { ...c, mode: on ? 'blur' : c.mode === 'blur' ? 'none' : c.mode };
}
// on with no picture chosen yet: the first of `pictures` (LordOS's wallpapers)
export function withBackground(cfg, on, pictures = []) {
  const c = normalizeStudio(cfg);
  if (!on) return { ...c, mode: c.mode === 'picture' ? 'none' : c.mode };
  const bg = c.background || (pictures[0] && pictures[0].src) || null;
  return bg ? { ...c, mode: 'picture', background: bg } : c;
}
export function withPicture(cfg, src) {
  const c = normalizeStudio({ ...normalizeStudio(cfg), background: src });
  return c.background ? { ...c, mode: 'picture' } : c;
}
// The Background page's three choices
export function withMode(cfg, mode) {
  const c = normalizeStudio(cfg);
  if (!STUDIO_MODES.includes(mode)) return c;
  if (mode === 'picture' && !c.background) return c;
  return { ...c, mode };
}

// The preview's size: a Raspberry Pi without the GPU path composites on its CPU, so 640×360 there (plan 3.3)
export function previewSize({ arch = '', gpu = true } = {}) {
  return /^(aarch64|arm)/.test(arch) && !gpu ? { width: 640, height: 360 } : { width: 1280, height: 720 };
}

// "In use" for the popover: the apps reading LordOS Studio now, by name, without the popover's own preview: LordOS's
// own (status.sources) and the Linux apps reading its camera device (status.apps, '' one LordOS can't name)
export function usersOf(status, nameOf = (a) => a, unknown = '') {
  const list = status && Array.isArray(status.sources) ? status.sources : [];
  const names = [];
  const add = (n) => { if (n && !names.includes(n)) names.push(n); };
  for (const s of list) {
    if (!s || !s.app || s.app === 'studio') continue;
    add(nameOf(s.app) || s.app);
  }
  for (const a of status && Array.isArray(status.apps) ? status.apps : []) {
    if (typeof a !== 'string') continue;
    add(a ? nameOf(a) || a : unknown);
  }
  return names;
}

// The "LordOS Studio" camera for every app (phase 2), as the popover's row says it:
//   'missing'  the device isn't there: the Store's item isn't installed, or its module needs a restart (Secure Boot)
//   'off'      installed, and the person turned it off
//   'stopped'  lord-studio for it kept stopping (3 times in a minute): off until the setting changes
//   'ready'    apps can choose it
export function loopbackState(status, cfg) {
  if (!status || status.loopback === 'missing' || !status.loopback) return 'missing';
  if (!normalizeStudio(cfg).on) return 'off';
  if (status.stopped) return 'stopped';
  return 'ready';
}

// The "studio-use" event of lord-server (media/mstudio.c): the Linux apps reading the "LordOS Studio" device, the real
// camera behind it and whether effects are on → { apps: [id], camera, effects }
export function studioUseOf(data) {
  const d = data && typeof data === 'object' ? data : {};
  const apps = (Array.isArray(d.apps) ? d.apps : []).filter((a) => typeof a === 'string' && a.length < 200);
  return { apps: [...new Set(apps)], camera: typeof d.camera === 'string' ? d.camera.slice(0, 80) : '', effects: d.effects === true };
}

// The capture indicator's line for an app reading LordOS Studio's camera: "Camera via LordOS Studio (Logitech C930e) ·
// effects on" → the locale keys (shell.capture.what.*) and their values
export function studioWhat(via) {
  if (!via) return [];
  const cam = typeof via.camera === 'string' ? via.camera.trim() : '';
  return [
    cam ? { key: 'shell.capture.what.viaStudio', params: { camera: cam } } : { key: 'shell.capture.what.viaStudioPlain', params: {} },
    { key: via.effects ? 'shell.capture.what.effectsOn' : 'shell.capture.what.effectsOff', params: {} },
  ];
}

// A native LordOS app's LordOS Studio source the popover can preview instead of opening the camera a second time
export function previewSource(status) {
  const list = status && Array.isArray(status.sources) ? status.sources : [];
  const own = list.find((s) => s && s.app && s.app !== 'studio' && !s.isolated && Number.isInteger(s.id));
  return own ? own.id : null;
}

// The performance line under the preview: frames per second and where the composite runs
export function perfLine(status) {
  if (!status || !(status.fps > 0)) return null;
  return { fps: Math.round(status.fps), gpu: !!status.gpu, ms: Math.round(((status.segMs || 0) + (status.fxMs || 0)) * 10) / 10 };
}
