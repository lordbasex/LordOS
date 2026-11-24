// vmiomodel.js — a running machine's own disk and network numbers on its row in Processes (docs/VIRTUALIZATION.md 6.14),
// pure logic (server/tests/monitor_vmio_test.mjs).
//
// Disk: the bytes read and written by the machine's QEMU, from its cgroup's io.stat (lord.vm.stats(dir).ioBytes, a
// counter: the rate is the difference of two reads over their time, `at`, lord-server's monotonic milliseconds). Network:
// what lord-netperm counts for the machine's scope (firewallState counts["lordos:vm-<id>"]: [allowed, denied, rx, tx,
// rxRate, txRate, …]), the same numbers as the machine's own row in Monitor → Network. Either is null when it isn't known
// (the machine isn't in its cgroup, this isn't a LordOS session, the firewall counts no bytes): the row then says nothing
// rather than a zero it didn't measure.

export const VM_IO_EVERY_MS = 2000;     // how often the rows' machines are read (lord.vm.stats asks lord-vm over its socket)

const num = (v) => (typeof v === 'number' && Number.isFinite(v) && v >= 0 ? v : null);

// One step for one machine: prev the last { at, io } (or null), s what lord.vm.stats(dir) returned. → { at, io, disk }:
// disk the bytes per second since prev, null on the first read, a counter that went back (the machine started again) or
// no counter at all.
export function diskStep(prev, s) {
  const at = num(s?.at), io = num(s?.ioBytes);
  if (at == null || io == null) return { at: null, io: null, disk: null };
  if (!prev || prev.at == null || prev.io == null || at <= prev.at || io < prev.io) return { at, io, disk: null };
  return { at, io, disk: ((io - prev.io) * 1000) / (at - prev.at) };
}

// The machine's network rates from the firewall's counts: { rx, tx } bytes per second, or null when not counted.
export function netOf(counts, id) {
  if (!counts || typeof id !== 'string' || !id) return null;
  const v = counts[`lordos:vm-${id}`];
  if (!Array.isArray(v)) return null;
  const rx = num(v[4]), tx = num(v[5]);
  return rx == null && tx == null ? null : { rx: rx || 0, tx: tx || 0 };
}

// The ids of the machines among the table's groups (the scope's app: desktop:lordos-vm-<id> or desktop:lordos-vm-tor-<id>)
const VM_APP = /^desktop:lordos-vm-(?:tor-)?([0-9a-f-]{1,36})$/;
export const machineIdOf = (app) => VM_APP.exec(app || '')?.[1] || null;
export function machineIds(groups) {
  const out = [];
  for (const g of groups || []) {
    const id = g && !g.gone && g.kind === 'app' ? machineIdOf(g.item?.app) : null;
    if (id && !out.includes(id)) out.push(id);
  }
  return out;
}

// A sweep over the machines on screen: dirOf(id) → the machine's folder (or null), stats(dir) → lord.vm.stats. prev and
// the result: Map id → { at, io, disk }; machines no longer shown are dropped.
export function sweep(prev, ids, dirOf, stats) {
  const out = new Map();
  for (const id of ids) {
    const dir = dirOf(id);
    if (!dir) continue;
    let s = null;
    try { s = stats(dir); } catch (e) { s = null; }
    out.set(id, diskStep(prev?.get(id) || null, s));
  }
  return out;
}
