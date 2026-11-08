// ticker.js — one timer shared by every view that shows the same changing value (0.8.8.6). With a menu bar on each
// display, three clocks read one ticker: one setInterval, and each bar re-renders only when the text changes.
//
//   const clock = sharedTicker((d) => formatTime(d), 1000);
//   const text = useStore(clock);          // in as many components as needed
//
// The timer runs only while someone is subscribed; the first subscriber reads the value at once (a format that
// depends on the settings is up to date when a bar appears). Tested by server/tests/menubar_model_test.mjs.
export function sharedTicker(read, ms = 1000) {
  let value, fresh = false;          // read on first use, not at import (the locale may not be loaded yet)
  let timer = 0;
  const subs = new Set();
  const tick = () => {
    const v = read(new Date());
    if (fresh && v === value) return;
    value = v;
    fresh = true;
    for (const fn of [...subs]) fn();
  };
  return {
    get: () => { if (!fresh) { value = read(new Date()); fresh = true; } return value; },
    subscribe(fn) {
      if (!subs.size) { tick(); timer = setInterval(tick, ms); }
      subs.add(fn);
      return () => {
        if (!subs.delete(fn) || subs.size) return;
        clearInterval(timer);
        timer = 0;
      };
    },
    running: () => !!timer,
  };
}

// A store that never changes: what a component subscribes to while it doesn't need a ticker (hooks can't be skipped).
export const idleStore = { get: () => 0, subscribe: () => () => {} };
