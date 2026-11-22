export function minimizeStarts(windows, prev, { reduce } = {}) {
  const started = [];
  for (const w of windows) {
    const wasMin = prev.get(w.id);
    if (!reduce && w.minimized && wasMin === false && !w.fullscreen && !w.pip) {
      started.push({ id: w.id, t0: Date.now() });
    }
    prev.set(w.id, !!w.minimized);
  }
  const seen = new Set(windows.map((w) => w.id));
  for (const id of prev.keys()) if (!seen.has(id)) prev.delete(id);
  return started;
}
