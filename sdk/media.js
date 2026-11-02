// media.js — video, music, photos, screenshots, camera and recorder (lord.media, see docs/MEDIA.md).
//
// Decoding and encoding is done by lord-media, an isolated helper (no network or file access)
// that lord-server launches on demand; here we only request things by id and receive events.
//
//   const id = media.play('player', path, { loop: false });   // a player
//   <MediaView media={id} style={{ flex: 1 }} />              // where it's shown (C draws it)
//   const p = usePlayer(id);   // { info, pos, duration, playing, state, subs, stats }
//   media.control(id, 'seek', 30000)
//
// Errors (the `error` of a job result and of a player's state) are code lines such as
// "media.notAnImage": show them with errorText() (sdk/errors.js). 'crash' means lord-media died.
//
// Camera, microphone and screen ask for permission (requestPermission, the system prompt) and the
// server checks it again. While in use, the menu bar indicators light up.
import { createElement as h, useEffect, useState } from 'react';
import { View, Text, Image, MediaView, addListener } from '@lordos/runtime';
import { createStore, useStore } from './store.js';
import { requestPermission, markDeviceUse } from './privacy.js';
import { t } from './i18n.js';
import { useTheme } from './theme.js';
import * as fs from './fs.js';

const api = () => (typeof lord !== 'undefined' && lord.media) || null;
const listeners = new Map();          // id → Set(fn(type, data))
const pending = new Map();            // job id → resolve
export const thumbRev = createStore(0);   // goes up when a thumbnail arrives

globalThis.__lordMedia = (id, type, data) => {
  if (type === 'job') {
    const done = pending.get(data && data.id);
    if (done) { pending.delete(data.id); done(data); }
    return;
  }
  if (type === 'thumb') { thumbRev.set((v) => v + 1); return; }
  const set = listeners.get(id);
  if (set) for (const fn of [...set]) { try { fn(type, data); } catch (e) { console.error('media:', e?.message || e); } }
};

// Promise for a job (probe, thumbnails, editing, export…): resolves with the "job" event.
function job(fn) {
  return new Promise((resolve) => {
    let id = 0;
    try { id = fn(); } catch (e) { resolve({ ok: false, error: e?.message || String(e) }); return; }
    if (!id) { resolve({ ok: false, error: 'media.noHelper' }); return; }
    pending.set(id, resolve);
  });
}

const call = (name, ...a) => { const m = api(); return m ? m[name](...a) : null; };

export const media = {
  available: () => !!api(),
  hwaccel: () => call('hwaccel') || 'none',
  on(id, fn) {
    if (!listeners.has(id)) listeners.set(id, new Set());
    listeners.get(id).add(fn);
    return () => { const s = listeners.get(id); if (s) { s.delete(fn); if (!s.size) listeners.delete(id); } };
  },
  // play (video, music, animated GIF): returns the id
  play(app, path, opts = {}) { const id = call('play', app, path, opts); if (id) track(id); return id; },
  control: (id, cmd, value) => !!call('control', id, cmd, value),
  close(id) { if (id) { untrack(id); call('close', id); listeners.delete(id); states.delete(id); } },
  view: (id, opts) => call('view', id, opts),
  info: (id) => call('info', id),
  // photos: id of a decoded image ("ready" event with { width, height, orientation, animated })
  image(app, path) { const id = call('image', app, path); if (id) track(id); return id; },
  // thumbnail in ~/.cache/lordos/thumbs (path) or null while it's being made (thumbRev arrives)
  thumb: (path, size = 256) => { try { return call('thumb', path, size); } catch (e) { return null; } },
  probe: (path) => job(() => call('probe', path)).then((r) => (r.ok ? r.result : null)),
  waveform: (path, bars = 200) => job(() => call('waveform', path, bars)).then((r) => (r.ok ? r.result : null)),
  // a video frame or the audio cover art, in RGBA pixels for <Image pixels> (Quick Preview)
  poster: (path, maxSize = 512) => job(() => call('poster', path, maxSize)).then((r) => (r.ok ? call('take', r.id) : null)),
  edit: (app, path, out, opts) => job(() => call('edit', app, path, out, opts)),
  transcode: (app, path, out, opts) => job(() => call('transcode', app, path, out, opts)),
  // screenshots (with protected windows in black)
  async screenshot(app, rect) {
    if (!(await requestPermission(app, 'screen'))) return null;
    return call('screenshot', app, rect || {});
  },
  crop: (id, x, y, w, hh) => call('crop', id, Math.round(x), Math.round(y), Math.round(w), Math.round(hh)),
  annotate: (base, ops, into) => call('annotate', base, ops, into || 0),
  saveImage: (id, path, opts = {}) => job(() => call('saveImage', id, path, opts)),
  copyImage: (id) => job(() => call('copyImage', id)),
  async screenRecord(app, path, opts = {}) {
    if (!(await requestPermission(app, 'screen'))) return null;
    if (opts.mic && !(await requestPermission(app, 'microphone'))) opts = { ...opts, mic: false };
    const id = call('screenRecord', app, path, opts);
    if (id) track(id);
    if (id && opts.mic) trackDevice(id, app, 'microphone');
    return id;
  },
  async audioRecord(app, path, opts = {}) {
    if (!(await requestPermission(app, 'microphone'))) return null;
    const id = call('audioRecord', app, path, opts);
    if (id) track(id);
    if (id) trackDevice(id, app, 'microphone');
    return id;
  },
  stop: (id) => !!call('stop', id),
  cameras: () => call('cameras') || [],
  async camera(app, device, opts = {}) {
    if (!(await requestPermission(app, 'camera'))) return null;
    const id = call('camera', app, device, opts);
    if (id) track(id);
    if (id) trackDevice(id, app, 'camera');
    return id;
  },
  photo: (id, path, mirror) => !!call('photo', id, path, !!mirror),
  async cameraRecord(id, app, path, opts = {}) {
    if (path && opts.mic !== false && !(await requestPermission(app, 'microphone'))) opts = { ...opts, mic: false };
    const ok = !!call('cameraRecord', id, path || null, opts);
    if (path && ok && opts.mic !== false) markDeviceUse(app, 'microphone', true);
    if (!path) markDeviceUse(app, 'microphone', false);
    return ok;
  },
  protectedCount: () => call('protectedCount') || 0,
};

