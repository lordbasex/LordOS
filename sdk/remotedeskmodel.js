// remotedeskmodel.js — Remote Desk's saved computers and credentials, without a screen (sdk/remotedesk.jsx,
// docs/FILES.md "Remote desktop"). Pure functions: the app, the tests (server/tests/remotedesk_test.mjs) and nothing else.
//
// settings.remoteDesks = { v: 2, devices: [Device], creds: [Cred], groups: ['name'], certs: { 'host:port': print },
//                          view: 'grid'|'list', sort: 'name'|'last', kind: 'all'|'rdp'|'vnc', sidebar, section, collapsed: {} }
// Device = { id, proto, host, port, name, cred, group, fav, last, shot, reconnect, admin, swap, res, full, scale, depth, sound, folders, legacy }
//   sound: 'here' (played on this computer), 'there' (left on the remote one) or 'none'. depth: 16, 24 or 32 bits.
//   cred: a Cred id, or '' (ask when required). group: '' is the default group. shot: when its picture was taken (ms).
//   legacy: 'host:port' of a device saved before 0.8.8.4, whose remembered password is still under that key.
// Cred = { id, user, domain, label } (label: an optional display name) — the password is only in the keyring (service rdesk://<id>/), never in settings.

import { compareNames } from './collate.js';
export const DEVICE = { id: '', proto: 'rdp', host: '', port: 0, name: '', cred: '', group: '', fav: false, last: 0, shot: 0,
  reconnect: true, admin: false, swap: false, res: 'auto', full: false, scale: 100, depth: 32, sound: 'here', folders: [], legacy: '' };
export const SOUNDS = ['here', 'there', 'none'];
export const DEPTHS = [32, 24, 16];
export const RESOLUTIONS = [['auto', 0, 0], ['1280x720', 1280, 720], ['1280x800', 1280, 800], ['1600x900', 1600, 900], ['1920x1080', 1920, 1080], ['2560x1440', 2560, 1440]];
export const SCALES = [100, 125, 150, 200];
export const MAX_DEVICES = 50;
export const MAX_CREDS = 50;

const IPV4 = /^(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(\.(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)){3}$/;
const LABEL = /^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$/;
export const isIp = (s) => IPV4.test(String(s || ''));
// a host name lord.rdesk accepts (rconnect_host_ok): letters, digits, dots and dashes
export const hostOk = (s) => { const v = String(s || ''); return v.length > 0 && v.length <= 253 && /^[A-Za-z0-9][A-Za-z0-9.-]*$/.test(v); };
// a name with dots whose labels are all valid (a domain name), not an IP
export const isFqdn = (s) => { const v = String(s || '').replace(/\.$/, ''); return !isIp(v) && v.includes('.') && v.length <= 253 && v.split('.').every((l) => LABEL.test(l)) && /[A-Za-z]/.test(v); };
// what can only be an address: an IP, a dotted name or localhost (a bare word may be a PC name that doesn't resolve)
export const looksLikeAddress = (s) => isIp(s) || isFqdn(s) || String(s || '').toLowerCase() === 'localhost';

const slug = (s) => String(s || '').replace(/[^A-Za-z0-9]+/g, '_').replace(/^_+|_+$/g, '').slice(0, 40) || 'x';
export const idOk = (s) => /^[A-Za-z0-9_-]{1,64}$/.test(String(s || ''));
export function newId(prefix = 'd', now = Date.now(), rnd = Math.random) {
  return `${prefix}${now.toString(36)}${Math.floor(rnd() * 0x100000).toString(36)}`.slice(0, 64);
}

// "PC name or address" as typed: name, address, "name:port"; '' or a bad port → null
export function parseAddress(text) {
  const s = String(text || '').trim();
  if (!s) return null;
  const m = /^\[?([^\]]*?)\]?(?::(\d{1,5}))?$/.exec(s);
  if (!m || !hostOk(m[1])) return null;
  const port = m[2] ? Number(m[2]) : 0;
  if (port > 65535) return null;
  return { host: m[1], port };
}
export const addressOf = (d) => (d.port ? `${d.host}:${d.port}` : d.host);
export const defaultPort = (proto) => (proto === 'vnc' ? 5900 : 3389);

