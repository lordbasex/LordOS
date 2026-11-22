// studiomodel.js — LordOS Studio's plain logic (sdk/studio.jsx; server/tests/studio_model_test.mjs; docs/CAMERA-STUDIO-PLAN.md 9.3).
//
// settings.studio = { camera, mode, blur, mirror, background }
//   camera      the real camera LordOS Studio uses, by its USB bus id (stable across reboots, unlike /dev/videoN); '' = the
//               first camera there is
//   mode        'none' | 'blur' (Portrait) | 'picture' (Background)
//   blur        0..100, the Portrait's strength
//   mirror      the video itself mirrored left to right (the preview is always shown mirrored, like a mirror)
//   background  'asset:wallpapers/…' (one of LordOS's wallpapers) or a picture's absolute path, or null
// These are new keys with defaults, so no migration (sdk/settings.js MIGRATIONS) is needed.

export const STUDIO_MODES = Object.freeze(['none', 'blur', 'picture']);
export const STUDIO_DEFAULTS = Object.freeze({ camera: '', mode: 'blur', blur: 50, mirror: false, background: null });
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
  return { camera: deviceFor(c.camera, list), mode: c.mode, blur: c.blur, mirror: c.mirror };
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

// "In use" for the popover: the apps reading LordOS Studio now, by name, without the popover's own preview
export function usersOf(status, nameOf = (a) => a) {
  const list = status && Array.isArray(status.sources) ? status.sources : [];
  const names = [];
  for (const s of list) {
    if (!s || !s.app || s.app === 'studio') continue;
    const n = nameOf(s.app) || s.app;
    if (!names.includes(n)) names.push(n);
  }
  return names;
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
