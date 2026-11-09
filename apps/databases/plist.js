// plist.js — Sequel Ace's and Sequel Pro's favorites (Favorites.plist, an XML property list) as favorites.json groups and
// favorites (docs/DATABASES.md, "Importing"). Only the keys listed here are read: a password, or anything that could
// hold one, never is. Pure: tested by server/tests/databases_plist_test.mjs.
import { newFavoriteId, newGroupId, blankFavorite, FAV_COLORS } from './model.js';

// Where each app keeps the file, under a home folder
export const SOURCES = [
  { id: 'sequel-ace', rel: 'Library/Containers/com.sequel-ace.sequel-ace/Data/Library/Application Support/Sequel Ace/Data/Favorites.plist' },
  { id: 'sequel-pro', rel: 'Library/Application Support/Sequel Pro/Data/Favorites.plist' },
];

// The places to look: the home folder, and every Users/<name> on the user's mounted disks (a Mac's disk).
// mounts: [{ path, name }] under /media/<user>; users: { [mount path]: [names in Users/] }
export function candidatePaths(home, mounts = [], users = {}) {
  const out = [];
  for (const s of SOURCES) out.push({ source: s.id, path: `${home}/${s.rel}`, disk: '' });
  for (const m of mounts) {
    for (const u of users[m.path] || []) {
      if (!u || u.startsWith('.') || u === 'Shared' || u.includes('/')) continue;
      for (const s of SOURCES) out.push({ source: s.id, path: `${m.path}/Users/${u}/${s.rel}`, disk: m.name || m.path.split('/').pop() });
    }
  }
  return out;
}

// ---- XML property lists ----

const ENT = { lt: '<', gt: '>', amp: '&', quot: '"', apos: "'" };
export function unescapeXml(s) {
  return String(s).replace(/&(#x[0-9a-fA-F]+|#\d+|[a-z]+);/g, (m, e) => {
    if (e[0] === '#') {
      const cp = e[1] === 'x' ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10);
      return Number.isFinite(cp) && cp >= 0 && cp <= 0x10ffff ? String.fromCodePoint(cp) : m;
    }
    return ENT[e] ?? m;
  });
}

export class PlistError extends Error {
  constructor(code) { super(code); this.code = code; }
}

