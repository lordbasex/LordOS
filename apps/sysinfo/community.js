// community.js — community data in the Compatibility panel (0.8.7, docs/BENCHMARK.md "Compatibility"): which
// devices of this machine other LordOS users confirmed, from the catalogue's signed snapshot
// (lord.sysinfo.community(): verified in C, server/src/sysreport/hwsnap.c, before JS sees it). Pure functions,
// tested by server/tests/hwreport_test.mjs; tools/hwreport/catalog.mjs builds the snapshot with the same keys.
//
// A device is named by a key that holds no identity of the machine: PCI ids, a driver name or a product
// name. Only entries for the same LordOS version (x.y.z, the channel ignored) are shown, as the design asks.

export const SNAPSHOT_FORMAT = 'lordos-hwsnapshot';
// the Compatibility rows a machine-wide key answers for (the GPU, network, Wi-Fi, Bluetooth and audio rows
// have keys of their own)
export const MACHINE_COMPONENTS = ['display', 'storage', 'power', 'suspend'];
export const COMPONENTS = ['gpu', 'display', 'storage', 'network', 'wifi', 'bluetooth', 'audio', 'power', 'isolation', 'suspend'];

const clean = (s, max = 96) => String(s || '').replace(/[\u0000-\u001f\u007f]+/g, ' ').replace(/\s+/g, ' ').trim().slice(0, max);
const hex = (s) => (/^[0-9a-fA-F]{1,8}$/.test(String(s || '')) ? String(s).toLowerCase() : '');
const word = (s) => (/^[A-Za-z0-9._+-]{1,40}$/.test(String(s || '')) ? String(s) : '');

// The devices of a report's `system` (lord.sysinfo.report() or a shared report's system: same fields), each
// as { key, component, name }. component is the Compatibility row it answers for.
export function deviceKeys(system) {
  const s = system || {};
  const out = [];
  const seen = new Set();
  const add = (key, component, name) => {
    const id = `${component} ${key}`;
    if (!key || seen.has(id)) return;
    seen.add(id);
    out.push({ key, component, name: clean(name) });
  };
  const m = s.machine || {};
  const mname = clean([m.vendor, m.product].filter(Boolean).join(' ') || m.model);
  const mkey = m.vendor && m.product ? `machine:${clean(m.vendor, 48)}|${clean(m.product, 64)}` : m.model ? `model:${clean(m.model, 96)}` : '';
  if (mkey && !m.virtualization) for (const c of MACHINE_COMPONENTS) add(mkey, c, mname);
  for (const g of s.gpus || []) {
    if (hex(g.vendorId) && hex(g.deviceId)) add(`pci:${hex(g.vendorId)}:${hex(g.deviceId)}`, 'gpu', [g.vendor, g.device].filter(Boolean).join(' '));
    else if (word(g.driver)) add(`gpu:${word(g.driver)}`, 'gpu', [g.vendor, g.device].filter(Boolean).join(' ') || g.driver);
  }
  for (const n of s.network || []) {
    if (!word(n.driver)) continue;
    if (n.kind === 'wifi') add(`wifi:${word(n.driver)}`, 'wifi', n.driver);
    else if (n.kind === 'ethernet') add(`net:${word(n.driver)}`, 'network', n.driver);
  }
  for (const b of s.bluetooth || []) if (word(b.driver)) add(`bt:${word(b.driver)}`, 'bluetooth', b.driver);
  for (const a of s.audio || []) if (clean(a)) add(`audio:${clean(a, 64)}`, 'audio', a);
  return out;
}

const base = (v) => { const m = /^(\d+\.\d+\.\d+(?:\.\d+)?)/.exec(String(v || '')); return m ? m[1] : ''; };

// For each Compatibility row, what the snapshot says about this machine's devices with this LordOS version:
// { [component]: [{ name, works, limits, no, reports, references }] }. A snapshot that isn't one (wrong format
// or version) gives nothing.
export function communityFor(snapshot, system, lordos) {
  const out = {};
  const sn = snapshot && typeof snapshot === 'object' ? snapshot : null;
  if (!sn || sn.format !== SNAPSHOT_FORMAT || sn.version !== 1 || !Array.isArray(sn.devices)) return out;
  const v = base(lordos);
  if (!v) return out;
  const index = new Map();
  for (const d of sn.devices) {
    if (!d || typeof d.key !== 'string' || typeof d.component !== 'string' || base(d.lordos) !== v) continue;
    index.set(`${d.component} ${d.key}`, d);
  }
  for (const dev of deviceKeys(system)) {
    const d = index.get(`${dev.component} ${dev.key}`);
    if (!d) continue;
    const n = (x) => (Number.isInteger(x) && x >= 0 ? x : 0);
    (out[dev.component] = out[dev.component] || []).push({
      name: dev.name, works: n(d.works), limits: n(d.limits), no: n(d.no), reports: n(d.reports), references: n(d.references),
    });
  }
  return out;
}

// One line per Compatibility row: the counts added over this machine's devices for that row, and whether
// they come only from the project's own reference machines. null when nobody reported it.
export function communitySummary(entries) {
  if (!entries || !entries.length) return null;
  const sum = { works: 0, limits: 0, no: 0, reports: 0, references: 0 };
  for (const e of entries) for (const k of Object.keys(sum)) sum[k] += e[k];
  if (!sum.reports) return null;
  const state = sum.no > sum.works + sum.limits ? 'no' : sum.limits > sum.works ? 'limits' : 'works';
  return { ...sum, state, onlyReferences: sum.references === sum.reports };
}
