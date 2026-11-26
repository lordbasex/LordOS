// guestappsmodel.js — a machine's guest apps in this computer's launcher before the machine runs (docs/VIRTUALIZATION.md
// 6.12), the pure part; sdk/apps.js lists them, apps/machines saves them and opens them. Tested by
// server/tests/guest_apps_test.mjs.
//
// What a guest listed (apps mode's LAG_APPLIST, through lord-vm and lord.vm.guestApps) is untrusted text: every name and
// token is cut to the protocol's limits, loses control and direction characters, and is only ever shown as text. A token
// is never run here or anywhere on this computer: choosing an entry starts the machine and sends that exact token to the
// guest only once the guest's own fresh list holds it (lord.vm.launch accepts nothing else).

export const GUEST_APP_PREFIX = 'vmapp:';
export const GUEST_APPS_MAX = 256;           // LAG_APPS_MAX
export const GUEST_NAME_MAX = 256;           // LAG_NAME_MAX (bytes there; characters here, then cut to the bytes)
export const GUEST_TOKEN_MAX = 512;          // LAG_TOKEN_MAX
export const GUEST_NAME_SHOWN = 80;          // what a launcher shows of a name

// C0, DEL, C1, and the direction controls U+200E/F, U+202A–E, U+2066–9 (a name that reads as something else)
// eslint-disable-next-line no-control-regex
const BAD = /[\u0000-\u001f\u007f-\u009f‎‏‪-‮⁦-⁩]/g;

const utf8Len = (s) => {
  let n = 0;
  for (const ch of s) { const c = ch.codePointAt(0); n += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; }
  return n;
};
// s cut to at most max UTF-8 bytes, on a character
function cutBytes(s, max) {
  if (utf8Len(s) <= max) return s;
  let out = '', n = 0;
  for (const ch of s) {
    const c = ch.codePointAt(0), l = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    if (n + l > max) break;
    out += ch; n += l;
  }
  return out;
}

// a name from the guest as text this computer shows: no control or direction characters, spaces collapsed, cut
export function cleanGuestText(v, max) {
  if (typeof v !== 'string') return '';
  return cutBytes(v.replace(BAD, ' ').replace(/\s+/g, ' ').trim(), max);
}

// a token kept as the guest sent it (it is compared byte for byte with the guest's next list), or '' when it isn't one
// the guest could have listed: a string of 1..GUEST_TOKEN_MAX bytes with no control character
export function cleanGuestToken(v) {
  if (typeof v !== 'string' || !v || utf8Len(v) > GUEST_TOKEN_MAX) return '';
  // eslint-disable-next-line no-control-regex
  return /[\u0000-\u001f\u007f-\u009f]/.test(v) ? '' : v;
}

// the list to save: [{ name, token }], each token once, names cleaned, at most GUEST_APPS_MAX, sorted by name
export function cleanGuestApps(list) {
  const out = [];
  const seen = new Set();
  for (const a of Array.isArray(list) ? list : []) {
    if (out.length >= GUEST_APPS_MAX) break;
    if (!a || typeof a !== 'object') continue;
    const token = cleanGuestToken(a.token), name = cleanGuestText(a.name, GUEST_NAME_MAX);
    if (!token || !name || seen.has(token)) continue;
    seen.add(token);
    out.push({ name, token });
  }
  return out.sort((x, y) => x.name.localeCompare(y.name) || (x.token < y.token ? -1 : x.token > y.token ? 1 : 0));
}

// true when two saved lists are the same (nothing to write)
export const sameGuestApps = (a, b) => Array.isArray(a) && Array.isArray(b) && a.length === b.length &&
  a.every((x, i) => x.name === b[i].name && x.token === b[i].token);