// XML plist text → JS values: dict → object, array → array, string, integer and real → number (integer beyond 2^53
// stays a string), true/false, date → ISO string, data → { data: base64 }
export function parsePlist(text) {
  const s = String(text ?? '');
  if (s.startsWith('bplist')) throw new PlistError('binary');
  let i = 0;
  const tags = [];
  // tokenize: [{ open, close, self, name, text }]
  const re = /<(\/?)([A-Za-z][\w.-]*)([^>]*?)(\/?)>|<!--[\s\S]*?-->|<\?[\s\S]*?\?>|<!DOCTYPE[^>[]*(?:\[[\s\S]*?\])?\s*>|<!\[CDATA\[([\s\S]*?)\]\]>|([^<]+)/g;
  let m;
  while ((m = re.exec(s))) {
    if (m[2]) tags.push({ name: m[2], close: m[1] === '/', self: m[4] === '/' });
    else if (m[5] !== undefined) tags.push({ text: m[5], raw: true });
    else if (m[6] !== undefined) tags.push({ text: m[6] });
  }
  const skipSpace = () => { while (i < tags.length && tags[i].text !== undefined && !tags[i].raw && !tags[i].text.trim()) i++; };
  const textUntil = (name) => {
    let out = '';
    while (i < tags.length && !(tags[i].close && tags[i].name === name)) {
      if (tags[i].text === undefined) throw new PlistError('syntax');
      out += tags[i].raw ? tags[i].text : unescapeXml(tags[i].text);
      i++;
    }
    if (i >= tags.length) throw new PlistError('syntax');
    i++;
    return out;
  };
  let depth = 0;
  const value = () => {
    skipSpace();
    const tg = tags[i++];
    if (!tg || tg.text !== undefined || tg.close) throw new PlistError('syntax');
    if (++depth > 64) throw new PlistError('deep');
    try {
      switch (tg.name) {
        case 'dict': {
          const o = {};
          if (tg.self) return o;
          for (;;) {
            skipSpace();
            const k = tags[i];
            if (!k) throw new PlistError('syntax');
            if (k.close && k.name === 'dict') { i++; return o; }
            if (k.name !== 'key' || k.close) throw new PlistError('syntax');
            i++;
            const key = k.self ? '' : textUntil('key');
            const v = value();
            if (key !== '__proto__') o[key] = v;
          }
        }
        case 'array': {
          const a = [];
          if (tg.self) return a;
          for (;;) {
            skipSpace();
            const k = tags[i];
            if (!k) throw new PlistError('syntax');
            if (k.close && k.name === 'array') { i++; return a; }
            a.push(value());
          }
        }
        case 'string': return tg.self ? '' : textUntil('string');
        case 'integer': {
          const t = tg.self ? '0' : textUntil('integer').trim();
          if (!/^[+-]?(?:\d+|0x[0-9a-fA-F]+)$/.test(t)) throw new PlistError('syntax');
          const n = Number(t);
          return Number.isSafeInteger(n) ? n : t;
        }
        case 'real': {
          const t = tg.self ? '0' : textUntil('real').trim();
          const n = Number(t);
          if (t === '' || Number.isNaN(n)) throw new PlistError('syntax');
          return n;
        }
        case 'true': if (!tg.self) textUntil('true'); return true;
        case 'false': if (!tg.self) textUntil('false'); return false;
        case 'date': return tg.self ? '' : textUntil('date').trim();
        case 'data': return { data: tg.self ? '' : textUntil('data').replace(/\s+/g, '') };
        case 'plist': {
          if (tg.self) return null;
          const v = value();
          skipSpace();
          if (!tags[i] || !(tags[i].close && tags[i].name === 'plist')) throw new PlistError('syntax');
          i++;
          return v;
        }
        default: throw new PlistError('syntax');
      }
    } finally { depth--; }
  };
  const v = value();
  skipSpace();
  if (i < tags.length) throw new PlistError('syntax');
  return v;
}

// ---- Sequel Ace / Sequel Pro → favorites.json ----

const str = (v, max = 4096) => (typeof v === 'string' ? v : typeof v === 'number' ? String(v) : '').replace(/[\0\r\n]/g, '').slice(0, max);
const int = (v, dflt) => { const n = typeof v === 'number' ? v : parseInt(str(v), 10); return Number.isInteger(n) ? n : dflt; };
const flag = (v) => v === true || v === 1 || v === '1' || v === 'YES' || v === 'true';
const isGroup = (n) => n && typeof n === 'object' && Array.isArray(n.Children);
const isFav = (n) => n && typeof n === 'object' && !Array.isArray(n.Children) && (n.host !== undefined || n.name !== undefined || n.socket !== undefined || n.id !== undefined);

// One favorite of the plist → a favorites.json favorite (without our id and group yet). Only these keys are read.
export function favoriteFrom(node, source) {
  const type0 = int(node.type, 0);
  const iam = flag(node.useAWSIAMAuth);
  const type = iam ? 'cloud' : type0 === 1 ? 'socket' : type0 === 2 ? 'ssh' : 'tcp';
  const ci = int(node.colorIndex, -1);
  const given = str(node.host, 253).trim();
  const host = type === 'socket' ? '' : given || (type === 'tcp' || type === 'ssh' ? '127.0.0.1' : '');
  const f = blankFavorite({
    name: str(node.name, 120).trim() || host || str(node.socket).split('/').pop() || '',
    color: ci >= 0 && ci < FAV_COLORS.length ? ci : null,
    type,
    host,
    port: Math.min(65535, Math.max(1, int(node.port, 3306) || 3306)),
    user: str(node.user, 128),
    database: str(node.database, 64),
    socket: type === 'socket' ? str(node.socket) : '',
    ssh: {
      host: str(node.sshHost, 253).trim(), port: Math.min(65535, Math.max(1, int(node.sshPort, 22) || 22)), user: str(node.sshUser, 128),
      key: flag(node.sshKeyLocationEnabled) ? str(node.sshKeyLocation) : '',
    },
    tls: {
      mode: flag(node.useSSL) ? 'required' : 'preferred',
      ca: flag(node.sslCACertFileLocationEnabled) ? str(node.sslCACertFileLocation) : '',
      cert: flag(node.sslCertificateFileLocationEnabled) ? str(node.sslCertificateFileLocation) : '',
      key: flag(node.sslKeyFileLocationEnabled) ? str(node.sslKeyFileLocation) : '',
    },
    cloud: iam ? { service: 'rds', region: str(node.awsRegion, 64), iamProfile: str(node.awsProfile, 128) } : { service: '', region: '', iamProfile: '' },
    timeZone: str(node.timeZone, 64),
    imported: { from: source, id: str(node.id, 128) || '' },
  });
  // a certificate without its key (or the reverse) can't be used: neither is kept
  if (!f.tls.cert !== !f.tls.key) { f.tls.cert = ''; f.tls.key = ''; }
  return f;
}

// plistText → { groups, favorites, found, skipped }: what to add to `existing` (favorites.json as it is). found counts
// every favorite in the file; skipped the ones already imported before (same source and imported id).
export function importSequel(plistText, { source = 'sequel-ace', existing = { groups: [], favorites: [] } } = {}) {
  const root = parsePlist(plistText);
  const top = root && typeof root === 'object' ? (root['Favorites Root'] || root) : null;
  if (!top || typeof top !== 'object') throw new PlistError('notfavorites');
  const children = Array.isArray(top.Children) ? top.Children : Array.isArray(top) ? top : null;
  if (!children) throw new PlistError('notfavorites');

  const groupIds = (existing.groups || []).map((g) => g.id);
  const favIds = (existing.favorites || []).map((f) => f.id);
  const already = new Set((existing.favorites || []).filter((f) => f.imported && f.imported.from === source && f.imported.id).map((f) => f.imported.id));
  const groups = [], favorites = [];
  let found = 0, skipped = 0;
  const allGroups = () => [...(existing.groups || []), ...groups];

  const walk = (list, parent, depth) => {
    for (const n of list) {
      if (depth > 16) continue;
      if (isGroup(n)) {
        const name = str(n.Name, 120).trim();
        let g = allGroups().find((x) => x.name === name && (x.parent || null) === parent);
        if (!g) {
          g = { id: newGroupId([...groupIds, ...groups.map((x) => x.id)]), name, parent, expanded: n.IsExpanded === undefined ? true : flag(n.IsExpanded) };
          groups.push(g);
        }
        walk(n.Children, g.id, depth + 1);
      } else if (isFav(n)) {
        found++;
        const f = favoriteFrom(n, source);
        if (f.imported.id && already.has(f.imported.id)) { skipped++; continue; }
        if (f.imported.id) already.add(f.imported.id);
        f.id = newFavoriteId(f.name, [...favIds, ...favorites.map((x) => x.id)]);
        f.group = parent;
        favorites.push(f);
      }
    }
  };
  walk(children, null, 0);
  // groups that ended up empty (everything in them was imported before) are not added
  const used = new Set();
  const mark = (id) => { for (let g = groups.find((x) => x.id === id); g && !used.has(g.id); g = groups.find((x) => x.id === g.parent)) used.add(g.id); };
  for (const f of favorites) if (f.group) mark(f.group);
  return { groups: groups.filter((g) => used.has(g.id)), favorites, found, skipped };
}

// existing favorites.json + an import → the new file
export function mergeImport(existing, imp) {
  return { version: 1, ...existing, groups: [...(existing.groups || []), ...imp.groups], favorites: [...(existing.favorites || []), ...imp.favorites] };
}
