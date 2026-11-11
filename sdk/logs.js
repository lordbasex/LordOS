// logs.js — logs for the Logs app (lord.logs, server/src/ui/js_logs.c; docs/SYSTEM.md).
//
//   await logs.query({ scope: 'user'|'system', since, until, prio, kernel, match, grep, limit })
//        → [{ t (µs), s, p, pid, uid, k, id, u, b, m }]  (read by a lord-server thread, as the
//          user: their journal, and the system one only if their group allows it)
//   await logs.system(query)      the same for the system journal, through the helper's
//                                 logs-system action (administrators only)
//   logs.sources() → { user, system, uid }   logs.files() / logs.tail(name, bytes)   logs.dir() → { dir, system }
//   logs.follow(on), logs.onChange((kind, name) => …)   kind: 'journal' | 'file'
import { admin } from './admin.js';

const pending = new Map();
const listeners = new Set();

globalThis.__lordLogs = (kind, id, name) => {
  if (kind === 'result') {
    const p = pending.get(id);
    if (!p) { try { lord.logs.take(id); } catch (e) { /* nobody asked for it */ } return; }
    pending.delete(id);
    let r = null;
    try { r = lord.logs.take(id); } catch (e) { p.reject(e); return; }
    p.resolve(r || []);
    return;
  }
  for (const fn of listeners) { try { fn(kind, name); } catch (e) { console.log(`logs.onChange: ${e}`); } }
};

function queryText(q) {
  const lines = [];
  for (const k of ['since', 'until', 'prio', 'limit']) if (q[k] != null && q[k] !== '') lines.push(`${k}=${Math.floor(q[k])}`);
  if (q.kernel) lines.push('kernel=1');
  if (q.match) lines.push(`match=${q.match}`);
  if (q.grep) lines.push(`grep=${String(q.grep).replace(/[\x00-\x1f\x7f]/g, ' ')}`);
  return lines.join('\n');
}

export const logs = {
  available: () => !!(globalThis.lord && lord.logs),
  sources: () => lord.logs.sources(),
  files: () => lord.logs.files(),
  dir: () => (lord.logs.dir ? lord.logs.dir() : { dir: '', system: false }),
  tail: (name, bytes) => lord.logs.tail(name, bytes),
  follow: (on) => lord.logs.follow(!!on),
  onChange(fn) { listeners.add(fn); return () => listeners.delete(fn); },
  query(q) {
    return new Promise((resolve, reject) => {
      try { pending.set(lord.logs.query(q), { resolve, reject }); } catch (e) { reject(e); }
    });
  },
  // unit or process names valid for filtering (the same check as the C side)
  validMatch: (s) => /^[A-Za-z0-9@._:-]{1,95}$/.test(s),
  async system(q) {
    const out = [];
    await admin.run('logs-system', queryText({ ...q, limit: Math.min(q.limit || 1000, 5000) }), (line) => {
      try { out.push(JSON.parse(line)); } catch (e) { /* truncated line */ }
    }, { quiet: true });
    return out;
  },
};
