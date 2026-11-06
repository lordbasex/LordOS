// Cloud without a screen (docs/CLOUD.md): the status glyphs, the space math, the provider sheet, the context menu,
// Files' sidebar, the menu bar item, notifications, the dev mock's shape against section 17, the glyph pixels, the
// file dialog's Cloud places and that every text the model names is in the catalogs.
// node server/tests/cloud_model_test.mjs
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as M from '../../sdk/cloudmodel.js';
import { glyphPixels, ringPixels, GLYPH_KINDS, glyphCacheSize } from '../../sdk/cloudglyphs.js';
import { cloudPlaces, cloudRoot, pathCrumbs } from '../../sdk/filepickermodel.js';
import { installCloudMock } from '../../sdk/cloudmock.js';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };
const near = (name, a, b, e = 1e-9) => eq(name, Math.abs(a - b) <= e, true);
const fmt = (n) => `${Math.round(n / 1e9)} GB`;
const H = '/home/ana';
const acc = (id, name, extra = {}) => ({ id, name, kind: 'webdav', provider: 'WebDAV', mount: `${H}/Cloud/${name}`, state: 'ok', about: null, cacheBytes: 0, keptBytes: 0,
  up: { files: 0, bytes: 0, done: 0 }, down: { files: 0, bytes: 0, done: 0 }, kept: [], linksSupported: true, ...extra });

// ---- status → glyph ----
eq('every status draws', M.STATUSES.map((s) => M.statusGlyph({ st: s }).kind), M.STATUSES);
eq('unknown draws nothing', [M.statusGlyph(null), M.statusGlyph({ st: 'weird' }), M.statusGlyph({})], [null, null, null]);
eq('pct only while moving', [M.statusGlyph({ st: 'down', pct: 0.4 }).pct, M.statusGlyph({ st: 'here', pct: 0.4 }).pct], [0.4, null]);
eq('pct in percent too, clamped', [M.statusGlyph({ st: 'up', pct: 64 }).pct, M.statusGlyph({ st: 'up', pct: -3 }).pct, M.statusGlyph({ st: 'up', pct: 1 }).pct], [0.64, 0, 1]);
eq('labels', M.statusGlyph({ st: 'kept' }).key, 'cloud.status.kept');
{
  const prev = { '/a': { st: 'cloud' }, '/b': { st: 'here' }, '/c': { st: 'up' } };
  const next = M.mergeStatus(prev, { '/a': { st: 'down', pct: 0.1 }, '/z': { st: 'kept' } }, ['/a', '/b']);
  eq('merge: asked replaces, missing goes, others stay, unasked ignored', next, { '/a': { st: 'down', pct: 0.1 }, '/c': { st: 'up' } });
  eq('merge drops junk', M.mergeStatus({}, { '/a': { st: 'nope' } }, ['/a']), {});
}
{
  const accs = [acc('1', 'Work'), acc('2', 'Personal')];
  eq('ask only cloud paths', M.pathsToAsk([`${H}/Cloud/Work/a`, `${H}/Documents/b`, `${H}/Cloud/Personal`, `${H}/Cloud/Workshop/x`], accs), [`${H}/Cloud/Work/a`, `${H}/Cloud/Personal`]);
  eq('ask at most n', M.pathsToAsk(Array.from({ length: 10 }, (_, i) => `${H}/Cloud/Work/${i}`), accs, 3).length, 3);
  eq('account of', [M.accountOf(`${H}/Cloud/Work/x/y`, accs)?.id, M.accountOf(`${H}/Cloud/Workshop`, accs), M.accountOf(`${H}/Cloud/Work`, accs)?.id], ['1', null, '1']);
  eq('rel path', [M.relPath(`${H}/Cloud/Work/Projects/2026/a.pdf`, accs[0]), M.relPath(`${H}/Cloud/Work`, accs[0])], ['Projects/2026/a.pdf', '']);
  eq('where line', [M.whereLine(`${H}/Cloud/Work/Projects/a.pdf`, accs[0]), M.whereLine(`${H}/Cloud/Work/Projects/2026`, accs[0], '·', false)], ['Work › Projects', 'Work · Projects · 2026']);
  eq('account colors in order, then around', [M.accountColor(accs, '1'), M.accountColor(accs, '2')], ['#0a84ff', '#bf5af2']);
  eq('cloud volume', [M.isCloudVolume({ fs: 'fuse.rclone', path: `${H}/Cloud/Work` }, H), M.isCloudVolume({ fs: 'fuse.rclone', path: `${H}/.local/share/lordos/remote/x` }, H), M.isCloudVolume({ fs: 'ext4', path: `${H}/Cloud/W` }, H)], [true, false, false]);
}

