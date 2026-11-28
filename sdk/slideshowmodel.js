// slideshowmodel.js — the wallpaper slideshow's decisions, as plain functions with no imports (0.8.9; tests:
// server/tests/slideshow_model_test.mjs). The SDK draws it (sdk/slideshow.jsx); a theme turns it on by declaring
// "wallpaper": { "slideshow": true } in its theme.json (sdk/thememeta.js). Workspaces is the only one today.
//
// settings.slideshow = { on, category, every, perDisplay }:
//   on          off by default
//   category    a wallpaper category (sdk/wallpapers.js), its pictures in turn
//   every       seconds between changes, one of SLIDESHOW_EVERY
//   perDisplay  several displays: each its own picture and its own shuffled order, changes staggered

export const SLIDESHOW_EVERY = [60, 300, 900, 1800, 3600];
export const SLIDESHOW_DEFAULTS = { on: false, category: 'cyberpunk', every: 60, perDisplay: false };
// The next picture is decoded this long before its change (sdk/slideshow.jsx; off the interface's thread), at most
// half the interval.
export const PRELOAD_MS = 8000;

const CATEGORY = /^[a-z][a-z0-9-]{0,31}$/;

// settings.slideshow, checked: anything unknown is its default
export function slideshowPrefs(s) {
  const p = s && s.slideshow && typeof s.slideshow === 'object' && !Array.isArray(s.slideshow) ? s.slideshow : {};
  return {
    on: p.on === true,
    category: typeof p.category === 'string' && CATEGORY.test(p.category) ? p.category : SLIDESHOW_DEFAULTS.category,
    every: SLIDESHOW_EVERY.includes(p.every) ? p.every : SLIDESHOW_DEFAULTS.every,
    perDisplay: p.perDisplay === true,
  };
}

// The settings patch for a change. Turning it on also turns the moving wallpapers off: a video would hide it.
export function slideshowPatch(s, change) {
  const next = { ...slideshowPrefs(s), ...change };
  return { slideshow: next, ...(change.on === true ? { motion: {} } : null) };
}
// Choosing a still picture or a video (Settings, the Control Center) ends the slideshow; nothing when it is off.
export const slideshowStop = (s) => (slideshowPrefs(s).on ? { slideshow: { ...slideshowPrefs(s), on: false } } : {});

// The categories it can go through: two pictures or more, in the order given (WALLPAPER_CATEGORIES first).
export function slideshowCategories(list, order = []) {
  const count = {};
  for (const w of list || []) if (w && w.category && w.category !== 'mine') count[w.category] = (count[w.category] || 0) + 1;
  const cats = Object.keys(count).filter((c) => count[c] >= 2);
  const at = (c) => (order.includes(c) ? order.indexOf(c) : order.length);
  return cats.sort((a, b) => at(a) - at(b) || (a < b ? -1 : a > b ? 1 : 0));
}
// The pictures of a category, by id; another category with pictures when this one has fewer than two.
export function slideshowIds(list, category) {
  const ids = (list || []).filter((w) => w && w.category === category).map((w) => w.id);
  if (ids.length >= 2) return ids;
  const other = slideshowCategories(list)[0];
  return other ? (list || []).filter((w) => w.category === other).map((w) => w.id) : ids;
}

// ---- the order: a shuffled deck per display ----

// Fisher–Yates with rand() in [0, 1); `notFirst` (the picture on screen) never comes first, so a new round never
// repeats it.
export function shuffled(ids, rand = Math.random, notFirst = null) {
  const a = [...ids];
  for (let i = a.length - 1; i > 0; i--) {
    const j = Math.min(i, Math.floor(rand() * (i + 1)));
    [a[i], a[j]] = [a[j], a[i]];
  }
  if (a.length > 1 && a[0] === notFirst) [a[0], a[1]] = [a[1], a[0]];
  return a;
}

