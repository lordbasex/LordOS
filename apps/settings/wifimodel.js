// wifimodel.js — the order and state of Settings → Network → Wi-Fi's lists (GitHub #167). No React, no lord.*: tested by
// server/tests/wifi_model_test.mjs.
//
//   wifiRows(found)            the scan's networks (net-wifi-scan: { ssid, bssid, signal, security, known, current }):
//                              the one LordOS is connected to first, whatever its signal, then by signal, strongest first
//   markCurrent(found, ssid)   after a "net wifi <if> connected <ssid>" or "disconnected" (ssid null) event: which one is
//                              connected, without scanning again
//   currentOf(found)           the connected network, or null
//   savedRows(known, found)    the saved networks (lord.net.wifiKnown(): { ssid, security }): the connected one first,
//                              then those in range, then the rest, each by name; with { current, inRange }

const num = (v) => (typeof v === 'number' && Number.isFinite(v) ? v : -1000);

export function wifiRows(found) {
  const list = (Array.isArray(found) ? found : []).filter((n) => n && typeof n.ssid === 'string');
  // only one can be connected: the first marked one wins (a second radio's scan never mixes in here)
  const cur = list.find((n) => n.current === true) || null;
  const rest = list.filter((n) => n !== cur).map((n) => (n.current ? { ...n, current: false } : n));
  rest.sort((a, b) => num(b.signal) - num(a.signal));
  return cur ? [cur, ...rest] : rest;
}

export function markCurrent(found, ssid) {
  const list = Array.isArray(found) ? found : [];
  return wifiRows(list.map((n) => ({ ...n, current: ssid != null && ssid !== '' && n.ssid === ssid })));
}

export const currentOf = (found) => (Array.isArray(found) ? found.find((n) => n && n.current === true) || null : null);

export function savedRows(known, found) {
  const scan = Array.isArray(found) ? found : [];
  const cur = currentOf(scan);
  const rows = (Array.isArray(known) ? known : []).filter((k) => k && typeof k.ssid === 'string' && k.ssid)
    .map((k) => ({ ...k, current: !!cur && cur.ssid === k.ssid, inRange: scan.some((n) => n && n.ssid === k.ssid) }));
  const rank = (r) => (r.current ? 0 : r.inRange ? 1 : 2);
  return rows.sort((a, b) => rank(a) - rank(b) || a.ssid.localeCompare(b.ssid));
}