// a user name as typed: "DOMAIN\user" is split; "user@example.com" stays whole (it is a full sign-in name, and a
// hosted account only signs in that way), and an explicit domain field wins over neither
export function splitUser(text, domain = '') {
  const s = String(text || '').trim();
  const d = String(domain || '').trim();
  const i = s.indexOf('\\');
  if (i > 0 && i < s.length - 1 && !s.slice(i + 1).includes('\\')) return { user: s.slice(i + 1), domain: s.slice(0, i) };
  return { user: s, domain: d };
}
// what lord.rdesk accepts as user and domain (js_rdesk.c: rdesk_user_ok, rconnect_user_ok)
export const userOk = (s) => { const v = String(s || ''); return v.length <= 128 && !/^[-@]/.test(v) && /^[A-Za-z0-9._@-]*$/.test(v); };
export const domainOk = (s) => { const v = String(s || ''); return v.length <= 64 && !v.startsWith('-') && /^[A-Za-z0-9._-]*$/.test(v); };
// the sign-in name (DOMAIN\\user), and what the lists show: the display name when it has one
export const credText = (c) => (!c ? '' : c.domain ? `${c.domain}\\${c.user}` : c.user);
export const credName = (c) => (!c ? '' : c.label || credText(c));

export const titleOf = (d) => d.name || addressOf(d);
export const keyOf = (d) => `${d.host}:${Number(d.port) || 0}`;

// ---- migration: what versions before 0.8.8.4 kept ----
// One list per protocol became one list (0.8.7); then the address and the domain: the form had "Computer" and
// "Domain", and a PC name typed in the first with its IP in the second never connected. A host that isn't an address
// with a domain that is one (an IP, or a dotted name that starts with the host's name) is that mistake: the name
// becomes the friendly name and the domain the address. The user and domain go to a saved credential.
export function migrate(r0) {
  const r = r0 && typeof r0 === 'object' ? r0 : {};
  if (r.v === 2) return { value: r, changed: false };
  const old = Array.isArray(r.devices) ? r.devices
    : ['rdp', 'vnc'].flatMap((proto) => (Array.isArray(r[proto]) ? r[proto] : []).map((x) => ({ ...x, proto })));
  const creds = Array.isArray(r.creds) ? r.creds.filter((c) => c && idOk(c.id)) : [];
  const ids = new Set();
  const devices = [];
  for (const x of old) {
    if (!x || typeof x !== 'object' || !x.host) continue;
    const proto = x.proto === 'vnc' ? 'vnc' : 'rdp';
    const port = Number(x.port) || 0;
    let host = String(x.host).trim(), name = String(x.name || '').trim(), domain = String(x.domain || '').trim();
    const legacy = `${host}:${port}`;
    const dom = domain.toLowerCase(), h = host.toLowerCase();
    if (!looksLikeAddress(host) && domain && (isIp(domain) || (isFqdn(domain) && dom.startsWith(`${h}.`)))) {
      name = name || host;
      host = domain;
      domain = '';
    }
    const { user, domain: dm } = splitUser(x.user, domain);
    let cred = '';
    if (user) {
      const found = creds.find((c) => c.user === user && (c.domain || '') === dm);
      if (found) cred = found.id;
      else {
        cred = `c-${slug(dm ? `${dm}-${user}` : user)}`.slice(0, 64);
        while (creds.some((c) => c.id === cred)) cred = `${cred.slice(0, 60)}${creds.length}`;
        creds.push({ id: cred, user, domain: dm });
      }
    }
    // the same computer saved twice (once with the mistake, once right): the first one, the newest, stays
    const same = devices.find((o) => o.proto === proto && o.host.toLowerCase() === host.toLowerCase() && o.port === port);
    if (same) { if (!same.name) same.name = name; if (!same.cred) same.cred = cred; continue; }
    let id = idOk(x.id) ? x.id : `${proto}-${slug(host)}-${port}`.slice(0, 64);
    while (ids.has(id)) id = `${id.slice(0, 60)}-${ids.size}`;
    ids.add(id);
    const d = { ...DEVICE, ...x, id, proto, host, port, name, cred, legacy };
    delete d.user; delete d.domain;
    devices.push(d);
  }
  const { rdp, vnc, ...rest } = r;
  return { value: { ...rest, v: 2, devices: devices.slice(0, MAX_DEVICES), creds: creds.slice(0, MAX_CREDS) }, changed: true };
}

// ---- the list ----
export function sortDevices(list, by = 'name') {
  const name = (d) => titleOf(d).toLowerCase();
  return [...list].sort((a, b) => (by === 'last' ? (b.last || 0) - (a.last || 0) : 0) || compareNames(name(a), name(b)) || a.id.localeCompare(b.id));
}
export function filterDevices(list, { query = '', kind = 'all', favorites = false, creds = [] } = {}) {
  const q = String(query).trim().toLowerCase();
  return list.filter((d) => (kind === 'all' || d.proto === kind) && (!favorites || d.fav) &&
    (!q || `${titleOf(d)} ${d.host} ${credText(creds.find((c) => c.id === d.cred))} ${d.group}`.toLowerCase().includes(q)));
}
// the sections of the content area, in order: the default group first, then the others by name
export function groupDevices(list, groups = []) {
  const names = [...new Set([...groups, ...list.map((d) => d.group || '')].filter(Boolean))].sort(compareNames);
  return [['', list.filter((d) => !d.group)], ...names.map((g) => [g, list.filter((d) => d.group === g)])].filter(([g, l]) => l.length || (g && groups.includes(g)));
}

