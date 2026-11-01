// motion.jsx — moving wallpapers (docs/WALLPAPERS.md). The still wallpaper stays drawn under the video, so a failure,
// a missing file or a machine without the hardware just shows it. Decisions are in motionmodel.js.
//
// One video per file, whatever the number of displays showing it. Files live in /var/lib/lordos/wallpapers, fetched by
// the closed action wallpaper-get from the catalog in the package (lord-server only keeps a file with the catalog's hash).
import { createElement as h, useEffect, useState } from 'react';
import { Image, MediaView, addListener } from '@lordos/runtime';
import catalog from '../assets/wallpapers/motion.json';
import { media } from './media.js';
import { settings, useSettings } from './settings.js';
import { createStore, useStore } from './store.js';
import { admin } from './admin.js';
import { windows } from './windows.js';
import { battery } from './hardware.jsx';
import { displayCovered, motionCodecs, nextRandom, pauseReason, pickVariant } from './motionmodel.js';

const STORE = '/var/lib/lordos/wallpapers';
export const MOTION_SECTIONS = ['dynamic', 'landscape', 'urban', 'underwater', 'earth'];
export const motionItems = catalog.items;
export const motionThumb = (it) => `asset:wallpapers/motion/${it.id}.thumb.jpg`;
export const motionPoster = (it) => `asset:wallpapers/motion/${it.id}.poster.jpg`;
export const motionDownloads = createStore({});     // id → percent while downloading

// What this machine can play: empty until lord-media answers, and empty for good without the hardware.
export function currentCodecs() {
  try { return motionCodecs(lord.media.hwDecode(), lord.media.composition()); } catch (e) { return []; }
}
export function useCodecs() {
  const [c, set] = useState(currentCodecs);
  useEffect(() => {
    let n = 0;
    const id = setInterval(() => { const x = currentCodecs(); if (x.length || ++n > 40) { set(x); clearInterval(id); } }, 250);
    return () => clearInterval(id);
  }, []);
  return c;
}

const pathOf = (v) => `${STORE}/${v.sha256}.mp4`;
const wanted = (s, display) => (s.motion || {})[display] || (s.motion || {})['*'] || null;

export function downloadMotion(it, variant) {
  const arg = `${it.id} ${variant.name}`;
  motionDownloads.set((d) => ({ ...d, [it.id]: 0 }));
  const done = () => motionDownloads.set((d) => { const x = { ...d }; delete x[it.id]; return x; });
  return admin.run('wallpaper-get', arg, (line) => {
    const m = /^PROGRESS (\d+)/.exec(line);
    if (m) motionDownloads.set((d) => ({ ...d, [it.id]: Number(m[1]) }));
  }, { quiet: true }).then(() => {
    done();
    settings.set({ motionFiles: { ...(settings.get().motionFiles || {}), [it.id]: variant.name } });
  }, (e) => { done(); throw e; });
}

export async function removeMotion(it) {
  const s = settings.get();
  const name = (s.motionFiles || {})[it.id];
  if (name) await admin.run('wallpaper-remove', `${it.id} ${name}`, null, { quiet: true });
  const files = { ...(s.motionFiles || {}) };
  delete files[it.id];
  const motion = Object.fromEntries(Object.entries(s.motion || {}).filter(([, v]) => v !== it.id));
  settings.set({ motionFiles: files, motion });
}

export const setLoginMotion = (it) => admin.run('wallpaper-login', it ? `${it.id} ${settings.get().motionFiles?.[it.id]}` : 'default', null, { quiet: true });

// The file behind a chosen id, or null.
function resolve(s, choice, w, h_, codecs) {
  if (!choice) return null;
  if (String(choice).startsWith('user:')) {
    const u = (s.motionUser || []).find((x) => x.id === choice);
    return u ? u.path : null;
  }
  const on = (s.motionFiles || {});
  let id = choice;
  if (id === 'random') id = randomFor(Object.keys(on));
  const it = catalog.items.find((x) => x.id === id);
  if (!it || !on[id]) return null;
  const v = it.variants.find((x) => x.name === on[id]);
  // only a format this machine decodes in hardware, however it was downloaded
  return v && codecs.includes(v.codec) ? pathOf(v) : null;
}
let lastRandom = null, randomPick = null;
function randomFor(ids) {
  if (!randomPick || !ids.includes(randomPick)) { randomPick = nextRandom(ids, lastRandom); lastRandom = randomPick; }
  return randomPick;
}

