// Machines' host USB devices, the app's pure side (apps/machines/model.js; docs/VIRTUALIZATION.md 6.6): the Devices menu's
// rows, the auto-connect rules, what the capture indicator shows, the permission an attach waits for.
// node server/tests/vm_usb_model_test.mjs
import assert from 'node:assert/strict';
import { usbRule, usbName, usbRows, usbCamLive, usbMicLive, usbNeedsPerm, usbAutoRows } from '../../apps/machines/model.js';

let n = 0;
const t = (name, f) => { f(); n++; };

const cam = { bus: 1, dev: 11, vendor: '046d', product: '0843', serial: 'FA11927E', model: 'Logitech Webcam C930e', camera: true, audio: true, holder: 0, why: '' };
const disk = { bus: 1, dev: 4, vendor: '0781', product: '5581', serial: '', maker: 'SanDisk', storage: true, holder: 2, why: '' };
const hub = { bus: 1, dev: 5, vendor: '05e3', product: '0608', why: 'vm.usbHub', holder: 0 };
const kb = { bus: 1, dev: 6, vendor: '046d', product: 'c52b', model: 'USB Receiver', why: 'vm.usbInputInUse', holder: 0 };
const bare = { bus: 2, dev: 3, vendor: '1234', product: 'abcd', holder: 0, why: '' };

t('rules and names', () => {
  assert.equal(usbRule(cam), '046d:0843:FA11927E');
  assert.equal(usbRule(disk), '0781:5581');
  assert.equal(usbName(cam), 'Logitech Webcam C930e');
  assert.equal(usbName(disk), 'SanDisk');
  assert.equal(usbName(bare), '1234:abcd');
  assert.equal(usbName({ model: 'x'.repeat(80) }).length, 48);
});

t('the menu: locked, no controller, empty', () => {
  assert.equal(usbRows([cam], { locked: true, controller: true }, 1).state, 'locked');
  assert.equal(usbRows([cam], null, 1).state, 'locked');
  assert.equal(usbRows([cam], { controller: false }, 1).state, 'nocontroller');
  assert.equal(usbRows([hub], { controller: true, devices: [] }, 1).state, 'empty');
  assert.equal(usbRows([], { controller: true, devices: [] }, 1).state, 'empty');
});

t('the menu: rows', () => {
  const v = usbRows([cam, disk, hub, kb, bare], { controller: true, devices: [{ bus: 1, dev: 11 }], auto: ['0781:5581'] }, 1);
  assert.equal(v.state, 'list');
  assert.deepEqual(v.rows.map((r) => r.name), ['Logitech Webcam C930e', 'SanDisk', 'USB Receiver', '1234:abcd'], 'a hub is never shown');
  const [c, d, k, b] = v.rows;
  assert.equal(c.on, true);
  assert.equal(c.why, '');
  assert.equal(c.camera && c.audio, true);
  assert.equal(d.on, false);
  assert.equal(d.why, 'vm.usbOtherMachine', 'another session holds it');
  assert.equal(d.auto, true);
  assert.equal(d.autoRule, '0781:5581');
  assert.equal(k.why, 'vm.usbInputInUse');
  assert.equal(b.why, '');
  assert.equal(c.auto, false);
  assert.equal(c.autoRule, '046d:0843:FA11927E', 'a new rule keeps the serial: only that device');
  // the same machine's holder isn't "another machine"
  assert.equal(usbRows([{ ...disk, holder: 1 }], { controller: true, devices: [] }, 1).rows[0].why, '');
  // a rule with the serial matches its device
  assert.equal(usbRows([cam], { controller: true, devices: [], auto: ['046d:0843:FA11927E'] }, 1).rows[0].auto, true);
});

t('the capture indicator and the permission an attach waits for', () => {
  assert.equal(usbCamLive({ devices: [{ camera: true }] }), true);
  assert.equal(usbCamLive({ devices: [{ camera: false, audio: true }] }), false);
  assert.equal(usbMicLive({ devices: [{ audio: true }] }), true);
  assert.equal(usbCamLive(undefined), false);
  assert.equal(usbMicLive({}), false);
  assert.equal(usbNeedsPerm('vm.usbCameraPerm'), 'camera');
  assert.equal(usbNeedsPerm('vm.usbMicPerm'), 'microphone');
  assert.equal(usbNeedsPerm(''), '');
  assert.equal(usbNeedsPerm('vm.usbHub'), '');
});

t("Configure's rules", () => {
  assert.deepEqual(usbAutoRows(['046d:0843:FA11927E', '0781:5581', 'ffff:0001'], [cam, disk]),
    [{ rule: '046d:0843:FA11927E', name: 'Logitech Webcam C930e' }, { rule: '0781:5581', name: 'SanDisk' }, { rule: 'ffff:0001', name: 'ffff:0001' }]);
  assert.deepEqual(usbAutoRows(undefined, []), []);
});

console.log(`vm-usb-model-test: ${n} passed`);
