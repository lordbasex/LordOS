// node server/tests/bt_power_model_test.mjs — Bluetooth's on/off kept across restarts (sdk/btpowermodel.js, issue #75).
import assert from 'node:assert/strict';
import { btPowerAction } from '../../sdk/btpowermodel.js';

const st = (powered, path = '/org/bluez/hci0') => ({ available: true, adapter: { path, powered }, devices: [] });

// no bluetoothd, no adapter: nothing, and nothing remembered
assert.deepEqual(btPowerAction(null, true, null), { off: false, done: null });
assert.deepEqual(btPowerAction({ available: false, adapter: null }, true, null), { off: false, done: null });
assert.deepEqual(btPowerAction({ available: true, adapter: null }, true, '/org/bluez/hci0'), { off: false, done: null });
// the user left it on (or never chose): BlueZ's AutoEnable has it on, nothing to do
assert.deepEqual(btPowerAction(st(true), false, null), { off: false, done: '/org/bluez/hci0' });
assert.deepEqual(btPowerAction(st(false), false, null), { off: false, done: '/org/bluez/hci0' });
// the user left it off: off once
assert.deepEqual(btPowerAction(st(true), true, null), { off: true, done: '/org/bluez/hci0' });
assert.deepEqual(btPowerAction(st(true), true, '/org/bluez/hci0'), { off: false, done: '/org/bluez/hci0' });   // turned on later: stays on
// found before AutoEnable powered it: waits for that, then turns it off
assert.deepEqual(btPowerAction(st(false), true, null), { off: false, done: null });
assert.deepEqual(btPowerAction(st(true), true, null), { off: true, done: '/org/bluez/hci0' });
// bluetoothd restarted or the dongle plugged in again: AutoEnable powers it, the choice is applied again
let done = btPowerAction(st(true), true, null).done;
done = btPowerAction({ available: false, adapter: null }, true, done).done;
assert.equal(done, null);
assert.deepEqual(btPowerAction(st(true), true, done), { off: true, done: '/org/bluez/hci0' });
// another adapter (a dongle instead of the built-in one)
assert.deepEqual(btPowerAction(st(true, '/org/bluez/hci1'), true, '/org/bluez/hci0'), { off: true, done: '/org/bluez/hci1' });
console.log('bt_power_model_test: ok');
