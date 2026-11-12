// minimize.js — which windows start Lord Flow's minimize effect (index.jsx useMinimizing; tests:
// server/tests/minimize_model_test.mjs). One that has just been minimized, not fullscreen and not picture in picture.
// With Reduce motion (Settings → Appearance) none starts and the window goes at once, as in Lord Classic (GitHub #112).
// prev (Map id → minimized) is what the last render saw, and it is updated here.
export function minimizeStarts(windows, prev, { reduce = false, now = Date.now() } = {}) {
  const started = [];
  for (const w of windows) {
    if (!reduce && w.minimized && prev.get(w.id) === false && !w.fullscreen && !w.pip) started.push({ id: w.id, t0: now });
    prev.set(w.id, !!w.minimized);
  }
  return started;
}
