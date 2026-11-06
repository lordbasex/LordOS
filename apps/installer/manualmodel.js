// The installer's manual partitioning, as data: what each partition may become, the free space left,
// the checks the person sees before going on and the fields install-run gets. No SDK and no text here
// (tests/installer_manual_test.mjs). The root side checks everything again, from the disk itself
// (server/src/install/manual.c): this only keeps the wizard from offering what it would refuse.

const MIB = 1024 * 1024, GIB = 1024 * MIB;
export const LIMITS = { root: 8 * GIB, home: GIB, swapMin: 256 * MIB, swapMax: 64 * GIB, espNew: 100 * MIB, espFormat: 64 * MIB };
export const HOME_FS = ['ext4', 'ext3', 'ext2', 'xfs', 'btrfs'];
export const ROLES = ['root', 'home', 'swap', 'esp'];
// what an existing partition can be turned into, and the role each action takes
export const ACTION_ROLE = { keep: null, delete: null, root: 'root', home: 'home', homeFormat: 'home', swap: 'swap', esp: 'esp', espFormat: 'esp' };

export const emptyManual = () => ({ acts: {}, adds: [] });

const bytesOf = (disk, s, e) => (e - s + 1) * disk.lsec;
const align = (disk) => MIB / disk.lsec;
const partsOf = (disk) => (disk.parts || []).filter((p) => Number.isInteger(p.num) && p.num > 0);

// The actions offered for one existing partition (the order of its menu).
export function actionsFor(p) {
  if (p.esp) return ['keep', 'esp', 'espFormat', 'delete'];
  return ['keep', 'root', ...(HOME_FS.includes(p.fs) ? ['home'] : []), 'homeFormat', 'swap', 'delete'];
}

// One role in one place: giving it to a partition takes it from any other, new ones included.
function withoutRole(m, role) {
  const acts = {};
  for (const [n, a] of Object.entries(m.acts)) if (ACTION_ROLE[a] !== role) acts[n] = a;
  return { acts, adds: m.adds.filter((a) => a.role !== role) };
}

export function setAction(m, num, act) {
  if (!(act in ACTION_ROLE)) return m;
  const role = ACTION_ROLE[act];
  const base = role ? withoutRole(m, role) : { acts: { ...m.acts }, adds: m.adds };
  if (act === 'keep') delete base.acts[num]; else base.acts[num] = act;
  return base;
}

export function addPartition(m, role, start, end) {
  if (!ROLES.includes(role)) return m;
  const base = withoutRole(m, role);
  return { acts: base.acts, adds: [...base.adds, { role, start, end }].sort((a, b) => a.start - b.start) };
}

export const removeAdded = (m, i) => ({ acts: m.acts, adds: m.adds.filter((_, k) => k !== i) });

// Unpartitioned space once the deletions and the new partitions are counted: aligned to 1 MiB,
// at least 1 MiB, in disk order.
export function freeGaps(disk, m) {
  const al = align(disk);
  const used = [
    ...partsOf(disk).filter((p) => m.acts[p.num] !== 'delete').map((p) => [p.start, p.end]),
    ...m.adds.map((a) => [a.start, a.end]),
  ].sort((a, b) => a[0] - b[0]);
  const gaps = [];
  let cur = disk.first;
  const consider = (from, to) => {
    const s = Math.ceil(from / al) * al, e = Math.floor((to + 1) / al) * al - 1;
    if (e >= s) gaps.push({ start: s, end: e, bytes: bytesOf(disk, s, e) });
  };
  for (const [s, e] of used) { if (s > cur) consider(cur, s - 1); cur = Math.max(cur, e + 1); }
  if (cur <= disk.last) consider(cur, disk.last);
  return gaps;
}

