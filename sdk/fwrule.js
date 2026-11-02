// fwrule.js — checks and normalizes what the user types for a firewall rule (Settings → Privacy →
// per-app network, and the Monitor's "Allow / Block this domain"), with the same rules as the C
// store (server/src/fw/rules.c: fw_parse_dest, fw_format_dest, norm_domain; docs/FIREWALL.md).
// lord.fw.addRule validates again in C: this module only lets the sheet say what is wrong before
// saving, and shows the rules in the order the engine weighs them. Pure functions, no imports, so
// node can test them (server/tests/fwrule_input_test.mjs, against the C parser with the shared cases
// in server/tests/fw_dest_cases.txt).
//
//   parseDest('*.Example.com.')  → { ok: true, kind: 'wildcard', dst: '*.example.com' }
//   parseDest('10.0.0.1/8')      → { ok: false, error: 'hostBits' }
//   parsePort('8000-8080')       → { ok: true, port: '8000-8080' }
//   checkRule({ dst, port, proto, action }) → { ok, rule } | { ok: false, field, error }
//   specificity(rule) / sortByPrecedence(rules): the engine has no priority numbers; the most
//   specific destination wins, then the smaller port range, a concrete protocol, deny, the newest.

// ---- domains -------------------------------------------------------------------------------------

// Lowercase, no trailing dot, labels of 1–63 from [a-z0-9-_], up to 253 in total; the last label
// can't be only digits (a mistyped IP). International names go in punycode (xn--…).
export function normDomain(s) {
  if (typeof s !== 'string') return null;
  let d = s;
  if (d.endsWith('.')) d = d.slice(0, -1);
  if (!d || d.length > 253) return null;
  d = d.replace(/[A-Z]/g, (c) => c.toLowerCase());
  const labels = d.split('.');
  for (const l of labels) if (!/^[a-z0-9_-]{1,63}$/.test(l)) return null;
  if (/^[0-9]+$/.test(labels[labels.length - 1])) return null;
  return d;
}

// ---- addresses (inet_pton / inet_ntop as glibc does them) -----------------------------------------

function parseV4(s) {
  const p = s.split('.');
  if (p.length !== 4) return null;
  const out = [];
  for (const x of p) {
    if (!/^[0-9]{1,3}$/.test(x) || (x.length > 1 && x[0] === '0')) return null;
    const v = Number(x);
    if (v > 255) return null;
    out.push(v);
  }
  return out;
}

function parseV6(s) {
  if (!/^[0-9a-fA-F:.]+$/.test(s)) return null;
  let tail4 = null;
  let str = s;
  const lastColon = str.lastIndexOf(':');
  if (str.includes('.')) {                                  // a trailing dotted IPv4
    tail4 = parseV4(str.slice(lastColon + 1));
    if (!tail4) return null;
    str = str.slice(0, lastColon + 1) + 'x';                // placeholder for two words
  }
  const dbl = str.indexOf('::');
  if (dbl !== str.lastIndexOf('::') && dbl >= 0) return null;
  const parseGroups = (part) => (part === '' ? [] : part.split(':'));
  let head, tail;
  if (dbl >= 0) { head = parseGroups(str.slice(0, dbl)); tail = parseGroups(str.slice(dbl + 2)); }
  else { head = parseGroups(str); tail = null; }
  const words = (list) => {
    const w = [];
    for (const g of list) {
      if (g === 'x') { if (!tail4) return null; w.push((tail4[0] << 8) | tail4[1], (tail4[2] << 8) | tail4[3]); continue; }
      if (!/^[0-9a-fA-F]{1,4}$/.test(g)) return null;
      w.push(parseInt(g, 16));
    }
    return w;
  };
  const hw = words(head);
  if (!hw) return null;
  // the IPv4 placeholder may only be the last group
  if (head.includes('x') && (tail !== null || head[head.length - 1] !== 'x')) return null;
  let all;
  if (tail === null) {
    if (hw.length !== 8) return null;
    all = hw;
  } else {
    const tw = words(tail);
    if (!tw) return null;
    if (tail.includes('x') && tail[tail.length - 1] !== 'x') return null;
    if (hw.length + tw.length > 7) return null;              // "::" stands for at least one word
    all = [...hw, ...new Array(8 - hw.length - tw.length).fill(0), ...tw];
  }
  const b = [];
  for (const w of all) b.push(w >> 8, w & 255);
  return b;
}

