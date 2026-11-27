// motion.jsx — moving wallpapers (docs/WALLPAPERS.md). The still wallpaper stays drawn under the video, so a failure,
// a missing file or a machine without the hardware just shows it. Decisions are in motionmodel.js.
//
// One video per file, whatever the number of displays showing it. Files live in /var/lib/lordos/wallpapers, fetched by
// the closed action wallpaper-get from the catalog in the package (lord-server only keeps a file with the catalog's hash).
import { createElement as h, useEffect, useRef, useState } from 'react';
import { Image, MediaView, addListener } from '@lordos/runtime';
import catalog from '../assets/wallpapers/motion.json';
import { media } from './media.js';
import { settings, useSettings } from './settings.js';
import { createStore, useStore } from './store.js';
import { admin } from './admin.js';
import { battery } from './hardware.jsx';
import { userWallpaperName, refreshStillFiles, stillFiles, stillStore } from './wallpapers.js';
import { motionCacheMb, motionCodecs, motionFpsCap, nextRandom, pauseReason, pickVariant, retryDelay, openFailure } from './motionmodel.js';
import { notify } from './notify.js';
import { t, tOptional } from './i18n.js';
import { errorText } from './errors.js';
import { motionCatalog, onDisk } from './wallpapergallery.js';

const STORE = '/var/lib/lordos/wallpapers';
export const MOTION_RATES = [100, 75, 50, 25];
export const MOTION_SECTIONS = ['dynamic', 'landscape', 'urban', 'underwater', 'earth'];
// The catalog's moving wallpapers: its still pictures (variants with codec jpeg, since 0.8.9.6.2) are Settings →
// Wallpaper's gallery's (sdk/wallpapers.js), never played here.
export const motionItems = motionCatalog(catalog.items);
export const motionThumb = (it) => `asset:wallpapers/motion/${it.id}.thumb.jpg`;
export const motionPoster = (it) => `asset:wallpapers/motion/${it.id}.poster.jpg`;
export const motionDownloads = createStore({});     // catalog id → percent while downloading (a video or a still picture)

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
// a display's own entry wins, even null (a still picture chosen for that display: sdk/wallpapers.js wallpaperPatch)
const wanted = (s, display) => { const m = s.motion || {}; return display in m ? m[display] || null : m['*'] || null; };

// One file of the catalog, by its item and variant name: root keeps it in the store (a video as .mp4, a picture as .jpg,
// from the catalog's codec) after checking its size and SHA-256. The progress goes to motionDownloads.
export function fetchCatalogFile(it, name) {
  motionDownloads.set((d) => ({ ...d, [it.id]: 0 }));
  const done = () => { motionDownloads.set((d) => { const x = { ...d }; delete x[it.id]; return x; }); refreshStillFiles(); motionFileChanged(); };
  return admin.run('wallpaper-get', `${it.id} ${name}`, (line) => {
    const m = /^PROGRESS (\d+)/.exec(line);
    if (m) motionDownloads.set((d) => ({ ...d, [it.id]: Number(m[1]) }));
  }, { quiet: true }).then(() => { done(); }, (e) => { done(); throw e; });
}
// Deletes one (administrators only: the store is every user's).
export const removeCatalogFile = (it, name) => admin.run('wallpaper-remove', `${it.id} ${name}`, null, { quiet: true }).finally(refreshStillFiles);

export function downloadMotion(it, variant) {
  return fetchCatalogFile(it, variant.name).then(() => {
    settings.set({ motionFiles: { ...(settings.get().motionFiles || {}), [it.id]: variant.name } });
  });
}

export async function removeMotion(it) {
  const s = settings.get();
  const name = (s.motionFiles || {})[it.id];
  if (name) await removeCatalogFile(it, name);
  const files = { ...(s.motionFiles || {}) };
  delete files[it.id];
  const motion = Object.fromEntries(Object.entries(s.motion || {}).filter(([, v]) => v !== it.id));
  settings.set({ motionFiles: files, motion });
}

