// netmodel.js — Monitor → Network's list, second generation (0.8.5, docs/MONITOR.md): search tokens,
// filters, grouping into a tree and sorting. Pure functions, no lord.* (tests/netmodel_test.mjs).
//
// The input is a flat list of flows, one per (app, destination, port):
//   { app, appName, name, addr, port, proto: 'tcp'|'udp'|'other', dir: 'out'|'in', rx, tx, rateRx, rateTx,
//     cc, scope, conns, denied, last, listed, system }
//   name: the name the system resolver gave for addr (or null), names: all of them (a CNAME chain);
//   last: ms of the last activity;
//   listed: the id of a subscribed blocklist that covers it (or null); system: an essential system
//   service (name resolution, time, the network service), shown but never blockable.
//
//   const q = parseSearch('app:firefox port:443 video');   // { chips: [{type:'app',value:'firefox'},…], rest: 'video' }
//   const shown = filterFlows(flows, { dir: 'all', proto: 'all', status: 'all' }, q, { hasRule });
//   const tree = sortTree(groupFlows(shown, 'app'), 'most');
//   // [{ key, kind: 'app'|'domain'|'ip'|'port'|'country', label, rx, tx, rateRx, rateTx, conns, denied,
//   //    last, flows, children: [...] }]

// LordOS's own parts that use the network, each a row of its own (0.8.5): label key, the built-in app
// whose icon it shows (or a glyph), whether the firewall can block it, and what blocking it means.
//   lordos:<name>  in its own scope (sec/netconn.h): the kernel enforces its rules
//   system:<name>  an essential system service: shown, never blocked
//   lordos:vpn     the VPN tunnels (their interfaces' counters): shown, managed in Settings → Network
export const COMPONENTS = {
  'lordos:mail': { label: 'monitor.comp.mail', icon: 'mail', hint: 'monitor.comp.mailHint' },
  'lordos:store': { label: 'monitor.comp.store', icon: 'store', hint: 'monitor.comp.storeHint', warn: 'monitor.comp.storeWarn' },
  'lordos:print': { label: 'monitor.comp.print', glyph: 'printer', hint: 'monitor.comp.printHint' },
  'lordos:terminal': { label: 'monitor.comp.terminal', icon: 'terminal', hint: 'monitor.comp.terminalHint' },
  'lordos:lan': { label: 'monitor.comp.lan', glyph: 'network', hint: 'monitor.comp.lanHint' },
  'lordos:sql': { label: 'monitor.comp.sql', glyph: 'database', hint: 'monitor.comp.sqlHint' },
  'lordos:lists': { label: 'monitor.comp.lists', glyph: 'shield', hint: 'monitor.comp.listsHint' },
  'lordos:machines': { label: 'monitor.comp.machines', icon: 'machines', hint: 'monitor.comp.machinesHint' },
  'lordos:vpn': { label: 'monitor.comp.vpn', glyph: 'tunnel', hint: 'monitor.comp.vpnHint', system: true },
  'system:dns': { label: 'monitor.comp.dns', glyph: 'dns', hint: 'monitor.comp.dnsHint', system: true },
  'system:time': { label: 'monitor.comp.time', glyph: 'clock', hint: 'monitor.comp.timeHint', system: true },
  'system:network': { label: 'monitor.comp.network', glyph: 'network', hint: 'monitor.comp.networkHint', system: true },
};
export const isSystemId = (id) => !!COMPONENTS[id]?.system || /^system:/.test(id || '');

// (0.8.8.6) A running virtual machine's traffic is the Machines component's: its QEMU and passt run in
// app-lordos-lordos-vm-<id>.scope, which lord-netperm and Monitor (fw_scope_app) both read as
// lordos:machines, so the Machines rule decides for the guest. conn.c adds the machine's id ("cgSub");
// the row names the machines: "Machines · Ubuntu 24.04". ids: the ids seen, names: { id: name }.
// Which row a connection of conn.c's list belongs to: a socket made in an app's scope is that app's
// ("cgApp"), whoever holds it (Mail's connector's are Mail's, a guest's are Machines'); otherwise
// lord-server's own, then the app, the executable or the process name.
export const connAppKey = (c) => c.cgApp || (c.self ? 'lordos' : c.app || c.exe || c.name || '?');

export function machinesLabel(base, ids, names = {}) {
  const list = [...new Set([...(ids || [])].map((id) => names?.[id]).filter((s) => typeof s === 'string' && s))];
  list.sort((a, b) => a.localeCompare(b));
  return list.length ? `${base} · ${list.join(', ')}` : base;
}

