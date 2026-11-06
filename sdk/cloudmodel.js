// cloudmodel.js — Cloud without a screen (docs/CLOUD.md): what a file's status draws, the space bars and rings, the
// provider sheet's groups and search, which menu items a cloud path gets, the Files sidebar's order, the menu bar
// glyph and panel lines, and which state changes deserve a notification. Settings → Cloud, Files and the menu bar
// item draw from these; node server/tests/cloud_model_test.mjs checks them.

// ---- status per file (section 7.1) ----

export const STATUSES = ['cloud', 'down', 'here', 'kept', 'up', 'error'];
// the glyph's color: the mock's, one per status; 'dim' is the theme's secondary text
export const STATUS_COLOR = { cloud: 'dim', down: '#0a84ff', here: '#30d158', kept: '#30d158', up: '#bf5af2', error: '#ff9f0a' };

// One entry of cloud.status() → what to draw: { kind, pct (0..1, only while moving), key (its label) }; null for nothing.
export function statusGlyph(s) {
  if (!s || !STATUSES.includes(s.st)) return null;
  const moving = s.st === 'down' || s.st === 'up';
  const pct = moving && Number.isFinite(s.pct) ? Math.max(0, Math.min(1, s.pct > 1 ? s.pct / 100 : s.pct)) : null;
  return { kind: s.st, pct, key: `cloud.status.${s.st === 'error' ? 'error' : s.st}` };
}

// The statuses Files keeps for the visible rows: the answer for the paths it asked about replaces theirs (a path
// missing from the answer has no status any more); the rest stays as it was.
export function mergeStatus(prev, answer, asked) {
  const next = { ...(prev || {}) };
  for (const p of asked || []) {
    const s = answer && answer[p];
    if (s && STATUSES.includes(s.st)) next[p] = s; else delete next[p];
  }
  return next;
}

// Which of the visible paths to ask about: only those under a cloud account, at most `max` (the rows in sight).
export function pathsToAsk(paths, accounts, max = 400) {
  return (paths || []).filter((p) => !!accountOf(p, accounts)).slice(0, max);
}

// ---- accounts ----

// one color per account, in the order they were added (the mocks' blue, purple, orange, green first)
export const ACCOUNT_COLORS = ['#0a84ff', '#bf5af2', '#ff9f0a', '#30d158', '#ff375f', '#5e5ce6', '#64d2ff', '#ffd60a'];
export const accountColor = (accounts, id) => {
  const i = Math.max(0, (accounts || []).findIndex((a) => a.id === id));
  return ACCOUNT_COLORS[i % ACCOUNT_COLORS.length];
};

const inside = (path, root) => !!root && (path === root || String(path).startsWith(`${root}/`));
export const accountOf = (path, accounts) => (accounts || []).find((a) => inside(path, a.mount)) || null;
// the path inside its account ('' for the account's folder itself)
export const relPath = (path, acc) => (acc && inside(path, acc.mount) ? String(path).slice(acc.mount.length).replace(/^\/+/, '') : '');
// "Work › Projects › 2026": the account's name, then the folders (the file itself left out with file: true)
export function whereLine(path, acc, arrow = '›', file = true) {
  if (!acc) return '';
  const parts = relPath(path, acc).split('/').filter(Boolean);
  if (file) parts.pop();
  return [acc.name, ...parts].join(` ${arrow} `);
}

// A rclone mount under ~/Cloud is one of Cloud's accounts: Files lists it under Cloud, not under Locations.
export const isCloudVolume = (v, home) => !!v && v.fs === 'fuse.rclone' && String(v.path).startsWith(`${home}/Cloud/`);

// account states that need the person: the sidebar dims the account and shows "!"
export const NEEDS_YOU = ['signin', 'error'];
export const DIM_STATES = ['signin', 'error', 'offline', 'locked'];

// ---- space (section 8) ----

// about → { used, total (null without a quota), free, pct (0..1 or null), trashed, other }
export function usage(about) {
  if (!about) return null;
  const used = Math.max(0, Number(about.used) || 0);
  const total = about.total > 0 ? Number(about.total) : about.free != null && about.free >= 0 ? used + Number(about.free) : null;
  return {
    used, total,
    free: total != null ? Math.max(0, about.free != null ? Number(about.free) : total - used) : null,
    pct: total ? Math.min(1, used / total) : null,
    trashed: Math.max(0, Number(about.trashed) || 0),
    other: Math.max(0, Number(about.other) || 0),
  };
}
export const isFull = (about) => { const u = usage(about); return !!u && u.total != null && u.free != null && u.free <= 0; };