// ---- space ----
{
  eq('no about', M.usage(null), null);
  const u = M.usage({ total: 1e12, used: 540e9, free: 460e9, trashed: 20e9, other: 30e9 });
  eq('usage', [u.used, u.total, u.free, u.pct, u.trashed, u.other], [540e9, 1e12, 460e9, 0.54, 20e9, 30e9]);
  eq('total from used + free', M.usage({ used: 10, free: 30 }).total, 40);
  eq('no quota', [M.usage({ used: 161e9 }).total, M.usage({ used: 161e9 }).pct, M.usage({}).used], [null, null, 0]);
  eq('over quota clamps', M.usage({ total: 10, used: 12 }).pct, 1);
  eq('full', [M.isFull({ total: 10, used: 10, free: 0 }), M.isFull({ total: 10, used: 9 }), M.isFull({ used: 5 }), M.isFull(null)], [true, false, false, false]);
  const accs = [acc('1', 'Personal', { about: { total: 2e12, used: 612e9 } }), acc('2', 'Work', { about: { total: 1e12, used: 540e9 } }),
    acc('3', 'Photos', { about: { used: 161e9 } }), acc('4', 'NAS', { about: { total: 300e9, used: 7.4e9 } }), acc('5', 'New', { about: null })];
  const bar = M.allAccountsBar(accs);
  eq('all accounts: used and total (no quota counts its used)', [bar.used, bar.total], [612e9 + 540e9 + 161e9 + 7.4e9, 2e12 + 1e12 + 161e9 + 300e9]);
  eq('segments skip unknown', bar.segs.map((s) => s.id), ['1', '2', '3', '4']);
  near('segment share', bar.segs[0].pct, 612e9 / 3.461e12);
  eq('segments fit', bar.segs.reduce((s, x) => s + x.pct, 0) <= 1, true);
  const parts = M.ringParts({ total: 1e12, used: 540e9, trashed: 20e9, other: 30e9 }, '#bf5af2');
  eq('ring parts', parts.map((p) => [p.key, p.bytes, p.color]), [['files', 490e9, '#bf5af2'], ['other', 30e9, '#5e5ce6'], ['trash', 20e9, '#8e8e96']]);
  near('ring adds to used share', parts.reduce((s, p) => s + p.frac, 0), 0.54);
  eq('ring needs a total', M.ringParts({ used: 5 }, '#000'), null);
  eq('ring with trash over used', M.ringParts({ total: 100, used: 10, trashed: 50 }, '#000').map((p) => [p.key, p.bytes]), [['trash', 10]]);
  eq('space lines', [M.spaceLine({ total: 1e12, used: 540e9 }, fmt), M.spaceLine({ used: 161e9 }, fmt), M.spaceLine(null, fmt).key],
    [{ key: 'cloud.space.usedOf', vars: { used: '540 GB', total: '1000 GB' } }, { key: 'cloud.space.usedOnly', vars: { used: '161 GB' } }, 'cloud.space.unknown']);
}