// inet_ntop(AF_INET6): the longest run (≥ 2) of zero words becomes "::" (the first on a tie);
// ::ffff:a.b.c.d and ::a.b.c.d (but not :: or ::1) keep the dotted IPv4, as glibc prints them.
function formatV6(b) {
  const w = [];
  for (let i = 0; i < 16; i += 2) w.push((b[i] << 8) | b[i + 1]);
  let best = -1, bestLen = 0;
  for (let i = 0; i < 8;) {
    if (w[i] !== 0) { i++; continue; }
    let j = i;
    while (j < 8 && w[j] === 0) j++;
    if (j - i > bestLen) { best = i; bestLen = j - i; }
    i = j;
  }
  if (bestLen < 2) best = -1;
  let out = '';
  for (let i = 0; i < 8; i++) {
    if (best >= 0 && i >= best && i < best + bestLen) {
      if (i === best) out += ':';
      continue;
    }
    if (i) out += ':';
    if (i === 6 && best === 0 && (bestLen === 6 || (bestLen === 5 && w[5] === 0xffff))) {
      out += `${b[12]}.${b[13]}.${b[14]}.${b[15]}`;
      return out;
    }
    out += w[i].toString(16);
  }
  if (best >= 0 && best + bestLen === 8) out += ':';
  return out;
}

const bitAt = (a, i) => (a[i >> 3] >> (7 - (i & 7))) & 1;
const hostBitsZero = (a, prefix) => { for (let i = prefix; i < a.length * 8; i++) if (bitAt(a, i)) return false; return true; };

function parseCidr(s) {
  const slash = s.indexOf('/');
  const ip = slash >= 0 ? s.slice(0, slash) : s;
  if (!ip || ip.length >= 64) return { ok: false, error: 'address' };
  let fam, a;
  if (ip.includes(':')) { a = parseV6(ip); fam = 6; } else { a = parseV4(ip); fam = 4; }
  if (!a) return { ok: false, error: 'address' };
  const bits = fam === 4 ? 32 : 128;
  let pfx = bits;
  if (slash >= 0) {
    const p = s.slice(slash + 1);
    if (!/^[0-9]{1,5}$/.test(p) || (p.length > 1 && p[0] === '0') || Number(p) > bits) return { ok: false, error: 'prefix' };
    pfx = Number(p);
  }
  if (fam === 6 && pfx >= 96 && a.slice(0, 10).every((x) => x === 0) && a[10] === 0xff && a[11] === 0xff) {
    a = a.slice(12);                                         // ::ffff:a.b.c.d/n is its IPv4
    fam = 4;
    pfx -= 96;
  }
  if (!hostBitsZero(a, pfx)) return { ok: false, error: 'hostBits' };
  const text = fam === 4 ? a.join('.') : formatV6(a);
  const full = pfx === (fam === 4 ? 32 : 128);
  return { ok: true, kind: full ? 'address' : 'network', fam, prefix: pfx, dst: full ? text : `${text}/${pfx}` };
}

// ---- destinations --------------------------------------------------------------------------------

// kind: 'domain' | 'wildcard' | 'address' | 'network' | 'lan' | 'internet' | 'any'.
// error: 'empty' | 'domain' | 'wildcard' | 'address' | 'prefix' | 'hostBits'.
export function parseDest(input) {
  if (typeof input !== 'string') return { ok: false, error: 'empty' };
  const s = input.trim();
  if (!s) return { ok: false, error: 'empty' };
  if (s === 'any' || s === 'lan' || s === 'internet') return { ok: true, kind: s, dst: s };
  if (s.includes(':') || s.includes('/')) return parseCidr(s);
  if (s[0] === '*') {
    const d = s[1] === '.' ? normDomain(s.slice(2)) : null;
    return d ? { ok: true, kind: 'wildcard', dst: `*.${d}`, domain: d } : { ok: false, error: 'wildcard' };
  }
  const c = parseCidr(s);
  if (c.ok) return c;
  const d = normDomain(s);
  if (d) return { ok: true, kind: 'domain', dst: d, domain: d };
  return { ok: false, error: /^[0-9.]+$/.test(s) ? 'address' : 'domain' };
}

// "" (any port), "443" or "8000-8080"; 1–65535, lo ≤ hi, no leading zeros.
export function parsePort(input) {
  const s = typeof input === 'string' ? input.trim() : input == null ? '' : String(input);
  if (!s) return { ok: true, port: '' };
  const m = /^([1-9][0-9]{0,4})(?:\s*-\s*([1-9][0-9]{0,4}))?$/.exec(s);
  if (!m) return { ok: false, error: 'port' };
  const lo = Number(m[1]), hi = m[2] ? Number(m[2]) : lo;
  if (hi > 65535 || lo > hi) return { ok: false, error: 'port' };
  return { ok: true, port: lo === hi ? String(lo) : `${lo}-${hi}`, lo, hi };
}

