// devicesmodel.js — the Devices widget's list, pure (tested by server/tests/devices_model_test.mjs).
//
// One list of everything with a battery: the computer's own (lord.power.state(): its supplies not of scope Device),
// the mice, keyboards and headsets the kernel reports (power_supply of scope Device: Logitech receivers and devices
// over HID++, Bluetooth HID devices as "hid-<address>-battery") and the Bluetooth devices BlueZ reports a battery for
// (lord.bt.state(), org.bluez.Battery1: headphones and the rest). A device seen both ways is listed once. Exact
// percentages from lord-hidpp, when there are any, fill in the Logitech devices that only give a level.

export const KINDS = ['laptop', 'mouse', 'keyboard', 'headset', 'gamepad', 'pen', 'phone', 'other'];

// the kind from BlueZ's icon name ("input-mouse", "audio-headset"…) or, failing that, words in the device's name
export function kindOf(icon, name) {
  const i = String(icon || '').toLowerCase();
  if (i.includes('mouse')) return 'mouse';
  if (i.includes('keyboard')) return 'keyboard';
  if (i.includes('headset') || i.includes('headphone') || i.includes('audio-card')) return 'headset';
  if (i.includes('gaming') || i.includes('joystick')) return 'gamepad';
  if (i.includes('tablet') || i.includes('pen')) return 'pen';
  if (i.includes('phone')) return 'phone';
  const n = String(name || '').toLowerCase();
  if (/\b(mouse|mx master|mx anywhere|trackball|lift|pebble|souris|maus|ratón|raton)\b/.test(n)) return 'mouse';
  if (/\b(keyboard|keys|teclado|clavier|tastatur)\b/.test(n) || /\bk\d{3}\b/.test(n) || /\bmx mechanical\b/.test(n)) return 'keyboard';
  if (/\b(headset|headphones?|buds|airpods|pods|earbuds|zone|auriculares)\b/.test(n)) return 'headset';
  if (/\b(controller|gamepad|dualsense|dualshock|joy-?con)\b/.test(n)) return 'gamepad';
  if (/\b(pen|stylus)\b/.test(n)) return 'pen';
  return 'other';
}

// a name that fits under a ring: the vendor and the generic words out ("Wireless Mouse MX Master 3" → "MX Master 3",
// "MX Keys Wireless Keyboard" → "MX Keys"); the full name when nothing is left
export function shortName(name) {
  const full = String(name || '').trim();
  const s = full.replace(/\b(logitech|logi|wireless|bluetooth|mouse|keyboard|headset|headphones|receiver|usb|inalámbrico|ratón|teclado)\b/gi, ' ')
    .replace(/\s+/g, ' ').trim();
  return s.length >= 2 ? s : full;
}

const LEVEL_FRACTION = { critical: 0.08, low: 0.2, normal: 0.6, high: 0.85, full: 1 };
// how full the ring is: the percentage, or roughly from the level
export const fractionOf = (d) => (d.percent >= 0 ? d.percent / 100 : LEVEL_FRACTION[d.level] ?? 0);
// 'crit' under 15 % (or a critical level), 'warn' under 30 % (or low), else 'ok'; charging is always ok
export function toneOf(d) {
  if (d.charging) return 'ok';
  if (d.percent >= 0) return d.percent < 15 ? 'crit' : d.percent < 30 ? 'warn' : 'ok';
  return d.level === 'critical' ? 'crit' : d.level === 'low' ? 'warn' : 'ok';
}

const macOf = (supplyName) => {
  const m = /^hid-([0-9a-f]{2}(?::[0-9a-f]{2}){5})-battery$/i.exec(String(supplyName || ''));
  return m ? m[1].toUpperCase() : '';
};
const viaOf = (supplyName) => (/^hidpp_/.test(supplyName) ? 'logitech' : macOf(supplyName) ? 'bluetooth' : 'usb');
const fold = (s) => String(s || '').toLowerCase().replace(/[^a-z0-9]+/g, ' ').trim();

// power: lord.power.state(); bt: lord.bt.state() ({ devices: [...] }); hidpp: [{ name, percent, charging }] from
// lord-hidpp. → [{ id, name, kind, percent (-1 unknown), level ('' or critical/low/normal/high/full), charging, via }]
export function devicesFrom(power, bt, hidpp) {
  const out = [];
  if (power && power.hasBattery) {
    out.push({ id: 'system', name: '', kind: 'laptop', percent: power.percent >= 0 ? power.percent : -1, level: '', charging: !!power.charging, via: 'internal' });
  }
  const macs = new Set();
  for (const s of (power && power.supplies) || []) {
    if (!s || !s.battery || !s.device || s.present === false) continue;
    const mac = macOf(s.name);
    if (mac) macs.add(mac);
    const name = String(s.model || '').trim() || s.name;
    out.push({ id: `ps:${s.name}`, name, kind: kindOf('', name), percent: s.percent >= 0 ? s.percent : -1,
      level: String(s.level || '').toLowerCase().replace(/[^a-z]/g, ''), charging: s.status === 'Charging', via: viaOf(s.name) });
  }
  for (const d of (bt && bt.devices) || []) {
    if (!d || !d.connected || !(d.battery >= 0)) continue;
    if (macs.has(String(d.address || '').toUpperCase())) continue;          // the kernel already lists it
    out.push({ id: `bt:${d.address}`, name: d.name || d.address, kind: kindOf(d.icon, d.name), percent: d.battery, level: '', charging: false, via: 'bluetooth' });
  }
  // lord-hidpp's exact numbers for the Logitech devices that gave only a level: by the device's own power supply
  // (lord.hidpp.batteries() names it), or by name for an older answer without one
  for (const h of hidpp || []) {
    if (!h || !(h.percent >= 0) && !h.charging) continue;
    const key = fold(h.name);
    const d = h.supply ? out.find((x) => x.id === `ps:${h.supply}`)
      : key ? out.find((x) => x.via === 'logitech' && x.percent < 0 && (fold(x.name).includes(key) || key.includes(fold(x.name)))) : null;
    if (!d) continue;
    if (h.percent >= 0) d.percent = Math.max(0, Math.min(100, Math.round(h.percent)));
    if (h.charging) d.charging = true;                   // charging with no level (an MX Keys on its cable): just that
  }
  const rank = (k) => KINDS.indexOf(k);
  return out.sort((a, b) => rank(a.kind) - rank(b.kind) || a.name.localeCompare(b.name));
}
