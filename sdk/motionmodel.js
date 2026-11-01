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
export function pauseReason({ screenOff, reduceMotion, idleSec, idleMax = 300, battery, covered, fullscreen, busy }) {
  if (reduceMotion) return 'reduceMotion';
  if (screenOff) return 'screenOff';
  if (fullscreen) return 'fullscreen';
  if (covered) return 'covered';
  if (idleSec >= idleMax) return 'idle';
  if (battery && battery.discharging && battery.percent <= 20) return 'battery';
  if (busy) return 'busy';
  return null;
}

// Is this display's desktop fully hidden by a window? rects: the windows on it, not minimized.
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