// The whole rule as the sheet has it. proto: '' | 'tcp' | 'udp'; action: 'allow' | 'deny'.
// A bare "any" (every destination, port and protocol) is the app's default, set apart in the UI.
export function checkRule({ dst, port, proto = '', action, dir = 'out' } = {}) {
  const d = parseDest(dst);
  if (!d.ok) return { ok: false, field: 'dst', error: d.error };
  const p = parsePort(port);
  if (!p.ok) return { ok: false, field: 'port', error: p.error };
  if (proto !== '' && proto !== 'tcp' && proto !== 'udp') return { ok: false, field: 'proto', error: 'proto' };
  if (action !== 'allow' && action !== 'deny') return { ok: false, field: 'action', error: 'action' };
  if (dir !== 'out' && dir !== 'in') return { ok: false, field: 'dir', error: 'dir' };
  if (d.kind === 'any' && !p.port && !proto && dir === 'out') return { ok: false, field: 'dst', error: 'isDefault' };
  return { ok: true, kind: d.kind, rule: { dst: d.dst, port: p.port, proto, action, dir } };
}

// The app's default ("allow everything" / "block everything except the rules") is the rule
// {out, any, any port, any protocol}, the Monitor's switch.
export const isDefaultRule = (r) => r && r.dir === 'out' && r.dst === 'any' && !r.port && !r.proto;

// ---- precedence (fw_decide's order, see rules.c "consider") ---------------------------------------

const SC_ANY = 0, SC_NET = 500, SC_CIDR = 1000, SC_WILD = 2000, SC_EXACT = 3000;

export function specificity(rule) {
  const d = parseDest(rule.dst);
  if (!d.ok) return -1;
  switch (d.kind) {
    case 'domain': return SC_EXACT;
    case 'wildcard': return SC_WILD + d.domain.split('.').length;
    case 'address': case 'network': return SC_CIDR + d.prefix;
    case 'lan': case 'internet': return SC_NET;
    default: return SC_ANY;
  }
}

const portWidth = (r) => { const p = parsePort(r.port || ''); return p.ok && p.port ? p.hi - p.lo : 65535; };

// Strongest first: what the engine would pick when several rules match the same connection.
export function comparePrecedence(a, b) {
  return specificity(b) - specificity(a) || portWidth(a) - portWidth(b) || (b.proto ? 1 : 0) - (a.proto ? 1 : 0) ||
    (b.action === 'deny' ? 1 : 0) - (a.action === 'deny' ? 1 : 0) || (b.id || 0) - (a.id || 0);
}
export const sortByPrecedence = (rules) => [...rules].sort(comparePrecedence);

// ---- one destination's verdict (the Monitor's per-domain switch, 0.8.2) --------------------------
// The same decision fw_decide makes for one connection of an app (rules.c, docs/FIREWALL.md
// "Precedence"), over that app's rules as lord.fw.rules(app) lists them: the restrictions first,
// then the most specific matching rule. Without a rule the mode decides (null here: allowed in the
// silent and off modes). server/tests/fw_domain_cases.txt is run through both this and fw_decide
// (build/fwdest-test and fwrule_input_test.mjs), so the Monitor shows what the kernel will do.
//
//   decide(rules, { names: ['api.example.com'], addr: '93.184.216.34', proto: 'tcp', port: 443 },
//          ['no-lan'])                       → { action: 'allow' | 'deny', rule, by: 'rule' | 'restricted' } | null

function addrBytes(a) {
  const c = typeof a === 'string' && a ? parseCidr(a) : null;
  if (!c || !c.ok || c.kind !== 'address') return null;
  return { fam: c.fam, b: c.fam === 4 ? parseV4(c.dst) : parseV6(c.dst) };
}

export function isLan(a) {
  const x = addrBytes(a);
  if (!x) return false;
  const b = x.b;
  if (x.fam === 4) {
    return b[0] === 0 || b[0] === 10 || b[0] === 127 || (b[0] === 100 && (b[1] & 0xc0) === 64) || (b[0] === 169 && b[1] === 254) ||
      (b[0] === 172 && (b[1] & 0xf0) === 16) || (b[0] === 192 && b[1] === 168) || (b[0] & 0xf0) === 224 || b.every((v) => v === 255);
  }
  if (b.slice(0, 15).every((v) => v === 0) && (b[15] === 0 || b[15] === 1)) return true;
  return (b[0] & 0xfe) === 0xfc || (b[0] === 0xfe && (b[1] & 0xc0) === 0x80) || b[0] === 0xff;
}

function cidrHas(rule, x) {
  const c = parseCidr(rule);
  if (!c.ok || !x || c.fam !== x.fam) return false;
  const net = c.fam === 4 ? parseV4(c.dst.split('/')[0]) : parseV6(c.dst.split('/')[0]);
  for (let i = 0; i < c.prefix; i++) if (bitAt(net, i) !== bitAt(x.b, i)) return false;
  return true;
}

