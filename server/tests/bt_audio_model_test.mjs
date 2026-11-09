// node server/tests/bt_audio_model_test.mjs — a Bluetooth headset's profile when it is chosen as the output or
// microphone, or shows up (sdk/btaudiomodel.js, issue #78). The cards are shaped like lord.sound.state().cards
// with the profile names and descriptions PipeWire 1.4's BlueZ support gives a headset.
import assert from 'node:assert/strict';
import {
  isBluetoothCard, outputProfile, inputProfile, arrivalProfile, micChoices, planDefault, planMic, pendingTarget, arrivals,
} from '../../sdk/btaudiomodel.js';

const P = (name, sinks, sources, priority, available = true) => ({ name, description: name, sinks, sources, priority, available });
const headsetCard = (active, extra = {}) => ({
  index: 57, name: 'bluez_card.00_AA_01_01_00_01', description: 'Test Headset', api: 'bluez5', bus: 'bluetooth',
  activeProfile: active,
  profiles: [P('off', 0, 0, 0), P('a2dp-sink-sbc', 1, 0, 18), P('a2dp-sink', 1, 0, 19), P('headset-head-unit-cvsd', 1, 1, 1),
    P('headset-head-unit', 1, 1, 2)],
  ...extra,
});
const speakerCard = (active) => ({   // a speaker with A2DP only
  index: 60, name: 'bluez_card.00_AA_01_01_00_02', description: 'Test Speaker', api: 'bluez5', bus: 'bluetooth',
  activeProfile: active, profiles: [P('off', 0, 0, 0), P('a2dp-sink', 1, 0, 19)],
});
const sink = (index, name, card, def = false) => ({ index, name, card, default: def, monitor: -1 });
const mic = (index, name, card, def = false) => ({ index, name, card, default: def, monitorOf: -1 });
const builtinOut = sink(40, 'alsa_output.pci-0000_00_1b.0.analog-stereo', 46, true);
const builtinMic = mic(41, 'alsa_input.pci-0000_00_1b.0.analog-stereo', 46, true);

// ---- which cards are Bluetooth ----
assert.equal(isBluetoothCard(headsetCard('a2dp-sink')), true);
assert.equal(isBluetoothCard({ api: 'alsa', bus: 'pci' }), false);
assert.equal(isBluetoothCard({ api: '', bus: 'bluetooth' }), true);
assert.equal(isBluetoothCard(null), false);

// ---- the profile for playing ----
assert.equal(outputProfile(headsetCard('a2dp-sink')), null);                       // already high fidelity
assert.equal(outputProfile(headsetCard('a2dp-sink-sbc')), null);                   // a codec the user chose stays
assert.equal(outputProfile(headsetCard('headset-head-unit-cvsd')), 'a2dp-sink');   // the report: SCO, no sound
assert.equal(outputProfile(headsetCard('off')), 'a2dp-sink');
// high fidelity not available (BlueZ didn't connect A2DP): headset mode is all it has, it stays
const noA2dp = headsetCard('headset-head-unit', { profiles: [P('off', 0, 0, 0), P('a2dp-sink', 1, 0, 19, false), P('headset-head-unit', 1, 1, 2)] });
assert.equal(outputProfile(noA2dp), null);
assert.equal(outputProfile({ ...noA2dp, activeProfile: 'off' }), 'headset-head-unit');
assert.equal(outputProfile({ index: 1, api: 'alsa', activeProfile: 'off', profiles: [P('output:analog-stereo', 1, 0, 6500)] }), null);
// LE Audio counts as high fidelity
assert.equal(outputProfile({ ...speakerCard('off'), profiles: [P('off', 0, 0, 0), P('bap-sink', 1, 0, 20)] }), 'bap-sink');

// ---- the profile for the microphone ----
assert.equal(inputProfile(headsetCard('a2dp-sink')), 'headset-head-unit');         // the highest priority with an input
assert.equal(inputProfile(headsetCard('headset-head-unit-cvsd')), null);            // has one already
assert.equal(inputProfile(headsetCard('a2dp-sink', { profiles: [P('a2dp-sink', 1, 0, 19), P('headset-head-unit', 1, 1, 2, false), P('headset-head-unit-cvsd', 1, 1, 1)] })),
  'headset-head-unit-cvsd');                                                       // an unavailable codec is skipped
assert.equal(inputProfile(speakerCard('a2dp-sink')), null);                        // no microphone at all

// ---- a card that shows up ----
assert.equal(arrivalProfile(headsetCard('headset-head-unit')), 'a2dp-sink');
assert.equal(arrivalProfile(headsetCard('a2dp-sink')), null);
assert.equal(arrivalProfile(headsetCard('off')), null);                            // off is left alone
assert.equal(arrivalProfile(noA2dp), null);

let r = arrivals({ cards: [headsetCard('headset-head-unit'), speakerCard('a2dp-sink'), { index: 46, name: 'alsa_card.x', api: 'alsa' }] }, null);
assert.deepEqual(r.profiles, [[57, 'a2dp-sink']]);
assert.deepEqual([...r.seen], ['bluez_card.00_AA_01_01_00_01', 'bluez_card.00_AA_01_01_00_02']);
// later states: the same card in headset mode (the user chose its microphone) is not switched back
r = arrivals({ cards: [headsetCard('headset-head-unit'), speakerCard('a2dp-sink')] }, r.seen);
assert.deepEqual(r.profiles, []);
// it went away and came back: switched again
r = arrivals({ cards: [speakerCard('a2dp-sink')] }, r.seen);
r = arrivals({ cards: [speakerCard('a2dp-sink'), headsetCard('headset-head-unit-cvsd')] }, r.seen);
assert.deepEqual(r.profiles, [[57, 'a2dp-sink']]);

