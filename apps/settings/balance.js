// Left/right balance of a stereo output (Settings → Sound). The sound server keeps one volume per
// channel; the balance is their ratio and the level is the louder one, so moving Volume keeps the
// balance (snd_set_level scales every channel) and moving Balance keeps the level.
//   balance: -1 (left only) … 0 (centered) … 1 (right only)

export function balanceOf(volume) {
  if (!Array.isArray(volume) || volume.length !== 2) return 0;
  const [l, r] = volume;
  if (l === r) return 0;
  return l < r ? 1 - l / r : -(1 - r / l);
}

export function volumesFor(level, balance) {
  const b = Math.max(-1, Math.min(1, balance));
  return [level * (b > 0 ? 1 - b : 1), level * (b < 0 ? 1 + b : 1)];
}

// Snaps to the center near it, so the slider can find 0 again by hand.
export function snapBalance(b) { return Math.abs(b) < 0.04 ? 0 : b; }
