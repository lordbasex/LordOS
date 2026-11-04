// hwkeys.js — what a keyboard's volume, brightness and keyboard-light keys do, in every theme
// (BASE in shortcuts.js; lord-server keeps these keys from apps, wl/keygrab.c). Each press shows the new
// level in the brief indicator (levelhud.js), also when it is already at an end.
import { soundState, defaultSink, sound } from './sound.js';
import { admin } from './admin.js';
import { nextVolume, nextBacklight, nextKeyLight, levelFraction } from './hwkeymodel.js';
import { showLevel } from './levelhud.js';

function changeVolume(dir) {
  const out = defaultSink(soundState.get());
  if (!out) return;
  if (dir > 0 && out.mute) sound.setMute('sink', out.index, false);
  const level = nextVolume(out.level, dir);
  sound.setVolume('sink', out.index, level);
  showLevel('volume', levelFraction(level), out.mute && dir < 0);
}
export const volumeUp = () => changeVolume(1);
export const volumeDown = () => changeVolume(-1);
export function volumeMute() {
  const out = defaultSink(soundState.get());
  if (!out) return;
  sound.setMute('sink', out.index, !out.mute);
  showLevel('volume', levelFraction(out.level), !out.mute);
}

// A held key is faster than the helper's round trip: the value on its way counts, not the file's.
const sent = new Map();
function step(action, field, next, dir, kind) {
  if (typeof lord.display === 'undefined') return;
  let shown = false;
  for (const d of lord.display.list()?.[field] || []) {
    const was = sent.get(d.name);
    const from = was && Date.now() - was.at < 500 ? was.value : d.value;
    const value = next(from, d.max, dir);
    if (!shown) { showLevel(kind, levelFraction(value, d.max)); shown = true; }    // the first one's level
    if (value === from) continue;
    sent.set(d.name, { value, at: Date.now() });
    admin.run(action, `${d.name} ${value}`, undefined, { quiet: true }).catch(() => {});
  }
}
export const brightnessUp = () => step('display-brightness', 'backlight', nextBacklight, 1, 'brightness');
export const brightnessDown = () => step('display-brightness', 'backlight', nextBacklight, -1, 'brightness');
export const keyLightUp = () => step('keyboard-light', 'keyboardLight', nextKeyLight, 1, 'keylight');
export const keyLightDown = () => step('keyboard-light', 'keyboardLight', nextKeyLight, -1, 'keylight');
