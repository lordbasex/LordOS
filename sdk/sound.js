// sound.js — session sound (PipeWire; the client is in C, server/src/sound — see
// docs/SOUND.md). A store with the full state that updates only when something changes,
// also if another program changed it.
//
//   const s = useSound();          // { connected, sinks, sources, inputs, outputs, defaultSink… }
//   const out = defaultSink(s);    // the default output (or null)
//   sound.setVolume('sink', out.index, 0.5); sound.setMute('sink', out.index, true)
import { createStore, useStore } from './store.js';
import { planDefault, planMic, pendingTarget, arrivals, micChoices } from './btaudiomodel.js';

const has = () => typeof lord.sound !== 'undefined';
const EMPTY = { connected: false, sinks: [], sources: [], inputs: [], outputs: [] };
export const soundState = createStore(has() ? { ...EMPTY, ...lord.sound.state() } : EMPTY);
export const soundPeak = createStore({ source: -1, level: 0 });

const call = (fn, ...a) => (has() && typeof lord.sound[fn] === 'function' ? lord.sound[fn](...a) : false);

// Bluetooth headsets (issue #78, sdk/btaudiomodel.js): a choice that needs the card's profile switched first
// waits here until the switched card's device shows up, then becomes the default; it gives up after a while.
const PENDING_MS = 10000;
let pending = null;
let seenCards = null;

function apply(plan) {
  pending = plan.pending ? { ...plan.pending, until: Date.now() + PENDING_MS } : null;
  let ok = true;
  for (const [card, profile] of plan.profiles) ok = call('setProfile', card, profile) && ok;
  return plan.setDefault ? call('setDefault', plan.setDefault.kind, plan.setDefault.name) : ok;
}

function onState(st) {
  const arr = arrivals(st, seenCards);                  // a headset that came back in headset mode: high fidelity
  seenCards = arr.seen;
  for (const [card, profile] of arr.profiles) call('setProfile', card, profile);
  if (!pending) return;
  if (Date.now() > pending.until) { pending = null; return; }
  const name = pendingTarget(st, pending);
  if (name) { const { kind } = pending; pending = null; call('setDefault', kind, name); }
}

globalThis.__lordSound = (type, data) => {
  if (type === 'state') { soundState.set({ ...EMPTY, ...data }); onState(soundState.get()); }
  else if (type === 'peak') soundPeak.set(data);
};
if (has()) onState(soundState.get());

export const sound = {
  available: has,
  setVolume: (kind, index, v) => call('setVolume', kind, index, v),
  setMute: (kind, index, on) => call('setMute', kind, index, !!on),
  // the default output or input; a Bluetooth headset's card gets the profile that choice needs first
  setDefault: (kind, name) => apply(planDefault(soundState.get(), kind, name)),
  // a headset's microphone that only exists in headset mode (headsetMics): switches it, then makes it the default
  useHeadsetMic: (card) => apply(planMic(soundState.get(), card)),
  setPort: (kind, index, port) => call('setPort', kind, index, port),
  move: (kind, app, to) => call('move', kind, app, to),
  meter: (source) => call('meter', source),
  play: (name) => call('play', name),
};

export const useSound = () => useStore(soundState);
export const useSoundPeak = () => useStore(soundPeak);
export const defaultSink = (s) => s.sinks.find((x) => x.default) || s.sinks[0] || null;
// Real microphones (not the outputs' monitors).
export const microphones = (s) => s.sources.filter((x) => x.monitorOf === -1);
export const defaultSource = (s) => microphones(s).find((x) => x.default) || microphones(s)[0] || null;
// Headset microphones that appear only once the headset is switched to headset mode: { card, name, label }.
export const headsetMics = (s) => micChoices(s);