export const CHIP_TYPES = ['app', 'domain', 'ip', 'port', 'proto', 'country'];
const ALIASES = { host: 'domain', name: 'domain', addr: 'ip', address: 'ip', cidr: 'ip', protocol: 'proto', cc: 'country' };
export const GROUPS = ['app', 'domain', 'ip', 'port', 'country'];
export const SORTS = ['name', 'most', 'least', 'recent', 'conns'];

// ---- addresses ----

function v4(s) {
  const p = s.split('.');
  if (p.length !== 4) return null;
  const b = p.map((x) => (/^\d{1,3}$/.test(x) ? Number(x) : -1));
  return b.every((x) => x >= 0 && x <= 255) ? b : null;
}

function v6(s) {
  if (!/^[0-9a-fA-F:.]+$/.test(s) || !s.includes(':')) return null;
  let tail = [];
  let t = s;
  const m = /^(.*:)(\d+\.\d+\.\d+\.\d+)$/.exec(s);
  if (m) { const b = v4(m[2]); if (!b) return null; tail = b; t = m[1].endsWith('::') ? m[1] : m[1].slice(0, -1); }
  const halves = t.split('::');
  if (halves.length > 2) return null;
  const words = (x) => (x ? x.split(':') : []);
  const a = words(halves[0]), c = halves.length === 2 ? words(halves[1]) : [];
  const need = 8 - tail.length / 2;
  if (halves.length === 1 && a.length !== need) return null;
  if (a.length + c.length > need) return null;
  const all = [...a, ...Array(halves.length === 2 ? need - a.length - c.length : 0).fill('0'), ...c];
  const out = [];
  for (const w of all) {
    if (!/^[0-9a-fA-F]{1,4}$/.test(w)) return null;
    const n = parseInt(w, 16);
    out.push(n >> 8, n & 255);
  }
  return [...out, ...tail];
}

// "192.0.2.7", "10.0.0.0/8", "2001:db8::/32" → { bytes, prefix } | null
export function parseCidr(s) {
  if (typeof s !== 'string' || !s) return null;
  const [ip, pre, extra] = s.split('/');
  if (extra !== undefined) return null;
  const b = v4(ip) || v6(ip);
  if (!b) return null;
  const max = b.length * 8;
  const prefix = pre === undefined ? max : /^\d{1,3}$/.test(pre) ? Number(pre) : -1;
  if (prefix < 0 || prefix > max) return null;
  return { bytes: b, prefix };
}

// is addr (text) inside cidr ({ bytes, prefix })? IPv4-mapped IPv6 addresses count as IPv4.
export function cidrContains(c, addr) {
  let b = v4(addr) || v6(addr);
  if (!b || !c) return false;
  if (b.length === 16 && c.bytes.length === 4 && b.slice(0, 10).every((x) => x === 0) && b[10] === 255 && b[11] === 255) b = b.slice(12);
  if (b.length !== c.bytes.length) return false;
  for (let i = 0; i < c.prefix; i++) {
    const bit = (x) => (x[i >> 3] >> (7 - (i & 7))) & 1;
    if (bit(b) !== bit(c.bytes)) return false;
  }
  return true;
}

// ---- search ----

// What a token typed without a "type:" means: an address or network, a port, a protocol, a
// two-letter country code (upper case), a name with a dot (a domain) or else an app.
export function chipFor(token) {
  const s = String(token || '').trim();
  if (!s) return null;
  const m = /^([a-z]+):(.+)$/i.exec(s);
  if (m && !parseCidr(s)) {
    const type = ALIASES[m[1].toLowerCase()] || m[1].toLowerCase();
    if (!CHIP_TYPES.includes(type)) return null;
    return normChip({ type, value: m[2] });
  }
  if (parseCidr(s)) return normChip({ type: 'ip', value: s });
  if (/^\d{1,5}$/.test(s) && Number(s) >= 1 && Number(s) <= 65535) return { type: 'port', value: String(Number(s)) };
  if (/^(tcp|udp|quic)$/i.test(s)) return normChip({ type: 'proto', value: s });
  if (/^[A-Z]{2}$/.test(s)) return { type: 'country', value: s };
  if (s.includes('.')) return normChip({ type: 'domain', value: s });
  return { type: 'app', value: s };
}

