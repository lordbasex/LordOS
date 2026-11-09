// thumbcache.js — what useThumb knows about lord-media's thumbnails (sdk/media.js), so a render doesn't ask lord-server.
//
// Until 0.8.8.7 every component showing a thumbnail asked lord.media.thumb on every render, and every thumbnail made
// (or failed) re-rendered all of them: a folder of 1,500 videos asked lord-server 1,500 x 1,500 times, each a realpath
// and stat on what may be a Cloud mount, and kept the desktop busy for minutes (without lord-media, as 0.8.8.6 shipped,
// every request failed at once and the storm came all together). Here:
//   - get(path, size) asks only the first time, then again at most every RETRY_MS while it is being made and every
//     KEEP_MS once known (a file edited since gets a new thumbnail; one that failed may be tried again);
//   - arrived(src, thumb) records an answer (lord-media's "thumb" event, src its real path) and wakes only the
//     components watching that path; an answer for a path nobody watches by that name (a link) wakes everyone once;
//   - the wake-ups of answers that come together go out as one (schedule), not one render per answer.
// Pure (no React, no lord.*). tests: server/tests/thumb_cache_test.mjs.

export const RETRY_MS = 5000;
export const KEEP_MS = 60000;

export function createThumbCache({ ask, now = () => Date.now(), schedule = (fn) => setTimeout(fn, 50), max = 4096, onAny = () => {} }) {
  const entries = new Map();      // `${size}|${path}` → { path, src, failed, at }
  const watchers = new Map();     // path → Set(fn)
  const due = new Set();          // fns to call at the next flush
  let any = false, planned = false;

  const flush = () => {
    planned = false;
    const fns = [...due];
    due.clear();
    const all = any;
    any = false;
    for (const fn of fns) { try { fn(); } catch (e) { /* one component's error doesn't stop the others */ } }
    if (all) onAny();
  };
  const plan = () => { if (!planned) { planned = true; schedule(flush); } };

  function get(path, size = 256) {
    if (!path) return null;
    const k = `${size}|${path}`;
    let e = entries.get(k);
    const t = now();
    if (e && t - e.at < (e.src || e.failed ? KEEP_MS : RETRY_MS)) {
      entries.delete(k); entries.set(k, e);          // most recently used last
      return e.src;
    }
    let src = null;
    try { src = ask(path, size) || null; } catch (err) { src = null; }
    if (!e) {
      e = { path, src: null, failed: false, at: 0 };
      if (entries.size >= max) entries.delete(entries.keys().next().value);
    } else entries.delete(k);
    entries.set(k, e);
    e.src = src;
    e.failed = false;
    e.at = t;
    return src;
  }

  function arrived(src, thumb) {
    if (!src) return;
    let known = false;
    for (const e of entries.values()) {
      if (e.path !== src) continue;
      known = true;
      // made (this size or another of the same file): asked again on the next render, which gets its path
      if (thumb && e.src === null) e.at = 0;
      if (!thumb && e.src === null) e.failed = true;
    }
    const w = watchers.get(src);
    if (w && w.size) { for (const fn of w) due.add(fn); plan(); } else if (!known) { any = true; plan(); }
  }

  function watch(path, fn) {
    if (!path) return () => {};
    let s = watchers.get(path);
    if (!s) { s = new Set(); watchers.set(path, s); }
    s.add(fn);
    return () => { const w = watchers.get(path); if (w) { w.delete(fn); if (!w.size) watchers.delete(path); } due.delete(fn); };
  }

  return { get, arrived, watch, size: () => entries.size, watching: () => watchers.size };
}
