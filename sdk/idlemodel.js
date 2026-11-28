// idlemodel.js — when "Require password after the display turns off" (Settings → Energy, power.lockDelay
// in seconds: 0 Immediately, -1 Never) locks the screen around a system sleep (issue #36,
// docs/SETTINGS.md). Closing the lid or pressing the power button puts the computer to sleep through
// logind: the display turns off then too, so the same setting applies.
//
//   lockBeforeSleep(delay)        Immediately: lock before the computer sleeps (lord-server holds the
//                                 sleep until the screen is taken, hw/sleeplock.c)
//   lockAfterWake(delay, slept)   a longer delay: lock on waking when the computer slept (plus the time
//                                 the screen had already been off) at least that long
//
// Pure functions, tested by server/tests/idle_model_test.mjs.

const delayOf = (d) => (typeof d === 'number' && Number.isFinite(d) ? d : 0);

export function lockBeforeSleep(lockDelay) {
  return delayOf(lockDelay) === 0;
}

export function lockAfterWake(lockDelay, slept) {
  const d = delayOf(lockDelay);
  if (d <= 0) return false;            // Never, or already locked before sleeping
  return typeof slept === 'number' && slept >= d;
}

// The keyboard's light while the display is off (GitHub #270): it stays lit until the computer sleeps unless the
// display's "off" turns it off too and its "wake" puts it back.
//
//   keyLightsOff(list)            list = lord.display.list().keyboardLight ([{ name, value, max }]): the lights that
//                                 are on, to remember, and the writes that turn them off
//   keyLightsBack(saved, list)    the writes that put the remembered ones back, only where the light is still off
//                                 (a light someone set again in the meantime is left as it is)
const lightOk = (l) => l && typeof l.name === 'string' && l.name.endsWith('::kbd_backlight')
  && Number.isInteger(l.value) && Number.isInteger(l.max) && l.max > 0;

export function keyLightsOff(list) {
  const saved = (Array.isArray(list) ? list : []).filter((l) => lightOk(l) && l.value > 0)
    .map((l) => ({ name: l.name, value: Math.min(l.value, l.max) }));
  return { saved, writes: saved.map((l) => ({ name: l.name, value: 0 })) };
}

export function keyLightsBack(saved, list) {
  const now = new Map((Array.isArray(list) ? list : []).filter(lightOk).map((l) => [l.name, l]));
  return (Array.isArray(saved) ? saved : []).filter((s) => now.get(s.name)?.value === 0)
    .map((s) => ({ name: s.name, value: Math.min(s.value, now.get(s.name).max) }));
}
