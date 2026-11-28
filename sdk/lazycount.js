// lazycount.js — a number that is slow to read (a synchronous native call), read off the interface's first render and
// then at most once per `ttl`. What draws it gets the last value at once (0 until the first read) and the new one on
// its next render; `subscribe` says when it changed.
//
//   lazyCount(read, { first, ttl, now, later })   → { get(), peek(), subscribe(fn), invalidate() }
//     read    () => number, or a Promise of one (an exception counts as 0; a rejection keeps the last value)
//     first   ms before the first read (default 15 s: after the session's start, never inside it)
//     ttl     ms a value stays fresh (default 60 s)
//     now, later   Date.now and setTimeout (tests pass their own)
//
// (QA 0.8.9.6.2) The crown menu's Store item counts the package updates (lord.pkg.upgrades()) on every render of the
// menu bar. The first one opens the package index and dpkg's status: 655 ms of the shell's first __lordTick on the
// Raspberry Pi's SD card. sdk/appcontrol.js storeUpdates() is now one of these.
export function lazyCount(read, opts = {}) {
  const first = opts.first ?? 15000, ttl = opts.ttl ?? 60000;
  const now = opts.now || (() => Date.now());
  const later = opts.later || ((fn, ms) => setTimeout(fn, ms));
  let value = 0, at = 0, pending = false, read0 = false;
  const subs = new Set();
  const apply = (v) => {
    const n = Number(v) || 0;
    pending = false;
    at = now();
    read0 = true;
    if (n !== value) { value = n; for (const fn of [...subs]) { try { fn(n); } catch (e) { /* a subscriber's own */ } } }
  };
  // read() may answer later (a Promise: a count made on one of lord-server's workers); one read at a time
  const run = () => {
    let r = 0;
    try { r = read(); } catch (e) { r = 0; }
    if (r && typeof r.then === 'function') r.then(apply, () => apply(value));   // a failed read keeps the last count
    else apply(r);
  };
  const schedule = () => {
    if (pending || (read0 && now() - at < ttl)) return;
    pending = true;
    later(run, read0 ? 0 : first);
  };
  return {
    get() { schedule(); return value; },
    peek: () => value,
    subscribe(fn) { subs.add(fn); return () => subs.delete(fn); },
    invalidate() { at = 0; if (read0) schedule(); },
  };
}