// A picture of the user's own: the action leaves a copy in their home and answers its id. The login screen can't read a
// home, so choosing one for it stores a copy of its own as root.
export const addWallpaperImage = (path) => admin.run('wallpaper-image', path, null, { quiet: true }).then((name) => `image:${name}`);
export const removeWallpaperImage = (id) => admin.run('wallpaper-image', `remove ${userWallpaperName(id)}`, null, { quiet: true });
export const setLoginImage = (id) => admin.run('wallpaper-login-image', userWallpaperName(id), null, { quiet: true });
export const setLoginStill = (id) => admin.run('wallpaper-login', `still ${id}`, null, { quiet: true });
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
  const it = motionItems.find((x) => x.id === id);
  if (!it || !on[id]) return null;
  const v = it.variants.find((x) => x.name === on[id]);
  // only a format this machine decodes in hardware, however it was downloaded, and only the catalog's file: one of
  // another size under its name (ws170 QA BUG-13) is not played (its still poster shows; Settings offers to fetch it)
  return v && codecs.includes(v.codec) && onDisk(v, stillFiles()) && !broken.has(pathOf(v)) ? pathOf(v) : null;
}
let lastRandom = null, randomPick = null;
function randomFor(ids) {
  if (!randomPick || !ids.includes(randomPick)) { randomPick = nextRandom(ids, lastRandom); lastRandom = randomPick; }
  return randomPick;
}

// ---- one player per file, and the reasons to pause ----

const players = new Map();                // path → { id, refs, views, paused }
let lastInput = Date.now(), cpu = null, busy = 0, ticking = 0;

// What JS alone decides (Reduce motion, idle, battery, a busy CPU) and the settings lord-server applies: which display's
// view shows is lord-server's (media/mambient.c), per display, so a covered or fullscreen monitor pauses only its own
// copy and the player pauses when no copy shows (docs/MOVING-WALLPAPERS.md).
const ambientOptions = (s, bat) => ({
  fpsCap: motionFpsCap({ cap: s.motionFpsCap, batteryMode: s.motionBattery, battery: bat }),
  cacheMb: motionCacheMb(s.motionCacheMb),
});

function evaluate() {
  const s = settings.get();
  const bat = battery.get();
  let b = false;
  try {
    const [t, i] = lord.system.cpuTimes();
    if (cpu) { const dt = t - cpu[0]; const load = dt > 0 ? 1 - (i - cpu[1]) / dt : 0; busy = load > 0.85 ? busy + 1 : 0; b = busy >= 2; }
    cpu = [t, i];
  } catch (e) { /* no reading: not busy */ }
  const opts = ambientOptions(s, bat);
  for (const p of players.values()) {
    const why = pauseReason({ reduceMotion: s.reduceMotion, idleSec: (Date.now() - lastInput) / 1000, battery: bat,
      batteryMode: s.motionBattery || 'pause', busy: b });
    if (!!why !== p.paused) { p.paused = !!why; media.control(p.id, why ? 'pause' : 'play'); }
    if (opts.fpsCap !== p.fpsCap) { p.fpsCap = opts.fpsCap; media.control(p.id, 'fpsCap', opts.fpsCap); }
    if (opts.cacheMb !== p.cacheMb) { p.cacheMb = opts.cacheMb; media.control(p.id, 'cacheMb', opts.cacheMb); }
  }
}

function startTicking() {
  if (ticking) return;
  ticking = setInterval(evaluate, 5000);
  settings.subscribe(() => { if (players.size) evaluate(); });
  // the power source changes: applied at once (On battery: pause, lower the frame rate or keep playing)
  battery.subscribe(() => { if (players.size) evaluate(); });
  const seen = () => { const was = Date.now() - lastInput > 300000; lastInput = Date.now(); if (was) evaluate(); };
  addListener('keydown', seen);
  addListener('pointerdown', seen);
  addListener('pointermove', seen);
}

// A new player can start while the last one still holds the decoder, and a moving wallpaper never falls back to software:
// its helper stops. So a stop that nobody asked for is tried again, a few times, before the still image stays.
export const motionBusy = createStore(false);       // a chosen wallpaper is starting
// Files lord-media could not open, given up for this session (motionmodel.js openFailure): their still poster shows,
// and a notice says so once. A new download of the file (Settings) clears it.
const broken = new Set();
const brokenRev = createStore(0);
let openFails = {};
function giveUp(file) {
  if (broken.has(file)) return;
  broken.add(file);
  brokenRev.set((n) => n + 1);
  const it = motionItems.find((x) => x.variants.some((v) => pathOf(v) === file));
  const name = it ? tOptional(`settings.motion.item.${it.id}`) || it.name || it.id : file.split('/').pop();
  try { notify({ app: 'settings', title: t('settings.motion.title'), body: `${name}: ${errorText('media.cannotOpen')}` }); } catch (e) { /* no notifications */ }
}
// A download finished: whatever it replaced may play again.
export function motionFileChanged() { if (broken.size || Object.keys(openFails).length) { broken.clear(); openFails = {}; brokenRev.set((n) => n + 1); } }

