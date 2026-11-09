// node server/tests/bt_list_model_test.mjs — Settings → Bluetooth's nearby list and pairing errors (sdk/btlistmodel.js,
// issue #78). Devices are shaped like lord.bt.state().devices (server/src/hw/bt.c): class is the Class of Device
// (0 when BlueZ never saw it over classic Bluetooth), services the 16-bit service UUIDs.
import assert from 'node:assert/strict';
import { hasAudio, lowEnergySide, classicTwin, nearbyDevices, btErrorKey } from '../../sdk/btlistmodel.js';

const dev = (path, name, extra = {}) => ({ path: `/org/bluez/hci0/dev_${path}`, name, named: true, paired: false, busy: false, class: 0, services: [], addressType: 'public', ...extra });
// the report: a headset's low-energy side (only its maker's service, 0xfebe) and, after more searching, the classic one
const leSide = dev('4C_87_5D_00_00_01', 'LE-Test QuietSet 35', { services: ['febe'], addressType: 'random', rssi: -50 });
const classic = dev('04_52_C7_00_00_01', 'Test QuietSet 35', { class: 0x240418, services: ['110b', '111e', '110e'], rssi: -60 });
const classicNoUuids = dev('04_52_C7_00_00_01', 'Test QuietSet 35', { class: 0x240418, services: [], rssi: -60 });   // found by inquiry, services not known yet
const speaker = dev('00_AA_01_01_00_02', 'Test Speaker', { class: 0x240414, services: ['110b'], rssi: -70 });
const mouse = dev('00_AA_01_01_00_03', 'Test Mouse', { addressType: 'random', services: ['1812'], rssi: -40 });

// ---- what a low-energy side is ----
assert.equal(lowEnergySide(leSide), 'Test QuietSet 35');
assert.equal(lowEnergySide({ ...leSide, name: 'LE_Test Headphones' }), 'Test Headphones');   // the other makers' spelling
assert.equal(lowEnergySide({ ...leSide, name: '  LE-  Spaced  ' }), 'Spaced');
assert.equal(lowEnergySide(classic), null);                       // not an LE- name
assert.equal(lowEnergySide({ ...leSide, class: 0x240418 }), null); // seen over classic Bluetooth: a real one
assert.equal(lowEnergySide({ ...leSide, services: ['febe', '184e'] }), null);   // LE Audio: plays sound, never hidden
assert.equal(lowEnergySide({ ...leSide, services: ['fdf0'] }), null);           // hearing aid
assert.equal(lowEnergySide({ ...leSide, name: 'LEGO Hub' }), null);
assert.equal(lowEnergySide({ ...leSide, name: 'LE-' }), null);
assert.equal(lowEnergySide(null), null);
assert.equal(hasAudio(classic), true);
assert.equal(hasAudio(mouse), false);
assert.equal(hasAudio({}), false);

// ---- its classic device ----
assert.equal(classicTwin(leSide, [leSide, classic, speaker]), classic);
assert.equal(classicTwin(leSide, [leSide, classicNoUuids]), classicNoUuids);   // the Class is enough
assert.equal(classicTwin(leSide, [leSide, { ...classic, name: 'test quietset 35 ' }]).path, classic.path);   // case and spaces
assert.equal(classicTwin(leSide, [leSide, speaker]), null);
assert.equal(classicTwin(leSide, [leSide, { ...classic, class: 0, services: [] }]), null);   // another LE thing with that name: not proof
assert.equal(classicTwin(classic, [leSide, classic]), null);
assert.equal(classicTwin(leSide, null), null);

// ---- the nearby list ----
const names = (l) => l.map((d) => d.name);
// only the low-energy side so far: offered, but last
assert.deepEqual(names(nearbyDevices([leSide, speaker, mouse])), ['Test Mouse', 'Test Speaker', 'LE-Test QuietSet 35']);
// the classic one appeared: the low-energy side is gone
assert.deepEqual(names(nearbyDevices([leSide, speaker, classic, mouse])), ['Test Mouse', 'Test QuietSet 35', 'Test Speaker']);
// the classic one is already paired (under My devices): still hidden
assert.deepEqual(names(nearbyDevices([leSide, { ...classic, paired: true }])), []);
// being paired right now: kept, so the row doesn't vanish under the user
assert.deepEqual(names(nearbyDevices([{ ...leSide, busy: true }, classic])), ['Test QuietSet 35', 'LE-Test QuietSet 35']);
// unnamed devices are not listed unless busy, as before
assert.deepEqual(names(nearbyDevices([{ ...mouse, named: false }, speaker])), ['Test Speaker']);
assert.deepEqual(nearbyDevices(null), []);

// ---- what a failed action says ----
const all = [leSide, classic];
assert.equal(btErrorKey({ op: 'pair', path: leSide.path, error: 'org.bluez.Error.AuthenticationFailed' }, all), 'leSide');
assert.equal(btErrorKey({ op: 'pair', path: leSide.path, error: 'org.bluez.Error.ConnectionAttemptFailed' }, all), 'leSide');
assert.equal(btErrorKey({ op: 'pair', path: leSide.path, error: 'org.bluez.Error.AuthenticationCanceled' }, all), 'canceled');
assert.equal(btErrorKey({ op: 'pair', path: classic.path, error: 'org.bluez.Error.AuthenticationFailed' }, all), 'authFailed');
assert.equal(btErrorKey({ op: 'pair', path: classic.path, error: 'org.bluez.Error.AuthenticationRejected' }, all), 'authFailed');
assert.equal(btErrorKey({ op: 'connect', path: classic.path, error: 'br-connection-profile-unavailable' }, all), 'noProfile');
assert.equal(btErrorKey({ op: 'power', path: '', error: 'org.bluez.Error.Blocked' }, all), 'blocked');
assert.equal(btErrorKey({ op: 'pair', path: classic.path, error: 'org.bluez.Error.AlreadyExists' }, all), '');
assert.equal(btErrorKey({ op: 'connect', path: classic.path, error: 'org.bluez.Error.Failed' }, all), 'failed');
assert.equal(btErrorKey({ op: 'pair', path: '/gone', error: 'org.bluez.Error.AuthenticationFailed' }, all), 'authFailed');
assert.equal(btErrorKey({}, null), 'failed');

console.log('bt_list_model_test: ok');
