// studiomodel.js — LordOS Studio's plain logic (sdk/studio.jsx; server/tests/studio_model_test.mjs; docs/CAMERA-STUDIO-PLAN.md 9.3).
//
// settings.studio = { on, camera, mode, blur, mirror, background, video, green, greenKey, similarity, smoothness, spill,
//                     match, stage, stageAmount, light, lightAmount, filter, filterStrength, retouch, retouchAmount,
//                     mic, micSource, bgFiles }
//   on          LordOS Studio's camera for every app (phase 2): the "LordOS Studio" V4L2 device that Linux apps (browsers,
//               OBS) see when the lordos-studio-camera package is installed; off, apps reading it get the "camera off"
//               card and the real camera stays closed
//   camera      the real camera LordOS Studio uses, by its USB bus id (stable across reboots, unlike /dev/videoN); '' = the
//               first camera there is
//   mode        'none' | 'blur' (Portrait) | 'picture' | 'video' (Background: a picture, or a short loop)
//   blur        0..100, the Portrait's strength
//   mirror      the video itself mirrored left to right (the preview is always shown mirrored, like a mirror)
//   background  the picture: 'asset:wallpapers/…' (one of LordOS's wallpapers), 'asset:studio/backgrounds/…' (one of the
//               catalog's built in), '/var/lib/lordos/studio/<sha256>.jpg' (downloaded), or a picture's absolute path
//   video       the loop: '/var/lib/lordos/studio/<sha256>.mp4' (downloaded) or a video's absolute path
//   green       a real green (or blue: greenKey) screen behind the person, with a picture or a video; similarity,
//               smoothness and spill are its sliders, 0..100
//   match       colour match of the person to the picture or video, 0..60 %
//   stage       Auto Framing, stageAmount 0..100 from wide to tight
//   light       Face Light, lightAmount 0..100
//   filter      'none' | 'warm' | 'cool' | 'vivid' | 'mono' | 'noir' | 'film' | 'fade' | 'sepia', filterStrength 0..100
//   retouch     Retouch, retouchAmount 0..100
//   mic         Mic Mode: 'standard' | 'voice' (Noise Reduction) | 'wide' (Full Range); micSource: the real
//               microphone Noise Reduction listens to (its PulseAudio name)
//   bgFiles     the catalog's backgrounds downloaded: { [id]: variant name }
// These are new keys with defaults, so no migration (sdk/settings.js MIGRATIONS) is needed.

export const STUDIO_MODES = Object.freeze(['none', 'blur', 'picture', 'video']);
export const STUDIO_FILTERS = Object.freeze(['none', 'warm', 'cool', 'vivid', 'mono', 'noir', 'film', 'fade', 'sepia']);
export const STUDIO_KEYS = Object.freeze(['green', 'blue']);
export const MIC_MODES = Object.freeze(['standard', 'voice', 'wide']);
export const MATCH_MAX = 60;
export const STUDIO_DEFAULTS = Object.freeze({
  on: true, camera: '', mode: 'blur', blur: 50, mirror: false, background: null, video: null,
  green: false, greenKey: 'green', similarity: 40, smoothness: 20, spill: 50, match: 30,
  stage: false, stageAmount: 50, light: false, lightAmount: 50, filter: 'none', filterStrength: 100,
  retouch: false, retouchAmount: 50, mic: 'standard', micSource: '', bgFiles: {},
});
export const STUDIO_DEVICE = 'studio';
export const STUDIO_STORE = '/var/lib/lordos/studio';

const SAFE = /^[^\u0000-\u001f\u007f]+$/;
const PICTURE_RE = /^(asset:wallpapers\/[A-Za-z0-9._-]+(\/[A-Za-z0-9._-]+)?|asset:studio\/backgrounds\/[A-Za-z0-9._-]+|\/[^\u0000-\u001f]+)\.(jpe?g|png|webp)$/i;
const VIDEO_RE = /^\/[^\u0000-\u001f]+\.(mp4|webm|mkv|mov|m4v)$/i;
export const isVideoPath = (p) => typeof p === 'string' && VIDEO_RE.test(p) && SAFE.test(p) && !p.includes('..');
export const isPicturePath = (p) => typeof p === 'string' && PICTURE_RE.test(p) && SAFE.test(p) && !p.includes('..');