// The "All accounts" bar: every account's used share of what all of them hold (their totals; the used space of one
// without a quota), in the account's color.
export function allAccountsBar(accounts) {
  const list = (accounts || []).map((a) => ({ a, u: usage(a.about) }));
  const used = list.reduce((s, x) => s + (x.u ? x.u.used : 0), 0);
  const total = list.reduce((s, x) => s + (x.u ? (x.u.total != null ? x.u.total : x.u.used) : 0), 0);
  const segs = list.filter((x) => x.u && x.u.used > 0).map((x) => ({ id: x.a.id, name: x.a.name, used: x.u.used,
    pct: total ? x.u.used / total : 0, color: accountColor(accounts, x.a.id) }));
  return { used, total, segs };
}

// The account page's ring, by what the provider tells: its files, other uses and the trash, then the free part.
// [{ key, bytes, frac, color }] whose fracs add up to the used share; null without a total (no ring, a bar).
export function ringParts(about, color) {
  const u = usage(about);
  if (!u || !u.total) return null;
  const trash = Math.min(u.trashed, u.used), other = Math.min(u.other, u.used - trash);
  const files = Math.max(0, u.used - trash - other);
  return [
    { key: 'files', bytes: files, color },
    { key: 'other', bytes: other, color: '#5e5ce6' },
    { key: 'trash', bytes: trash, color: '#8e8e96' },
  ].filter((p) => p.bytes > 0).map((p) => ({ ...p, frac: p.bytes / u.total }));
}

// What the account's row and the "ready" sheet say about its space: { key, vars } for t().
export function spaceLine(about, fmt) {
  const u = usage(about);
  if (!u) return { key: 'cloud.space.unknown', vars: {} };
  if (u.total == null) return { key: 'cloud.space.usedOnly', vars: { used: fmt(u.used) } };
  return { key: 'cloud.space.usedOf', vars: { used: fmt(u.used), total: fmt(u.total) } };
}

// ---- the account's state line (Settings rows, the menu bar panel) ----

const pctOf = (x) => (x && x.bytes > 0 ? Math.round(Math.min(1, (x.done || 0) / x.bytes) * 100) : null);
export function stateLine(acc) {
  const up = acc.up || {}, down = acc.down || {};
  switch (acc.state) {
    case 'syncing':
      if (up.files > 0 && up.name) return { key: 'cloud.state.uploading', vars: { name: up.name, pct: pctOf(up) ?? 0 }, tone: 'busy' };
      if (down.files > 0 && down.name) return { key: 'cloud.state.downloading', vars: { name: down.name, pct: pctOf(down) ?? 0 }, tone: 'busy' };
      return { key: 'cloud.state.syncing', vars: { count: (up.files || 0) + (down.files || 0) }, tone: 'busy' };
    case 'paused': return { key: up.files > 0 ? 'cloud.state.pausedWaiting' : 'cloud.state.paused', vars: { count: up.files || 0 }, tone: 'dim' };
    case 'signin': return { key: 'cloud.state.signin', vars: {}, tone: 'warn' };
    case 'offline': return { key: up.files > 0 ? 'cloud.state.offlineWaiting' : 'cloud.state.offline', vars: { count: up.files || 0 }, tone: 'warn' };
    case 'error': return { key: 'cloud.state.error', vars: {}, tone: 'warn' };
    case 'locked': return { key: 'cloud.state.locked', vars: {}, tone: 'warn' };
    default: return { key: 'cloud.state.ok', vars: {}, tone: 'dim' };
  }
}

// Files' header pill and the panel's header: files coming down and going up, in one account or in all.
export function transfers(accounts, id = null) {
  let down = 0, up = 0;
  for (const a of accounts || []) {
    if (id && a.id !== id) continue;
    down += (a.down && a.down.files) || 0;
    up += (a.up && a.up.files) || 0;
  }
  return { down, up };
}