// Camera/microphone indicators while the source lives.
const devices = new Map();            // id → { app, kind }
function trackDevice(id, app, kind) {
  markDeviceUse(app, kind, true);
  devices.set(id, { app, kind });
  const off = media.on(id, (type, d) => {
    if (type === 'closed' || type === 'done' || (type === 'state' && (d.state === 'done' || d.state === 'failed'))) { untrack(id); off(); }
  });
}
function untrack(id) {
  const d = devices.get(id);
  if (d) { devices.delete(id); markDeviceUse(d.app, d.kind, false); }
}

// State of a player (or camera, or recording) for React. It is built here even if no
// component is watching it, so an app that remounts finds it just the same.
const EMPTY = { info: null, pos: 0, duration: 0, playing: false, state: 'loading', subs: [], stats: null, error: null, level: 0, ms: 0, closed: false };
const states = new Map();             // id → state
function reduce(s, type, d) {
  switch (type) {
    case 'info': return { ...s, info: d, duration: d.duration || s.duration };
    case 'pos': return { ...s, pos: d.pos, duration: d.duration || s.duration, playing: d.playing };
    case 'state': return { ...s, state: d.state, playing: d.state === 'playing' };
    case 'sub': return { ...s, subs: [...s.subs.filter((x) => x.end > d.start - 60000).slice(-200), d] };
    case 'stats': return { ...s, stats: d };
    case 'level': return { ...s, level: d.level / 1000, ms: d.ms };
    case 'error': return { ...s, error: d.message };
    case 'ready': return { ...s, info: d, state: 'ready' };
    case 'closed': return { ...s, closed: true, error: d.crashed ? 'crash' : s.error };
    case 'done': return { ...s, state: 'done', done: d };
    case 'photo': case 'video': return { ...s, [type]: d };
    default: return s;
  }
}
function track(id) {
  if (states.has(id)) return;
  states.set(id, EMPTY);
  media.on(id, (type, d) => states.set(id, reduce(states.get(id) || EMPTY, type, d)));
}

export function usePlayer(id) {
  const [, force] = useState(0);
  if (id) track(id);
  useEffect(() => (id ? media.on(id, () => force((n) => n + 1)) : undefined), [id]);
  return (id && states.get(id)) || EMPTY;
}
export const useMedia = usePlayer;

// mm:ss o h:mm:ss
export function mediaFormatTime(ms) {
  const s = Math.max(0, Math.floor((ms || 0) / 1000));
  const hh = Math.floor(s / 3600), mm = Math.floor((s % 3600) / 60), ss = s % 60;
  const p = (n) => String(n).padStart(2, '0');
  return hh ? `${hh}:${p(mm)}:${p(ss)}` : `${mm}:${p(ss)}`;
}

