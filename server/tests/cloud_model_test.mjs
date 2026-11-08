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
import { runCheck, buildReport, issueUrl, checkFolder, randomBytes, hashBytes, aboutFields, checkWorks, CHECK_STEPS, CHECK_SIZE, ISSUE_URL_MAX } from '../../sdk/cloudcheck.js';

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
  eq('all accounts: used; no total while one has no quota (U4); its used still on the scale', [bar.used, bar.total, bar.scale], [612e9 + 540e9 + 161e9 + 7.4e9, null, 2e12 + 1e12 + 161e9 + 300e9]);
  eq('all accounts with a quota: a total', M.allAccountsBar([acc('1', 'A', { about: { total: 10, used: 4 } }), acc('2', 'B', { about: { total: 20, used: 1 } })]).total, 30);
  eq('one measured account: used, never "of" itself', M.allAccountsBar([acc('1', 'A', { about: { used: 5.7e6, measuredAt: 1.7e12 } })]).total, null);
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
  // #71: Pause All on an account in error: lord-server's pauseAll says it took, though the account still shows the error
  eq('all paused by the option', [M.allPaused({ accounts: [acc('1', 'A', { state: 'error' })], options: { pauseAll: true } }),
    M.allPaused({ accounts: [acc('1', 'A', { state: 'error', paused: true }), acc('2', 'B', { state: 'paused' })], options: { pauseAll: false } }),
    M.allPaused({ accounts: [acc('1', 'A', { state: 'error' })], options: { pauseAll: false } }), M.allPaused({ accounts: [], options: { pauseAll: true } })], [true, true, false, false]);
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
    'status', 'keep', 'unkeep', 'free', 'link', 'unlink', 'links', 'pause', 'resume', 'setOptions', 'restart', 'showLog', 'answer', 'emptyTrash', 'measure', 'delete'];
  eq('every call of section 17', API.filter((k) => typeof c[k] !== 'function'), []);
  const st = c.state();
  eq('state keys', Object.keys(st).filter((k) => k !== 'conflicts').sort(), ['accounts', 'options', 'recent', 'service', 'signin']);
  eq('service keys', ['running', 'version', 'uptimeS', 'rssKb', 'restarts'].every((k) => k in st.service), true);
  eq('options keys', Object.keys(st.options).sort(), ['autoFree', 'bwDown', 'bwUp', 'freeBelowGb', 'pauseOnBattery', 'pauseOnMetered']);
  const AK = ['id', 'name', 'kind', 'provider', 'mount', 'state', 'about', 'cacheBytes', 'keptBytes', 'up', 'down', 'kept', 'linksSupported'];
  eq('account keys', st.accounts.every((a) => AK.every((k) => k in a)), true);
  eq('account states are known', st.accounts.every((a) => ['ok', 'syncing', 'paused', 'signin', 'offline', 'error', 'locked'].includes(a.state)), true);
  eq('recent shape', st.recent.every((r) => r.path && r.account && ['up', 'down'].includes(r.dir) && r.at), true);
  const provP = c.providers();
  eq('providers: a Promise (17.2)', typeof provP.then, 'function');
  for (const f of timers.splice(0)) f();
  const prov = await provP;
  eq('providers: description and verified', prov.every((p) => typeof p.description === 'string' && typeof p.verified === 'boolean'), true);
  eq('providers shape', prov.every((p) => p.kind && p.label && ['oauth', 'server', 'object', 'crypt'].includes(p.group) && Array.isArray(p.fields)), true);
  eq('fields shape', prov.flatMap((p) => p.fields).every((f) => 'name' in f && 'label' in f && 'secret' in f && 'required' in f && 'default' in f && Array.isArray(f.examples)), true);
  const work = st.accounts.find((a) => a.name === 'Work');
  const s = c.status([`${work.mount}/Projects/Notes.md`, `${work.mount}/Projects/Kickoff recording.mp4`, `${H}/Documents/x`]);
  eq('status shape', [s[`${work.mount}/Projects/Notes.md`].st, s[`${work.mount}/Projects/Kickoff recording.mp4`].st, `${H}/Documents/x` in s], ['error', 'down', false]);
  c.keep([`${work.mount}/Projects/Interviews.zip`]);
  eq('keep → kept, pushed', [c.status([`${work.mount}/Projects/Interviews.zip`])[`${work.mount}/Projects/Interviews.zip`].st, !!pushed], ['kept', true]);
  eq('free skips kept', c.free([`${work.mount}/Projects/Interviews.zip`]).skipped.map((x) => x.reason), ['kept']);
  eq('link; WebDAV has none', [!!c.link(`${work.mount}/Projects/Floor plan.pdf`, '7d').url, c.link(`${H}/Cloud/Home NAS/Shared`, '7d').error], [true, 'unsupported']);
  eq('links shape', c.links(work.id).every((l) => l.path && l.url && 'expires' in l && 'made' in l), true);
  eq('remove with pending', c.remove(work.id, { discardPending: false }).error, 'pending');
  const si = c.beginSignIn('dropbox', 'Mine', {});
  eq('sign-in shape', [!!si.id, !!si.url, si.expiresAt > 0, pushed.signin.phase], [true, true, true, 'waiting']);
  for (const f of timers.splice(0)) f();
  eq('sign-in finishes', [pushed.signin.phase, pushed.accounts.some((a) => a.name === 'Mine')], ['done', true]);
  const q1 = c.add('iclouddrive', 'Phone', {});
  eq('a question from add (17.2)', [!!q1.question, q1.question.state], [true, 'mock-2fa']);
  const q2 = await c.answer('mock-2fa', '12');
  eq('a wrong answer asks again', !!q2.question, true);
  const doneP = c.answer('mock-2fa', '123456');
  for (const f of timers.splice(0)) f();
  const done = await doneP;
  eq('the right answer makes the account', [!!done.id, pushed.accounts.some((a) => a.name === 'Phone')], [true, true]);
  const ms = c.measure(done.id); for (const f of timers.splice(0)) f();
  eq('measure answers bytes and count, about.measuredAt set', [(await ms).count > 0, !!pushed.accounts.find((a) => a.id === done.id).about.measuredAt], [true, true]);
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