// ---- the provider sheet (section 6.1) ----

export const GROUPS = ['oauth', 'server', 'object'];      // crypt (an encrypted folder) shows with object storage, as in the mock
const SHORT = { oauth: 5, server: 3, object: 2 };
const groupOf = (p) => (p.group === 'crypt' ? 'object' : p.group);
const fold = (s) => String(s || '').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '');

// → [{ group, items, more }]: each group's first few (more = how many "More…" holds), all of a group once expanded,
// and every match while searching (label, hint and kind).
export function groupProviders(list, query = '', expanded = []) {
  const q = fold(query).trim();
  const out = [];
  for (const g of GROUPS) {
    let items = (list || []).filter((p) => groupOf(p) === g);
    if (q) items = items.filter((p) => [p.label, p.description, p.hint, p.kind].some((x) => fold(x).includes(q)));
    if (!items.length) continue;
    // the encrypted folder always last in its group
    items = [...items.filter((p) => p.group !== 'crypt'), ...items.filter((p) => p.group === 'crypt')];
    const n = SHORT[g] + (g === 'object' && items.some((p) => p.group === 'crypt') ? 1 : 0);
    if (q || expanded.includes(g) || items.length <= n + 1) { out.push({ group: g, items, more: 0 }); continue; }
    const crypt = items.filter((p) => p.group === 'crypt');
    const head = [...items.filter((p) => p.group !== 'crypt').slice(0, SHORT[g]), ...crypt];
    out.push({ group: g, items: head, more: items.length - head.length });
  }
  return out;
}

// the provider tile's letter: its label's first letters ("S3", "B2" keep their digits)
export function providerMark(label) {
  const words = String(label || '').trim().split(/\s+/).filter(Boolean);
  if (!words.length) return '?';
  const last = words[words.length - 1];
  if (/^[A-Za-z][0-9]$/.test(last)) return last.toUpperCase();          // "Backblaze B2" → B2
  if (/^[A-Za-z][0-9]/.test(words[0])) return words[0].slice(0, 2).toUpperCase();   // "S3" → S3
  return [...words[0]][0];
}
const MARK_COLORS = ['#0a84ff', '#2f6fd6', '#17a2b8', '#e5484d', '#d9822b', '#30d158', '#5e5ce6', '#8e8e93', '#ff9f0a', '#e0413a', '#bf5af2'];
export const providerColor = (p) => {
  if (p && p.group === 'crypt') return '#3a3a42';
  let h = 0;
  for (const c of String(p && p.kind)) h = (h * 31 + c.charCodeAt(0)) >>> 0;
  return MARK_COLORS[h % MARK_COLORS.length];
};

// ---- account name, credentials ----

export const NAME_MAX = 40;
// what is wrong with a name for a new account (its folder in ~/Cloud), or null
export function nameProblem(name, accounts, self = null) {
  const n = String(name || '').trim();
  if (!n) return 'empty';
  if (n.length > NAME_MAX) return 'long';
  if (/[/\\\x00-\x1f]/.test(n) || n === '.' || n === '..' || n.startsWith('.')) return 'chars';
  if ((accounts || []).some((a) => a.id !== self && a.name.toLowerCase() === n.toLowerCase())) return 'taken';
  return null;
}
// a provider's form: its fields with the defaults filled in
export const formValues = (provider, prev = {}) => Object.fromEntries((provider?.fields || []).map((f) => [f.name, prev[f.name] ?? (f.default != null ? String(f.default) : '')]));
// the first required field still empty, or null
export const missingField = (provider, values) => (provider?.fields || []).find((f) => f.required && !String(values[f.name] ?? '').trim())?.name || null;
// only the fields the provider has, without control characters (lord-server checks again)
export const cleanFields = (provider, values) => Object.fromEntries((provider?.fields || []).filter((f) => String(values[f.name] ?? '') !== '')
  .map((f) => [f.name, String(values[f.name]).replace(/[\x00-\x1f\x7f]/g, '')]));

// ---- sign-in in the browser (section 6.2) ----

export const toMs = (t) => (!t ? 0 : t < 1e12 ? t * 1000 : t);
export function countdown(expiresAt, now = Date.now()) {
  const s = Math.max(0, Math.ceil((toMs(expiresAt) - now) / 1000));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}

