// deniedmodel.js — Monitor → Network's "Recently denied": the firewall's denied connections of the last ten minutes
// (lord.fw events), the latest of each (app, direction, destination) with how many. Every direction counts: an
// incoming connection or a port an app wasn't let open is a denial too, and lord-netperm's Denied counter includes
// them; listing only outgoing ones showed "Nothing denied" beside a Denied count (QA ws174). Tested by
// server/tests/monitor_denied_test.mjs.

export const DENIED_WINDOW_MS = 10 * 60 * 1000;

// what a row names: the destination (its resolved name first) for an outgoing one, the local port for an incoming one
export const deniedTarget = (e) => (e?.dir === 'in' ? `← :${Number(e.port) || 0}` : (e?.hosts && e.hosts[0]) || e?.addr || '?');

// → { rows: [{ e, n }] (at most max, newest first), total }
export function recentDenials(events, now, max = 4, windowMs = DENIED_WINDOW_MS) {
  const denied = (Array.isArray(events) ? events : []).filter((e) => e && e.v === 'deny' && (!e.at || now - e.at < windowMs));
  const byKey = new Map();
  for (const e of denied) {
    const k = `${e.app}|${e.dir === 'in' ? 'in' : 'out'}|${deniedTarget(e)}`;
    const prev = byKey.get(k);
    if (prev) byKey.delete(k);                       // the latest of each goes last, so it is among the newest
    byKey.set(k, { e, n: (prev?.n || 0) + 1 });
  }
  return { rows: [...byKey.values()].slice(-max).reverse(), total: denied.length };
}