// ---- verified providers (docs/CLOUD.md 19) ----
{
  const data = JSON.parse(fs.readFileSync(path.join(root, 'assets/cloud/providers.json'), 'utf8'));
  const v = Object.entries(data.verified || {});
  eq('verified: only tested kinds, each with since, date and evidence', v.every(([k, e]) => /^[a-z0-9]+$/.test(k) && /^\d+\.\d+\.\d+(\.\d+)?$/.test(e.since) && /^\d{4}-\d{2}-\d{2}$/.test(e.date)
    && /^(maintainer test: .{20,}|issue #\d+)/.test(e.evidence)), true);
  eq('verified today', v.map(([k]) => k).sort(), ['s3', 'sftp', 'webdav']);
  const P = (kind, label, group) => ({ kind, label, group, hint: '', fields: [] });
  const list = [P('drive', 'Drive', 'oauth'), P('dropbox', 'Dropbox', 'oauth'), P('webdav', 'WebDAV', 'server'), P('sftp', 'SFTP', 'server'), P('smb', 'SMB', 'server'),
    P('s3', 'S3', 'object'), P('b2', 'Backblaze B2', 'object'), P('crypt', 'Encrypted folder', 'crypt')];
  const sp = M.splitProviders(list, data);
  eq('verified first, by kind', sp.verified.map((g) => [g.group, g.items.map((p) => p.kind)]), [['server', ['webdav', 'sftp']], ['object', ['s3']]]);
  eq('the rest not verified, all of them, closed', [sp.unverified.flatMap((g) => g.items.map((p) => p.kind)), sp.unverifiedCount, sp.open], [['drive', 'dropbox', 'smb', 'b2', 'crypt'], 5, false]);
  eq('search opens Not verified yet when it matches', [M.splitProviders(list, data, 'drop').open, M.splitProviders(list, data, 'dav').open, M.splitProviders(list, data, '', [], true).open], [true, false, true]);
  eq('search looks in both', M.splitProviders(list, data, 's').verified.length > 0 && M.splitProviders(list, data, 's').unverifiedCount > 0, true);
  eq('own app offered: OAuth not verified only', [M.ownAppOffered(list[0], data), M.ownAppOffered(list[2], data), M.ownAppOffered(list[0], { verified: { drive: {} } }), M.ownAppOffered(null, data)], [true, false, false, false]);
  eq('rclone pages', ['drive', 'gcs', 'nextcloud', 'b2', 'x y'].map(M.rcloneDocsUrl), ['https://rclone.org/drive/', 'https://rclone.org/googlecloudstorage/', 'https://rclone.org/webdav/', 'https://rclone.org/b2/', 'https://rclone.org/overview/']);
  eq('redirect', M.OAUTH_REDIRECT, 'http://127.0.0.1:53682/');
}

// ---- Check This Account: the report and GitHub ----
{
  const steps = CHECK_STEPS.map((id, i) => ({ id, result: i === 7 ? 'skipped' : 'pass', ms: 10 * i + 0.4, note: i === 7 ? 'noLinks' : '' }));
  const personal = { name: 'Ana Secret Work', mount: '/home/ana/Cloud/Ana Secret Work', email: 'ana@example.test', message: 'token abc123 for ana@example.test',
    url: 'https://share.example.test/s/XZk4p9', path: '/home/ana/Cloud/Ana Secret Work/taxes.pdf' };
  const rep = buildReport({ ...personal, version: '0.8.8.5', arch: 'x86_64', rclone: 'v1.75.2', kind: 'pcloud', label: 'pCloud', verified: false, ownApp: true,
    aboutFields: aboutFields({ total: 1, used: 2, email: 'ana@example.test' }), linksSupported: true,
    steps: [...steps, { id: 'secret', result: 'pass', ms: 1, note: 'x' }, { id: 'free', result: 'fail', note: '/home/ana/Cloud/taxes.pdf' }] });
  eq('report: nothing personal', ['Ana', 'Secret', 'ana@', 'example.test', 'abc123', 'XZk4', 'taxes', '/home'].filter((w) => rep.includes(w)), []);
  eq('report: what it says', ['LordOS: 0.8.8.5 (x86_64)', 'rclone: v1.75.2', 'Provider: pCloud (pcloud)', 'Verified on LordOS: no', 'Own app: yes', 'Space info: total, used', 'Links supported: yes',
    '  link      skipped  70 ms      noLinks', 'Result: 9 passed, 1 failed, 1 skipped'].filter((l) => !rep.includes(l)), []);
  eq('report: only the check\'s steps', rep.includes('secret'), false);
  eq('report: an odd version or label stays plain', buildReport({ version: 'x\nEvil: 1', label: 'A\nB', steps: [] }).split('\n').length, 10);
  eq('works', [checkWorks(steps), checkWorks([{ result: 'fail' }]), checkWorks([{ result: 'fail' }, { result: 'fail' }, { result: 'fail' }])], ['yes', 'partly', 'no']);
  const u = issueUrl({ label: 'pCloud', report: rep, version: '0.8.8.5 · Beta' });
  eq('issue url', [u.startsWith('https://github.com/lordbasex/LordOS/issues/new?template=cloud-provider.yml&title=Cloud%20provider%3A%20pCloud&provider=pCloud&version=0.8.8.5%20%C2%B7%20Beta&report='),
    decodeURIComponent(u.split('&report=')[1]) === rep], [true, true]);
  eq('issue url without a report (Add Account\'s line)', issueUrl({ label: 'Box' }), 'https://github.com/lordbasex/LordOS/issues/new?template=cloud-provider.yml&title=Cloud%20provider%3A%20Box&provider=Box');
  eq('special characters encoded', issueUrl({ label: 'A&B=C #1' }).includes('provider=A%26B%3DC%20%231'), true);
  const long = Array.from({ length: 900 }, (_, i) => `  line ${i} ä€`).join('\n');
  const lu = issueUrl({ label: 'X', report: long });
  eq('long report: under the limit, truncated at a line, said so', [lu.length <= ISSUE_URL_MAX, decodeURIComponent(lu.split('&report=')[1]).endsWith('\n[truncated]'), /line \d+ ä€\n\[truncated\]$/.test(decodeURIComponent(lu.split('&report=')[1]))], [true, true, true]);
  const one = issueUrl({ label: 'X', report: 'y'.repeat(20000) });
  eq('one huge line: under the limit', [one.length <= ISSUE_URL_MAX, one.endsWith(encodeURIComponent('\n[truncated]'))], [true, true]);
  eq('the template exists with those field ids', ['id: provider', 'id: works', 'id: report', 'id: version', 'cloud-provider'].every((x) => fs.readFileSync(path.join(root, '.github/ISSUE_TEMPLATE/cloud-provider.yml'), 'utf8').includes(x)), true);
}

// ---- Check This Account: the run, against stand-ins ----
{
  eq('random bytes: repeatable, not flat', [hashBytes(randomBytes(64, 7)) === hashBytes(randomBytes(64, 7)), hashBytes(randomBytes(64, 7)) === hashBytes(randomBytes(64, 8)), new Set(randomBytes(256, 3)).size > 100], [true, false, true]);
  eq('hash sees one byte', hashBytes(new Uint8Array([1, 2, 3])) === hashBytes(new Uint8Array([1, 2, 4])), false);
  eq('check folder', [checkFolder({ group: 'server', mount: '/m' }, [], 't').path, checkFolder({ group: 'object', mount: '/m' }, [{ name: 'zeta', dir: true }, { name: 'alpha', dir: true }, { name: 'f', dir: false }], 't').path,
    checkFolder({ group: 'object', mount: '/m' }, [], 't').path], ['/m/.lordos-check-t', '/m/alpha/.lordos-check-t', '/m/lordos-check-t']);
  // a world: files in a map, a cloud that sends what is written up after two looks, a clock that sleeps instantly
  const world = (opts = {}) => {
    let clock = 1.7e12;
    const files = new Map([['/h/Cloud/W', 'dir'], ['/h/Cloud/W/docs', 'dir']]);
    const st = new Map();
    const looks = new Map();
    const trash = [];
    let jobs = [];
    const acc = { id: 'a1', name: 'W', mount: '/h/Cloud/W', group: opts.group || 'server', linksSupported: !!opts.links, about: null, up: { files: 0 }, kind: 'webdav' };
    const fsx = {
      home: () => '/h', list: (p) => [...files.keys()].filter((k) => k.startsWith(`${p}/`) && !k.slice(p.length + 1).includes('/')).map((k) => ({ name: k.split('/').pop(), dir: files.get(k) === 'dir' })),
      mkdir: (p) => files.set(p, 'dir'), write: (p, d) => { files.set(p, new Uint8Array(d)); st.set(p, 'up'); acc.up = { files: 1 }; },
      read: (p) => { const d = files.get(p); return (opts.corrupt ? d.map((x, i) => (i ? x : x ^ 1)) : d).buffer; },
      stat: (p) => { const d = files.get(p); if (!d) throw Object.assign(new Error(`ENOENT ${p}`), { code: 'fs.notFound' }); return d === 'dir' ? { dir: true, size: 0 } : { dir: false, size: d.length }; },
      move: (a, b) => { for (const k of [...files.keys()]) if (k === a || k.startsWith(`${a}/`)) { files.set(b + k.slice(a.length), files.get(k)); files.delete(k); } acc.up = { files: 0 }; jobs.push({ id: 9, done: true }); return 9; },
      jobs: () => jobs, forgetJob: () => { jobs = []; }, trash: (p) => { trash.push({ id: 't1', original: p }); for (const k of [...files.keys()]) if (k.startsWith(p)) files.delete(k); }, trashList: () => trash,
      removeFromTrash: (id) => { trash.splice(trash.findIndex((x) => x.id === id), 1); },
    };
    const cl = {
      state: () => ({ accounts: [acc] }), refreshAbout: () => { if (!opts.noAbout) acc.about = { used: 5, total: 10 }; },
      status: (ps) => Object.fromEntries(ps.map((p) => {
        const n = (looks.get(p) || 0) + 1; looks.set(p, n);
        if (opts.fail && st.get(p) === 'up') return [p, { st: 'error', error: 'cloud.transferFailed' }];
        if (st.get(p) === 'up' && n > 2) { st.set(p, 'here'); acc.up = { files: 0 }; }
        if (!st.get(p) && acc.up.files) acc.up = { files: 0 };
        return [p, { st: st.get(p) || 'cloud' }];
      })),
      free: (ps) => { st.set(ps[0], 'cloud'); return { freed: CHECK_SIZE, skipped: [] }; },
      link: async () => ({ url: 'https://x.test/s/1' }), unlink: async () => ({ ok: true }),
    };
    return { acc, files, trash, env: { fs: fsx, cloud: cl, sleep: async (ms) => { clock += ms; }, now: () => clock } };
  };
  const res = (s) => s.map((x) => `${x.id}:${x.result}${x.note ? `(${x.note})` : ''}`).join(' ');
  let w = world({ links: true });
  let seen = 0;
  let out = await runCheck(w.acc, w.env, () => { seen++; });
  eq('a good account: every step passes', res(out), CHECK_STEPS.map((id) => `${id}:pass`).join(' '));
  eq('steps reported as they go', seen >= CHECK_STEPS.length, true);
  eq('nothing left: not in the cloud, not in the home, not in the Trash', [[...w.files.keys()].filter((k) => k.includes('lordos-check')), w.trash.length], [[], 0]);
  w = world({ noAbout: true });
  out = await runCheck(w.acc, w.env);
  eq('no space info, no links: skipped, not failed', [out[0].result, out[0].note, out.find((x) => x.id === 'link').note], ['skipped', 'noSpaceInfo', 'noLinks']);
  w = world({ fail: true });
  out = await runCheck(w.acc, w.env);
  eq('an upload that fails: that step fails, the rest is skipped, the folder still goes', res(out).replace(/about:\w+ /, ''),
    'list:pass folder:pass write:pass up:fail(cloud.transferFailed) free:skipped(notReached) read:skipped(notReached) link:skipped(notReached) delete:pass deleteUp:pass');
  eq('and nothing left', [...w.files.keys()].filter((k) => k.includes('lordos-check')), []);
  w = world({ corrupt: true });
  out = await runCheck(w.acc, w.env);
  eq('different bytes back: read fails', out.find((x) => x.id === 'read').note, 'cloud.checkMismatch');
  w = world({ group: 'object' });
  out = await runCheck(w.acc, w.env);
  eq('object storage: inside the first bucket', [out.every((x) => x.result !== 'fail'), [...w.files.keys()].some((k) => k.startsWith('/h/.cache/'))], [true, false]);
  w = world();
  w.env.fs.mkdir = () => { throw Object.assign(new Error('EACCES /home/ana/Cloud/Ana Secret'), { code: 'sys.errno' }); };
  out = await runCheck(w.acc, w.env);
  eq('an error\'s text never becomes a note (it can hold a path)', [out.find((x) => x.id === 'folder').note, out.some((x) => /Ana|home/.test(x.note))], ['sys.errno', false]);
}


// ---- 17.2: providers from rclone, questions, notes, Measure, delete in the cloud ----
{
  const P = (kind, label, group, description, verified) => ({ kind, label, group, description, verified, fields: [] });
  const list = [P('drive', 'Drive Thing', 'oauth', 'Files of a large drive', false), P('webdav', 'WebDAV', 'server', 'Any WebDAV server', true),
    P('koofr', 'Koofr', 'server', 'European storage', false), P('s3', 'S3', 'object', 'Amazon S3 compliant', true)];
  const sp = M.splitProviders(list, { verified: {} });
  eq('the item\'s own verified wins over the file', [sp.verified.flatMap((g) => g.items.map((p) => p.kind)), sp.unverified.flatMap((g) => g.items.map((p) => p.kind))], [['webdav', 's3'], ['drive', 'koofr']]);
  eq('search by description', M.splitProviders(list, {}, 'european').unverified.flatMap((g) => g.items.map((p) => p.kind)), ['koofr']);
  eq('labels untouched (rclone\'s names)', M.groupProviders(list).flatMap((g) => g.items.map((p) => p.label)), ['Drive Thing', 'WebDAV', 'Koofr', 'S3']);
  eq('own app from the item', [M.ownAppOffered(list[0], {}), M.ownAppOffered({ ...list[0], verified: true }, {})], [true, false]);
  const q = (o) => ({ state: 's', name: 'n', help: 'h', type: 'string', examples: [], isPassword: false, required: true, default: '', ...o });
  eq('question inputs', [M.questionInput(q({})).input, M.questionInput(q({ isPassword: true })).input, M.questionInput(q({ type: 'bool', default: true })), M.questionInput(q({ type: 'int' })).input,
    M.questionInput(q({ examples: [{ value: 'a', help: 'Choice A' }, { value: 'b', help: 'Choice B' }], default: 'b' }))],
    ['text', 'password', { input: 'bool', options: [], value: 'true' }, 'number', { input: 'choice', options: [['a', 'Choice A'], ['b', 'Choice B']], value: 'b' }]);
  eq('answers', [M.questionAnswer(q({}), ' 123456 '), M.questionAnswer(q({}), ''), M.questionAnswer(q({ required: false }), ''), M.questionAnswer(q({ type: 'bool' }), 'true'),
    M.questionAnswer(q({ type: 'int' }), 'x'), M.questionAnswer(q({ type: 'int' }), '42'), M.questionAnswer(q({ isPassword: true }), ' p w ')], ['123456', null, '', 'true', null, '42', ' p w ']);
  eq('notes before a provider', [M.notesFor('iclouddrive').length, M.notesFor('webdav')], [4, []]);
  eq('#65: a refused password says which one', [M.errorHint('iclouddrive', { error: 'cloud.authFailed' }), M.errorHint('iclouddrive', { error: { code: 'cloud.badPassword' } }),
    M.errorHint('iclouddrive', { error: 'cloud.unreachable' }), M.errorHint('webdav', { error: 'cloud.authFailed' }), M.errorHint('iclouddrive', null)],
  ['cloud.notes.iclouddrive.password', 'cloud.notes.iclouddrive.password', null, null, null]);
  const cat = JSON.parse(fs.readFileSync(path.join(root, 'assets/locales/en/cloud.json'), 'utf8'));
  eq('the notes are in the catalog', M.notesFor('iclouddrive').every((k) => k.slice(6) in cat), true);
  eq('measure: only an answer without a total', [M.canMeasure({ used: 5 }), M.canMeasure({ used: 5, total: 9 }), M.canMeasure(null)], [true, false, false]);
  eq('measured line', [M.measuredLine({ used: 7 }, String, () => 'x'), M.measuredLine({ used: 7e9, measuredAt: 1.7e12 }, (n) => `${n / 1e9} GB`, () => 'today')],
    [null, { key: 'cloud.measure.done', vars: { used: '7 GB', when: 'today' } }]);
  const accs = [acc('1', 'Work', { provider: 'pCloud', trashSupported: true }), acc('2', 'NAS', { provider: 'WebDAV' })];
  const t = M.splitTrash([`${H}/Documents/a`, `${H}/Cloud/Work/x`, `${H}/Cloud/Work/y`, `${H}/Cloud/NAS/z`, `${H}/Cloud/Work`], accs);
  eq('Move to Trash: local, then each account; an account\'s own folder never', [t.local, t.cloud.map((g) => [g.acc.id, g.paths.length])], [[`${H}/Documents/a`], [['1', 2], ['2', 1]]]);
  eq('the question: its trash, or no undo', [M.deletePrompt(accs[0], 2), M.deletePrompt(accs[1], 1).detail.key],
    [{ title: { key: 'cloud.delete.title', vars: { provider: 'pCloud', count: 2 } }, detail: { key: 'cloud.delete.toTrash', vars: { provider: 'pCloud' } } }, 'cloud.delete.final']);
  const src = fs.readFileSync(path.join(root, 'apps/files/index.jsx'), 'utf8');
  eq('Files: Move to Trash and dragging to the Trash go through the cloud split', [/splitTrash\(paths, cf\.accounts\)/.test(src), /target === 'trash:'\) \{ trashRef\.current/.test(src)], [true, true]);
}

// ---- Cloud's sheets stay in the Settings window; a sign-in waits inline and opens the page once ----
{
  const src = fs.readFileSync(path.join(root, 'apps/settings/Cloud.jsx'), 'utf8');
  const idx = fs.readFileSync(path.join(root, 'apps/settings/index.jsx'), 'utf8');
  const files = fs.readFileSync(path.join(root, 'apps/files/cloud.jsx'), 'utf8');
  eq('no Cloud sheet in the shell\'s modal layer (Settings, Files)', [/<Layer\b/.test(src), /<Layer\b/.test(files)], [false, false]);
  eq('the sheets are drawn at the Settings window\'s root', /section === 'cloud' \? <CloudSheets \/> : null/.test(idx), true);
  eq('the sign-in waits inline in the pane, not in a sheet', [/v\.signin \? <SignInCard/.test(src), /kind: 'signin'/.test(src)], [true, false]);
  eq('every sign-in goes through one guarded start', [(src.match(/cloud\.beginSignIn\(/g) || []).length, /if \(signinStarting\) return;/.test(src)], [1, true]);
  eq('the page isn\'t opened again on start (only by its button)', (src.match(/cloud\.openSignInPage\(/g) || []).length, 1);
}


// ---- the credentials form, an account that isn't connected (the Pi test) ----
{
  const F = (name, o = {}) => ({ name, label: '', help: '', secret: false, required: false, default: '', examples: [], ...o });
  eq('short labels from rclone\'s help', [M.shortLabel('Raw PEM-encoded private key.\n\nIf specified, will override key_file.', 'key_pem'),
    M.shortLabel('When set forces the usage of the ssh-agent.', 'key_use_agent'), M.shortLabel('SSH host to connect to.\n\nE.g. "example.com".', 'host'),
    M.shortLabel('', 'odd'), M.shortLabel('Path to PEM-encoded private key file (leave blank or set key-use-agent to use ssh-agent).', 'key_file')],
    ['Raw PEM-encoded private key', 'When set forces the usage of the ssh-agent', 'SSH host to connect to', 'odd', 'Path to PEM-encoded private key file']);
  eq('only a password is masked (Sensitive host and user stay plain)', [M.fieldInput(F('host', { secret: true, isPassword: false })).input, M.fieldInput(F('user', { secret: true, isPassword: false })).input,
    M.fieldInput(F('pass', { secret: true, isPassword: true })).input, M.fieldInput(F('pass', { secret: true })).input, M.fieldInput(F('host', { secret: true })).input], ['text', 'text', 'password', 'password', 'text']);
  eq('switches, menus, numbers', [M.fieldInput(F('a', { type: 'bool' })).input, M.fieldInput(F('a', { default: 'false' })).input, M.fieldInput(F('a', { examples: ['x', 'y', 'z'] })),
    M.fieldInput(F('a', { type: 'int' })).input], ['bool', 'bool', { input: 'choice', options: ['x', 'y', 'z'] }, 'number']);
  const sftp = { kind: 'sftp', fields: ['host', 'user', 'port', 'pass', 'key_pem', 'key_file', 'key_file_pass', 'pubkey', 'key_use_agent', 'use_insecure_cipher', 'disable_hashcheck', 'ssh'].map((n) => F(n, { required: n === 'host' })) };
  const L = M.formLayout(sftp);
  eq('SFTP: Server, Port, User, then the sign-in choice; the rest is advanced', [L.main.map((f) => f.name), L.auth, L.advanced.map((f) => f.name)],
    [['host', 'port', 'user'], { password: 'pass', key: 'key_pem', passphrase: 'key_file_pass' }, ['key_file', 'pubkey', 'key_use_agent', 'use_insecure_cipher', 'disable_hashcheck', 'ssh']]);
  const s3 = { kind: 's3', fields: ['provider', 'env_auth', 'access_key_id', 'secret_access_key', 'region', 'endpoint', 'acl'].map((n) => F(n)) };
  eq('S3: provider, endpoint, region, the keys', M.formLayout(s3).main.map((f) => f.name), ['provider', 'endpoint', 'region', 'access_key_id', 'secret_access_key']);
  eq('WebDAV', M.formLayout({ kind: 'webdav', fields: ['url', 'vendor', 'user', 'pass', 'bearer_token'].map((n) => F(n)) }).main.map((f) => f.name), ['url', 'vendor', 'user', 'pass']);
  const other = { kind: 'koofr', fields: [F('mountid', { default: 'x' }), F('user', { required: true }), F('password', { required: true, secret: true }), F('setmtime', { default: 'true' }), F('endpoint')] };
  eq('another kind: required ones and those without a default (no switches)', [M.formLayout(other).main.map((f) => f.name), M.formLayout(other).advanced.map((f) => f.name)], [['user', 'password', 'endpoint'], ['mountid', 'setmtime']]);
  eq('a key for rclone: one line', M.pemForRclone('-----BEGIN OPENSSH PRIVATE KEY-----\r\nabc\ndef\n-----END OPENSSH PRIVATE KEY-----\n'), '-----BEGIN OPENSSH PRIVATE KEY-----\\nabc\\ndef\\n-----END OPENSSH PRIVATE KEY-----');
  const running = { service: { running: true } }, stopped = { service: { running: false } };
  const locked = { error: 'cloud.keyringLocked', state: 'locked', about: null };
  eq('"the service isn\'t running" only when it isn\'t', [M.explainError('cloud.notRunning', running, locked), M.explainError('cloud.notRunning', running, {}), M.explainError('cloud.notRunning', stopped, locked),
    M.explainError('cloud.measureFailed', running, locked), M.explainError(null, running, locked)], ['cloud.keyringLocked', 'cloud.notConnected', 'cloud.notRunning', 'cloud.measureFailed', null]);
  eq('space: a quota, no quota (Measure), not connected (its reason), not known yet', [M.spaceState({ about: { total: 9, used: 1 } }), M.spaceState({ about: { used: 1 } }), M.spaceState(locked), M.spaceState({ about: null })],
    ['quota', 'noQuota', 'reason', 'unknown']);
  eq('no Measure without an answer', [M.canMeasure(locked.about), M.canMeasure({ used: 1 })], [false, true]);
  eq('unlock', [M.needsUnlock(locked), M.needsUnlock({ state: 'ok' }), M.needsUnlock({ state: 'locked' })], [true, false, true]);
  const src = fs.readFileSync(path.join(root, 'apps/settings/Cloud.jsx'), 'utf8');
  eq('Unlock Keyring… uses the system prompt; the menu bar panel too', [/askKeyringUnlock\(\)/.test(src), /askKeyringUnlock\(\)/.test(fs.readFileSync(path.join(root, 'sdk/cloudstatus.jsx'), 'utf8'))], [true, true]);
  eq('after unlocking: cloud.retry, no restart', [/askKeyringUnlock\(\)\.then\(\(ok\) => \{ if \(ok\) cloud\.retry\(null\)/.test(src), /cloud\.restart\(\);\n/.test(src.split('function unlockKeyring')[1].split('}')[0])], [true, false]);
  eq('Sign In Again names the account by its id (17.2 item 10)', [/accountId: a\.id \}, p \|\|/.test(src), /again: true, accountId: a\.id/.test(src), /sheet\.accountId \? \{ accountId: sheet\.accountId \}/.test(src)], [true, true, true]);
  eq('the form\'s body scrolls between its fixed title and buttons', /<ScrollView style=\{\{ maxHeight: Math\.max\(180, frameH - 220\) \}\}>/.test(src), true);
}

// ---- e2e findings: short provider names, trust questions, errors with details and their field, Measure for S3 ----
{
  eq('provider names: the list\'s label, else the description cut short', [M.providerName({ kind: 's3', provider: 'Amazon S3 Compliant Storage Providers including AWS, Alibaba' }, [{ kind: 's3', label: 'S3' }]),
    M.providerName({ kind: 's3', provider: 'Amazon S3 Compliant Storage Providers including AWS, Alibaba, ArvanCloud' }, []), M.providerName({ kind: 'webdav', provider: 'WebDAV' }), M.providerName({ kind: 'x' })],
    ['S3', 'Amazon S3 Compliant Storage Providers', 'WebDAV', 'x']);
  eq('the delete question uses the short name', M.deletePrompt({ provider: 'Amazon S3 Compliant Storage Providers including AWS' }, 1).title.vars.provider, 'Amazon S3 Compliant Storage Providers');
  eq('trust questions', [M.trustKind({ kind: 'hostKey' }), M.trustKind({ kind: 'certificate' }), M.trustKind({ name: 'config_2fa' }), M.trustKind(null)], ['hostKey', 'certificate', null, null]);
  const d = (x) => x.toISOString().slice(0, 10);
  eq('error details', [M.errorVars({ error: 'cloud.hostKeyChanged', old: 'SHA256:a', new: 'SHA256:b' }, d), M.errorVars({ error: 'cloud.cantRevoke', expires: 1794614400000 }, d).vars.date, M.errorVars({ error: { code: 'cloud.unreachable', host: 'h' } }, d)],
    [{ code: 'cloud.hostKeyChanged', vars: { old: 'SHA256:a', new: 'SHA256:b' } }, d(new Date(1794614400000)), { code: 'cloud.unreachable', vars: { host: 'h' } }]);
  const sftp = { fields: [{ name: 'host' }, { name: 'user' }, { name: 'pass' }] };
  eq('the field an error belongs to', [M.errorField({ error: 'cloud.unreachable' }, sftp), M.errorField({ error: 'cloud.authFailed' }, sftp), M.errorField({ error: 'x', field: 'user' }, sftp), M.errorField({ error: 'cloud.badAddress' }, { fields: [{ name: 'endpoint' }] })],
    ['host', 'pass', 'user', 'endpoint']);
  eq('S3 connected without an answer: Measure', [M.spaceState({ about: null, mounted: true }), M.measurable({ about: null, mounted: true }), M.measurable({ about: { total: 5 }, mounted: true }), M.measurable({ about: null, error: 'cloud.keyringLocked' })],
    ['noQuota', true, false, false]);
  const cat = JSON.parse(fs.readFileSync(path.join(root, 'assets/locales/en/errors.json'), 'utf8'));
  eq('each new error has its own text', ['cloud.hostKeyChanged', 'cloud.certUntrusted', 'cloud.unreachable', 'cloud.badAddress', 'cloud.cantRevoke', 'cloud.notConnected', 'cloud.keyringLocked', 'cloud.noKeyring', 'cloud.notRunning', 'cloud.notFound', 'cloud.badKind'].filter((k) => !cat[k]), []);
  eq('fingerprints shown apart, never as {old}/{new} in the text (U10); the date in its text', [/\{(old|new)\}/.test(cat['cloud.hostKeyChanged'] + cat['cloud.certChanged']), /\{date\}/.test(cat['cloud.cantRevoke'])], [false, true]);
  eq('the fingerprints of a change: the answer\'s, else the account\'s message', [M.changedPrints({ error: 'cloud.hostKeyChanged', old: 'SHA256:a', new: 'SHA256:b' }),
    M.changedPrints(null, { error: 'cloud.hostKeyChanged', message: 'host key is now SHA256:Xy9+/z=' }), M.changedPrints({ error: 'cloud.unreachable' }), M.changedPrints(null, { error: 'cloud.keyringLocked' })],
    [{ old: 'SHA256:a', new: 'SHA256:b' }, { old: null, new: 'SHA256:Xy9+/z=' }, null, null]);
  eq('the owner trusts a changed key (question kind)', M.trustKind({ kind: 'hostKeyChanged', old: 'a', new: 'b' }), 'hostKeyChanged');
  eq('too many attempts has its own text', !!cat['cloud.tooManyAttempts'], true);
  const cl = JSON.parse(fs.readFileSync(path.join(root, 'assets/locales/en/cloud.json'), 'utf8'));
  eq('SFTP\'s note is about the server\'s key (U9)', [/key/i.test(cl['cred.noteSftp']), /certificate/i.test(cl['cred.noteSftp'])], [true, false]);
}

// ---- the e2e first pass: U3, U4, U6, U7 ----
{
  const P = '/c/a.mp4';
  let st = M.mergeStatusHeld({}, { [P]: { st: 'down', pct: 0.9 } }, [P], 1000);
  eq('U3: coming down', [st[P].st, st[P].downAt], ['down', 1000]);
  st = M.mergeStatusHeld(st, { [P]: { st: 'cloud' } }, [P], 3000);
  eq('U3: a stale "in the cloud" right after keeps "coming down"', [st[P].st, st[P].pct], ['down', 1]);
  const here = M.mergeStatusHeld(st, { [P]: { st: 'here' } }, [P], 4000);
  eq('U3: then on this computer', here[P].st, 'here');
  eq('U3: still "in the cloud" long after: it is', M.mergeStatusHeld(st, { [P]: { st: 'cloud' } }, [P], 20000)[P].st, 'cloud');
  eq('U3: Free Up Space goes to the cloud at once', M.mergeStatusHeld({ [P]: { st: 'here' } }, { [P]: { st: 'cloud' } }, [P], 5000)[P].st, 'cloud');
  eq('U4: unmeasured', [M.notMeasured({ used: 0 }), M.notMeasured({ used: 0, measuredAt: 1 }), M.notMeasured({ used: 3 }), M.notMeasured({ total: 5, used: 0 }), M.spaceLine({ used: 0 }, String).key],
    [true, false, false, false, 'cloud.space.notMeasured']);
  const vendor = { name: 'vendor', default: 'fastmail', examples: ['fastmail', 'nextcloud', 'owncloud', 'sharepoint', 'other'] };
  eq('U6: plain WebDAV first and by default', [M.presetField(vendor).examples[0], M.formValues({ fields: [vendor] }).vendor, M.formLayout({ kind: 'webdav', fields: [{ name: 'url' }, vendor] }).main[1].default],
    ['other', 'other', 'other']);
  eq('U6: other fields untouched', M.presetField({ name: 'provider', default: 'AWS', examples: ['AWS', 'Other'] }).default, 'AWS');
  const a = acc('1', 'A', { state: 'syncing', up: { files: 1, name: 'x', bytes: 10, done: 5 } });
  eq('U7: the service stopped: not connected, whatever the account said', [M.stateLine(a, { running: false }).key, M.stateLine(a, { running: true }).key], ['cloud.state.serviceDown', 'cloud.state.uploading']);
  eq('U7: the sidebar dims them with "!"', M.cloudSideItems([a], [], { running: false }).map((x) => [x.dim, x.alert]), [[true, true]]);
  const en = JSON.parse(fs.readFileSync(path.join(root, 'assets/locales/en/cloud.json'), 'utf8'));
  eq('the form promises no fingerprint prompt', /fingerprint/i.test(en['cred.noteServer']), false);
}

// ---- 17.2 items 19–31 in the interface ----
{
  eq('search hits as entries', M.searchEntries([{ path: '/h/Cloud/W/a/Report.pdf', dir: false, size: 5, mtime: 9 }, { path: '/h/Cloud/W/b', dir: true }]),
    [{ name: 'Report.pdf', path: '/h/Cloud/W/a/Report.pdf', dir: false, size: 5, mtime: 9 }, { name: 'b', path: '/h/Cloud/W/b', dir: true, size: 0, mtime: 0 }]);
  eq('S3 links: 1 or 7 days only', [M.expiryChoices({ kind: 's3' }), M.expiryChoices({ kind: 'dropbox' })], [['1d', '7d'], M.EXPIRY]);
  eq('a file only in the cloud can\'t open when the account is offline, locked or not mounted', [M.cantOpen({ state: 'offline' }, { st: 'cloud' }), M.cantOpen({ state: 'locked' }, { st: 'cloud' }),
    M.cantOpen({ state: 'ok', mounted: false }, { st: 'cloud' }), M.cantOpen({ state: 'ok', mounted: true }, { st: 'cloud' }), M.cantOpen({ state: 'offline' }, { st: 'here' })], [true, true, true, false, false]);
  const en = JSON.parse(fs.readFileSync(path.join(root, 'assets/locales/en/cloud.json'), 'utf8'));
  eq('conflicts: ours keeps the name, the cloud copy next to it', [/Your version kept its name/.test(en['notify.conflictBody']), en.cloudCopy, 'thisComputer' in en], [true, 'cloud copy', false]);
  const files = fs.readFileSync(path.join(root, 'apps/files/index.jsx'), 'utf8'), locs = fs.readFileSync(path.join(root, 'apps/files/locations.js'), 'utf8');
  eq('Files: cloud search through cloud.search, the 5000 cap told', [/cloud\.search\(loc\.root, loc\.query\)/.test(files), /out\.capped = raw\.length === LIST_CAP/.test(locs), /cloud\.listCapped/.test(files)], [true, true, true]);
  const panels = fs.readFileSync(path.join(root, 'apps/files/panels.jsx'), 'utf8');
  eq('Get Info never spins on a mount (du answers { remote: true })', /du\.remote \|\| du\.failed/.test(panels), true);
}

// ---- N4: the other accounts that kept the old key ----
{
  const accs = [{ id: '1', name: 'Work', old: 'SHA256:a', new: 'SHA256:b', error: 'cloud.hostKeyChanged' }, { id: '2', name: 'Backup', old: 'SHA256:a', error: 'cloud.hostKeyChanged' },
    { id: '3', name: 'Other', old: 'SHA256:z' }, { id: '4', name: 'Plain' }];
  eq('others with the same old key, not the one asking', [M.othersWithKey(accs, { old: 'SHA256:a' }, { id: '1' }), M.othersWithKey(accs, { old: 'SHA256:a' }, { name: 'work' }), M.othersWithKey(accs, { old: 'SHA256:q' }, { id: '1' }), M.othersWithKey(accs, {}, {})], [1, 1, 0, 0]);
  const hp = [{ id: '1', name: 'Work', host: 'nas.test', port: 22 }, { id: '2', name: 'Backup', host: 'NAS.test', port: null }, { id: '3', name: 'Web', host: 'nas.test', port: 2222 },
    { id: '4', name: 'Locked', host: null, old: 'SHA256:a' }, { id: '5', name: 'Drive', host: null }];
  eq('by host:port when known (22 when no port), else by the old key', [M.othersWithKey(hp, { host: 'nas.test', port: 22, old: 'SHA256:a' }, { id: '1' }),
    M.othersWithKey(hp, { host: 'nas.test', port: 2222 }, { name: 'New' }), M.othersWithKey(hp, { host: 'other.test', port: 22, old: 'SHA256:q' }, {})], [2, 1, 0]);
  eq('the account\'s own old and new in its status box', M.changedPrints(null, accs[0]), { old: 'SHA256:a', new: 'SHA256:b' });
  const src = fs.readFileSync(path.join(root, 'apps/settings/Cloud.jsx'), 'utf8');
  eq('the answer passes applyToOthers', /cloud\.answer\(q\.state, 'trust', trust === 'hostKeyChanged' \? \{ applyToOthers: others > 0 && alsoOthers \}/.test(src), true);
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