function destMatches(r, names, a, lan) {
  const d = parseDest(r.dst);
  if (!d.ok) return false;
  switch (d.kind) {
    case 'any': return true;
    case 'lan': return !!a && lan;                          // an unknown address is neither (rules.c)
    case 'internet': return !!a && !lan;
    case 'domain': return names.includes(d.domain);
    case 'wildcard': return names.some((n) => n === d.domain || n.endsWith(`.${d.domain}`));
    default: return cidrHas(r.dst, a);
  }
}

// (0.8.5) opts: { listed } — a subscribed blocklist covers the destination: it denies unless the
// winning rule names the destination (a domain, an address or a network), as rules.c's FwQuery.listed;
// { mode } — 'deny' ("deny silently"): what no rule allows is denied, loopback excepted. Without a
// deciding rule, list or deny mode the result stays null (the app's default: allowed).
const isLoopback = (a) => {
  if (!a) return false;
  const b = Array.from(a.b);
  if (a.fam === 4) return b[0] === 127;
  if (b.slice(0, 10).every((x) => x === 0) && b[10] === 255 && b[11] === 255) return b[12] === 127;   /* ::ffff:127.x.y.z */
  return b.slice(0, 15).every((x) => x === 0) && b[15] === 1;
};
export function decide(rules, { names = [], addr = '', proto = 'tcp', port = 0, dir = 'out' } = {}, restricted = [], opts = {}) {
  const a = addrBytes(addr);
  const lan = a ? isLan(addr) : false;
  const has = (f) => restricted.includes(f);
  if (has('no-network') || (dir === 'in' && has('no-incoming')) || (has('no-internet') && !lan) || (has('no-lan') && lan)) {
    return { action: 'deny', rule: null, by: 'restricted' };
  }
  const ns = names.map((n) => normDomain(n)).filter(Boolean);
  const hits = (rules || []).filter((r) => {
    if ((r.dir || 'out') !== dir) return false;
    if (r.proto && r.proto !== proto) return false;
    const p = parsePort(r.port || '');
    if (!p.ok || (p.port && (port < p.lo || port > p.hi))) return false;
    return destMatches(r, ns, a, lan);
  });
  const fallback = () => {
    if (opts.listed) return { action: 'deny', rule: null, by: 'list' };
    if (opts.mode === 'deny' && !isLoopback(a)) return { action: 'deny', rule: null, by: 'mode' };
    return null;
  };
  if (!hits.length) return fallback();
  // as rules.c weighs them: an exact name also counts its labels (a CNAME chain's longest name wins)
  const spec = (r) => { const d = parseDest(r.dst); return d.ok && d.kind === 'domain' ? SC_EXACT + d.domain.split('.').length : specificity(r); };
  const best = [...hits].sort((x, y) => spec(y) - spec(x) || comparePrecedence(x, y))[0];
  if (opts.listed && spec(best) < SC_CIDR) return { action: 'deny', rule: null, by: 'list' };
  return { action: best.action, rule: best, by: 'rule' };
}

// What one click on a destination's switch does: the rule for exactly that name (or address), any
// port and protocol, set to the other verdict; if dropping that rule already gives the other
// verdict, it is removed instead of piling up rules (like the Monitor's per-app switch).
//   → { op: 'add', rule } | { op: 'update', id, rule } | { op: 'remove', id } | null
export function toggleOp(rules, { name, addr, proto = 'tcp', port = 0 }, restricted = [], opts = {}) {
  const q = { names: name ? [name] : [], addr, proto, port };
  const now = decide(rules, q, restricted, opts);
  if (now?.by === 'restricted') return null;
  const allowedNow = now ? now.action === 'allow' : true;    // no rule: the mode ("ask" is applied as silent)
  const target = allowedNow ? 'deny' : 'allow';
  const dst = name ? normDomain(name) : parseDest(addr).ok ? parseDest(addr).dst : null;
  if (!dst) return null;
  const own = (rules || []).find((r) => (r.dir || 'out') === 'out' && r.dst === dst && !r.port && !r.proto);
  if (own) {
    const without = decide(rules.filter((r) => r !== own), q, restricted, opts);
    const allowedWithout = without ? without.action === 'allow' : true;
    if ((allowedWithout ? 'allow' : 'deny') === target) return { op: 'remove', id: own.id };
    return { op: 'update', id: own.id, rule: { dir: 'out', dst, port: '', proto: '', action: target } };
  }
  return { op: 'add', rule: { dir: 'out', dst, port: '', proto: '', action: target } };
}
