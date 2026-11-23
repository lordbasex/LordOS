// sentinela scenes: native calls the SDK modules make while loading, before the scene sets its own
const L = globalThis.lord;
const ns = (o, fns) => { L[o] = L[o] || {}; for (const [k, v] of Object.entries(fns)) if (typeof L[o][k] !== 'function') L[o][k] = v; };
ns('sound', { state: () => ({}), devices: () => [] });
ns('keyboard', { current: () => ({}), layouts: () => [] });
ns('input', { configure: () => [] });
// any other native call of a namespace the stub lord has: a no-op (this scene draws one page; the rest isn't checked)
for (const k of Object.keys(L)) {
  const o = L[k];
  if (!o || typeof o !== 'object' || Array.isArray(o)) continue;
  L[k] = new Proxy(o, { get: (t, p) => { const v = t[p]; return v !== undefined || typeof p !== 'string' ? v : () => []; } });
}
const noop = () => new Proxy({}, { get: (t, p) => (p in t ? t[p] : typeof p === 'string' ? () => [] : undefined) });
globalThis.lord = new Proxy(L, { get: (t, p) => { const v = t[p]; if (v !== undefined || typeof p !== 'string') return v; t[p] = noop(); return t[p]; } });