// ---- state lines and transfers ----
{
  eq('ok', M.stateLine(acc('1', 'A')).key, 'cloud.state.ok');
  eq('uploading', M.stateLine(acc('1', 'A', { state: 'syncing', up: { files: 1, bytes: 100, done: 40, name: 'Budget 2027.ods' } })), { key: 'cloud.state.uploading', vars: { name: 'Budget 2027.ods', pct: 40 }, tone: 'busy' });
  eq('syncing count', M.stateLine(acc('1', 'A', { state: 'syncing', up: { files: 2 }, down: { files: 1 } })).vars.count, 3);
  eq('waiting', [M.stateLine(acc('1', 'A', { state: 'offline', up: { files: 4 } })).key, M.stateLine(acc('1', 'A', { state: 'paused' })).key], ['cloud.state.offlineWaiting', 'cloud.state.paused']);
  eq('signin warns', M.stateLine(acc('1', 'A', { state: 'signin' })).tone, 'warn');
  const accs = [acc('1', 'A', { up: { files: 1 }, down: { files: 2 } }), acc('2', 'B', { up: { files: 3 } })];
  eq('transfers', [M.transfers(accs), M.transfers(accs, '2'), M.transfers([])], [{ down: 2, up: 4 }, { down: 0, up: 3 }, { down: 0, up: 0 }]);
}

// ---- the provider sheet ----
{
  const P = (kind, label, group, hint = '') => ({ kind, label, group, hint, fields: [] });
  const list = [P('dropbox', 'Dropbox', 'oauth'), P('box', 'Box', 'oauth'), P('pcloud', 'pCloud', 'oauth'), P('yandex', 'Yandex Disk', 'oauth'), P('zoho', 'Zoho WorkDrive', 'oauth', 'Business'),
    P('hidrive', 'HiDrive', 'oauth'), P('jotta', 'Jottacloud', 'oauth'), P('webdav', 'WebDAV', 'server', 'Nextcloud, ownCloud…'), P('sftp', 'SFTP', 'server'), P('smb', 'SMB', 'server'),
    P('crypt', 'Encrypted folder', 'crypt'), P('s3', 'S3', 'object'), P('b2', 'Backblaze B2', 'object'), P('swift', 'Swift', 'object')];
  const g = M.groupProviders(list);
  eq('groups in order', g.map((x) => x.group), ['oauth', 'server', 'object']);
  eq('short list and More…', [g[0].items.length, g[0].more], [5, 2]);
  eq('servers all fit', [g[1].items.map((p) => p.kind), g[1].more], [['webdav', 'sftp', 'smb'], 0]);
  eq('object storage: one more than fits shows, no "1 more" tile; the encrypted folder last', [g[2].items.map((p) => p.kind), g[2].more], [['s3', 'b2', 'swift', 'crypt'], 0]);
  const more = M.groupProviders([...list, P('minio', 'MinIO', 'object'), P('wasabi', 'Wasabi', 'object')]);
  eq('object storage: two, the encrypted folder, then More…', [more[2].items.map((p) => p.kind), more[2].more], [['s3', 'b2', 'crypt'], 3]);
  eq('expanded', M.groupProviders(list, '', ['oauth'])[0].items.length, 7);
  eq('search label', M.groupProviders(list, 'drive').flatMap((x) => x.items.map((p) => p.kind)), ['zoho', 'hidrive']);
  eq('search hint, accents folded', M.groupProviders(list, 'NEXTCLOUD').flatMap((x) => x.items.map((p) => p.kind)), ['webdav']);
  eq('search kind', M.groupProviders(list, 'b2').flatMap((x) => x.items.map((p) => p.kind)), ['b2']);
  eq('no match', M.groupProviders(list, 'zzz'), []);
  eq('marks', [M.providerMark('S3'), M.providerMark('Backblaze B2'), M.providerMark('pCloud'), M.providerMark('')], ['S3', 'B2', 'p', '?']);
  eq('crypt color', M.providerColor({ group: 'crypt', kind: 'crypt' }), '#3a3a42');
  eq('stable color', M.providerColor({ kind: 'webdav' }) === M.providerColor({ kind: 'webdav' }), true);
}

