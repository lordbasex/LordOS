// btaudiomodel.js — which profile a Bluetooth headset's card gets (issue #78). Pure logic for sound.js and
// Settings → Sound (tests: server/tests/bt_audio_model_test.mjs).
//
// A headset is one card with several profiles (lord.sound.state().cards, from PipeWire's BlueZ support):
//   - high fidelity playback: "a2dp-sink" (and one per codec, "a2dp-sink-sbc"…), "bap-sink", "asha-sink": an
//     output only (a2dp-duplex / bap-duplex also have an input);
//   - headset mode: "headset-head-unit" (and "-cvsd", "-msbc"…), HSP/HFP over SCO: a low-quality output plus
//     the microphone;
//   - "off".
// WirePlumber's automatic switch to headset mode whenever an app records is turned off by LordOS
// (/etc/wireplumber/wireplumber.conf.d/lordos-bluez-profiles.conf): with it, merely opening Settings → Sound (its
// microphone meter) moved a headset to headset mode, and on adapters where SCO audio is broken the output went
// silent. Instead:
//   - choosing a headset as the output gives it its high fidelity profile (unless its microphone is the chosen
//     input: then it stays in headset mode, the only one with both);
//   - choosing its microphone (micChoices) switches it to headset mode; choosing another microphone gives the
//     headsets in headset mode their high fidelity profile back;
//   - a headset that shows up in headset mode (WirePlumber restored that profile) is moved to high fidelity.
// Profiles are chosen like WirePlumber does: the highest priority among the available ones.

const HIFI = /^(a2dp|bap|asha)-(sink|duplex)/;
const HEADSET = /^headset-/;

export const isBluetoothCard = (c) => !!c && (c.api === 'bluez5' || c.bus === 'bluetooth');
const cardsOf = (st) => (st && Array.isArray(st.cards) ? st.cards : []);
const sinksOf = (st) => (st && Array.isArray(st.sinks) ? st.sinks : []);
const micsOf = (st) => (st && Array.isArray(st.sources) ? st.sources : []).filter((x) => x && x.monitorOf === -1);
const usable = (p) => !!p && p.available !== false;
const best = (list) => list.reduce((a, p) => (!a || p.priority > a.priority ? p : a), null);
const activeOf = (card) => (card.profiles || []).find((p) => p.name === card.activeProfile) || null;
const hifiOf = (card) => best((card.profiles || []).filter((p) => usable(p) && p.sinks > 0 && HIFI.test(p.name)));

export const cardOf = (st, dev) => (dev && dev.card >= 0 ? cardsOf(st).find((c) => c.index === dev.card) || null : null);
export const inHeadsetMode = (card) => isBluetoothCard(card) && HEADSET.test(card.activeProfile || '');

// The profile to switch to so the card plays well, or null to leave it as it is.
export function outputProfile(card) {
  if (!isBluetoothCard(card)) return null;
  const cur = activeOf(card);
  if (cur && cur.sinks > 0 && HIFI.test(cur.name)) return null;
  const hifi = hifiOf(card);
  if (hifi) return hifi.name;
  if (cur && cur.sinks > 0) return null;                 // headset mode is all it has
  const any = best((card.profiles || []).filter((p) => usable(p) && p.sinks > 0));
  return any ? any.name : null;
}

// The profile to switch to so the card has a microphone, or null (it has one, or never will).
export function inputProfile(card) {
  if (!isBluetoothCard(card)) return null;
  const cur = activeOf(card);
  if (cur && cur.sources > 0) return null;
  const p = best((card.profiles || []).filter((x) => usable(x) && x.sources > 0));
  return p ? p.name : null;
}

// A card that has just appeared: out of headset mode if it can play in high fidelity.
export function arrivalProfile(card) {
  if (!inHeadsetMode(card)) return null;
  const hifi = hifiOf(card);
  return hifi ? hifi.name : null;
}

// Headset microphones that need a profile switch to exist: one entry per Bluetooth card without an input now.
export function micChoices(st) {
  const mics = micsOf(st);
  return cardsOf(st).filter((c) => isBluetoothCard(c) && !mics.some((m) => m.card === c.index) && inputProfile(c))
    .map((c) => ({ card: c.index, name: c.name, label: c.description || c.name }));
}

// What choosing a default output (kind 'sink') or input ('source') by name does:
//   profiles:   [[card, profile], …] to set first;
//   setDefault: { kind, name } to make the default now (or null: the device only exists after the switch);
//   pending:    { kind, card, profile, stale } whose device becomes the default once the card has switched
//               (stale: the card's device indices before the switch, which are going away), or null.
// An output that is switched is also made the default right away: PipeWire names a headset's output the same in
// both profiles, and if the switch fails the click still chose it.
export function planDefault(st, kind, name) {
  const list = kind === 'sink' ? sinksOf(st) : micsOf(st);
  const dev = list.find((x) => x && x.name === name);
  const card = cardOf(st, dev);
  const profiles = [];
  let pending = null;
  if (kind === 'sink' && isBluetoothCard(card)) {
    const micHere = micsOf(st).some((m) => m.default && m.card === card.index);
    const p = micHere ? null : outputProfile(card);
    if (p) { profiles.push([card.index, p]); pending = waitFor(st, kind, card, p); }
  }
  if (kind === 'source') {
    for (const c of cardsOf(st)) {                      // headsets no longer used as the microphone: back to high fidelity
      if (c === card || !inHeadsetMode(c)) continue;
      const hifi = hifiOf(c);
      if (hifi) profiles.push([c.index, hifi.name]);
    }
  }
  return { profiles, setDefault: { kind, name }, pending };
}

function waitFor(st, kind, card, profile) {
  const devs = kind === 'sink' ? sinksOf(st) : micsOf(st);
  return { kind, card: card.index, profile, stale: devs.filter((d) => d.card === card.index).map((d) => d.index) };
}

// Choosing a headset microphone from micChoices (its card index): headset mode, then its input is the default.
export function planMic(st, cardIndex) {
  const card = cardsOf(st).find((c) => c.index === cardIndex);
  const p = inputProfile(card);
  const mic = card ? micsOf(st).find((m) => m.card === card.index) : null;
  if (!p) return mic ? planDefault(st, 'source', mic.name) : { profiles: [], setDefault: null, pending: null };
  const plan = planDefault(st, 'source', '');
  return { profiles: [[card.index, p], ...plan.profiles], setDefault: null, pending: waitFor(st, 'source', card, p) };
}

// The device a pending choice is waiting for: its name once the card has switched and its new device exists.
export function pendingTarget(st, pending) {
  if (!pending) return null;
  const card = cardsOf(st).find((c) => c.index === pending.card);
  if (!card || card.activeProfile !== pending.profile) return null;
  const stale = pending.stale || [];
  const dev = (pending.kind === 'sink' ? sinksOf(st) : micsOf(st)).find((d) => d.card === pending.card && !stale.includes(d.index));
  return dev ? dev.name : null;
}

// Cards seen for the first time (by name) that arrived in headset mode. seen: the names already handled.
// Returns { profiles: [[card, profile], …], seen: the names of the Bluetooth cards there now }.
export function arrivals(st, seen) {
  const now = new Set();
  const profiles = [];
  for (const c of cardsOf(st)) {
    if (!isBluetoothCard(c)) continue;
    now.add(c.name);
    if (seen && seen.has(c.name)) continue;
    const p = arrivalProfile(c);
    if (p) profiles.push([c.index, p]);
  }
  return { profiles, seen: now };
}
