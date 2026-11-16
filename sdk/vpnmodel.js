// vpnmodel.js — the VPN widget's plain logic (0.8.8.10; server/tests/vpn_model_test.mjs). The VPNs are the ones set up in
// Settings → Network → VPN (lord.net.vpnList(): name and type); connecting and disconnecting are the same closed actions
// Settings uses (net-vpn-up / net-vpn-down through sdk/admin.js, open to the session's user); whether one is up, its
// address and its traffic come from its interface in lord.net.interfaces() (no root, no extra polling of lord-netd).
//
//   ifnameOf(type, name)       the interface lord-netd gives it (server/src/net/wg.c vpn_ifname)
//   vpnState(vpn, ifs)         { up, ip, rx, tx } from the interfaces (rx/tx: byte counters)
//   rates(prev, cur, dt)       { rx, tx } bytes per second, never negative
//   targetOf(opts, list)       the VPN a widget drives: its own choice if it still exists, else the only one, else null
//   eventOf(text)              an admin notice "net vpn <name> <what> …" → { name, what } or null
const PREFIX = { wireguard: 'wg', openvpn: 'ovpn', ipsec: 'ips', pptp: 'ppp', openconnect: 'oc' };

export function ifnameOf(type, name) {
  const pre = PREFIX[type] || 'fvpn';
  return `${pre}-${String(name || '').slice(0, 14 - pre.length)}`.toLowerCase();
}

export function vpnState(vpn, ifs) {
  if (!vpn) return { up: false, ip: '', rx: 0, tx: 0 };
  const want = ifnameOf(vpn.type, vpn.name);
  const i = (ifs || []).find((x) => x && x.name === want);
  const ip = i?.addresses?.find((a) => a.family === 4)?.address || i?.addresses?.[0]?.address || '';
  return { up: !!(i && i.up !== false), ip, rx: Number(i?.rxBytes) || 0, tx: Number(i?.txBytes) || 0 };
}

export function rates(prev, cur, dt) {
  if (!prev || !cur || !(dt > 0)) return null;
  return { rx: Math.max(0, (cur.rx - prev.rx) / dt), tx: Math.max(0, (cur.tx - prev.tx) / dt) };
}

export function targetOf(opts, list) {
  const names = (list || []).map((v) => v && v.name).filter(Boolean);
  if (opts?.vpn && names.includes(opts.vpn)) return opts.vpn;
  return names.length === 1 ? names[0] : null;
}

export function eventOf(text) {
  const m = /^net vpn (\S+) (\S+)/.exec(String(text || ''));
  return m ? { name: m[1], what: m[2] } : null;
}