// ---- names and credentials ----
{
  const accs = [acc('1', 'Work')];
  eq('names', ['', ' ', 'work', 'Wo/rk', '.hidden', 'a'.repeat(41), 'Personal', 'Work'].map((n) => M.nameProblem(n, accs)), ['empty', 'empty', 'taken', 'chars', 'chars', 'long', null, 'taken']);
  eq('renaming itself', M.nameProblem('Work', accs, '1'), null);
  const p = { fields: [{ name: 'url', required: true }, { name: 'vendor', default: 'nextcloud' }, { name: 'pass', secret: true, required: true }] };
  eq('defaults', M.formValues(p), { url: '', vendor: 'nextcloud', pass: '' });
  eq('keeps typed', M.formValues(p, { url: 'https://x' }).url, 'https://x');
  eq('missing', [M.missingField(p, { url: ' ', pass: 'x' }), M.missingField(p, { url: 'u', pass: '' }), M.missingField(p, { url: 'u', pass: 'p' })], ['url', 'pass', null]);
  eq('clean: only its fields, no control characters, no empties', M.cleanFields(p, { url: 'https://x\n', vendor: '', pass: 'p\u0000w', evil: 'x' }), { url: 'https://x', pass: 'pw' });
}

// ---- sign-in, links ----
{
  const T = 1.7e12;
  eq('countdown', [M.countdown(T + 271e3, T), M.countdown(T - 5, T), M.countdown(1700000300, 1700000000 * 1000)], ['4:31', '0:00', '5:00']);
  eq('expiry choices', M.EXPIRY, ['1d', '7d', '30d', 'never']);
  eq('default expiry is a choice', M.EXPIRY.includes(M.DEFAULT_EXPIRY), true);
  const d = (x) => x.toISOString().slice(0, 10);
  eq('expiry lines (seconds or ms)', [M.expiryLine({ expires: 0 }, d).key, M.expiryLine({ expires: 1794614400 }, d), M.expiryLine({ expires: 1794614400000 }, d).vars.date],
    ['cloud.link.never', { key: 'cloud.link.expires', vars: { date: d(new Date(1794614400000)) } }, d(new Date(1794614400000))]);
  eq('link of', M.linkOf([{ path: '/a', url: 'u' }], '/a').url, 'u');
}

// ---- the context menu ----
{
  const ids = (l) => l.map((x) => (x === 'sep' ? '|' : x.id + (x.checked ? '+' : '') + (x.disabled ? '-' : ''))).join(' ');
  eq('a file in the cloud, links', ids(M.cloudMenu([{ st: 'cloud' }], { single: true, linksSupported: true, website: true })), 'keep free- | copyLink shareLink openWebsite');
  eq('a folder: Free Up Space is offered', ids(M.cloudMenu([{ st: 'cloud', dir: true }], { single: true })), 'keep free');
  eq('kept: checked (a click stops keeping)', ids(M.cloudMenu([{ st: 'kept' }, { st: 'kept' }], {})), 'keep+ free');
  eq('mixed', ids(M.cloudMenu([{ st: 'kept' }, { st: 'here' }], {})), 'keep free');
  eq('shared adds Stop Sharing', ids(M.cloudMenu([{ st: 'here' }], { single: true, linksSupported: true, shared: true })), 'keep free | copyLink shareLink stopSharing');
  eq('no public links: only the website', ids(M.cloudMenu([{ st: 'here' }], { single: true, linksSupported: false, website: true })), 'keep free | openWebsite');
  eq('many: no link items', ids(M.cloudMenu([{ st: 'here' }, { st: 'cloud' }], { single: false, linksSupported: true, website: true })), 'keep free');
  eq('unknown status counts as not kept', ids(M.cloudMenu([{}], {})), 'keep free');
  eq('nothing selected', M.cloudMenu([], {}), []);
}

