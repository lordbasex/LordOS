// btlistmodel.js — the nearby devices Settings → Bluetooth offers, and what a failed pairing means (issue #78).
// Pure logic (tests: server/tests/bt_list_model_test.mjs) over lord.bt.state().devices.
//
// Some headsets are two devices on the air: the classic one that plays audio ("Name", found by an inquiry, so
// BlueZ reports its Class of Device and, once known, the audio services A2DP and hands-free) and a low-energy
// one named "LE-Name" or "LE_Name" (no Class: never seen over classic Bluetooth), which only carries the
// maker's own service for its phone app. Pairing that one fails, and it plays nothing. So:
//   - a low-energy side whose classic device is also listed (paired or nearby) is not offered;
//   - one listed alone is offered last, with a hint to wait for the classic name;
//   - if pairing it fails, the message says why, instead of "the code did not match".
// A low-energy side is recognized only by what BlueZ reports: the "LE-"/"LE_" name, no Class and no audio
// service (classic or LE Audio). A device that offers audio over LE (LE Audio, hearing aids) is never hidden.

// 16-bit service UUIDs (Device1.UUIDs on the Bluetooth base UUID, as bt.c lists them).
const AUDIO = new Set([
  '1108', '1112', '1131',             // headset (HSP)
  '110a', '110b', '110d',             // A2DP source, sink, profile
  '111e', '111f',                     // hands-free (HFP)
  '1203',                             // generic audio
  '184e', '184f', '1850', '1853', '1854', '1855',   // LE Audio: stream control, broadcast scan, capabilities, common, hearing access, telephony and media
  'fdf0',                             // hearing aids over LE (ASHA)
]);

const LE_NAME = /^LE[-_ ]\s*(\S.*)$/;
const norm = (s) => String(s || '').trim().toLowerCase();

export const hasAudio = (d) => Array.isArray(d && d.services) && d.services.some((u) => AUDIO.has(u));

// The classic name a low-energy side stands for ("LE-Name" → "Name"), or null if d is not one.
export function lowEnergySide(d) {
  if (!d || d.class || hasAudio(d)) return null;
  const m = LE_NAME.exec(String(d.name || '').trim());
  return m ? m[1].trim() : null;
}

// The classic device listed under the name a low-energy side stands for, or null.
export function classicTwin(d, devices) {
  const base = lowEnergySide(d);
  if (!base) return null;
  return (devices || []).find((e) => e && e !== d && e.path !== d.path && norm(e.name) === norm(base) && (e.class || hasAudio(e))) || null;
}

// The devices to offer under Nearby: not paired, named (or being paired), without the low-energy sides whose
// classic device is listed; strongest signal first, lone low-energy sides last.
export function nearbyDevices(devices) {
  const all = Array.isArray(devices) ? devices : [];
  const le = (d) => (lowEnergySide(d) ? 1 : 0);
  return all.filter((d) => d && !d.paired && (d.named || d.busy) && (d.busy || !classicTwin(d, all)))
    .sort((x, y) => (le(x) - le(y)) || ((y.rssi ?? -200) - (x.rssi ?? -200)));
}

// The key under prefs.bt.err for a failed action (r: a "bt-result"), or '' when there is nothing to say.
export function btErrorKey(r, devices) {
  const e = String((r && r.error) || '');
  if (/Blocked|rfkill/i.test(e)) return 'blocked';
  if (/Canceled|AuthenticationCanceled/i.test(e)) return 'canceled';
  if (/AlreadyExists/i.test(e)) return '';
  const d = (devices || []).find((x) => x && x.path === (r && r.path));
  if (r && r.op === 'pair' && lowEnergySide(d)) return 'leSide';
  if (/profile-unavailable|NotAvailable|NotSupported/i.test(e)) return 'noProfile';
  if (/AuthenticationFailed|AuthenticationRejected|Rejected/i.test(e)) return 'authFailed';
  return 'failed';
}