function normChip(c) {
  const v = String(c.value).trim();
  if (!v) return null;
  switch (c.type) {
    case 'ip': return parseCidr(v) ? { type: 'ip', value: v.toLowerCase() } : null;
    case 'port': return /^\d{1,5}$/.test(v) && Number(v) >= 1 && Number(v) <= 65535 ? { type: 'port', value: String(Number(v)) } : null;
    case 'proto': { const p = v.toLowerCase() === 'quic' ? 'udp' : v.toLowerCase(); return p === 'tcp' || p === 'udp' ? { type: 'proto', value: p } : null; }
    case 'country': return /^[A-Za-z]{2}$/.test(v) ? { type: 'country', value: v.toUpperCase() } : { type: 'country', value: v };
    case 'domain': return { type: 'domain', value: v.toLowerCase().replace(/^\*\./, '').replace(/\.$/, '') };
    default: return { type: c.type, value: v };
  }
}

// The text of the search field: every "type:value" token becomes a chip, the rest is free text
// (also turned into chips when commit is true: the user pressed Enter).
export function parseSearch(text, commit = false) {
  const chips = [];
  const rest = [];
  for (const tok of String(text || '').split(/\s+/).filter(Boolean)) {
    const typed = /^([a-z]+):(.+)$/i.test(tok) && !parseCidr(tok);
    const c = typed || commit ? chipFor(tok) : null;
    if (c) chips.push(c); else rest.push(tok);
  }
  return { chips, rest: rest.join(' ') };
}

// Adds chips without repeating one.
export function addChips(list, more) {
  const out = [...(list || [])];
  for (const c of more || []) if (c && !out.some((x) => x.type === c.type && x.value === c.value)) out.push(c);
  return out;
}

const lc = (s) => String(s || '').toLowerCase();

function chipMatches(f, c, countryName) {
  switch (c.type) {
    case 'app': return lc(f.appName).includes(lc(c.value)) || lc(f.app).includes(lc(c.value));
    case 'domain': {
      const v = lc(c.value);
      return (f.names && f.names.length ? f.names : [f.name]).some((x) => { const n = lc(x); return !!n && (n === v || n.endsWith(`.${v}`) || (!v.includes('.') && n.includes(v))); });
    }
    case 'ip': { const x = parseCidr(c.value); return !!f.addr && (x ? cidrContains(x, f.addr) : lc(f.addr).startsWith(lc(c.value))); }
    case 'port': return Number(f.port) === Number(c.value);
    case 'proto': return f.proto === c.value;
    case 'country': return c.value.length === 2 ? f.cc === c.value : lc(countryName?.(f.cc)).includes(lc(c.value));
    default: return true;
  }
}

// Every chip type must match (two chips of the same type: either one); the free text anywhere.
export function matchesSearch(f, { chips = [], rest = '' } = {}, countryName) {
  const byType = new Map();
  for (const c of chips) byType.set(c.type, [...(byType.get(c.type) || []), c]);
  for (const list of byType.values()) if (!list.some((c) => chipMatches(f, c, countryName))) return false;
  if (rest) {
    const hay = lc([f.appName, f.app, f.name, ...(f.names || []), f.addr, f.port, f.proto, f.cc, countryName?.(f.cc)].join(' '));
    for (const w of lc(rest).split(/\s+/)) if (w && !hay.includes(w)) return false;
  }
  return true;
}

// filters: { dir: 'all'|'in'|'out', proto: 'all'|'tcp'|'udp', status: 'all'|'allowed'|'denied'|'rules' }
// ctx: { hasRule(flow) → bool, countryName(cc) → text }
export function filterFlows(flows, filters = {}, search = {}, ctx = {}) {
  const { dir = 'all', proto = 'all', status = 'all' } = filters;
  return (flows || []).filter((f) => {
    if (dir !== 'all' && f.dir !== dir) return false;
    if (proto !== 'all' && f.proto !== proto) return false;
    if (status === 'allowed' && (f.denied || f.blocked)) return false;
    if (status === 'denied' && !(f.denied || f.blocked)) return false;
    if (status === 'rules' && !(ctx.hasRule && ctx.hasRule(f))) return false;
    return matchesSearch(f, search, ctx.countryName);
  });
}

// ---- the tree ----

const portLabel = (f) => (f.port ? `${f.proto === 'other' ? '' : f.proto.toUpperCase()} ${f.port}`.trim() : f.proto.toUpperCase());
const LEVELS = {
  app: ['app', 'dest', 'ip', 'port'],
  domain: ['dest', 'app', 'port'],
  ip: ['ip', 'app', 'port'],
  port: ['port', 'app', 'dest'],
  country: ['country', 'app', 'dest'],
};