// ---- Files' sidebar ----
{
  eq('sections', [M.sideSections([]), M.sideSections([acc('1', 'A')])], [['favorites', 'locations', 'tags'], ['favorites', 'cloud', 'locations', 'tags']]);
  eq('cloud between Favorites and Locations', M.SIDE_SECTIONS.indexOf('cloud') === M.SIDE_SECTIONS.indexOf('favorites') + 1 && M.SIDE_SECTIONS.indexOf('locations') === M.SIDE_SECTIONS.indexOf('cloud') + 1, true);
  const accs = [acc('1', 'Personal', { about: { total: 100, used: 31 } }), acc('2', 'Work', { state: 'syncing' }), acc('3', 'NAS', { state: 'signin', about: { total: 10, used: 1 } }), acc('4', 'Off', { state: 'offline' })];
  const items = M.cloudSideItems(accs, ['2']);
  eq('rows', items.map((i) => [i.name, i.pct, i.alert, i.dim]), [['Personal', 0.31, false, false], ['NAS', 0.1, true, true], ['Off', null, false, true]]);
  eq('row path and color', [items[0].path, items[0].color], [`${H}/Cloud/Personal`, '#0a84ff']);
}

// ---- the menu bar item ----
{
  const st = (accounts) => ({ accounts });
  eq('idle', M.menuGlyph(st([acc('1', 'A')])), { arrow: null, badge: null });
  eq('uploading', M.menuGlyph(st([acc('1', 'A', { state: 'syncing', up: { files: 2 }, down: { files: 1 } })])).arrow, 'up');
  eq('downloading', M.menuGlyph(st([acc('1', 'A', { state: 'syncing', down: { files: 1 } })])).arrow, 'down');
  eq('needs you', M.menuGlyph(st([acc('1', 'A', { state: 'signin' })])).badge, '!');
  eq('full with waiting', M.menuGlyph(st([acc('1', 'A', { about: { total: 1, used: 1, free: 0 }, up: { files: 3 } })])).badge, '!');
  eq('waiting changes count', M.menuGlyph(st([acc('1', 'A', { state: 'offline', up: { files: 4 } }), acc('2', 'B', { state: 'paused', up: { files: 1 } })])).badge, 5);
  eq('all paused', [M.allPaused(st([acc('1', 'A', { state: 'paused' })])), M.allPaused(st([acc('1', 'A', { state: 'paused' }), acc('2', 'B')])), M.allPaused(st([]))], [true, false, false]);
  const now = 1.7e12;
  eq('ago', [M.ago(now - 10e3, now).key, M.ago(now - 60e3, now).vars.count, M.ago(now - 4 * 60e3, now).vars.count, M.ago(now - 3600e3, now).key, M.ago(now - 3 * 864e5, now).vars.count],
    ['cloud.ago.now', 1, 4, 'cloud.ago.h', 3]);
  const accs = [acc('2', 'Work')];
  const rows = M.recentRows([{ path: `${H}/Cloud/Work/Projects/Budget 2027.ods`, account: '2', dir: 'up', at: now }, { path: `${H}/Cloud/Work/x.jpg`, account: '9', dir: 'down', at: now - 7200e3 }], accs, now);
  eq('recent rows', rows.map((r) => [r.name, r.where, r.dir, r.ago.key]), [['Budget 2027.ods', 'Work › Projects', 'up', 'cloud.ago.now'], ['x.jpg', 'Work', 'down', 'cloud.ago.h']]);
  eq('recent at most n', M.recentRows(Array.from({ length: 20 }, (_, i) => ({ path: `/x/${i}`, at: now })), [], now, 5).length, 5);
}

