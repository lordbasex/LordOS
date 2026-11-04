// hwkeymodel.js — the next level for a keyboard's volume, brightness and keyboard-light keys. Plain
// numbers, so tests/hwkeys_test.mjs can run them without a session.

export const VOLUME_STEP = 0.05;

// dir: 1 or -1. Levels land on the steps; a level Settings raised above 100 % is kept, not cut.
export function nextVolume(level, dir) {
  const steps = dir > 0 ? Math.floor(level / VOLUME_STEP + 1e-6) + 1 : Math.ceil(level / VOLUME_STEP - 1e-6) - 1;
  const top = Math.max(1, level);
  return Math.min(top, Math.max(0, Math.round(steps * VOLUME_STEP * 100) / 100));
}

// 16 steps; the screen never reaches 0, which turns some panels fully off (Settings → Displays).
export function nextBacklight(value, max, dir) {
  const step = Math.max(1, Math.round(max / 16));
  return Math.min(max, Math.max(1, value + dir * step));
}

// The keyboard's light does go off.
export function nextKeyLight(value, max, dir) {
  const step = Math.max(1, Math.round(max / 8));
  return Math.min(max, Math.max(0, value + dir * step));
}

// ---- the level indicator (sdk/levelhud.js) ----

// How long it stays after the last press, and its fade out (none with Reduce motion).
export const LEVEL_HUD_MS = 1500;
export const LEVEL_HUD_FADE = 200;
export const LEVEL_SEGMENTS = 16;

// What the indicator shows for a level: a fraction of 0..1 (a volume above 100 % fills the bar).
export function levelFraction(value, max = 1) {
  if (!(max > 0) || !Number.isFinite(value)) return 0;
  return Math.min(1, Math.max(0, value / max));
}

// How many of the bar's segments are lit. Any level above zero lights at least one, and only a full
// level lights them all.
export function litSegments(fraction, n = LEVEL_SEGMENTS) {
  const f = Math.min(1, Math.max(0, fraction || 0));
  if (f <= 0) return 0;
  if (f >= 1) return n;
  return Math.min(n - 1, Math.max(1, Math.round(f * n)));
}

// The speaker's waves: 0 to 3, none when muted or silent.
export function speakerWaves(fraction, muted) {
  if (muted || !(fraction > 0)) return 0;
  return fraction < 0.34 ? 1 : fraction < 0.67 ? 2 : 3;
}

// The panel's opacity `ms` after the last press: 1, then a linear fade, then 0 (hidden).
export function hudOpacity(ms, reduceMotion) {
  if (ms < LEVEL_HUD_MS) return 1;
  if (reduceMotion) return 0;
  const k = (ms - LEVEL_HUD_MS) / LEVEL_HUD_FADE;
  return k >= 1 ? 0 : +(1 - k).toFixed(3);
}
