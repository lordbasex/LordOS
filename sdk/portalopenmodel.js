// portalopenmodel.js — what the shell does when a Linux app asks the desktop to open something
// (org.freedesktop.portal.OpenURI, server/src/ui/portal.c). lord-server has already checked a local file
// (in the home folder or the user's disks, not a protected folder, never an executable or a launcher) and
// says whether the app that asks is the one in front (the bus's pid against the keyboard's).
//
//   openPlan(req)   { action, ask, target }: action 'url' (the default browser), 'file' (our apps or the
//                   default app for the type), 'folder' (Files there), 'reveal' (Files on its folder, the
//                   file selected) or 'refuse'. Only http and https leave as addresses: LordOS opens no
//                   other scheme for an app. ask: an app in the background makes the person confirm it.
//   urlHost(url)    the host the question shows first, so a long address can't hide where it goes; '' when the
//                   address is not one LordOS opens
//   splitPath(p)    { name, dir } for the question about a file
//
// The question must name the host the browser will go to, so only addresses every URL parser reads the same
// way pass (a browser reads them with the WHATWG URL rules): printable ASCII only (no space, no control, no
// raw international text: an international name comes as punycode, xn--…, and is shown that way), no
// backslash, no user info (an "@" before the path, "https://bank.example@evil.example/"), no "%" in the host,
// a port of digits, a numeric host only as a plain dotted IPv4 (never 0x7f.1 or 2130706433) and a valid
// IPv6 in brackets. Where the runtime has URL (node), its hostname has to agree as well. portal.c refuses
// the same addresses before the shell hears of them (portalcheck.c, portal_web_host).

const WEB_MAX = 4096;

function ipv4(h) {
  const p = h.split('.');
  return p.length === 4 && p.every((x) => /^(0|[1-9][0-9]{0,2})$/.test(x) && Number(x) <= 255);
}

function ipv6(h) {
  let s = h;
  if (/^[0-9a-f:]*:(\d+\.){3}\d+$/.test(s)) {          // a trailing IPv4 counts as two groups
    const i = s.lastIndexOf(':');
    if (!ipv4(s.slice(i + 1))) return false;
    s = `${s.slice(0, i + 1)}0:0`;
  }
  if (!/^[0-9a-f:]+$/.test(s)) return false;
  const halves = s.split('::');
  if (halves.length > 2) return false;
  const groups = (x) => (x === '' ? [] : x.split(':'));
  const a = groups(halves[0]), b = halves.length === 2 ? groups(halves[1]) : [];
  if (![...a, ...b].every((g) => /^[0-9a-f]{1,4}$/.test(g))) return false;
  const n = a.length + b.length;
  return halves.length === 2 ? n <= 7 : n === 8;
}

// The host in lower case, or '' when the address is refused
function webHost(url) {
  const u = String(url || '');
  if (!u || u.length > WEB_MAX || !/^[\x21-\x7e]+$/.test(u) || u.includes('\\')) return '';
  const m = /^https?:\/\//i.exec(u);
  if (!m) return '';
  const rest = u.slice(m[0].length);
  const end = rest.search(/[/?#]/);
  const auth = (end < 0 ? rest : rest.slice(0, end)).toLowerCase();
  if (!auth || auth.includes('@') || auth.includes('%')) return '';
  let host, port = '';
  if (auth[0] === '[') {
    const c = auth.indexOf(']');
    if (c < 0) return '';
    host = auth.slice(0, c + 1);
    const after = auth.slice(c + 1);
    if (after) { if (after[0] !== ':') return ''; port = after.slice(1); if (!port) return ''; }
    if (!ipv6(host.slice(1, -1))) return '';
  } else {
    const c = auth.indexOf(':');
    host = c < 0 ? auth : auth.slice(0, c);
    if (c >= 0) { port = auth.slice(c + 1); if (!port) return ''; }
    if (!host || host.length > 253 || !/^[a-z0-9._-]+$/.test(host)) return '';
    const labels = (host.endsWith('.') ? host.slice(0, -1) : host).split('.');
    if (labels.some((l) => !l || l.length > 63)) return '';
    // a last label that is a number makes the whole host an IPv4 to a browser
    if (/^(\d+|0x[0-9a-f]*)$/.test(labels[labels.length - 1]) && (host.endsWith('.') || !ipv4(host))) return '';
  }
  if (port && (!/^\d{1,5}$/.test(port) || Number(port) > 65535)) return '';
  if (typeof URL === 'function') {
    let parsed;
    try { parsed = new URL(u); } catch (e) { return ''; }
    if (parsed.hostname !== host && !(host[0] === '[' && parsed.hostname[0] === '[')) return '';
  }
  return host;
}

export function urlHost(url) {
  return webHost(url);
}

export function splitPath(p) {
  const s = String(p || '').replace(/\/+$/, '');
  const i = s.lastIndexOf('/');
  return i < 0 ? { name: s, dir: '' } : { name: s.slice(i + 1), dir: s.slice(0, i) || '/' };
}

export function openPlan(req) {
  const refuse = { action: 'refuse', ask: false, target: '' };
  if (!req || typeof req.target !== 'string' || !req.target) return refuse;
  const ask = req.front !== true;
  const { kind, target } = req;
  if (kind === 'uri') {
    if (!webHost(target)) return refuse;
    return { action: 'url', ask, target };
  }
  if (kind === 'file' || kind === 'folder' || kind === 'reveal') {
    if (target[0] !== '/') return refuse;
    return { action: kind, ask, target };
  }
  return refuse;
}