// a short stable hash of a token (FNV-1a, 32 bits, hex): the entry's id, so a pinned search usage survives a new list
export function tokenHash(token) {
  let h = 0x811c9dc5;
  for (const ch of String(token)) { h ^= ch.codePointAt(0); h = Math.imul(h, 0x01000193) >>> 0; }
  return h.toString(16).padStart(8, '0');
}
const MID = /^[A-Za-z0-9._-]{1,64}$/;
export const guestAppId = (mid, token) => `${GUEST_APP_PREFIX}${mid}:${tokenHash(token)}`;
export const isGuestAppId = (id) => typeof id === 'string' && id.startsWith(GUEST_APP_PREFIX);
// "vmapp:<machine id>:<hash>" → { mid, hash }, or null
export function parseGuestAppId(id) {
  if (!isGuestAppId(id)) return null;
  const rest = id.slice(GUEST_APP_PREFIX.length);
  const i = rest.lastIndexOf(':');
  if (i < 1) return null;
  const mid = rest.slice(0, i), hash = rest.slice(i + 1);
  return MID.test(mid) && /^[0-9a-f]{8}$/.test(hash) ? { mid, hash } : null;
}

// Whether a machine's apps are in the launcher: its saved list (meta.guestApps), the person's switch (meta.launcherApps:
// only `true` shows them, since 0.8.9.6.2: absent is off, so a machine's apps never mix with this computer's own unless
// the person turns it on in Configure; before it was on once the machine had a list), never a machine locked for anonymity or a member of a group (a Whonix
// member: its agent is off and nothing of its guest may show outside its window); the caller leaves out the unreadable
export function showsGuestApps(m, meta) {
  if (!m || !meta || m.profileLock || m.groupDir) return false;
  if (meta.launcherApps !== true) return false;
  return Array.isArray(meta.guestApps) && meta.guestApps.length > 0;
}

// The launcher's entries: for each machine (lord.vm.list()) that isn't running and shows its apps, one per saved app:
// { id, name (the app's), machine (the machine's name), mid, dir, token, kind: 'machine' }. active(m): the machine runs
// (its apps are its apps mode windows then, not entries here); locked(m): the anonymity profile.
export function guestLauncherItems(machines, metaOf, { active = () => false, locked = () => false } = {}) {
  const out = [];
  const ids = new Set();
  for (const m of Array.isArray(machines) ? machines : []) {
    if (!m || !m.dir) continue;
    const mid = String(m.id || '');
    if (!MID.test(mid) || active(m) || locked(m)) continue;
    let meta = null;
    try { meta = metaOf(m.dir) || null; } catch (e) { meta = null; }
    if (!showsGuestApps(m, meta)) continue;
    const machine = cleanGuestText(m.name || '', GUEST_NAME_SHOWN * 4) || mid;
    for (const a of cleanGuestApps(meta.guestApps)) {
      const id = guestAppId(mid, a.token);
      if (ids.has(id)) continue;           // two tokens with one hash: the first keeps it
      ids.add(id);
      const name = [...a.name].length > GUEST_NAME_SHOWN ? `${[...a.name].slice(0, GUEST_NAME_SHOWN - 1).join('')}…` : a.name;
      out.push({ id, name, machine, mid, dir: m.dir, token: a.token, kind: 'machine' });
    }
  }
  return out;
}

// What a machine's window does with a launch the launcher asked for, given the guest's current list (apps mode's, [] until
// the guest sent one) and how long it has waited: 'launch' (the exact token is in the guest's own list), 'missing' (the
// guest listed its apps and that one isn't there any more: nothing is launched), 'wait', or 'timeout'
export const LAUNCH_ASK_MS = 10 * 60 * 1000;
export function launchAskStep(ask, apps, now) {
  if (!ask || !cleanGuestToken(ask.token)) return 'missing';
  if (Array.isArray(apps) && apps.length) return apps.some((a) => a && a.token === ask.token) ? 'launch' : 'missing';
  return now - (ask.at || 0) > LAUNCH_ASK_MS ? 'timeout' : 'wait';
}