// ---- notifications ----
{
  const a = (extra) => acc('1', 'Home NAS', extra);
  eq('sign-in expired once', M.transitions({ accounts: [a()] }, { accounts: [a({ state: 'signin' })] }), [{ type: 'signin', id: '1', name: 'Home NAS' }]);
  eq('still expired: quiet', M.transitions({ accounts: [a({ state: 'signin' })] }, { accounts: [a({ state: 'signin' })] }), []);
  const full = { total: 2, used: 2, free: 0 };
  eq('full with changes waiting', M.transitions({ accounts: [a({ about: full })] }, { accounts: [a({ about: full, up: { files: 14 } })] }), [{ type: 'full', id: '1', name: 'Home NAS', waiting: 14 }]);
  eq('full and nothing waiting: quiet', M.transitions({ accounts: [a()] }, { accounts: [a({ about: full })] }), []);
  eq('a new account: quiet', M.transitions({ accounts: [] }, { accounts: [a({ state: 'signin' })] }), []);
  const c = { path: `${H}/Cloud/Home NAS/Proposal v3.odt`, kept: `${H}/Cloud/Home NAS/Proposal v3 (this computer).odt`, account: '1', at: 5 };
  eq('conflict once', M.transitions({ accounts: [a()], conflicts: [] }, { accounts: [a()], conflicts: [c] }).map((e) => [e.type, e.name, e.path]), [['conflict', 'Home NAS', c.path]]);
  eq('conflict seen', M.transitions({ accounts: [a()], conflicts: [c] }, { accounts: [a()], conflicts: [c] }), []);
  eq('first state: quiet', M.transitions(null, { accounts: [a({ state: 'signin' })] }), []);
  eq('conflict name', [M.conflictName('Proposal v3.odt', 'this computer'), M.conflictName('Notes', 'este equipo')], ['Proposal v3 (this computer).odt', 'Notes (este equipo)']);
  eq('rate', M.rateOf([{ at: 0, done: 0 }, { at: 2000, done: 44e6 }], 66e6), { rate: 22e6, left: 1 });
  eq('rate needs movement', [M.rateOf([{ at: 0, done: 5 }], 10), M.rateOf([{ at: 0, done: 5 }, { at: 1000, done: 5 }], 10)], [null, null]);
}

// ---- glyph pixels ----
{
  for (const k of GLYPH_KINDS) {
    const g = glyphPixels(k, 18, '#0a84ff', { pct: 0.5, arrow: 'up' });
    let lit = 0;
    for (let i = 3; i < g.data.length; i += 4) if (g.data[i] > 0) lit++;
    eq(`${k}: 2× and drawn`, [g.width, g.height, g.data.length, lit > 20 && lit < g.width * g.height], [36, 36, 36 * 36 * 4, true]);
  }
  eq('unknown glyph', glyphPixels('nope', 18, '#fff'), null);
  eq('same drawing, same object (no re-upload)', glyphPixels('cloud', 18, '#a4a4ad') === glyphPixels('cloud', 18, '#a4a4ad'), true);
  eq('progress in steps', glyphPixels('down', 18, '#0a84ff', { pct: 0.501 }) === glyphPixels('down', 18, '#0a84ff', { pct: 0.5 }), true);
  const r = ringPixels(14, [{ frac: 0.25, color: '#ff0000' }]);
  const at = (x, y) => Array.from(r.data.slice((y * r.width + x) * 4, (y * r.width + x) * 4 + 4));
  // a filled pie: the top right quarter is the part's color, the bottom left the track's
  eq('ring part from 12 o\'clock clockwise', [at(20, 6).slice(0, 3), at(6, 20).slice(0, 3)], [[255, 0, 0], [0x48, 0x48, 0x4f]]);
  eq('ring corners clear', at(0, 0)[3], 0);
  eq('cache bounded', glyphCacheSize() <= 400, true);
}

