// hwkeys.js — what a keyboard's volume, brightness and keyboard-light keys do, in every theme
// (BASE in shortcuts.js; lord-server keeps these keys from apps, wl/keygrab.c). Each press shows the new
// level in the brief indicator (levelhud.js), also when it is already at an end.
import { soundState, defaultSink, sound } from './sound.js';
import { admin } from './admin.js';
import { nextVolume, nextBacklight, nextKeyLight, levelFraction } from './hwkeymodel.js';
import { showLevel } from './levelhud.js';
import { useState } from 'react';
import { useStore } from './store.js';
import { displays } from './hardware.jsx';
import { backlightOf, backlightValue } from './menubarmodel.js';

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

// The brightness slider of Flow's Control Center and Classic's Quick Settings (issue #114): the same closed
// admin action as the keys and Settings → Displays (display-brightness, checked on the root side), sent at most every
// 120 ms while the slider moves, always with its last position.
let pending = null, timer = 0;
export function setBacklight(name, value) {
  sent.set(name, { value, at: Date.now() });
  pending = { name, value };
  if (timer) return;
  timer = setTimeout(() => {
    timer = 0;
    const p = pending;
    pending = null;
    if (p) admin.run('display-brightness', `${p.name} ${p.value}`, undefined, { quiet: true }).catch(() => {});
  }, 120);
}

// null without a backlight LordOS can set (a desktop's monitor, a virtual machine): then there is no slider. Else
// { name, fraction, set(fraction) }; while it moves (and a moment after) it shows its own position, not the file's.
export function useBacklight() {
  const list = useStore(displays);
  const [drag, setDrag] = useState(null);
  const b = backlightOf(list);
  if (!b) return null;
  const mine = drag && drag.name === b.name && Date.now() - drag.at < 1500;
  const value = mine ? drag.value : b.value;
  return {
    name: b.name, fraction: levelFraction(value, b.max),
    set: (f) => { const v = backlightValue(f, b.max); setDrag({ name: b.name, value: v, at: Date.now() }); setBacklight(b.name, v); },
  };
}