// deck = { order, pos }. The next picture: the deck's next one that isn't `busy` (on another display, or arriving
// there), the deck shuffled again when it runs out. With fewer pictures than displays a busy one is taken all the
// same (the deck's next). Returns { id, deck }.
export function drawNext(deck, ids, busy = [], rand = Math.random, current = null) {
  if (!ids.length) return { id: null, deck: { order: [], pos: 0 } };
  let d = deck && Array.isArray(deck.order) && deck.order.length === ids.length && deck.order.every((x) => ids.includes(x))
    ? deck : { order: shuffled(ids, rand, current), pos: 0 };
  const taken = new Set(busy);
  if (current) taken.add(current);
  for (let tries = 0; tries < 2; tries++) {
    for (let i = d.pos; i < d.order.length; i++) {
      const id = d.order[i];
      if (taken.has(id)) continue;
      // the skipped ones stay for later in this round: the one taken moves to the deck's position
      const order = [...d.order];
      order.splice(i, 1);
      order.splice(d.pos, 0, id);
      return { id, deck: { order, pos: d.pos + 1 } };
    }
    d = { order: shuffled(ids, rand, current), pos: 0 };
  }
  // every picture is busy: the next that isn't the one on screen, or the deck's next
  const id = d.order.find((x) => x !== current) || d.order[0];
  return { id, deck: { order: d.order, pos: Math.min(d.order.length, d.order.indexOf(id) + 1) } };
}

// ---- when ----

// Display i of n changes this long after display 0 (perDisplay): spread over the interval, so two displays never
// cross-fade in the same frames.
export const staggerMs = (i, n, everyMs) => (n > 1 && i > 0 ? Math.round((i % n) * everyMs / n) : 0);

// The next change of a display at or after `now`: start + offset + k·every.
export function nextDue(now, start, everyMs, offset = 0) {
  const t0 = start + offset;
  if (now <= t0) return t0;
  return t0 + Math.ceil((now - t0) / everyMs) * everyMs;
}
// When the next picture starts decoding, before its change.
export const preloadAt = (due, everyMs) => due - Math.min(PRELOAD_MS, Math.floor(everyMs / 2));

// ---- how ----

// The transition to the next picture. Like a film's cross-dissolve: the next picture fades in over the one on screen
// while it drifts slowly into its place (a Ken Burns pan), easing in and out. Only the arriving picture moves, and only
// during the transition: at rest nothing is animated. With Reduce motion it is a short plain crossfade.
//   { kind: 'drift' | 'fade', ms, from: { x, y } }    x, y: fractions of the display's size, |x|, |y| ≤ MARGIN
// Every picture is drawn MARGIN larger than its display on each side (sdk/slideshow.jsx), so the pan never shows an
// edge. A pan, not a zoom: a translation keeps the picture a 1:1 copy of its decoded pixels (lord-server blits it),
// where a zoom composites a scaled raster every frame: 53 ms a frame against 10 on a 1080p display on the CPU
// (build/engine-test bench-slideshow, docs/WORKSPACES.md).
export const DRIFT_MS = 2400;
export const FADE_MS = 400;
export const MARGIN = 0.02;
export function transitionFor({ reduceMotion } = {}, rand = Math.random) {
  if (reduceMotion) return { kind: 'fade', ms: FADE_MS, from: { x: 0, y: 0 } };
  // from one of eight directions, the whole margin on a side (a diagonal: both)
  const a = Math.floor(rand() * 8) * Math.PI / 4;
  const round = (v) => Math.round(v * 10000) / 10000 || 0;
  const c = Math.cos(a), sn = Math.sin(a), k = MARGIN / Math.max(Math.abs(c), Math.abs(sn));
  return { kind: 'drift', ms: DRIFT_MS, from: { x: round(c * k), y: round(sn * k) } };
}
// ease in and out (cubic)
export const easeInOut = (p) => (p < 0.5 ? 4 * p * p * p : 1 - (-2 * p + 2) ** 3 / 2);
// The arriving picture's style at progress p (0…1) of a transition.
export function transitionFrame(tr, p) {
  const e = easeInOut(Math.min(1, Math.max(0, p)));
  const r4 = (v) => Math.round(v * 10000) / 10000 || 0;
  return { opacity: r4(e), x: r4(tr.from.x * (1 - e)), y: r4(tr.from.y * (1 - e)) };
}

// ---- whether ----

// Why no change should happen now, or null: each is a reason not to spend power on a picture nobody sees.
//   seen          lord.system.seen(): the displays on and the session not locked
//   powerSaver    the power profile is Power Saver, or the battery is discharging at 20 % or less
//   covered       every display that shows it is hidden by a window, or a window is full screen
export function slideshowPause({ seen = true, powerProfile = null, battery = null, covered = false, fullscreen = false } = {}) {
  if (!seen) return 'unseen';
  if (powerProfile === 'power-saver' || (battery && battery.discharging && battery.percent <= 20)) return 'powerSaver';
  if (fullscreen) return 'fullscreen';
  if (covered) return 'covered';
  return null;
}