// ---- sharing (section 9) ----

export const EXPIRY = ['1d', '7d', '30d', 'never'];
export const DEFAULT_EXPIRY = '7d';
export const linkOf = (links, path) => (links || []).find((l) => l.path === path) || null;
// a link's line: { key, vars }; expires is a time (s or ms), 0/null for never
export function expiryLine(link, fmtDate) {
  if (!link || !link.expires) return { key: 'cloud.link.never', vars: {} };
  return { key: 'cloud.link.expires', vars: { date: fmtDate(new Date(toMs(link.expires))) } };
}

// ---- the context menu (section 7.1) ----

// What Files adds for files and folders of an account. sel: [{ st, dir }] (st may be undefined), opts: { single,
// linksSupported, shared, website }. → [{ id, checked?, disabled? }] in the mock's order; 'sep' between groups.
export function cloudMenu(sel, { single = false, linksSupported = false, shared = false, website = false } = {}) {
  const list = sel || [];
  if (!list.length) return [];
  const allKept = list.every((x) => x.st === 'kept');
  const nothingHere = list.every((x) => !x.dir && x.st === 'cloud');
  const out = [{ id: 'keep', checked: allKept }, { id: 'free', disabled: nothingHere }];
  const share = [];
  if (single && linksSupported) share.push({ id: 'copyLink' }, { id: 'shareLink' });
  if (single && website) share.push({ id: 'openWebsite' });
  if (single && shared) share.push({ id: 'stopSharing' });
  return share.length ? [...out, 'sep', ...share] : out;
}

// ---- Files' sidebar ----

export const SIDE_SECTIONS = ['favorites', 'cloud', 'locations', 'tags'];
export const sideSections = (accounts) => SIDE_SECTIONS.filter((s) => s !== 'cloud' || (accounts || []).length > 0);
// the Cloud section's rows: the accounts in their order; hidden: ids the person took out of Files
export function cloudSideItems(accounts, hidden = []) {
  return (accounts || []).filter((a) => !hidden.includes(a.id)).map((a) => {
    const u = usage(a.about);
    return { id: a.id, name: a.name, path: a.mount, color: accountColor(accounts, a.id), pct: u && u.pct != null ? u.pct : null,
      alert: NEEDS_YOU.includes(a.state), dim: DIM_STATES.includes(a.state) };
  });
}

// ---- the menu bar item (section 11) ----

// { arrow: 'up' | 'down' | null, badge: '!' | count | null }: an arrow while files move, "!" when an account needs
// the person, else how many changes wait (offline or paused).
export function menuGlyph(state) {
  const accs = (state && state.accounts) || [];
  const t = transfers(accs.filter((a) => a.state === 'syncing'));
  const arrow = t.up > 0 ? 'up' : t.down > 0 ? 'down' : null;
  if (accs.some((a) => NEEDS_YOU.includes(a.state) || isFull(a.about) && a.up && a.up.files > 0)) return { arrow, badge: '!' };
  const waiting = accs.filter((a) => a.state === 'offline' || a.state === 'paused').reduce((s, a) => s + ((a.up && a.up.files) || 0), 0);
  return { arrow, badge: waiting > 0 ? waiting : null };
}
export const allPaused = (state) => { const a = (state && state.accounts) || []; return a.length > 0 && a.every((x) => x.state === 'paused'); };

// how long ago, as the panel says it: now, minutes, hours, days
export function ago(at, now = Date.now()) {
  const s = Math.max(0, (now - toMs(at)) / 1000);
  if (s < 45) return { key: 'cloud.ago.now', vars: {} };
  if (s < 3600) return { key: 'cloud.ago.min', vars: { count: Math.max(1, Math.round(s / 60)) } };
  if (s < 86400) return { key: 'cloud.ago.h', vars: { count: Math.round(s / 3600) } };
  return { key: 'cloud.ago.d', vars: { count: Math.round(s / 86400) } };
}
// The panel's Recent rows: [{ path, name, where, dir, ago }] newest first, at most n.
export function recentRows(recent, accounts, now = Date.now(), n = 5) {
  return (recent || []).slice(0, n).map((r) => {
    const acc = (accounts || []).find((a) => a.id === r.account) || accountOf(r.path, accounts);
    return { path: r.path, name: String(r.path).split('/').pop(), where: whereLine(r.path, acc), dir: r.dir === 'up' ? 'up' : 'down', ago: ago(r.at, now) };
  });
}