// Thumbnail of a file (image, video or cover art): path or null; updates itself.
export const thumbFor = (path, size = 256) => (path ? media.thumb(path, size) : null);
export function useThumb(path, size = 256) {
  useStore(thumbRev);
  return thumbFor(path, size);
}

// User folders (xdg-user-dirs; if missing, the English defaults).
export function userDirs() {
  let conf = '';
  try { conf = fs.read('.config/user-dirs.dirs'); } catch (e) { /* no xdg-user-dirs */ }
  const home = fs.home();
  const get = (key, def) => {
    const m = new RegExp(`^XDG_${key}_DIR="\\$HOME/([^"]+)"`, 'm').exec(conf);
    return `${home}/${m ? m[1] : def}`;
  };
  return { home, pictures: get('PICTURES', 'Pictures'), music: get('MUSIC', 'Music'), videos: get('VIDEOS', 'Videos'), desktop: get('DESKTOP', 'Desktop'),
    documents: get('DOCUMENTS', 'Documents'), downloads: get('DOWNLOAD', 'Downloads') };
}

// ---- what's playing now (Control Center and media keys) ----
// The playing app publishes { app, window, title, artist, album, cover, playing, pos, duration,
// toggle(), next(), prev(), stop() }; null when nothing is playing.
export const nowPlaying = createStore(null);
export const useNowPlaying = () => useStore(nowPlaying);

// Media keys (evdev): they work even if the window is not in front (also with a Linux
// app focused: the compositor doesn't pass them on).
// What the media keys do (their entries are in sdk/shortcuts.js): nothing without something playing.
export const hasNowPlaying = () => !!nowPlaying.get();
export const mediaToggle = () => nowPlaying.get()?.toggle?.();
export const mediaNext = () => nowPlaying.get()?.next?.();
export const mediaPrev = () => nowPlaying.get()?.prev?.();
export const mediaStop = () => nowPlaying.get()?.stop?.();

function Btn({ glyph, onClick, size = 16, color }) {
  return h(View, { onClick, style: { width: 28, height: 28, borderRadius: 14, alignItems: 'center', justifyContent: 'center' } },
    h(Text, { style: { fontSize: size, color } }, glyph));
}

// Tile for a theme's Control Center: cover art, title, artist and controls.
export function NowPlayingTile({ style }) {
  const np = useNowPlaying();
  const th = useTheme();
  if (!np) return null;
  const pct = np.duration ? Math.min(1, np.pos / np.duration) : 0;
  return h(View, { style: [{ padding: 12, borderRadius: 12, backgroundColor: th.hover, border: `1px solid ${th.separator}`, gap: 8 }, style] },
    h(View, { style: { flexDirection: 'row', alignItems: 'center', gap: 10 } },
      h(View, { style: { width: 44, height: 44, borderRadius: 8, overflow: 'hidden', backgroundColor: th.control, alignItems: 'center', justifyContent: 'center' } },
        np.cover ? h(Image, { src: np.cover, style: { width: 44, height: 44, objectFit: 'cover' } }) : h(Text, { style: { fontSize: 20 } }, '♪')),
      h(View, { style: { flex: 1, gap: 2, overflow: 'hidden' } },
        h(Text, { style: { fontSize: 12, fontWeight: 600, color: th.text, whiteSpace: 'nowrap' } }, np.title || t('media.nowPlaying.unknown')),
        h(Text, { style: { fontSize: 11, color: th.textDim, whiteSpace: 'nowrap' } }, np.artist || t('media.nowPlaying.title'))),
      h(View, { style: { flexDirection: 'row', alignItems: 'center', direction: 'ltr' } },
        h(Btn, { glyph: '⏮', onClick: () => np.prev?.(), color: th.text }),
        h(Btn, { glyph: np.playing ? '⏸' : '▶', size: 18, onClick: () => np.toggle?.(), color: th.text }),
        h(Btn, { glyph: '⏭', onClick: () => np.next?.(), color: th.text }))),
    h(View, { style: { height: 3, borderRadius: 2, backgroundColor: th.control, direction: 'ltr' } },
      h(View, { style: { width: `${Math.round(pct * 100)}%`, height: 3, borderRadius: 2, backgroundColor: th.accent } })));
}

export { MediaView };
