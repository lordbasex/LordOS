// store.js — minimal global state with subscription, to use from React with useStore().
import { useSyncExternalStore } from 'react';

// a subscriber's error is reported after every subscriber ran (in a job of its own, so set() itself doesn't throw)
function reportSubscriberError(e) {
  try { if (typeof console !== 'undefined') console.warn('store: a subscriber failed: ' + (e && e.message ? e.message : String(e))); } catch (err) { /* nothing to report with */ }
}

export function createStore(initial) {
  let state = initial;
  const subs = new Set();
  return {
    get: () => state,
    set(patch) {
      // function: new state; object over object: merged; null or another value: replaces
      const next = typeof patch === 'function' ? patch(state)
        : patch !== null && typeof patch === 'object' && state !== null && typeof state === 'object' && !Array.isArray(patch) ? { ...state, ...patch }
          : patch;
      if (next === state) return;
      state = next;
      // each subscriber on its own: one that throws (a native call refused while a community theme's code is on the
      // stack, js_theme.c) must not keep the others from hearing the change
      let failure = null;
      for (const fn of subs) { try { fn(); } catch (e) { if (!failure) failure = e; } }
      if (failure) reportSubscriberError(failure);
    },
    subscribe(fn) { subs.add(fn); return () => subs.delete(fn); },
  };
}

export function useStore(store, select = (s) => s) {
  return useSyncExternalStore(store.subscribe, () => select(store.get()));
}