// a copy with its own id and no picture
export function duplicate(d, list, copyName) {
  const used = new Set(list.map((x) => titleOf(x)));
  let name = copyName(titleOf(d)), n = 2;
  while (used.has(name)) name = `${copyName(titleOf(d))} ${n++}`;
  return { ...d, id: newId(), name, shot: 0, last: 0, fav: false, legacy: '' };
}

// what the remote side receives from the pointer: the left and right buttons swapped if asked (bit 1 left, 4 right)
export const swapButtons = (b, on) => (!on ? b : (b & ~5) | ((b & 1) << 2) | ((b & 4) >> 2));

// ~/.local/share/lordos/remote-desk/thumbs/<id>.png, with its version so a new picture is loaded again (ui/image.c)
export const thumbPath = (home, d) => (d.shot && idOk(d.id) ? `${home}/.local/share/lordos/remote-desk/thumbs/${d.id}.png#${d.shot}` : '');

// ---- .rdp files (the connection files remote desktop clients exchange): "key:type:value" lines ----
// bytes → text: these files are often UTF-16 with a byte order mark
export function decodeRdp(bytes) {
  const b = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes || []);
  if (b[0] === 0xff && b[1] === 0xfe) { let s = ''; for (let i = 2; i + 1 < b.length; i += 2) s += String.fromCharCode(b[i] | (b[i + 1] << 8)); return s; }
  const start = b[0] === 0xef && b[1] === 0xbb && b[2] === 0xbf ? 3 : 0;
  return new TextDecoder().decode(b.subarray(start));
}
// what we take from one: { device, user, domain } or null if it has no address
export function parseRdp(text) {
  const kv = {};
  for (const line of String(text || '').split(/\r?\n/)) {
    const m = /^\s*([a-z0-9 ]+):([isb]):(.*)$/i.exec(line);
    if (m) kv[m[1].trim().toLowerCase()] = m[2] === 'i' ? Number(m[3].trim()) : m[3].trim();
  }
  const a = parseAddress(kv['full address'] || kv['alternate full address'] || '');
  if (!a) return null;
  const port = a.port || (Number(kv['server port']) > 0 && Number(kv['server port']) !== 3389 ? Number(kv['server port']) : 0);
  const w = Number(kv.desktopwidth) || 0, ht = Number(kv.desktopheight) || 0;
  const res = RESOLUTIONS.find(([, rw, rh]) => rw === w && rh === ht);
  const device = {
    ...DEVICE, proto: 'rdp', host: a.host, port,
    full: kv['screen mode id'] === 2,
    sound: SOUNDS[[0, 1, 2].includes(kv.audiomode) ? kv.audiomode : 0],
    depth: DEPTHS.includes(kv['session bpp']) ? kv['session bpp'] : 32,
    admin: kv['administrative session'] === 1,
    res: res ? res[0] : 'auto',
  };
  if (w >= 640 && ht >= 480 && !res) device.res = `${w}x${ht}`;
  const { user, domain } = splitUser(kv.username || '', kv.domain || '');
  return { device, user: userOk(user) ? user : '', domain: domainOk(domain) ? domain : '' };
}
// one computer as an .rdp file (no password: those files would keep it in a form other clients can read)
export function exportRdp(d, cred) {
  const [, w, ht] = RESOLUTIONS.find((r) => r[0] === d.res) || (/^\d+x\d+$/.test(d.res || '') ? [d.res, ...d.res.split('x').map(Number)] : RESOLUTIONS[0]);
  const lines = [`full address:s:${addressOf(d)}`, `screen mode id:i:${d.full ? 2 : 1}`, `session bpp:i:${d.depth || 32}`,
    `audiomode:i:${Math.max(0, SOUNDS.indexOf(d.sound || 'here'))}`, `administrative session:i:${d.admin ? 1 : 0}`];
  if (w) lines.push(`desktopwidth:i:${w}`, `desktopheight:i:${ht}`);
  if (cred) lines.push(`username:s:${credText(cred)}`);
  return `${lines.join('\r\n')}\r\n`;
}
export const fileNameOf = (d) => `${titleOf(d).replace(/[^A-Za-z0-9 ._-]+/g, '_').trim() || 'computer'}.rdp`;
// a resolution as kept ('auto', a preset or a custom "WxH") → [w, h]; [0, 0] is automatic
export function resSize(res) {
  const m = /^(\d{3,4})x(\d{3,4})$/.exec(String(res || ''));
  const w = m ? Number(m[1]) : 0, ht = m ? Number(m[2]) : 0;
  return w >= 640 && ht >= 480 && w <= 7680 && ht <= 4320 ? [w, ht] : [0, 0];
}
export const resOk = (res) => resSize(res)[0] > 0;