const pct = (v, def, max = 100) => (Number.isFinite(v) ? Math.round(Math.min(max, Math.max(0, v))) : def);
const D = STUDIO_DEFAULTS;

// Any stored value → a valid one (unknown keys dropped, numbers clamped)
export function normalizeStudio(s) {
  const o = s && typeof s === 'object' ? s : {};
  const bg = isPicturePath(o.background) ? o.background : null;
  const video = isVideoPath(o.video) ? o.video : null;
  let mode = STUDIO_MODES.includes(o.mode) ? o.mode : D.mode;
  if (mode === 'picture' && !bg) mode = 'blur';
  if (mode === 'video' && !video) mode = bg ? 'picture' : 'blur';
  const files = {};
  if (o.bgFiles && typeof o.bgFiles === 'object') {
    for (const [k, v] of Object.entries(o.bgFiles)) if (/^[a-z0-9-]{1,48}$/.test(k) && typeof v === 'string' && /^[a-z0-9.-]{1,48}$/.test(v)) files[k] = v;
  }
  return {
    on: o.on !== false,
    camera: typeof o.camera === 'string' && o.camera.length < 128 ? o.camera : '',
    mode,
    blur: pct(o.blur, D.blur),
    mirror: o.mirror === true,
    background: bg,
    video,
    green: o.green === true,
    greenKey: STUDIO_KEYS.includes(o.greenKey) ? o.greenKey : D.greenKey,
    similarity: pct(o.similarity, D.similarity),
    smoothness: pct(o.smoothness, D.smoothness),
    spill: pct(o.spill, D.spill),
    match: pct(o.match, D.match, MATCH_MAX),
    stage: o.stage === true,
    stageAmount: pct(o.stageAmount, D.stageAmount),
    light: o.light === true,
    lightAmount: pct(o.lightAmount, D.lightAmount),
    filter: STUDIO_FILTERS.includes(o.filter) ? o.filter : D.filter,
    filterStrength: pct(o.filterStrength, D.filterStrength),
    retouch: o.retouch === true,
    retouchAmount: pct(o.retouchAmount, D.retouchAmount),
    mic: MIC_MODES.includes(o.mic) ? o.mic : D.mic,
    micSource: typeof o.micSource === 'string' && o.micSource.length < 256 && (!o.micSource || SAFE.test(o.micSource)) ? o.micSource : '',
    bgFiles: files,
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

// The file behind the background now (what lord.studio.setBackground gets): the picture or the video, by the mode
export function activeBackground(cfg) {
  const c = normalizeStudio(cfg);
  return c.mode === 'video' ? c.video : c.background;
}
const pictured = (c) => c.mode === 'picture' || c.mode === 'video';

// What lord.studio.set receives
export function serverConfig(cfg, list) {
  const c = normalizeStudio(cfg);
  const on = (b, v) => (b ? Math.max(1, v) : 0);
  return {
    on: c.on, camera: deviceFor(c.camera, list), mode: c.mode, blur: c.blur, mirror: c.mirror,
    key: c.green && pictured(c) ? c.greenKey : 'none', similarity: c.similarity, smoothness: c.smoothness, spill: c.spill,
    match: pictured(c) ? c.match : 0, framing: on(c.stage, c.stageAmount), light: on(c.light, c.lightAmount),
    filter: c.filter, filterStrength: c.filterStrength, retouch: on(c.retouch, c.retouchAmount),
  };
}

// Portrait (the blur) and Background (a picture or a video) are the two switches of the popover; only one can be on
export const portraitOn = (cfg) => normalizeStudio(cfg).mode === 'blur';
export const backgroundOn = (cfg) => pictured(normalizeStudio(cfg));
export function withPortrait(cfg, on) {
  const c = normalizeStudio(cfg);
  return { ...c, mode: on ? 'blur' : c.mode === 'blur' ? 'none' : c.mode };
}
// on with nothing chosen yet: the picture or video chosen last, else the first of `pictures`
export function withBackground(cfg, on, pictures = []) {
  const c = normalizeStudio(cfg);
  if (!on) return { ...c, mode: pictured(c) ? 'none' : c.mode };
  if (c.background) return { ...c, mode: 'picture' };
  if (c.video) return { ...c, mode: 'video' };
  const bg = (pictures[0] && pictures[0].src) || null;
  return bg ? normalizeStudio({ ...c, mode: 'picture', background: bg }) : c;
}
// a picture or a video chosen: it shows at once
export function withPicture(cfg, src) {
  const c = normalizeStudio(cfg);
  if (isVideoPath(src)) return { ...c, mode: 'video', video: src };
  const n = normalizeStudio({ ...c, background: src });
  return n.background ? { ...n, mode: 'picture' } : n;
}
// The Background page's choices: none, blur, picture, video (each needs its file chosen once)
export function withMode(cfg, mode) {
  const c = normalizeStudio(cfg);
  if (!STUDIO_MODES.includes(mode)) return c;
  if (mode === 'picture' && !c.background) return c;
  if (mode === 'video' && !c.video) return c;
  return { ...c, mode };
}
// Is any effect on (the status item's dot, the indicator's "effects on")
export function effectsOn(cfg) {
  const c = normalizeStudio(cfg);
  return c.mode !== 'none' || c.stage || c.light || c.retouch || (c.filter !== 'none' && c.filterStrength > 0);
}
// A row's short state for the popover's main page (null: nothing to say)
export const filterOf = (cfg) => { const c = normalizeStudio(cfg); return c.filter === 'none' ? null : c.filter; };

// ---- the background catalog (assets/studio/backgrounds.json; docs/CAMERA-STUDIO-PLAN.md 6.2) ----
// The catalog's pictures ship as thumbnails (and one picture per category in full); the rest are downloaded on demand by
// the closed admin action studio-bg-get "<id> <variant>" into /var/lib/lordos/studio/<sha256>.jpg or .mp4.
export const BG_CATEGORIES = Object.freeze(['office', 'home', 'studio', 'nature', 'city', 'abstract', 'space']);
export const bgItems = (catalog, kind) => (catalog && Array.isArray(catalog.items) ? catalog.items.filter((it) => it && it.kind === kind) : []);
export const bgThumb = (it) => `asset:studio/backgrounds/${it.id}.thumb.jpg`;
// the variant to fetch: a picture has one; a loop 720p (the camera's usual size) unless the output is bigger
export function bgVariant(it, { big = false } = {}) {
  const vs = it && Array.isArray(it.variants) ? it.variants : [];
  if (it?.kind !== 'video') return vs[0] || null;
  return (big && vs.find((v) => v.h >= 1080)) || vs.find((v) => v.h === 720) || vs[0] || null;
}
// the file the background reads, once it is on disk (a built-in picture always is)
export function bgSrc(it, variantName) {
  if (!it) return null;
  if (it.kind === 'picture' && it.builtin) return `asset:studio/backgrounds/${it.id}.jpg`;
  const v = (it.variants || []).find((x) => x.name === variantName);
  if (!v || !/^[0-9a-f]{64}$/.test(v.sha256 || '')) return null;
  return `${STUDIO_STORE}/${v.sha256}.${it.kind === 'video' ? 'mp4' : 'jpg'}`;
}
export const bgReady = (it, files) => !!it && ((it.kind === 'picture' && !!it.builtin) || !!(files && files[it.id]));
// The credits page's line for an item: its name, author, license and where it comes from (the catalog's own words: the
// names and credits are the authors', not translated)
export function bgCredit(it) {
  if (!it) return '';
  return [it.name, it.credit || it.author, it.license, it.source].filter(Boolean).join(' — ');
}
// an item generated with AI (the catalog's "ai"), which the credits page says in the person's language
export const bgAi = (it) => !!(it && it.ai === true);
// which catalog item a background path is (null for the person's own files)
export function bgItemOf(catalog, path) {
  if (typeof path !== 'string') return null;
  const m = /^asset:studio\/backgrounds\/([a-z0-9-]+)\.jpg$/.exec(path) || null;
  const items = catalog && Array.isArray(catalog.items) ? catalog.items : [];
  if (m) return items.find((it) => it.id === m[1]) || null;
  const h = /^\/var\/lib\/lordos\/studio\/([0-9a-f]{64})\.(jpg|mp4)$/.exec(path);
  if (!h) return null;
  return items.find((it) => (it.variants || []).some((v) => v.sha256 === h[1])) || null;
}

// ---- Mic Mode (docs/CAMERA-STUDIO-PLAN.md 7; the drop-in tools/iso/rootfs/usr/share/pipewire/pipewire.conf.d/lordos-voice.conf)
// "LordOS Noise Reduction" is one more microphone that PipeWire's echo-cancel module makes; choosing a mode only sets the
// default microphone and points the filter at the real one, through lord.sound (the PulseAudio client). Standard and
// Full Range both use the microphone as it is (no processing at all); Noise Reduction is the filtered one.
export const VOICE_SOURCE = 'lordos_voice_isolation';
export const VOICE_CAPTURE = 'lordos_voice_capture';
export const VOICE_INTERNAL_SINKS = Object.freeze(['lordos_voice_reference']);
export const VOICE_INTERNAL_STREAMS = Object.freeze(['lordos_voice_capture', 'lordos_voice_playback']);

const list = (x) => (Array.isArray(x) ? x : []);
// the state the sound UI shows: without the filter's own echo reference and its two streams (they are no app's)
export function withoutVoiceInternals(st) {
  if (!st || typeof st !== 'object') return st;
  return {
    ...st,
    sinks: list(st.sinks).filter((x) => !VOICE_INTERNAL_SINKS.includes(x?.name)),
    inputs: list(st.inputs).filter((x) => !VOICE_INTERNAL_STREAMS.includes(x?.name)),
    outputs: list(st.outputs).filter((x) => !VOICE_INTERNAL_STREAMS.includes(x?.name)),
  };
}
export const voiceAvailable = (st) => list(st?.sources).some((s) => s && s.name === VOICE_SOURCE);
// the real microphones (not monitors, not Noise Reduction)
export const hardwareMics = (st) => list(st?.sources).filter((s) => s && s.monitorOf === -1 && s.name !== VOICE_SOURCE);
// the mode shown: Noise Reduction while it is the default microphone; else what was chosen
export function micModeOf(st, cfg) {
  const c = normalizeStudio(cfg);
  if (st && st.defaultSource === VOICE_SOURCE) return 'voice';
  return c.mic === 'wide' ? 'wide' : 'standard';
}
// What choosing a mode does: [fn, ...args] calls of lord.sound, and the real microphone kept in settings.studio.micSource.
// raw: lord.sound.state() as it is (with the filter's capture stream). ok false: no microphone, or no Noise Reduction.
export function micPlan(raw, mode, saved = '') {
  const hw = hardwareMics(raw);
  const def = raw?.defaultSource || '';
  const mic = hw.find((m) => m.name === def) || hw.find((m) => m.name === saved) || hw.find((m) => m.default) || hw[0];
  if (!mic || !MIC_MODES.includes(mode)) return { ok: false, calls: [], mic: saved };
  if (mode === 'voice') {
    if (!voiceAvailable(raw)) return { ok: false, calls: [], mic: mic.name };
    const calls = [];
    const cap = list(raw.outputs).find((o) => o && o.name === VOICE_CAPTURE);
    if (cap && cap.source !== mic.index) calls.push(['move', 'output', cap.index, mic.name]);
    if (def !== VOICE_SOURCE) calls.push(['setDefault', 'source', VOICE_SOURCE]);
    return { ok: true, calls, mic: mic.name };
  }
  return { ok: true, calls: def === mic.name ? [] : [['setDefault', 'source', mic.name]], mic: mic.name };
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