// ---- one player per file, and the reasons to pause ----

const players = new Map();                // path → { id, refs, views, paused }
let lastInput = Date.now(), cpu = null, busy = 0, ticking = 0;

function evaluate() {
  const s = settings.get();
  const ws = windows.get();
  const live = ws.filter((w) => !w.minimized);
  let b = false;
  try {
    const [t, i] = lord.system.cpuTimes();
    if (cpu) { const dt = t - cpu[0]; const load = dt > 0 ? 1 - (i - cpu[1]) / dt : 0; busy = load > 0.85 ? busy + 1 : 0; b = busy >= 2; }
    cpu = [t, i];
  } catch (e) { /* no reading: not busy */ }
  for (const p of players.values()) {
    const rects = live.map((w) => ({ x: w.x, y: w.y, w: w.w, h: w.h }));
    const covered = p.views.size > 0 && [...p.views.values()].every((d) => displayCovered(d, rects));
    const why = pauseReason({ reduceMotion: s.reduceMotion, idleSec: (Date.now() - lastInput) / 1000, battery: battery.get(),
      covered, fullscreen: live.some((w) => w.fullscreen), busy: b });
    if (!!why !== p.paused) { p.paused = !!why; media.control(p.id, why ? 'pause' : 'play'); }
  }
}

function startTicking() {
  if (ticking) return;
  ticking = setInterval(evaluate, 5000);
  windows.subscribe(() => { if (players.size) evaluate(); });
  settings.subscribe(() => { if (players.size) evaluate(); });
  const seen = () => { const was = Date.now() - lastInput > 300000; lastInput = Date.now(); if (was) evaluate(); };
  addListener('keydown', seen);
  addListener('pointerdown', seen);
  addListener('pointermove', seen);
}

function acquire(path, key, display) {
  let p = players.get(path);
  if (!p) {
    let id = 0;
    try { id = media.play('wallpaper', path, { loop: true, audio: 0, hwOnly: 1, appName: 'LordOS' }); } catch (e) { return null; }
    if (!id) return null;
    p = { id, views: new Map(), paused: false, playing: createStore(false) };
    media.on(id, (type, d) => {
      if (type === 'state' && d.state === 'playing') p.playing.set(true);
      else if (type === 'error' || type === 'closed') { p.playing.set(false); players.delete(path); }
    });
    players.set(path, p);
    startTicking();
  }
  p.views.set(key, display);
  return p;
}
function release(path, key) {
  const p = players.get(path);
  if (!p) return;
  p.views.delete(key);
  if (!p.views.size) { players.delete(path); media.close(p.id); }
}

function Layer({ path, display, style }) {
  const [p, setP] = useState(null);
  useEffect(() => {
    const key = {};
    setP(acquire(path, key, display));
    return () => { setP(null); release(path, key); };
  }, [path, display.x, display.y, display.w, display.h]);
  const on = useStore(p ? p.playing : idle);
  return on && p ? h(MediaView, { media: p.id, style }) : null;
}
const idle = createStore(false);

// Over a display's still wallpaper. Nothing at all when there's no hardware or nothing chosen.
export function MotionWallpaper({ display }) {
  const [s] = useSettings();
  const codecs = useCodecs();
  const px = (display.scale || 100) / 100;
  const path = codecs.length ? resolve(s, wanted(s, display.id), display.w * px, display.h * px, codecs) : null;
  const chosen = wanted(s, display.id);
  const it = catalog.items.find((x) => x.id === chosen);
  if (!path) {
    // chosen, but this machine can't play it: its still poster
    return it && !codecs.length ? h(Image, { src: motionPoster(it), style: { position: 'absolute', left: display.x, top: display.y, width: display.w, height: display.h, objectFit: 'cover' } }) : null;
  }
  return h(Layer, { path, display, style: { position: 'absolute', left: display.x, top: display.y, width: display.w, height: display.h } });
}

export const variantFor = (it, codecs, display) => pickVariant(it, codecs, display.w, display.h);