// ---- notifications (the States mock) ----

// What changed between two pushed states that the person should hear about:
//   { type: 'signin', id, name }   an account started to need a new sign-in
//   { type: 'full', id, name, waiting }   an account filled up with changes waiting to go up
//   { type: 'conflict', id, name, path, kept }   a file changed here and in the cloud (state.conflicts, newest first)
export function transitions(prev, next) {
  const out = [];
  if (!prev || !next) return out;
  const before = new Map((prev.accounts || []).map((a) => [a.id, a]));
  for (const a of next.accounts || []) {
    const b = before.get(a.id);
    if (!b) continue;
    if (a.state === 'signin' && b.state !== 'signin') out.push({ type: 'signin', id: a.id, name: a.name });
    const waiting = (a.up && a.up.files) || 0;
    if (isFull(a.about) && waiting > 0 && !(isFull(b.about) && ((b.up && b.up.files) || 0) > 0)) out.push({ type: 'full', id: a.id, name: a.name, waiting });
  }
  const seen = new Set((prev.conflicts || []).map((c) => `${c.path}|${c.at}`));
  for (const c of next.conflicts || []) {
    if (seen.has(`${c.path}|${c.at}`)) continue;
    const acc = (next.accounts || []).find((a) => a.id === c.account) || accountOf(c.path, next.accounts);
    out.push({ type: 'conflict', id: acc ? acc.id : null, name: acc ? acc.name : '', path: c.path, kept: c.kept });
  }
  return out;
}

// "Proposal v3.odt" → "Proposal v3 (this computer).odt", with the translated words
export function conflictName(name, words) {
  const i = name.lastIndexOf('.');
  return i > 0 ? `${name.slice(0, i)} (${words})${name.slice(i)}` : `${name} (${words})`;
}

// ---- transfer speed (Get Info while a file comes down) ----

// samples [{ at (ms), done (bytes) }] → { rate (bytes/s), left (s) } over the last few seconds, or null
export function rateOf(samples, total) {
  const s = (samples || []).filter((x) => Number.isFinite(x.done));
  if (s.length < 2) return null;
  const a = s[0], b = s[s.length - 1];
  const dt = (b.at - a.at) / 1000;
  if (dt <= 0 || b.done <= a.done) return null;
  const rate = (b.done - a.done) / dt;
  return { rate, left: total > b.done ? Math.ceil((total - b.done) / rate) : 0 };
}

// ---- verified providers (docs/CLOUD.md 19, assets/cloud/providers.json) ----

// data: the file's object; → its entry for a kind ({ since, date, evidence }) or null
export const verifiedInfo = (kind, data) => ((data && data.verified) || {})[kind] || null;
export const isVerified = (kind, data) => !!verifiedInfo(kind, data);
// a provider item: its own `verified` (17.2, from lord-server's overlay) or else the file's
const verifiedItem = (p, data) => (p && typeof p.verified === 'boolean' ? p.verified : isVerified(p && p.kind, data));
// The sheet's two parts: { verified: groups (groupProviders), unverified: groups, unverifiedCount, open }. While searching
// both are searched and the Not verified part opens by itself when it has matches.
export function splitProviders(list, data, query = '', expanded = [], showUnverified = false) {
  const v = (list || []).filter((p) => verifiedItem(p, data));
  const u = (list || []).filter((p) => !verifiedItem(p, data));
  const q = String(query || '').trim();
  const unverified = groupProviders(u, q, ['oauth', 'server', 'object']);
  return { verified: groupProviders(v, q, expanded), unverified, unverifiedCount: unverified.reduce((s, g) => s + g.items.length, 0),
    open: showUnverified || (!!q && unverified.length > 0) };
}
// a browser sign-in provider not verified yet: the add sheet offers the person's own app right away
export const ownAppOffered = (p, data) => !!p && p.group === 'oauth' && !verifiedItem(p, data);
// rclone's page for a backend (How to make one): its kind, but a few pages have their own names
const DOC_PAGE = { gcs: 'googlecloudstorage', 'google cloud storage': 'googlecloudstorage', nextcloud: 'webdav', owncloud: 'webdav' };
export const rcloneDocsUrl = (kind) => {
  const k = String(kind || '').toLowerCase();
  const page = DOC_PAGE[k] || (/^[a-z0-9]+$/.test(k) ? k : '');
  return page ? `https://rclone.org/${page}/` : 'https://rclone.org/overview/';
};
export const OAUTH_REDIRECT = 'http://127.0.0.1:53682/';

