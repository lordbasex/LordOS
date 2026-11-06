// lib.js — prepended by lc.py to every command (driver.js scope: cloud, cloudState, wm, lord, sleep, log)
const S = () => cloud.state();
const A = (name) => (S().accounts || []).find((a) => a.name === name || a.id === name);
const brief = (a) => a && ({ id: a.id, name: a.name, kind: a.kind, state: a.state, mounted: a.mounted, error: a.error, message: a.message, mount: a.mount });
const until = async (fn, ms = 30000, every = 250) => { const end = Date.now() + ms; for (;;) { const v = await fn(); if (v) return v; if (Date.now() > end) return null; await sleep(every); } };
const mounted = (name, ms = 40000) => until(() => { const a = A(name); return a && a.mounted ? a : null; }, ms);
const home = lord.fs.home();
const C = (rel) => `${home}/Cloud/${rel}`;
// a call that answers later: its result, by the { req } it gave, or (when the req came back 0, B1) the next reply after it
const call = async (fn, ms = 60000) => {
  const n0 = replies.length ? replies[replies.length - 1].at : 0; const t0 = Date.now();
  const r = await fn();
  if (!r || typeof r !== 'object' || !('req' in r)) return r;
  const hit = await until(() => replies.find((x) => (r.req ? x.req === r.req : x.at >= t0 && x.at > n0)), ms, 100);
  return hit ? { ...hit.res, _req: hit.req, _ms: hit.at - t0 } : { error: 'e2e.noReply' };
};