// ---- the file dialog's Cloud places and crumbs ----
{
  const accs = [acc('1', 'Work'), acc('2', 'Locked', { state: 'locked' }), acc('3', 'Hidden'), { id: '4', name: 'Odd', mount: '/mnt/x', state: 'ok' }];
  eq('places', cloudPlaces(accs, H, ['3']).map((p) => p.name), ['Work']);
  eq('cloud root', [cloudRoot(`${H}/Cloud/Work/a/b`, H), cloudRoot(`${H}/Cloud`, H), cloudRoot(`${H}/Documents`, H)], [`${H}/Cloud/Work`, null, null]);
  eq('crumbs start at the account', pathCrumbs(`${H}/Cloud/Work/Projects`, H).map((c) => c.label), ['Work', 'Projects']);
  eq('home crumbs unchanged', pathCrumbs(`${H}/Documents/a`, H).map((c) => c.label), ['ana', 'Documents', 'a']);
}

// ---- the dev mock answers the shape of section 17 ----
{
  const files = new Map();
  const timers = [];
  globalThis.setInterval = (fn) => { timers.push(fn); return 0; };
  globalThis.setTimeout = (fn) => { timers.push(fn); return 0; };
  globalThis.lord = { fs: { home: () => H, mkdir: (p) => files.set(p, 'dir'), write: (p, d) => files.set(p, d), stat: (p) => { if (!files.has(p)) throw new Error('ENOENT'); return {}; } } };
  let pushed = null;
  globalThis.__lordCloud = (st) => { pushed = st; };
  installCloudMock(H);
  const c = globalThis.lord.cloud;
  const API = ['available', 'state', 'providers', 'beginSignIn', 'openSignInPage', 'cancelSignIn', 'add', 'remove', 'rename', 'refreshAbout', 'websiteUrl',
    'status', 'keep', 'unkeep', 'free', 'link', 'unlink', 'links', 'pause', 'resume', 'setOptions', 'restart', 'showLog'];
  eq('every call of section 17', API.filter((k) => typeof c[k] !== 'function'), []);
  const st = c.state();
  eq('state keys', Object.keys(st).filter((k) => k !== 'conflicts').sort(), ['accounts', 'options', 'recent', 'service', 'signin']);
  eq('service keys', ['running', 'version', 'uptimeS', 'rssKb', 'restarts'].every((k) => k in st.service), true);
  eq('options keys', Object.keys(st.options).sort(), ['autoFree', 'bwDown', 'bwUp', 'freeBelowGb', 'pauseOnBattery', 'pauseOnMetered']);
  const AK = ['id', 'name', 'kind', 'provider', 'mount', 'state', 'about', 'cacheBytes', 'keptBytes', 'up', 'down', 'kept', 'linksSupported'];
  eq('account keys', st.accounts.every((a) => AK.every((k) => k in a)), true);
  eq('account states are known', st.accounts.every((a) => ['ok', 'syncing', 'paused', 'signin', 'offline', 'error', 'locked'].includes(a.state)), true);
  eq('recent shape', st.recent.every((r) => r.path && r.account && ['up', 'down'].includes(r.dir) && r.at), true);
  const prov = c.providers();
  eq('providers shape', prov.every((p) => p.kind && p.label && ['oauth', 'server', 'object', 'crypt'].includes(p.group) && Array.isArray(p.fields)), true);
  eq('fields shape', prov.flatMap((p) => p.fields).every((f) => 'name' in f && 'label' in f && 'secret' in f && 'required' in f && 'default' in f && Array.isArray(f.examples)), true);
  const work = st.accounts.find((a) => a.name === 'Work');
  const s = c.status([`${work.mount}/Projects/Notes.md`, `${work.mount}/Projects/Kickoff recording.mp4`, `${H}/Documents/x`]);
  eq('status shape', [s[`${work.mount}/Projects/Notes.md`].st, s[`${work.mount}/Projects/Kickoff recording.mp4`].st, `${H}/Documents/x` in s], ['error', 'down', false]);
  c.keep([`${work.mount}/Projects/Interviews.zip`]);
  eq('keep → kept, pushed', [c.status([`${work.mount}/Projects/Interviews.zip`])[`${work.mount}/Projects/Interviews.zip`].st, !!pushed], ['kept', true]);
  eq('free skips kept', c.free([`${work.mount}/Projects/Interviews.zip`]).skipped.map((x) => x.reason), ['kept']);
  eq('link', [!!c.link(`${work.mount}/Projects/Floor plan.pdf`, '7d').url, c.link(`${H}/Cloud/Photos archive/2024`, '7d').error], [true, 'unsupported']);
  eq('links shape', c.links(work.id).every((l) => l.path && l.url && 'expires' in l && 'made' in l), true);
  eq('remove with pending', c.remove(work.id, { discardPending: false }).error, 'pending');
  const si = c.beginSignIn('dropbox', 'Mine', {});
  eq('sign-in shape', [!!si.id, !!si.url, si.expiresAt > 0, pushed.signin.phase], [true, true, true, 'waiting']);
  for (const f of timers.splice(0)) f();
  eq('sign-in finishes', [pushed.signin.phase, pushed.accounts.some((a) => a.name === 'Mine')], ['done', true]);
}

