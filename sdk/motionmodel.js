// motionmodel.js — the decisions behind moving wallpapers, with no screen (docs/WALLPAPERS.md; tests:
// server/tests/motion_model_test.mjs). Hardware only: a moving wallpaper needs the GPU composing the session and
// hardware decoding of the file's format. There is no software mode: anything else shows the still poster.

// Which formats this machine can play. hw is lord.media.hwDecode(); composition lord.media.composition().
export function motionCodecs(hw, composition) {
  if (composition !== 'gpu' || !hw || hw.virtualGpu) return [];
  const out = [];
  if (hw.hevc) out.push('hevc');
  if (hw.h264) out.push('h264');
  return out;
}

// The variant to play on a screen: HEVC before H.264, then the smallest that covers the screen. Screens above the
// file's size use its largest. A format this machine doesn't decode in hardware is never chosen.
export function pickVariant(item, codecs, width, height) {
  for (const c of codecs) {
    const list = (item.variants || []).filter((v) => v.codec === c).sort((a, b) => a.w - b.w);
    if (!list.length) continue;
    return list.find((v) => v.w >= width && v.h >= height) || list[list.length - 1];
  }
  return null;
}

// Why the video should be paused now, or null. Each is a reason not to spend power on something nobody sees.
// What covers which display is lord-server's own business (media/mambient.c): every display's view is looked at on its
// own (opaque windows over its whole area, a fullscreen app on it, the screen off), so a game in full screen on one
// monitor pauses only that monitor's copy, and the player pauses only when no view shows. JS keeps the reasons it
// alone knows. batteryMode: Settings' "On battery" (MOTION_BATTERY): 'pause' (the default) pauses on battery, 'lower'
// caps the frame rate (motionFpsCap) and pauses at 20 % or less, 'keep' never pauses for the battery.
export const MOTION_BATTERY = ['pause', 'lower', 'keep'];
export const MOTION_FPS_CAPS = [0, 30, 15];
export const MOTION_CACHE_MB = [0, 256, 512, 1024, 2048];
export const onBattery = (b) => !!(b && b.discharging);

export function pauseReason({ reduceMotion, idleSec, idleMax = 300, battery, batteryMode = 'pause', busy }) {
  if (reduceMotion) return 'reduceMotion';
  if (idleSec >= idleMax) return 'idle';
  if (onBattery(battery) && batteryMode !== 'keep' && (batteryMode !== 'lower' || battery.percent <= 20)) return 'battery';
  if (busy) return 'busy';
  return null;
}

// The frame-rate cap to ask (0 the video's own): Settings' choice, and 15 on battery in 'lower' mode. lord-server's
// mloop_cap applies the same rule (server/tests/mloop_test.c).
export function motionFpsCap({ cap, batteryMode = 'pause', battery }) {
  let c = cap === 30 || cap === 15 ? cap : 0;
  if (batteryMode === 'lower' && onBattery(battery) && (c === 0 || c > 15)) c = 15;
  return c;
}

// Settings' motionCacheMb: one of MOTION_CACHE_MB (1 GB when never set)
export const motionCacheMb = (v) => (MOTION_CACHE_MB.includes(v) ? v : 1024);

// Is this display's desktop fully hidden by a window? rects: the windows on it, not minimized (the slideshow's rule;
// moving wallpapers ask lord-server instead, per view).
export function displayCovered(display, rects) {
  return rects.some((r) => r.x <= display.x && r.y <= display.y && r.x + r.w >= display.x + display.w && r.y + r.h >= display.y + display.h);
}

// An added video: a duration up to 10 s in a format this machine plays in hardware.
export const MAX_SECONDS = 10;
export function userVideoProblem(info, codecs) {
  if (!info || !info.duration) return 'unreadable';
  if (info.duration > MAX_SECONDS + 0.05) return 'tooLong';
  const c = String(info.codec || '').toLowerCase();
  const fmt = c === 'hevc' || c === 'h265' ? 'hevc' : c === 'h264' ? 'h264' : '';
  return codecs.includes(fmt) ? null : 'format';
}

// Random: a different one from the last, among those on disk.
export function nextRandom(ids, last, pick = Math.random) {
  const list = ids.filter((i) => i !== last);
  if (!list.length) return ids[0] || null;
  return list[Math.floor(pick() * list.length) % list.length];
}

// A player that stops by itself (its helper ended, the decoder still held by the last one) is started again after a
// growing pause: 0.5 s, 1 s, 2 s, 4 s, then no more (-1) and the still picture stays.
export const MOTION_RETRIES = 4;
export const retryDelay = (tries, base = 500) => (tries >= 0 && tries < MOTION_RETRIES ? base * 2 ** tries : -1);
// A file lord-media can't open (media.cannotOpen: damaged, not a video) is given up after OPEN_FAILS failures in a row,
// for every display at once and until it changes (a new download) or the session ends: it was started again every
// 0.6 s for good, a new helper each time (ws170 QA BUG-13). A play that reached "playing" clears the count.
export const OPEN_FAILS = 3;
export function openFailure(counts, path, message) {
  if (message !== 'media.cannotOpen') return { counts, broken: false };
  const n = (counts[path] || 0) + 1;
  return { counts: { ...counts, [path]: n }, broken: n >= OPEN_FAILS };
}
