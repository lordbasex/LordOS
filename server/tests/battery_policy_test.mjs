// Settings → Battery's choices over time (sdk/batterypolicy.js): the plug-in notice, heat protection, charging once
// to 100 %, discharging on power to the limit, and the devices' notices — each once per crossing.
import assert from 'node:assert/strict';
import { batteryStep, batteryPrefs, HEAT_PAUSE_C } from '../../sdk/batterypolicy.js';

const bat = (o) => ({ percent: 60, status: 'Discharging', tempC: 30, behaviour: 1, behaviours: 7, startThreshold: 70, endThreshold: 80, ...o });

// prefs are clamped
assert.deepEqual([batteryPrefs({ limit: 120, resume: 99, plugAt: 1 }).limit, batteryPrefs({ limit: 120, resume: 99 }).resume, batteryPrefs({ plugAt: 1 }).plugAt], [100, 95, 5]);
assert.equal(batteryPrefs({ limit: 60, resume: 70 }).resume, 55, 'resume stays below the limit');

// plug in at 20 %, once
let r = batteryStep(bat({ percent: 25 }), {}, [], {});
assert.equal(r.notify.length, 0);
r = batteryStep(bat({ percent: 20 }), {}, [], r.mem);
assert.deepEqual(r.notify.map((n) => n.id), ['plug']);
r = batteryStep(bat({ percent: 18 }), {}, [], r.mem);
assert.equal(r.notify.length, 0, 'not again while still low');
r = batteryStep(bat({ percent: 19, status: 'Charging' }), {}, [], r.mem);
r = batteryStep(bat({ percent: 19 }), {}, [], r.mem);
assert.deepEqual(r.notify.map((n) => n.id), ['plug'], 'again after it charged');

// heat: paused while charging hot, resumed when cool, only what it paused
r = batteryStep(bat({ status: 'Charging', tempC: HEAT_PAUSE_C + 1 }), {}, [], {});
assert.equal(r.behaviour, 'inhibit-charge');
assert.deepEqual(r.notify.map((n) => n.id), ['heat']);
let r2 = batteryStep(bat({ status: 'Not charging', tempC: 38, behaviour: 2 }), {}, [], r.mem);
assert.equal(r2.behaviour, null, 'still warm: stays paused');
r2 = batteryStep(bat({ status: 'Not charging', tempC: 35, behaviour: 2 }), {}, [], r2.mem);
assert.equal(r2.behaviour, 'auto');
assert.equal(batteryStep(bat({ status: 'Charging', tempC: 45 }), { heatGuard: false }, [], {}).behaviour, null, 'off: nothing');
assert.equal(batteryStep(bat({ status: 'Charging', tempC: 45, behaviours: 1 }), {}, [], {}).behaviour, null, 'no inhibit on this battery');
assert.equal(batteryStep(bat({ status: 'Charging', tempC: -1000 }), {}, [], {}).behaviour, null, 'no temperature: nothing');

// charge once to 100 %, then back to the person's limit
r = batteryStep(bat({ percent: 79, status: 'Charging' }), { fullOnce: true }, [], {});
assert.deepEqual(r.thresholds, [70, 100]);
r = batteryStep(bat({ percent: 90, status: 'Charging' }), { fullOnce: true }, [], r.mem);
assert.equal(r.thresholds, null);
r = batteryStep(bat({ percent: 100, status: 'Full' }), { fullOnce: true }, [], r.mem);
assert.deepEqual([r.thresholds, r.done], [[70, 80], ['fullOnce']]);
// unplugged before full: back too
r = batteryStep(bat({ percent: 85, status: 'Charging' }), { fullOnce: true }, [], {});
r = batteryStep(bat({ percent: 86, status: 'Charging' }), { fullOnce: true }, [], r.mem);
r = batteryStep(bat({ percent: 86, status: 'Discharging' }), { fullOnce: true }, [], r.mem);
assert.deepEqual(r.thresholds, [70, 80]);
// a battery without thresholds: nothing to do
assert.equal(batteryStep(bat({ endThreshold: -1 }), { fullOnce: true }, [], {}).thresholds, null);

// discharge on power to the limit
r = batteryStep(bat({ percent: 95, status: 'Not charging' }), { dischargeTo: true }, [], {});
assert.equal(r.behaviour, 'force-discharge');
r = batteryStep(bat({ percent: 80, status: 'Discharging', behaviour: 4 }), { dischargeTo: true }, [], r.mem);
assert.deepEqual([r.behaviour, r.done], ['auto', ['dischargeTo']]);
assert.equal(batteryStep(bat({ percent: 95, behaviours: 3 }), { dischargeTo: true }, [], {}).behaviour, null, 'not offered by this battery');

// devices: low and full notices, once each
const mouse = (o) => ({ id: 'ps:hidpp_battery_0', name: 'MX Master 3', kind: 'mouse', percent: 50, charging: false, ...o });
r = batteryStep(null, {}, [mouse({ percent: 20 })], {});
assert.deepEqual(r.notify.map((n) => n.id), ['low:ps:hidpp_battery_0']);
r = batteryStep(null, {}, [mouse({ percent: 15 })], r.mem);
assert.equal(r.notify.length, 0);
r = batteryStep(null, {}, [mouse({ percent: 82, charging: true })], r.mem);
assert.deepEqual(r.notify.map((n) => n.id), ['full:ps:hidpp_battery_0']);
r = batteryStep(null, {}, [mouse({ percent: 85, charging: true })], r.mem);
assert.equal(r.notify.length, 0);
assert.equal(batteryStep(null, { devicesNotify: false }, [mouse({ percent: 10 })], {}).notify.length, 0);
assert.equal(batteryStep(null, {}, [mouse({ percent: -1, charging: true })], {}).notify.length, 0, 'no level: nothing');

console.log('battery policy: ok');