function keyOf(f, level) {
  if (!f.addr && (level === 'dest' || level === 'ip' || level === 'country' || (level === 'port' && !f.port))) return null;   /* an app with no destination yet */
  switch (level) {
    case 'app': return { key: `a:${f.app}`, kind: 'app', label: f.appName || f.app, app: f.app };
    case 'dest': return f.name ? { key: `d:${f.name}`, kind: 'domain', label: f.name, name: f.name } : { key: `i:${f.addr}`, kind: 'ip', label: f.addr, addr: f.addr };
    case 'ip': return { key: `i:${f.addr}`, kind: 'ip', label: f.addr, addr: f.addr };
    case 'port': return { key: `p:${f.proto}/${f.port}/${f.dir}`, kind: 'port', label: portLabel(f), port: f.port, proto: f.proto, dir: f.dir };
    case 'country': return { key: `c:${f.cc || f.scope || '?'}`, kind: 'country', label: f.cc || f.scope || '?', cc: f.cc || '', scope: f.scope || '' };
    default: return null;
  }
}

function node(k) {
  return { ...k, rx: 0, tx: 0, rateRx: 0, rateTx: 0, conns: 0, denied: 0, blocked: 0, last: 0, listed: null, system: false, flows: [], children: [] };
}

function add(n, f) {
  n.rx += f.rx || 0;
  n.tx += f.tx || 0;
  n.rateRx += f.rateRx || 0;
  n.rateTx += f.rateTx || 0;
  n.conns += f.conns || 0;
  n.denied += f.denied || 0;
  n.blocked += f.blocked || 0;
  if ((f.last || 0) > n.last) n.last = f.last;
  if (f.listed && !n.listed) n.listed = f.listed;
  if (f.system) n.system = true;
  n.flows.push(f);
}

function build(flows, levels) {
  const [level, ...rest] = levels;
  if (!level) return [];
  const map = new Map();
  for (const f of flows) {
    const k = keyOf(f, level);
    if (!k) continue;
    const n = map.get(k.key) || map.set(k.key, node(k)).get(k.key);
    add(n, f);
  }
  const out = [...map.values()];
  for (const n of out) {
    let next = rest;
    // an address level only under a domain row, and only when that name has several addresses
    if (next[0] === 'ip' && (n.kind !== 'domain' || new Set(n.flows.map((f) => f.addr)).size < 2)) next = next.slice(1);
    n.children = build(n.flows, next);
  }
  return out;
}

export function groupFlows(flows, group = 'app') {
  return build(flows || [], LEVELS[group] || LEVELS.app);
}

const total = (n) => (n.rx || 0) + (n.tx || 0);
const cmp = {
  name: (a, b) => String(a.label).localeCompare(String(b.label)),
  most: (a, b) => total(b) - total(a) || b.conns - a.conns || String(a.label).localeCompare(String(b.label)),
  least: (a, b) => total(a) - total(b) || a.conns - b.conns || String(a.label).localeCompare(String(b.label)),
  recent: (a, b) => (b.last || 0) - (a.last || 0) || total(b) - total(a),
  conns: (a, b) => b.conns - a.conns || total(b) - total(a),
};

// Sorts every level (ports always by number under their parent).
export function sortTree(nodes, sort = 'most') {
  const f = cmp[sort] || cmp.most;
  return [...(nodes || [])].sort((a, b) => (a.kind === 'port' && b.kind === 'port' && sort === 'name' ? Number(a.port) - Number(b.port) : f(a, b)))
    .map((n) => ({ ...n, children: sortTree(n.children, sort) }));
}

// The first `shown` children, and how many more ("N more…").
export function fold(children, shown = 8, all = false) {
  const list = children || [];
  if (all || list.length <= shown) return { list, more: 0 };
  return { list: list.slice(0, shown), more: list.length - shown };
}

// The view settings kept per user (settings.monitor.netView), with defaults and only known values.
export function normView(v) {
  const o = v && typeof v === 'object' ? v : {};
  return {
    group: GROUPS.includes(o.group) ? o.group : 'app',
    dir: ['all', 'in', 'out'].includes(o.dir) ? o.dir : 'all',
    proto: ['all', 'tcp', 'udp'].includes(o.proto) ? o.proto : 'all',
    status: ['all', 'allowed', 'denied', 'rules'].includes(o.status) ? o.status : 'all',
    sort: SORTS.includes(o.sort) ? o.sort : 'most',
  };
}
