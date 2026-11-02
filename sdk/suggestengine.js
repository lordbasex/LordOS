// suggestengine.js — the rules of the Store's "Suggested for your computer" (pure functions, no
// `lord`, tested by server/tests/suggest_engine_test.mjs). The rules are data:
// assets/store/suggestions.json; the hardware facts come from lord.hwinfo.scan() (hw/hwscan.c) and
// the package state from lord.pkg.info(). See docs/PACKAGES.md, "Suggested for your computer".
//
//   evaluate(data, facts, { info, dismissed, check })  → [Suggestion], most useful first
//   firmwareFromLog(lines)                              → [{ file, driver }] from hw-firmware-missing's LOG lines
//   fresh(list, notified)                               → the installable ones not notified yet
//
// Suggestion = { id, text, params, packages, install, state, free, component, notes, icon }
//   id        stable key ("nvidiaDriver", "printer:03f0", "firmware:firmware-realtek"), used to
//             dismiss it and to notify once
//   text      the locale key under store.suggest.* for its title and reason ("nvidiaDriver",
//             "printer", "firmware.wifi")
//   state     'install' (install lists what is missing), 'installed', or 'unavailable' (the
//             packages are not in the configured sources: the card explains why)
// Nothing here downloads, runs or sends anything.

const HEX4 = /^[0-9a-f]{4}$/;
const PKG = /^[a-z0-9][a-z0-9+.-]+$/;

const arr = (v) => (Array.isArray(v) ? v : []);
const str = (v) => (typeof v === 'string' ? v : '');

// ---- conditions ----

function matchPci(dev, want) {
  if (want.vendor && !arr(want.vendor).includes(str(dev.vendor))) return false;
  if (want.device && !arr(want.device).includes(str(dev.device))) return false;
  if (want.class && !arr(want.class).some((p) => str(dev.class).startsWith(p))) return false;
  return true;
}

// One condition of a rule's "when" against the facts.
export function matches(cond, facts) {
  if (!cond || typeof cond !== 'object') return false;
  const keys = Object.keys(cond);
  if (keys.length !== 1) return keys.length > 1 && keys.every((k) => matches({ [k]: cond[k] }, facts));
  const [k] = keys;
  const v = cond[k];
  switch (k) {
    case 'any': return arr(v).some((c) => matches(c, facts));
    case 'all': return arr(v).length > 0 && arr(v).every((c) => matches(c, facts));
    case 'dmi': return Object.entries(v || {}).every(([f, vals]) => arr(vals).includes(str(facts.dmi?.[f])));
    case 'pci': return arr(facts.pci).some((d) => matchPci(d, v || {}));
    case 'virtio': return arr(facts.virtio).some((n) => arr(v).includes(n));
    case 'compatible': return arr(facts.compatible).some((n) => arr(v).includes(n));
    case 'arch': return arr(v).includes(str(facts.arch));
    default: return false;                       // an unknown condition never matches
  }
}

// ---- package state ----

// state of a list of packages: which are known to the index, installed, and what to install.
function pkgState(names, info) {
  const known = [], missing = [], installed = [];
  for (const n of names) {
    const i = PKG.test(n) ? info(n) : null;
    if (!i) { missing.push(n); continue; }
    known.push(n);
    if (i.installed) installed.push(n);
  }
  return { known, missing, installed, install: known.filter((n) => !installed.includes(n)) };
}

// A suggestion from a list of packages: require 'all' (each must exist) or 'any' (those that exist).
function build(base, names, info, { require = 'all', installedWhen = null, ifUnavailable = 'hide', check = null } = {}) {
  const st = pkgState(names, info);
  const whenNames = installedWhen ? arr(installedWhen) : names;
  const doneByKey = installedWhen ? whenNames.every((n) => st.installed.includes(n)) : false;
  let state;
  if (doneByKey || (st.known.length && !st.install.length && (require === 'any' || !st.missing.length))) state = 'installed';
  else if (require === 'all' ? st.missing.length > 0 : st.known.length === 0) state = 'unavailable';
  else state = 'install';
  // every package exists, but the plan can't be made (a dependency in a component that isn't enabled)
  if (state === 'install' && check && !check(st.install)) state = 'unavailable';
  if (state === 'unavailable' && ifUnavailable !== 'explain') return null;
  return {
    ...base,
    packages: names,
    install: state === 'install' ? st.install : [],
    missing: st.missing.length || state !== 'unavailable' ? st.missing : st.install,
    state,
  };
}

// ---- the kinds of suggestion ----

function ruleSuggestions(data, facts, info, check) {
  const out = [];
  for (const r of arr(data.rules)) {
    if (!r || typeof r.id !== 'string' || !matches(r.when, facts)) continue;
    const s = build({
      id: r.id, text: r.id, params: {}, free: r.free !== false, component: str(r.component),
      notes: arr(r.notes), icon: str(r.icon) || 'package',
    }, arr(r.packages), info, { require: r.require, installedWhen: r.installedWhen, ifUnavailable: r.ifUnavailable, check });
    if (s) out.push(s);
  }
  return out;
}