// ---- 17.2: rclone's questions, notes before a provider, Measure, delete in the cloud ----

// How to ask one of rclone's questions ({ state, name, help, type, examples, isPassword, required, default }):
// { input: 'bool' | 'choice' | 'number' | 'password' | 'text', options: [[value, label]], value: the starting value }
export function questionInput(q) {
  const qq = q || {};
  const type = String(qq.type || '').toLowerCase();
  const ex = (qq.examples || []).map((e) => (e && typeof e === 'object' ? [String(e.value ?? e.Value ?? ''), String(e.help ?? e.Help ?? e.value ?? e.Value ?? '')] : [String(e), String(e)]))
    .filter(([v]) => v !== '');
  const def = qq.default == null ? '' : String(qq.default);
  if (type === 'bool') return { input: 'bool', options: [], value: def === 'true' ? 'true' : 'false' };
  if (qq.isPassword) return { input: 'password', options: [], value: '' };
  if (ex.length >= 2 && ex.length <= 12 && !/^(int|sizesuffix|duration)/.test(type)) return { input: 'choice', options: ex, value: ex.some(([v]) => v === def) ? def : ex[0][0] };
  if (/^(int|uint|float|sizesuffix)/.test(type)) return { input: 'number', options: [], value: def };
  return { input: 'text', options: ex, value: def };
}
// the answer as rclone takes it, or null while it can't be sent (required and empty, a number that isn't one)
export function questionAnswer(q, raw) {
  const how = questionInput(q);
  const v = raw == null ? '' : String(raw).trim();
  if (how.input === 'bool') return v === 'true' ? 'true' : 'false';
  if (how.input === 'number' && v && !/^-?\d+(\.\d+)?[a-zA-Z]?$/.test(v)) return null;
  if (!v && q && q.required) return null;
  return how.input === 'password' ? String(raw ?? '') : v;
}

// What some providers need said before their sign-in starts (from rclone's documentation, in our words): keys of cloud.json
const NOTES = { iclouddrive: ['cloud.notes.iclouddrive.adp', 'cloud.notes.iclouddrive.web', 'cloud.notes.iclouddrive.code'] };
export const notesFor = (kind) => NOTES[String(kind || '')] || [];

// Measure (17.2): a provider without a quota can be measured; the line once it was
export const canMeasure = (about) => !about || !(about.total > 0);
export function measuredLine(about, fmt, fmtTime) {
  if (!about || !about.measuredAt) return null;
  return { key: 'cloud.measure.done', vars: { used: fmt(about.used || 0), when: fmtTime(new Date(toMs(about.measuredAt))) } };
}

// Move to Trash in Files (17.2): paths in a cloud account are deleted there through cloud.delete, never moved into a
// .Trash-<uid> folder of the cloud. → { local: [paths], cloud: [{ acc, paths }] }; an account's own folder never goes.
export function splitTrash(paths, accounts) {
  const local = [], by = new Map();
  for (const p of paths || []) {
    const a = accountOf(p, accounts);
    if (!a) { local.push(p); continue; }
    if (p === a.mount) continue;
    if (!by.has(a.id)) by.set(a.id, { acc: a, paths: [] });
    by.get(a.id).paths.push(p);
  }
  return { local, cloud: [...by.values()] };
}
// the confirmation for one account's paths: { title, detail } keys and vars
export const deletePrompt = (acc, count) => ({
  title: { key: 'cloud.delete.title', vars: { provider: (acc && (acc.provider || acc.kind)) || '', count } },
  detail: { key: acc && acc.trashSupported ? 'cloud.delete.toTrash' : 'cloud.delete.final', vars: { provider: (acc && (acc.provider || acc.kind)) || '' } },
});
