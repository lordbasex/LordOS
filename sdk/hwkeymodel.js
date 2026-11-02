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
