// vmspace.js — the machines' space in Cleaner's Storage view (pure; server/tests/vm_model_test.mjs).
// Is path the Machines library (~/Machines), in it, or a machine's or a machine group's folder anywhere (the same places
// clean_machine_path refuses)? Then its space is reclaimed in Machines' Storage window, not here.
export function machinesSpace(path, home) {
  const p = String(path || '').replace(/\/+$/, '');
  if (!p) return false;
  if (home && (p === `${home}/Machines` || p.startsWith(`${home}/Machines/`))) return true;
  return p.split('/').some((seg) => /\.lord(vm|group)$/.test(seg));
}