// ---- choosing the output ----
const hfpOut = sink(70, 'bluez_output.00_AA_01_01_00_01.1', 57);
const hfpMic = mic(71, 'bluez_input.00_AA_01_01_00_01.0', 57);
let st = { cards: [headsetCard('headset-head-unit')], sinks: [builtinOut, hfpOut], sources: [builtinMic, hfpMic] };
let plan = planDefault(st, 'sink', hfpOut.name);
assert.deepEqual(plan.profiles, [[57, 'a2dp-sink']]);
assert.deepEqual(plan.setDefault, { kind: 'sink', name: hfpOut.name });
assert.deepEqual(plan.pending, { kind: 'sink', card: 57, profile: 'a2dp-sink', stale: [70] });
// the card switched but the old output is still listed: not yet
assert.equal(pendingTarget({ ...st, cards: [headsetCard('a2dp-sink')] }, plan.pending), null);
// not switched yet: not yet
assert.equal(pendingTarget({ ...st, sinks: [builtinOut, sink(80, 'bluez_output.00_AA_01_01_00_01.1', 57)] }, plan.pending), null);
// switched and its new output is there
const a2dpOut = sink(80, 'bluez_output.00_AA_01_01_00_01.1', 57);
assert.equal(pendingTarget({ cards: [headsetCard('a2dp-sink')], sinks: [builtinOut, a2dpOut], sources: [builtinMic] }, plan.pending), a2dpOut.name);
assert.equal(pendingTarget({ cards: [], sinks: [a2dpOut], sources: [] }, plan.pending), null);   // the headset went away
assert.equal(pendingTarget(st, null), null);
// already playing in high fidelity: just the default
st = { cards: [headsetCard('a2dp-sink')], sinks: [builtinOut, a2dpOut], sources: [builtinMic] };
assert.deepEqual(planDefault(st, 'sink', a2dpOut.name), { profiles: [], setDefault: { kind: 'sink', name: a2dpOut.name }, pending: null });
// its microphone is the chosen input (a call): it stays in headset mode, the only one with both
st = { cards: [headsetCard('headset-head-unit')], sinks: [builtinOut, hfpOut], sources: [{ ...builtinMic, default: false }, { ...hfpMic, default: true }] };
assert.deepEqual(planDefault(st, 'sink', hfpOut.name), { profiles: [], setDefault: { kind: 'sink', name: hfpOut.name }, pending: null });
// a non-Bluetooth output
assert.deepEqual(planDefault(st, 'sink', builtinOut.name), { profiles: [], setDefault: { kind: 'sink', name: builtinOut.name }, pending: null });
// a name that isn't there: passed on (lord.sound refuses it)
assert.deepEqual(planDefault(st, 'sink', 'nope'), { profiles: [], setDefault: { kind: 'sink', name: 'nope' }, pending: null });
assert.deepEqual(planDefault({}, 'sink', 'x').profiles, []);

// ---- the microphone ----
st = { cards: [headsetCard('a2dp-sink'), speakerCard('a2dp-sink')], sinks: [builtinOut, a2dpOut], sources: [builtinMic] };
assert.deepEqual(micChoices(st), [{ card: 57, name: 'bluez_card.00_AA_01_01_00_01', label: 'Test Headset' }]);   // not the speaker
plan = planMic(st, 57);
assert.deepEqual(plan.profiles, [[57, 'headset-head-unit']]);
assert.equal(plan.setDefault, null);
assert.deepEqual(plan.pending, { kind: 'source', card: 57, profile: 'headset-head-unit', stale: [] });
assert.equal(pendingTarget({ ...st, cards: [headsetCard('headset-head-unit')], sources: [builtinMic, hfpMic] }, plan.pending), hfpMic.name);
assert.deepEqual(planMic(st, 99), { profiles: [], setDefault: null, pending: null });
// in headset mode already: its microphone is listed with the others, no extra choice
st = { cards: [headsetCard('headset-head-unit')], sinks: [builtinOut, hfpOut], sources: [builtinMic, hfpMic] };
assert.deepEqual(micChoices(st), []);
assert.deepEqual(planMic(st, 57), { profiles: [], setDefault: { kind: 'source', name: hfpMic.name }, pending: null });
// choosing another microphone gives the headset its high fidelity back
assert.deepEqual(planDefault(st, 'source', builtinMic.name),
  { profiles: [[57, 'a2dp-sink']], setDefault: { kind: 'source', name: builtinMic.name }, pending: null });
// choosing its own microphone keeps it
assert.deepEqual(planDefault(st, 'source', hfpMic.name).profiles, []);
// a headset whose high fidelity is not available stays as it is
st = { cards: [noA2dp], sinks: [builtinOut, hfpOut], sources: [builtinMic, hfpMic] };
assert.deepEqual(planDefault(st, 'source', builtinMic.name).profiles, []);

console.log('bt_audio_model_test: ok');
