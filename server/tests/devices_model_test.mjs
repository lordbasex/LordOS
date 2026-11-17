// The Devices widget's list (sdk/devicesmodel.js): the computer, the kernel's device batteries and BlueZ's, once
// each, with their kind, and lord-hidpp's percentages filling in the Logitech devices that give only a level.
import assert from 'node:assert/strict';
import { devicesFrom, kindOf, fractionOf, toneOf, shortName } from '../../sdk/devicesmodel.js';

// kinds, from BlueZ's icon or the name
assert.equal(kindOf('input-mouse', ''), 'mouse');
assert.equal(kindOf('audio-headset', 'WH-1000XM4'), 'headset');
assert.equal(kindOf('', 'Wireless Mouse MX Master 3'), 'mouse');
assert.equal(kindOf('', 'MX Keys Wireless Keyboard'), 'keyboard');
assert.equal(kindOf('', 'Logitech K380'), 'keyboard');
assert.equal(kindOf('', 'AirPods Pro'), 'headset');
assert.equal(kindOf('', 'DualSense Wireless Controller'), 'gamepad');
assert.equal(kindOf('', 'Something'), 'other');

// the maintainer's desktop: no system battery, a mouse and a keyboard on a Logitech receiver giving only a level,
// a Bluetooth mouse seen by both the kernel and BlueZ, and headphones only BlueZ knows
const power = { hasBattery: false, percent: -1, charging: false, supplies: [
  { name: 'hidpp_battery_0', battery: true, device: true, present: true, percent: -1, level: 'Normal', status: 'Discharging', model: 'Wireless Mouse MX Master 3' },
  { name: 'hidpp_battery_1', battery: true, device: true, present: true, percent: -1, level: '', status: 'Discharging', model: 'MX Keys Wireless Keyboard' },
  { name: 'hid-aa:bb:cc:dd:ee:ff-battery', battery: true, device: true, present: true, percent: 55, level: '', status: 'Discharging', model: 'Travel Mouse' },
  { name: 'AC', battery: false, device: false, online: true },
] };
const bt = { devices: [
  { address: 'AA:BB:CC:DD:EE:FF', name: 'Travel Mouse', icon: 'input-mouse', connected: true, battery: 55 },
  { address: '11:22:33:44:55:66', name: 'Auriculares', icon: 'audio-headphones', connected: true, battery: 18 },
  { address: '77:88:99:AA:BB:CC', name: 'Old speaker', icon: 'audio-card', connected: false, battery: 80 },
  { address: '22:22:22:22:22:22', name: 'No battery', icon: 'input-keyboard', connected: true, battery: -1 },
] };
const list = devicesFrom(power, bt, []);
assert.deepEqual(list.map((d) => [d.kind, d.name, d.percent, d.level, d.via]), [
  ['mouse', 'Travel Mouse', 55, '', 'bluetooth'],
  ['mouse', 'Wireless Mouse MX Master 3', -1, 'normal', 'logitech'],
  ['keyboard', 'MX Keys Wireless Keyboard', -1, '', 'logitech'],
  ['headset', 'Auriculares', 18, '', 'bluetooth'],
]);
assert.equal(fractionOf(list[1]), 0.6, 'a level fills the ring roughly');
assert.equal(fractionOf(list[2]), 0, 'no percentage and no level: an empty ring');
assert.deepEqual(list.map(toneOf), ['ok', 'ok', 'ok', 'warn']);

// a laptop first, charging; lord-hidpp's numbers fill the Logitech ones that had none
const lap = devicesFrom({ ...power, hasBattery: true, percent: 100, charging: true }, bt,
  [{ name: 'MX Master 3', percent: 85 }, { name: 'MX Keys', percent: 9.6, charging: false }, { name: 'Unknown thing', percent: 50 }]);
assert.deepEqual(lap.map((d) => [d.kind, d.percent, d.charging]),
  [['laptop', 100, true], ['mouse', 55, false], ['mouse', 85, false], ['keyboard', 10, false], ['headset', 18, false]]);
assert.equal(toneOf(lap[3]), 'crit', 'under 15 %');
assert.equal(toneOf({ percent: 5, charging: true }), 'ok', 'charging is never an alarm');
assert.equal(toneOf({ percent: -1, level: 'critical' }), 'crit');

// lord.hidpp.batteries() as lord-server gives it: by power supply, exact even when the names differ
const bySupply = devicesFrom(power, bt, [{ supply: 'hidpp_battery_0', percent: 85, charging: false, full: false }, { supply: 'hidpp_battery_1', percent: 40, charging: true }, { supply: 'hidpp_battery_9', percent: 1 }]);
assert.deepEqual(bySupply.filter((d) => d.via === 'logitech').map((d) => [d.name, d.percent, d.charging]),
  [['Wireless Mouse MX Master 3', 85, false], ['MX Keys Wireless Keyboard', 40, true]]);

// short names under a ring
assert.equal(shortName('Wireless Mouse MX Master 3'), 'MX Master 3');
assert.equal(shortName('MX Keys Wireless Keyboard'), 'MX Keys');
assert.equal(shortName('Logitech K380'), 'K380');
assert.equal(shortName('Keyboard'), 'Keyboard', 'nothing left: the full name');
assert.equal(shortName('AirPods Pro'), 'AirPods Pro');

// nothing at all
assert.deepEqual(devicesFrom(null, null, null), []);
assert.deepEqual(devicesFrom({ hasBattery: false, supplies: [] }, { devices: [] }), []);
console.log('devices model: ok');
