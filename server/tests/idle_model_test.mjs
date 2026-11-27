// node server/tests/idle_model_test.mjs — the lock around a system sleep (sdk/idlemodel.js, issue #36).
import assert from 'node:assert/strict';
import { lockBeforeSleep, lockAfterWake } from '../../sdk/idlemodel.js';

// Immediately (0, also the default when the setting is missing): lock before sleeping
assert.equal(lockBeforeSleep(0), true);
assert.equal(lockBeforeSleep(undefined), true);
assert.equal(lockAfterWake(0, 3600), false);          // already locked before sleeping
// Never
assert.equal(lockBeforeSleep(-1), false);
assert.equal(lockAfterWake(-1, 86400), false);
// a delay: not before sleeping, on waking only after sleeping at least that long
for (const d of [5, 60, 300, 900, 3600]) {
  assert.equal(lockBeforeSleep(d), false);
  assert.equal(lockAfterWake(d, d - 1), false);
  assert.equal(lockAfterWake(d, d), true);
  assert.equal(lockAfterWake(d, 6 * 3600), true);
}
assert.equal(lockAfterWake(60, undefined), false);
console.log('idle_model_test: ok');

// GitHub #270: the keyboard's light goes off with the display and comes back on waking
{
  const { keyLightsOff, keyLightsBack } = await import('../../sdk/idlemodel.js');
  const list = [{ name: 'smc::kbd_backlight', value: 128, max: 255 }, { name: 'dell::kbd_backlight', value: 0, max: 2 },
    { name: 'input3::capslock', value: 1, max: 1 }, { name: 'bad::kbd_backlight', value: 'x', max: 2 }];
  const off = keyLightsOff(list);
  assert.deepEqual(off.saved, [{ name: 'smc::kbd_backlight', value: 128 }]);
  assert.deepEqual(off.writes, [{ name: 'smc::kbd_backlight', value: 0 }]);
  // waking: put back where it is still off
  assert.deepEqual(keyLightsBack(off.saved, [{ name: 'smc::kbd_backlight', value: 0, max: 255 }]), [{ name: 'smc::kbd_backlight', value: 128 }]);
  // someone set it again meanwhile, or the light is gone: left alone
  assert.deepEqual(keyLightsBack(off.saved, [{ name: 'smc::kbd_backlight', value: 40, max: 255 }]), []);
  assert.deepEqual(keyLightsBack(off.saved, []), []);
  // a smaller max after a driver reload: clamped
  assert.deepEqual(keyLightsBack(off.saved, [{ name: 'smc::kbd_backlight', value: 0, max: 100 }]), [{ name: 'smc::kbd_backlight', value: 100 }]);
  assert.deepEqual(keyLightsOff(undefined), { saved: [], writes: [] });
  assert.deepEqual(keyLightsBack(null, list), []);
  console.log('idle_model_test: keyboard light ok');
}