const ifaceClass = (i) => str(i).slice(0, 2);

function usbSuggestions(data, facts, info, check) {
  const out = [];
  const vendors = data.usb?.vendors || {};
  const seen = new Set();
  let ipp = false;
  for (const d of arr(facts.usb)) {
    const vendor = str(d.vendor);
    if (!HEX4.test(vendor)) continue;
    const ifaces = arr(d.ifaces);
    const printer = ifaces.some((i) => ifaceClass(i) === '07');
    if (ifaces.includes('070104')) ipp = true;    // IPP over USB (driverless printing and scanning)
    const v = vendors[vendor];
    if (!v) continue;
    const add = (kind, names) => {
      const id = `${kind}:${vendor}`;
      if (!arr(names).length || seen.has(id)) return;
      seen.add(id);
      const s = build({ id, text: kind, params: { vendor: str(v.name) }, free: true, component: '', notes: [], icon: kind },
        arr(names), info, { check });
      if (s) out.push(s);
    };
    if (printer) add('printer', v.printer);
    if (d.sane === true) add('scanner', v.scanner);
  }
  if (ipp && arr(data.usb?.ippUsb).length) {
    const s = build({ id: 'ippUsb', text: 'ippUsb', params: {}, free: true, component: '', notes: [], icon: 'printer' },
      arr(data.usb.ippUsb), info, { check });
    if (s) out.push(s);
  }
  return out;
}

function firmwarePackage(map, file) {
  return arr(map).find((m) => m && typeof m.prefix === 'string' && m.prefix && file.startsWith(m.prefix)) || null;
}

function firmwareSuggestions(data, facts, info, check) {
  const fw = data.firmware || {};
  const groups = new Map();                       // package → { kind, files, drivers }
  for (const m of arr(facts.firmware?.missing)) {
    const file = str(m.file);
    const hit = file && firmwarePackage(fw.map, file);
    if (!hit || !PKG.test(hit.package)) continue;
    const g = groups.get(hit.package) || { kind: hit.kind, files: [], drivers: [] };
    if (!g.files.includes(file)) g.files.push(file);
    const drv = str(m.driver);
    if (drv && drv !== '-' && !g.drivers.includes(drv)) g.drivers.push(drv);
    groups.set(hit.package, g);
  }
  const out = [];
  for (const [pkg, g] of groups) {
    const i = info(pkg);
    if (i && i.installed) continue;               // installed and still missing: nothing to offer
    const kind = ['wifi', 'bluetooth', 'network', 'graphics'].includes(g.kind) ? g.kind : 'other';
    const s = build({
      id: `firmware:${pkg}`, text: `firmware.${kind}`,
      params: { package: pkg, driver: g.drivers.join(', '), files: g.files.slice(0, 3).join(', '), count: g.files.length },
      free: false, component: str(fw.component) || 'non-free-firmware', notes: ['firmware'], icon: kind,
    }, [pkg], info, { ifUnavailable: 'explain', check });
    if (s) out.push(s);
  }
  return out;
}

const ORDER = { setup: 0, install: 0, unavailable: 1, installed: 2 };

// Every suggestion for these facts. info(name) → lord.pkg.info(name) (null: not in the index);
// check(names) → whether an install plan can be made (lord.pkg.plan(...).ok), optional.
export function evaluate(data, facts, { info = () => null, dismissed = [], check = null } = {}) {
  if (!data || !facts) return [];
  const no = new Set(arr(dismissed));
  // a case found on the board: nothing to install, one setting to keep (facts.caseFound, sdk/suggestions.js)
  const found = facts.caseFound === 'argon-one-v5'
    ? [{ id: 'argonCase', text: 'argonCase', params: {}, packages: [], install: [], state: 'setup', free: true, component: '', notes: [], icon: 'board' }] : [];
  const all = [...found, ...ruleSuggestions(data, facts, info, check), ...usbSuggestions(data, facts, info, check), ...firmwareSuggestions(data, facts, info, check)];
  return all.filter((s) => !no.has(s.id))
    .map((s, i) => [s, i]).sort((a, b) => ORDER[a[0].state] - ORDER[b[0].state] || a[1] - b[1]).map(([s]) => s);
}

// hw-firmware-missing's progress lines: "fw <file> <driver|->" (the root side already validated them).
export function firmwareFromLog(lines) {
  const out = [];
  for (const l of arr(lines)) {
    const m = /^fw ([A-Za-z0-9._+,\/-]{1,199}) ([A-Za-z0-9_-]{1,32})$/.exec(str(l));
    if (m && !m[1].includes('..') && !m[1].startsWith('/')) out.push({ file: m[1], driver: m[2] === '-' ? '' : m[2] });
  }
  return out;
}

// The installable suggestions not notified yet (the badge and the one-time notification).
export function fresh(list, notified = []) {
  const seen = new Set(arr(notified));
  return arr(list).filter((s) => (s.state === 'install' || s.state === 'setup') && !seen.has(s.id));
}
