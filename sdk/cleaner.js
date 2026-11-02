// cleaner.js — Cleaner's native API (lord.clean, docs/CLEANER-ARCHITECTURE.md).
//
//   cleaner.available()                  the sandboxed scanner is installed
//   cleaner.start(kind, opts)            a new scan ('rules', 'tree', 'dups' or 'images'): its id
//   cleaner.poll(kind)                   { job, state, error, scan, plan, emptied }: real counters
//   cleaner.volumes() / tree / groups    what a tree or a duplicates scan found
//   cleaner.restore(planId, id)          back from the Trash; history() / clearHistory(); emptyTrash()
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
  start: (kind, opts) => (kind ? lord.clean.start(kind, opts || {}) : lord.clean.start()),
  poll: (kind) => (has() ? lord.clean.poll(kind) : null),
  volumes: () => (has() ? lord.clean.volumes() : []),
  tree: (scanId, node, limit) => lord.clean.tree(scanId, node, limit),
  groups: (scanId, offset, limit) => lord.clean.groups(scanId, offset, limit),
  restore: (planId, id) => lord.clean.restore(planId, id),
  history: () => (has() ? lord.clean.history() : []),
  clearHistory: () => lord.clean.clearHistory(),
  noteSystem: (name, before) => lord.clean.noteSystem(name, before),
  saved: () => (has() ? lord.clean.saved() : null),
  forgetSaved: () => lord.clean.forgetSaved(),
  emptyTrash: () => lord.clean.emptyTrash(),
  cancel: (forget) => { if (has()) lord.clean.cancel(!!forget); },
  results: (source, offset, limit, filter) => lord.clean.results(source, offset, limit, filter),
  plan: (scanId, ids) => lord.clean.plan(scanId, ids),
  execute: (planId) => lord.clean.execute(planId),
};
