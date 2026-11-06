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