// ---- every text the model and the screens name is in each catalog ----
{
  const dynamic = [...M.STATUSES.map((s) => `cloud.status.${s}`), ...M.EXPIRY.map((x) => `cloud.expiry.${x}`), ...M.GROUPS.map((g) => `cloud.group.${g}`),
    ...['empty', 'long', 'chars', 'taken'].map((x) => `cloud.name.${x}`), ...['files', 'other', 'trash'].map((x) => `cloud.space.part.${x}`)];
  const src = fs.readFileSync(path.join(root, 'sdk/cloudmodel.js'), 'utf8');
  const named = [...src.matchAll(/'(cloud\.[a-z]+\.[A-Za-z]+)'/g)].map((m) => m[1]);
  for (const lang of ['en', 'es', 'fr', 'it', 'pt']) {
    const cat = JSON.parse(fs.readFileSync(path.join(root, `assets/locales/${lang}/cloud.json`), 'utf8'));
    const missing = [...new Set([...dynamic, ...named])].filter((k) => !(k.slice(6) in cat));
    eq(`${lang}: every key`, missing, []);
  }
}

// ---- right to left: the screens mirror (docs/DESIGN.md "Logical edges") ----
// No one-sided physical edge in Cloud's screens: text aligns to start/end, insets are insetInline*, and a
// physical left/top is only for a cover of the whole box, a place on the screen (the window's frame) or a ring
// drawn over a box of its own size. Chevrons and the back button turn with the direction.
{
  for (const f of ['apps/settings/Cloud.jsx', 'apps/files/cloud.jsx', 'sdk/cloudui.jsx', 'sdk/cloudstatus.jsx']) {
    const lines = fs.readFileSync(path.join(root, f), 'utf8').split('\n');
    const bad = lines.map((l, i) => [l, i + 1]).filter(([l]) => /textAlign: '(left|right)'|\b(margin|padding)(Left|Right)\b|\bborder(Left|Right)/.test(l)
      || (/[{,]\s*(left|right):/.test(l) && !(/\bleft:/.test(l) && /\bright:/.test(l)) && !/left: g\.x/.test(l) && !/left: 0, top: 0 \}/.test(l)));
    eq(`${f}: no one-sided physical edge`, bad.map(([, n]) => n), []);
  }
  const settings = fs.readFileSync(path.join(root, 'apps/settings/Cloud.jsx'), 'utf8');
  eq('chevron and back turn in RTL', [/direction\(\) === 'rtl' \? '‹' : '›'/.test(settings), /direction\(\) === 'rtl' \? '›' : '‹'/.test(settings)], [true, true]);
}

console.log(`cloud model: ${ok} checks ${process.exitCode ? 'with failures' : 'passed'}`);
