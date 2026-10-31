// oldkernels.js — which installed kernels Cleaner may offer to remove (docs/CLEANER.md). Only the choice lives here:
// removing goes through the package engine, with lord.pkg.plan shown first and the admin helper's pkg-remove, as the
// Store does. Kept: the running kernel, the newest, and the newest one below the running kernel as a way back. Only
// packages of the running kernel's own flavour are looked at, and with no running kernel to compare nothing is offered.

const parts = (name) => {
  const m = /^linux-image-(\d+\.\d+\.\d+(?:-\d+)?(?:\+[a-z0-9.]+)?)-([a-z0-9][a-z0-9-]*)$/.exec(name);
  return m ? { name, version: m[1], flavour: m[2] } : null;
};

const numbers = (v) => v.split(/\D+/).filter(Boolean).map(Number);
function cmp(a, b) {
  const x = numbers(a), y = numbers(b);
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    const d = (x[i] || 0) - (y[i] || 0);
    if (d) return d;
  }
  return 0;
}

// installed: lord.pkg.installed(); release: the running kernel's release. The packages that may go, oldest first.
export function oldKernels(installed, release) {
  const run = parts(`linux-image-${release || ''}`);
  if (!run) return [];
  const same = installed.map((p) => ({ ...p, k: parts(p.name) })).filter((p) => p.k && p.k.flavour === run.flavour);
  if (!same.some((p) => p.name === run.name)) return [];
  same.sort((a, b) => cmp(a.k.version, b.k.version));
  const below = same.filter((p) => cmp(p.k.version, run.version) < 0);
  return below.slice(0, -1).map(({ k, ...p }) => p);
}

// A plan is shown only if it stays within kernel packages and leaves the running one alone.
export function kernelPlanOk(plan, release) {
  if (!plan || !plan.ok || !plan.items.length) return false;
  return plan.items.every((i) => i.action === 'remove' && /^linux-(image|headers|modules|kbuild|support)-\d/.test(i.name) && !i.name.includes(release));
}