// A new partition of `mib` MiB at the start of a gap (all of it when mib is 0 or more than it has).
export function placeIn(disk, gap, mib) {
  const al = align(disk);
  const want = Math.round(mib) > 0 ? Math.round(mib) * al : Infinity;
  const end = Math.min(gap.end, gap.start + want - 1);
  return { start: gap.start, end: Math.floor((end + 1) / al) * al - 1 };
}

// Partitions, new partitions and free space, in disk order, for the screen.
export function rows(disk, m) {
  const out = [
    ...partsOf(disk).map((p) => ({ kind: 'part', start: p.start, p, act: m.acts[p.num] || 'keep', bytes: bytesOf(disk, p.start, p.end) })),
    ...m.adds.map((a, i) => ({ kind: 'new', start: a.start, a, index: i, bytes: bytesOf(disk, a.start, a.end) })),
    ...freeGaps(disk, m).map((g) => ({ kind: 'free', start: g.start, gap: g, bytes: g.bytes })),
  ];
  return out.sort((x, y) => x.start - y.start || (x.kind === 'part' ? -1 : 1));
}

// What the plan does, the problems left (error codes the root side also uses) and the fields of
// install-run. `swapFile` is the swap file's MiB.
export function review(disk, m, swapFile) {
  const parts = partsOf(disk);
  const byNum = Object.fromEntries(parts.map((p) => [p.num, p]));
  const where = {};
  for (const [n, a] of Object.entries(m.acts)) if (ACTION_ROLE[a] && byNum[n]) where[ACTION_ROLE[a]] = { part: byNum[n], act: a, bytes: bytesOf(disk, byNum[n].start, byNum[n].end) };
  for (const a of m.adds) where[a.role] = { add: a, bytes: bytesOf(disk, a.start, a.end) };
  const deleted = parts.filter((p) => m.acts[p.num] === 'delete');
  const errors = [];
  if (!where.root) errors.push('install.manual.noRoot');
  else if (where.root.bytes < LIMITS.root) errors.push('install.manual.rootTooSmall');
  if (!where.esp) errors.push('install.manual.noEsp');
  else if (where.esp.add && where.esp.bytes < LIMITS.espNew) errors.push('install.manual.espTooSmall');
  else if (where.esp.act === 'espFormat' && where.esp.bytes < LIMITS.espFormat) errors.push('install.manual.espTooSmall');
  if (where.home?.add && where.home.bytes < LIMITS.home) errors.push('install.manual.homeTooSmall');
  if (where.swap && (where.swap.bytes < LIMITS.swapMin || where.swap.bytes > LIMITS.swapMax)) errors.push('install.manual.swapSize');
  if (where.swap && swapFile > 0) errors.push('install.manual.twoSwaps');

  // every change, in the order it happens
  const changes = [
    ...deleted.map((p) => ({ kind: 'delete', part: p })),
    ...m.adds.map((a) => ({ kind: 'create', role: a.role, bytes: bytesOf(disk, a.start, a.end) })),
    ...parts.filter((p) => ['root', 'homeFormat', 'swap', 'espFormat'].includes(m.acts[p.num])).map((p) => ({ kind: 'format', act: m.acts[p.num], part: p })),
    ...parts.filter((p) => ['home', 'esp'].includes(m.acts[p.num])).map((p) => ({ kind: 'reuse', act: m.acts[p.num], part: p })),
  ];
  // anything that loses data already on the disk asks for the disk's name too
  const destructive = changes.some((c) => c.kind === 'delete' || c.kind === 'format');

  const num = (r) => (where[r]?.part ? where[r].part.num : 0);
  const fields = {
    mode: 'manual', tableTag: disk.tableTag,
    delete: deleted.map((p) => p.num).join(','),
    create: m.adds.map((a) => `${a.role}:${a.start}:${a.end}`).join(','),
    rootPart: num('root'), homePart: num('home'), swapPart: num('swap'), espPart: num('esp'),
    homeFormat: where.home?.act === 'homeFormat', espFormat: where.esp?.act === 'espFormat',
  };
  return { errors, changes, destructive, fields, where };
}