function acquire(file, rate, key, display, onLost) {
  const tag = `${file}|${rate}`;
  let p = players.get(tag);
  if (!p) {
    let id = 0;
    motionBusy.set(true);
    const opts = ambientOptions(settings.get(), battery.get());
    // ambient: lord-server pauses it where no display shows it, stops it after a while unseen and keeps its loop on the GPU
    try { id = media.play('wallpaper', file, { loop: true, audio: 0, hwOnly: 1, rate, appName: 'LordOS', ambient: 1, ...opts }); } catch (e) { motionBusy.set(false); return null; }
    if (!id) { motionBusy.set(false); return null; }
    p = { id, tag, views: new Map(), paused: false, playing: createStore(false), ...opts };
    media.on(id, (type, d) => {
      if (type === 'state' && d.state === 'playing') {
        p.playing.set(true);
        motionBusy.set(false);
        if (openFails[file]) { openFails = { ...openFails }; delete openFails[file]; }
      } else if (type === 'error' || type === 'closed') {
        if (type === 'error') {
          const r = openFailure(openFails, file, d && d.message);
          openFails = r.counts;
          if (r.broken) giveUp(file);
        }
        p.playing.set(false);
        if (players.get(tag) === p) { players.delete(tag); if (broken.has(file)) motionBusy.set(false); else p.onLost?.(); }
      }
    });
    players.set(tag, p);
    startTicking();
  }
  p.onLost = onLost;
  p.views.set(key, display);
  return p;
}
function release(p, key) {
  p.views.delete(key);
  if (!p.views.size && players.get(p.tag) === p) { players.delete(p.tag); media.close(p.id); }
}

function Layer({ path, rate, display, style }) {
  const [p, setP] = useState(null);
  const [turn, setTurn] = useState(0);
  const tries = useRef(0);
  useEffect(() => { tries.current = 0; }, [path, rate]);
  useEffect(() => {
    const key = {};
    let timer = 0;
    const lost = () => {
      const wait = retryDelay(tries.current++);
      if (wait >= 0) timer = setTimeout(() => setTurn((n) => n + 1), wait);
      else motionBusy.set(false);
    };
    const mine = acquire(path, rate, key, display, lost);
    setP(mine);
    return () => { clearTimeout(timer); setP(null); if (mine) release(mine, key); };
  }, [path, rate, turn, display.x, display.y, display.w, display.h]);
  const on = useStore(p ? p.playing : idle);
  return on && p ? h(MediaView, { media: p.id, style }) : null;
}
const idle = createStore(false);

// Over a display's still wallpaper. Nothing at all when there's no hardware or nothing chosen.
export function MotionWallpaper({ display }) {
  const [s] = useSettings();
  const codecs = useCodecs();
  useStore(stillStore);          // the store's listing changed (a download, a removal)
  useStore(brokenRev);           // a file given up, or fetched again
  const px = (display.scale || 100) / 100;
  const path = codecs.length ? resolve(s, wanted(s, display.id), display.w * px, display.h * px, codecs) : null;
  const chosen = wanted(s, display.id);
  const it = motionItems.find((x) => x.id === chosen);
  if (!path) {
    // chosen, but this machine can't play it, or its file isn't the catalog's or can't be opened: its still poster
    return it && (!codecs.length || (s.motionFiles || {})[it.id]) ? h(Image, { src: motionPoster(it), style: { position: 'absolute', left: display.x, top: display.y, width: display.w, height: display.h, objectFit: 'cover', pointerEvents: 'none' } }) : null;
  }
  // pointerEvents none: the picture is drawn over the still wallpaper, whose node takes the desktop's clicks (its right-click
  // menu, a drop, a click that ends a selection); before 0.8.9.6.2 the video took them and the desktop's menu never opened
  return h(Layer, { path, rate: MOTION_RATES.includes(s.motionRate) ? s.motionRate : 100, display, style: { position: 'absolute', left: display.x, top: display.y, width: display.w, height: display.h, pointerEvents: 'none' } });
}

export const variantFor = (it, codecs, display) => pickVariant(it, codecs, display.w, display.h);
