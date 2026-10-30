// cleaner.js — Cleaner's native API (lord.clean, docs/CLEANER-ARCHITECTURE.md).
//
//   cleaner.available()                  the sandboxed scanner is installed
//   cleaner.start()                      a new scan of every rule: its id. It takes no path
//   cleaner.poll()                       { job, state, error, scan, plan }: real counters
//   cleaner.cancel(forget)               stops the job; forget also drops the results
//   cleaner.results(scanId | 'plan', offset, limit, filter) a page of { id, rule, path, size, allocated, … }
//   cleaner.plan(scanId, ids)            a plan, built and checked in lord-server: its id
//   cleaner.execute(planId)              runs it once
//   cleaner.status()                     { available, free, total }
//
// JS holds candidate ids and display paths only: authority stays in C and is never a path. Calls
// throw translated errors (sdk/errors.js).
const has = () => typeof lord !== 'undefined' && !!lord.clean;

export const cleaner = {
  available: () => {
    if (!has()) return false;
    try { return lord.clean.status().available; } catch (e) { return false; }
  },
  status: () => (has() ? lord.clean.status() : { available: false, free: -1, total: -1 }),
  start: () => lord.clean.start(),
  poll: () => (has() ? lord.clean.poll() : null),
  cancel: (forget) => { if (has()) lord.clean.cancel(!!forget); },
  results: (source, offset, limit, filter) => lord.clean.results(source, offset, limit, filter),
  plan: (scanId, ids) => lord.clean.plan(scanId, ids),
  execute: (planId) => lord.clean.execute(planId),
};
