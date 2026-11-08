// voiceplayermodel.js — where the reading-aloud player goes and how its wave moves (sdk/voiceplayer.js). Pure; tested
// by server/tests/voiceplayer_model_test.mjs.
//
//   placePlayer(displays, saved, near, size)   → { x, y } in desktop coordinates
//       saved: { display, dx, dy } where the person left it (the display's name, the offset inside it), or null
//       near: the display of the window that reads (else the main one); size: { w, h }
//   rememberPlace(displays, x, y, size)        → { display, dx, dy } to save after a drag
//   smoothBands(prev, next, dt)                → the bars' heights 0..1: they rise at once and fall slowly
//   PLAYER_BARS                                how many bars the wave has (one per band lord-say measures)

export const PLAYER_BARS = 16;
const MARGIN = 8;
const BOTTOM = 0.13;          // above the Dock, as the volume indicator

const mainOf = (displays) => displays.find((d) => d.main) || displays[0] || { x: 0, y: 0, w: 1280, h: 800 };
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

function inside(d, x, y, size) {
  return { x: clamp(Math.round(x), d.x + MARGIN, d.x + d.w - size.w - MARGIN), y: clamp(Math.round(y), d.y + MARGIN, d.y + d.h - size.h - MARGIN) };
}

export function placePlayer(displays, saved, near, size) {
  const list = Array.isArray(displays) && displays.length ? displays : [mainOf([])];
  const d0 = saved && typeof saved.display === 'string' ? list.find((d) => (d.name || '') === saved.display) : null;
  if (d0 && Number.isFinite(saved.dx) && Number.isFinite(saved.dy)) return inside(d0, d0.x + saved.dx, d0.y + saved.dy, size);
  const d = (near && list.find((x) => x.id === near.id)) || mainOf(list);
  const bottom = Math.max(96, Math.round(d.h * BOTTOM));
  return inside(d, d.x + (d.w - size.w) / 2, d.y + d.h - bottom - size.h, size);
}

// the display with the player's center (else the nearest)
export function rememberPlace(displays, x, y, size) {
  const cx = x + size.w / 2, cy = y + size.h / 2;
  let best = null, dist = Infinity;
  for (const d of displays || []) {
    const dx = Math.max(d.x - cx, 0, cx - (d.x + d.w - 1)), dy = Math.max(d.y - cy, 0, cy - (d.y + d.h - 1));
    if (dx * dx + dy * dy < dist) { dist = dx * dx + dy * dy; best = d; }
  }
  const d = best || mainOf(displays || []);
  const p = inside(d, x, y, size);
  return { display: d.name || '', dx: p.x - d.x, dy: p.y - d.y };
}

// levels: lord.voice.levels()'s 0–255 per band, or null; dt in ms
export function smoothBands(prev, levels, dt) {
  const fall = Math.min(1, dt / 220);
  const out = [];
  for (let i = 0; i < PLAYER_BARS; i++) {
    const target = levels && Number.isFinite(levels[i]) ? clamp(levels[i] / 255, 0, 1) : 0;
    const p = prev && Number.isFinite(prev[i]) ? prev[i] : 0;
    out.push(target >= p ? target : p + (target - p) * fall);
  }
  return out;
}
