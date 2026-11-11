// webapps_model_test.mjs — web apps as plain data (sdk/webappsmodel.js, docs/WEBAPPS.md): the window's app id and
// whose process it is, what the Dock, the launcher and the switcher show (name, profile, the "New window: <profile>"
// menu), the Store's "Add from an address" sheet (the address rules and its steps), the catalog
// (assets/webapps/catalog.json: every entry valid, its icon a real PNG, its description and category translated in
// every language) and the Settings page's rows.
//
//   node tests/webapps_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.join(here, '..', '..');
const M = await import(path.join(root, 'sdk/webappsmodel.js'));

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

const chat = {
  id: 'chat', name: 'Chat', origin: 'https://web.chat.example', startUrl: 'https://web.chat.example/', scope: 'https://web.chat.example/',
  profiles: [{ id: 'personal', name: 'Personal' }, { id: 'work', name: 'Work' }],
  permissions: { notifications: 'ask', camera: 'allow', microphone: 'ask', screen: 'deny', location: 'deny' },
};
const solo = { ...chat, id: 'notes', name: 'Notes', profiles: [{ id: 'personal', name: 'Personal' }] };

// ---- ids, the window's app id ----
eq('id ok', ['a', '0chat', 'notes-online', 'x'.repeat(40)].map(M.isWebId), [true, true, true, true]);
eq('id refused', ['', '-a', 'A', 'a_b', 'a.b', 'a/b', 'x'.repeat(41), null, 3].map(M.isWebId), [false, false, false, false, false, false, false, false, false]);
eq('window id', M.parseWindowAppId('lordos-web.chat.work'), { app: 'chat', profile: 'work' });
eq('window id made', M.windowAppId('chat', 'work'), 'lordos-web.chat.work');
for (const bad of ['lordos-web.chat', 'lordos-web.chat.work.x', 'lordos-web..work', 'lordos-web.Chat.work', 'lordos-webx.chat.work', 'org.chat.work', '', null])
  eq(`window id refused ${JSON.stringify(bad)}`, M.parseWindowAppId(bad), null);

// the window counts as the web app only when lord-server says the process is that lord-web
eq('window: owner agrees', M.windowWebApp('lordos-web.chat.work', { app: 'chat', profile: 'work' }, [chat])?.profile, 'work');
eq('window: no owner (any program can set the app id)', M.windowWebApp('lordos-web.chat.work', null, [chat]), null);
eq('window: owner of another profile', M.windowWebApp('lordos-web.chat.work', { app: 'chat', profile: 'personal' }, [chat]), null);
eq('window: owner of another app', M.windowWebApp('lordos-web.chat.work', { app: 'notes', profile: 'work' }, [chat, solo]), null);
eq('window: not installed any more', M.windowWebApp('lordos-web.chat.work', { app: 'chat', profile: 'work' }, []), null);

// ---- what the Dock, the launcher and the switcher show ----
eq('label: two profiles', M.windowLabel(chat, 'work'), { name: 'Chat', profile: 'Work' });
eq('label: one profile', M.windowLabel(solo, 'personal'), { name: 'Notes', profile: null });
eq('label: a profile no longer there', M.windowLabel(chat, 'gone'), { name: 'Chat', profile: null });
eq('label: no entry', M.windowLabel(null, 'work'), null);
eq('dock key', [M.webKey('chat'), M.webKey('Bad'), M.webAppOfKey('web:chat'), M.webAppOfKey('web:../x'), M.webAppOfKey('app:chat')], ['web:chat', null, 'chat', null, null]);
eq('default profile', M.defaultProfile(chat)?.id, 'personal');
eq('default profile of nothing', M.defaultProfile({ profiles: [] }), null);
eq('dock menu: the other profiles', M.profileMenu(chat, [{ app: 'chat', profile: 'work', pid: 9 }, { app: 'notes', profile: 'personal' }]),
  [{ profile: 'work', name: 'Work', running: true }]);
eq('dock menu: one profile, nothing', M.profileMenu(solo, []), []);
eq('dock menu: no entry', M.profileMenu(null, []), []);
eq('launcher item', M.launcherItem(chat), { id: 'web:chat', kind: 'web', name: 'Chat', comment: 'web.chat.example', category: 'Network', webApp: 'chat' });
eq('launcher item refused', [M.launcherItem({ id: 'Bad', name: 'x' }), M.launcherItem({ id: 'ok' }), M.launcherItem(null)], [null, null, null]);

// ---- the Store: the address ----
eq('address: a bare host', M.normalizeAddress('  Notes.Example  '), { url: 'https://notes.example/' });
eq('address: a path kept', M.normalizeAddress('https://notes.example/App?x=1#y'), { url: 'https://notes.example/App?x=1#y' });
eq('address: a port', M.normalizeAddress('notes.example:8443/a'), { url: 'https://notes.example:8443/a' });
eq('address: empty', M.normalizeAddress('   '), { error: 'empty' });
eq('address: http', M.normalizeAddress('http://notes.example/'), { error: 'scheme' });
eq('address: another scheme', M.normalizeAddress('file:///etc/passwd'), { error: 'scheme' });
eq('address: javascript', M.normalizeAddress('javascript:alert(1)'), { error: 'invalid' });
eq('address: a user in it', M.normalizeAddress('https://me@notes.example/'), { error: 'invalid' });
eq('address: spaces in the path', M.normalizeAddress('https://notes.example/a b'), { error: 'invalid' });
eq('address: not ASCII', M.normalizeAddress('https://notes.example/ñ'), { error: 'invalid' });
eq('address: a host with ..', M.normalizeAddress('https://notes..example/'), { error: 'invalid' });
eq('address: too long', M.normalizeAddress(`https://notes.example/${'a'.repeat(2100)}`), { error: 'long' });

// ---- the Store: the sheet's steps ----
let s = M.ADD_IDLE;
const step = (a) => { s = M.addReducer(s, a); return s; };
step({ type: 'type', text: 'http://x.example' });
eq('sheet: an http address fails before any probe', [step({ type: 'probe' }).step, s.error], ['failed', 'address.scheme']);
step({ type: 'type', text: 'notes.example' });
eq('sheet: typing again starts over', [s.step, s.error], ['idle', null]);
eq('sheet: probing', [step({ type: 'probe' }).step, s.url], ['probing', 'https://notes.example/']);
eq('sheet: typing while it probes changes nothing', step({ type: 'type', text: 'other' }).address, 'notes.example');
eq('sheet: a second probe while it probes changes nothing', step({ type: 'probe' }).step, 'probing');
step({ type: 'started', id: 7 });
eq('sheet: an answer of another probe is ignored', step({ type: 'result', id: 3, result: { ok: true, name: 'Evil' } }).step, 'probing');
const found = { ok: true, name: 'Notes Online', origin: 'https://notes.example', manifest: true, id: 7 };
eq('sheet: found', [step({ type: 'result', id: 7, result: found }).step, s.name], ['found', 'Notes Online']);
step({ type: 'rename', name: ' ' });
eq('sheet: no install with a bad name', step({ type: 'install' }).step, 'found');
step({ type: 'rename', name: 'My Notes' });
eq('sheet: installing', step({ type: 'install' }).step, 'installing');
eq('sheet: what is sent (no URL: lord-server has it from the probe)', M.addressEntry(s, 'Personal'), { source: 'url', probe: 7, name: 'My Notes', profile: 'Personal' });
eq('sheet: installed', [step({ type: 'installed', app: 'my-notes' }).step, s.app], ['installed', 'my-notes']);
eq('sheet: reset', step({ type: 'reset' }), M.ADD_IDLE);
s = M.addReducer(M.addReducer({ ...M.ADD_IDLE, address: 'down.example' }, { type: 'probe' }), { type: 'started', id: 2 });
eq('sheet: a failed probe', [step({ type: 'result', id: 2, result: { ok: false, error: 'webapps.probeTimeout' } }).step, s.error], ['failed', 'webapps.probeTimeout']);
eq('catalog entry sent', M.catalogEntry('chat', 'Personal'), { source: 'catalog', id: 'chat', profile: 'Personal' });

// names: the rule lord-server applies (webreg_name_ok)
eq('name ok', ['Chat', 'Mi trabajo', 'x'.repeat(64), 'ñ'.repeat(32)].map(M.nameOk), [true, true, true, true]);
eq('name refused', ['', ' Chat', 'Chat ', 'a‮b', 'a​b', 'a\nb', 'x'.repeat(65), 'ñ'.repeat(33), null].map(M.nameOk),
  [false, false, false, false, false, false, false, false, false]);

// ---- the Store: the section ----
eq('section without lord-web: the list, no address, the engine first', M.storeSection({ available: false, installed: [] }), { view: 'list', canAdd: false, needsEngine: true, count: 0 });
eq('section with lord-web', M.storeSection({ available: true, installed: [chat] }), { view: 'list', canAdd: true, needsEngine: false, count: 1 });
const cat = { apps: [
  { id: 'chat', name: 'Chat', startUrl: 'https://web.chat.example/', scope: 'https://web.chat.example/', origin: 'https://web.chat.example', icon: 'chat.png', description: 'webapps.desc.chat', keywords: ['messages'] },
  { id: 'evil', name: 'Evil', startUrl: 'https://evil.example/', scope: 'https://other.example/', origin: 'https://evil.example', icon: 'evil.png', description: 'x' },
  { id: 'plain', name: 'Plain', startUrl: 'http://plain.example/', scope: 'http://plain.example/', origin: 'http://plain.example', icon: 'plain.png', description: 'x' },
  { id: 'path', name: 'Path', startUrl: 'https://p.example/', scope: 'https://p.example/', origin: 'https://p.example', icon: '../p.png', description: 'x' },
] };
eq('catalog: bad entries left out, installed marked', M.catalogApps(cat, [chat]).map((a) => [a.id, a.installed]), [['chat', true]]);
eq('catalog: search by keyword', M.catalogApps(cat, [], 'mess').map((a) => a.id), ['chat']);
eq('catalog: search by host', M.catalogApps(cat, [], 'web.chat').map((a) => a.id), ['chat']);
eq('catalog: no match', M.catalogApps(cat, [], 'zzz'), []);
eq('catalog: none', M.catalogApps(null, []), []);

// ---- LordOS's own catalog: valid, its icons real PNGs, its texts in every language ----
const shipped = JSON.parse(fs.readFileSync(path.join(root, 'assets/webapps/catalog.json'), 'utf8'));
const CATS = ['AudioVideo', 'Development', 'Education', 'Game', 'Graphics', 'Network', 'Office', 'Science', 'System', 'Utility'];
const langs = fs.readdirSync(path.join(root, 'assets/locales')).filter((l) => fs.existsSync(path.join(root, 'assets/locales', l, 'webapps.json')));
eq('the six languages have webapps.json', langs.sort(), ['en', 'es', 'fr', 'it', 'lt', 'pt']);
const loc = (l, ns) => JSON.parse(fs.readFileSync(path.join(root, 'assets/locales', l, `${ns}.json`), 'utf8'));
eq('shipped catalog: every entry valid', shipped.apps.filter((a) => !M.catalogEntryOk(a)).map((a) => a.id), []);
eq('shipped catalog: ids unique', new Set(shipped.apps.map((a) => a.id)).size, shipped.apps.length);
for (const a of shipped.apps) {
  const icon = fs.readFileSync(path.join(root, 'assets/webapps/icons', a.icon));
  eq(`shipped ${a.id}: icon is a PNG under 1 MB`, icon.length <= 1 << 20 && icon.subarray(0, 8).equals(Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])), true);
  eq(`shipped ${a.id}: a known category`, CATS.includes(a.category), true);
  eq(`shipped ${a.id}: tested is a boolean`, typeof a.tested, 'boolean');
  for (const l of langs) {
    eq(`shipped ${a.id}: description in ${l}`, typeof loc(l, 'webapps')[a.description.replace(/^webapps\./, '')], 'string');
    eq(`shipped ${a.id}: category in ${l}`, typeof loc(l, 'store')[`cat.${a.category}`], 'string');
  }
}
// two apps on one origin (docs.google.com's Docs and Sheets) each keep to their own path: no scope holds another's
// start address, so neither one's window keeps the other's links (docs/WEBAPPS.md, "The catalog")
eq('shipped catalog: no app starts inside another app\'s scope', shipped.apps.flatMap((a) => shipped.apps
  .filter((b) => b.id !== a.id && b.startUrl.startsWith(a.scope)).map((b) => `${b.id} in ${a.id}`)), []);
eq('shipped catalog: no two apps with one scope', new Set(shipped.apps.map((a) => a.scope)).size, shipped.apps.length);
eq('shipped catalog: every icon file is used', fs.readdirSync(path.join(root, 'assets/webapps/icons')).filter((f) => !shipped.apps.some((a) => a.icon === f)), []);
eq('shipped catalog: only whatsapp checked on LordOS so far', shipped.apps.filter((a) => a.tested).map((a) => [a.id, a.verified]), [['whatsapp', '2026-11-11']]);
eq('shipped catalog: protected music says so', shipped.apps.filter((a) => (a.limitations || []).includes('drm')).map((a) => a.id).sort(), ['applemusic', 'spotify']);
eq('shipped catalog: a catalog of 37', shipped.apps.length, 37);
eq('shipped catalog: lowercase keywords (the search lowers the query only)', shipped.apps.flatMap((a) => a.keywords || []).filter((k) => k !== k.toLowerCase()), []);

for (const a of shipped.apps) {
  const f = M.catalogFacts(a);
  eq(`shipped ${a.id}: a publisher`, typeof f.publisher, 'string');
  eq(`shipped ${a.id}: verified is a real date or left out`, a.verified === undefined || M.validDate(a.verified), true);
  eq(`shipped ${a.id}: verified only when tested`, !a.verified || a.tested === true, true);
  eq(`shipped ${a.id}: typicalMemoryMB a whole number or left out`, a.typicalMemoryMB === undefined || f.typicalMemoryMB === a.typicalMemoryMB, true);
  eq(`shipped ${a.id}: limitations are known keys`, (a.limitations || []).filter((k) => !M.LIMITATIONS.includes(k)), []);
  eq(`shipped ${a.id}: only the documented keys`, Object.keys(a).filter((k) => !['id', 'name', 'startUrl', 'scope', 'origin', 'icon', 'themeColor', 'background', 'category',
    'description', 'keywords', 'tested', 'publisher', 'verified', 'typicalMemoryMB', 'limitations'].includes(k)), []);
}
eq('shipped whatsapp: the memory measured on VM 127', M.catalogFacts(shipped.apps.find((a) => a.id === 'whatsapp')).typicalMemoryMB, 810);
for (const l of langs) for (const k of M.LIMITATIONS) eq(`limit ${k} in ${l}`, typeof loc(l, 'webapps')[`limit.${k}`], 'string');

// ---- the Store: Discover and the categories ----
const two = { apps: [cat.apps[0], { ...cat.apps[0], id: 'mail', name: 'Mail', category: 'Office', keywords: ['mail'] }] };
eq('featured: the catalog\'s first ones, installed marked', M.featuredApps(two, [chat], 1).map((a) => [a.id, a.installed]), [['chat', true]]);
eq('featured: never more than asked', M.featuredApps(two, [], 4).length, 2);
eq('category apps', M.categoryApps(two, [], 'Office').map((a) => a.id), ['mail']);
eq('category apps: none', M.categoryApps(two, [], 'Game'), []);

// ---- the Store: Get, with the engine first when lordos-web isn't installed ----
const run = (actions, from = M.GET_IDLE) => actions.reduce((st, a) => M.getReducer(st, a), from);
const plan = { ok: true, download: 52428800, installedDelta: 209715200, items: [{ name: 'lordos-web', action: 'install', requested: true }] };
eq('get: with the engine, straight to the app', run([{ type: 'start', available: true }]).step, 'app');
eq('get: with the engine, installed', run([{ type: 'start', available: true }, { type: 'installed', app: 'chat' }]), { ...M.GET_IDLE, step: 'done', app: 'chat' });
eq('get: without it, the engine step with its plan', run([{ type: 'start', available: false, plan }]), { ...M.GET_IDLE, step: 'engine', plan });
eq('get: confirm runs the package job', run([{ type: 'start', available: false, plan }, { type: 'confirm' }]).step, 'engineJob');
eq('get: the job ends well, the app goes on with no click', run([{ type: 'start', available: false, plan }, { type: 'confirm' }, { type: 'jobDone', ok: true, available: true }]).step, 'app');
eq('get: … and is installed', run([{ type: 'start', available: false, plan }, { type: 'confirm' }, { type: 'jobDone', ok: true, available: true }, { type: 'installed', app: 'chat' }]).step, 'done');
eq('get: the job failed', run([{ type: 'start', available: false, plan }, { type: 'confirm' }, { type: 'jobDone', ok: false }]), { ...M.GET_IDLE, step: 'failed', plan });
eq('get: installed but lord-web not found', run([{ type: 'start', available: false, plan }, { type: 'confirm' }, { type: 'jobDone', ok: true, available: false }]).error, 'webapps.noRuntime');
eq('get: no confirm without a usable plan', run([{ type: 'start', available: false, plan: { ok: false, error: 'pkg.noIndex' } }, { type: 'confirm' }]).step, 'engine');
eq('get: no confirm without a plan at all', run([{ type: 'start', available: false, plan: null }, { type: 'confirm' }]).step, 'engine');
eq('get: a second start while the job runs is ignored', run([{ type: 'start', available: false, plan }, { type: 'confirm' }, { type: 'start', available: true }]).step, 'engineJob');
eq('get: cancel is ignored while the job runs', run([{ type: 'start', available: false, plan }, { type: 'confirm' }, { type: 'cancel' }]).step, 'engineJob');
eq('get: cancel from the engine step', run([{ type: 'start', available: false, plan }, { type: 'cancel' }]), M.GET_IDLE);
eq('get: a job result out of turn is ignored', run([{ type: 'jobDone', ok: true, available: true }]), M.GET_IDLE);
eq('get: install threw', run([{ type: 'start', available: true }, { type: 'error', error: 'webapps.exists' }]).error, 'webapps.exists');
eq('get: retry after a failure', run([{ type: 'start', available: true }, { type: 'error' }, { type: 'start', available: true }]).step, 'app');
eq('get: from the second app on, no engine step', run([{ type: 'start', available: true, plan }]).plan, null);
eq('get: busy', ['idle', 'engine', 'engineJob', 'app', 'done', 'failed'].map((step) => M.getBusy({ step })), [false, false, true, true, false, false]);
eq('engine sizes', M.engineSizes(plan), { download: 52428800, disk: 209715200 });
eq('engine sizes: a shrinking plan reads as 0', M.engineSizes({ ok: true, download: 0, installedDelta: -5 }), { download: 0, disk: 0 });
eq('engine sizes: no plan', [M.engineSizes(null), M.engineSizes({ ok: false })], [null, null]);

// ---- the Store: a web app's page ----
eq('valid dates', ['2026-11-11', '2026-02-30', '2026-1-1', '', null, 20261111].map(M.validDate), [true, false, false, false, false, false]);
eq('facts', M.catalogFacts({ publisher: ' Chat Inc. ', verified: '2026-11-11', typicalMemoryMB: 810, limitations: ['push', 'teleport', 'drm'] }),
  { publisher: 'Chat Inc.', verified: '2026-11-11', typicalMemoryMB: 810, limitations: ['drm', 'push'] });
eq('facts: wrong values read as missing', M.catalogFacts({ publisher: 5, verified: 'soon', typicalMemoryMB: -3, limitations: 'drm' }),
  { publisher: null, verified: null, typicalMemoryMB: null, limitations: [] });
eq('facts: none', M.catalogFacts(null), { publisher: null, verified: null, typicalMemoryMB: null, limitations: [] });
eq('tested: verified on a day', M.testedState({ tested: true, verified: '2026-11-11' }), { state: 'verified', date: '2026-11-11' });
eq('tested: with no day', M.testedState({ tested: true }), { state: 'tested' });
eq('tested: not yet', [M.testedState({ tested: false }), M.testedState({})], [{ state: 'untested' }, { state: 'untested' }]);
const chatCat = { ...cat.apps[0], category: 'Network', background: true };
const catOne = { apps: [chatCat] };
eq('detail: a catalog app not installed', M.detailOf('chat', catOne, []),
  { id: 'chat', entry: null, catalog: { ...chatCat, installed: false }, installed: false, name: 'Chat', origin: 'https://web.chat.example', source: 'catalog', background: true });
const mine = { ...chat, source: 'catalog', background: false };
eq('detail: installed from the catalog', (({ installed, source, background, name }) => ({ installed, source, background, name }))(M.detailOf('chat', catOne, [mine])),
  { installed: true, source: 'catalog', background: false, name: 'Chat' });
const added = { ...solo, id: 'notes', source: 'url', origin: 'https://notes.example' };
eq('detail: added from an address', (({ catalog, source, origin }) => ({ catalog, source, origin }))(M.detailOf('notes', catOne, [added])),
  { catalog: null, source: 'url', origin: 'https://notes.example' });
eq('detail: an address app with a catalog app\'s id is not the catalog\'s', M.detailOf('chat', catOne, [{ ...chat, source: 'url' }]).source, 'url');
eq('detail: unknown', M.detailOf('nope', catOne, []), null);
eq('disk: not installed', M.diskUsage(null), { installed: false, total: 0, complete: true, profiles: [] });
eq('disk: per profile and total', M.diskUsage(chat, { personal: { size: 1000, complete: true }, work: { size: 500, complete: false } }),
  { installed: true, total: 1500, complete: false, profiles: [{ id: 'personal', name: 'Personal', bytes: 1000, complete: true }, { id: 'work', name: 'Work', bytes: 500, complete: false }] });
eq('disk: a profile not measured (no folder yet) is 0', M.diskUsage(solo, { personal: null }).total, 0);
eq('memory: live, by profile', M.memoryView({ kb: 829440, processes: 5, profiles: [{ profile: 'personal', kb: 829440 }] }, M.catalogFacts({ typicalMemoryMB: 810 })),
  { kind: 'live', bytes: 849346560, profiles: [{ profile: 'personal', bytes: 849346560 }] });
eq('memory: not running, what LordOS measured', M.memoryView(null, M.catalogFacts({ typicalMemoryMB: 810 })), { kind: 'typical', bytes: 849346560, profiles: [] });
eq('memory: unknown', M.memoryView({ kb: 0 }, M.catalogFacts({})), { kind: 'unknown', bytes: 0, profiles: [] });
eq('upstream versions', ['2.48.7-1~deb13u1', '1:2.0+dfsg-3', '2.50', '', null, 'x1'].map(M.upstreamVersion), ['2.48.7', '2.0', '2.50', '', '', '']);

// ---- Settings → Web apps ----
eq('permission rows', M.permissionRows(chat).map((r) => r.value), ['ask', 'allow', 'ask', 'deny', 'deny']);
eq('permission rows: unknown values', M.permissionRows({ permissions: { camera: 'maybe' } }).map((r) => r.value), ['ask', 'ask', 'ask', 'ask', 'deny']);
eq('permission rows: every kind', M.permissionRows(null).map((r) => r.kind), M.PERM_KINDS);
for (const l of langs) for (const k of M.PERM_KINDS) eq(`perm ${k} in ${l}`, [typeof loc(l, 'webapps')[`perm.${k}`], typeof loc(l, 'webapps')[`ask.${k}`]], ['string', 'string']);
eq('remove a profile', [M.canRemoveProfile(chat), M.canRemoveProfile(solo), M.canRemoveProfile(null)], [true, false, false]);
eq('add a profile', [M.canAddProfile(chat), M.canAddProfile({ profiles: Array.from({ length: 16 }, (_, i) => ({ id: `p${i}`, name: `P${i}` })) })], [true, false]);
eq('next profile name', [M.nextProfileName(solo, 'Work'), M.nextProfileName(chat, 'Work'), M.nextProfileName({ profiles: [{ name: 'work' }, { name: 'Work 2' }] }, 'Work')],
  ['Work', 'Work 2', 'Work 3']);
eq('data folder', [M.dataDir('chat', 'work'), M.dataDir('chat'), M.dataDir('../x', 'work'), M.dataDir('chat', '..')],
  ['.local/share/lordos/web/chat/work', '.local/share/lordos/web/chat', null, null]);
eq('the dialog\'s answers', [M.askAnswer(true, true), M.askAnswer(true, false), M.askAnswer(false, true), M.askAnswer(false, false)],
  ['allow', 'once', 'deny', 'deny-once']);

// ---- the site's own icon: the cache's names, and which icon a notification may name ----
eq('icon cache names', [M.iconCacheName('whatsapp', 'png'), M.iconCacheName('my-chat', 'svg'), M.iconCacheName('../x', 'png'), M.iconCacheName('chat', 'gif'),
  M.iconCacheName('Chat', 'png'), M.iconCacheName('chat.x', 'png')], ['whatsapp.png', 'my-chat.svg', null, null, null, null]);
const H = '/home/u';
eq('a notification\'s icon: only the app\'s own files', [
  M.webIconPathOk('/home/u/.config/lordos/web/icons/chat.png', 'chat', H),
  M.webIconPathOk('/home/u/.cache/lordos/web/icons/chat.svg', 'chat', H),
  M.webIconPathOk('/home/u/.config/lordos/web/icons/chat.work.png', 'chat', H, 'work'),
  M.webIconPathOk('/home/u/.config/lordos/web/icons/chat.work.png', 'chat', H),
  M.webIconPathOk('/home/u/.config/lordos/web/icons/mail.png', 'chat', H),
  M.webIconPathOk('/etc/passwd', 'chat', H),
  M.webIconPathOk('/home/u/.config/lordos/web/icons/../../../.ssh/id.png', 'chat', H),
  M.webIconPathOk('/home/u/.config/lordos/web/icons/chat.png', 'chat', 'relative'),
  M.webIconPathOk('/home/u/.config/lordos/web/icons/chat.png', '../x', H)],
  [true, true, true, false, false, false, false, false, false]);
eq('when to fetch the site\'s icon', [M.wantsSiteIcon({ available: true, cached: false, tried: false }), M.wantsSiteIcon({ available: false, cached: false, tried: false }),
  M.wantsSiteIcon({ available: true, cached: true, tried: false }), M.wantsSiteIcon({ available: true, cached: false, tried: true })], [true, false, false, false]);

// ---- unread counts ----
let b = {};
b = M.badgeReducer(b, { app: 'chat', profile: 'personal', count: 3 });
b = M.badgeReducer(b, { app: 'chat', profile: 'work', count: 120 });
b = M.badgeReducer(b, { app: 'mail', profile: 'personal', count: 1 });
eq('the counts by app and profile', b, { chat: { personal: 3, work: 120 }, mail: { personal: 1 } });
eq('summed per app, or one profile', [M.badgeTotal(b, 'chat'), M.badgeTotal(b, 'chat', 'work'), M.badgeTotal(b, 'none')], [123, 120, 0]);
eq('bad events change nothing', [M.badgeReducer(b, { app: '../x', profile: 'a', count: 1 }) === b, M.badgeReducer(b, null) === b], [true, true]);
eq('clamped to 0..9999', M.badgeReducer({}, { app: 'a', profile: 'p', count: 123456 }), { a: { p: 9999 } });
eq('a 0 drops it', [M.badgeReducer(b, { app: 'mail', profile: 'personal', count: 0 }).mail, M.badgeReducer(b, { app: 'chat', profile: 'work', count: -4 }).chat],
  [undefined, { personal: 3 }]);
eq('what the badge says', [M.badgeLabel(0), M.badgeLabel(1), M.badgeLabel(99), M.badgeLabel(100), M.badgeLabel(9999), M.badgeLabel(NaN)], ['', '1', '99', '99+', '99+', '']);
const prefsOn = () => ({ badge: true, dockPerProfile: false }), prefsOff = () => ({ badge: false, dockPerProfile: false }), perProfile = () => ({ badge: true, dockPerProfile: true });
eq('the Dock: the app\'s icon sums its profiles', M.dockBadge(b, 'web:chat', prefsOn), '99+');
eq('the Dock: turned off', M.dockBadge(b, 'web:chat', prefsOff), '');
eq('the Dock: not a web app', [M.dockBadge(b, 'app:files', prefsOn), M.dockBadge(b, 'linux:firefox', prefsOn)], ['', '']);
eq('the Dock: per-profile icons', [M.dockBadge(b, 'web:chat', perProfile, (app, p) => p === 'work'), M.dockBadge(b, 'linux:lordos-web.chat.work', perProfile),
  M.dockBadge(b, 'web:chat', perProfile, () => false)], ['3', '99+', '99+']);

// ---- the shell's choices per app, the Dock entry of a window ----
eq('per-app choices: defaults and stored', [M.webPrefs({}, 'chat'), M.webPrefs({ webAppPrefs: { chat: { badge: false, dockPerProfile: true } } }, 'chat'), M.webPrefs(null, 'chat')],
  [{ badge: true, dockPerProfile: false }, { badge: false, dockPerProfile: true }, { badge: true, dockPerProfile: false }]);
eq('a window\'s Dock entry', [M.webDockId(chat, 'work', false), M.webDockId(chat, 'work', true), M.webDockId(chat, 'personal', true), M.webDockId(chat, 'ghost', true), M.webDockId(null, 'work', true)],
  ['web:chat', 'linux:lordos-web.chat.work', 'web:chat', 'web:chat', null]);

// ---- notification sounds ----
eq('whose sound', [M.notifySoundOf({}), M.notifySoundOf({ notifySound: 'app' }), M.notifySoundOf({ notifySound: 'loud' }), M.notifySoundOf(null)], ['auto', 'app', 'auto', 'auto']);
eq('LordOS plays its sound', [
  M.playsSystemSound({}, false), M.playsSystemSound({}, true), M.playsSystemSound({ notifySound: 'system' }, true),
  M.playsSystemSound({ notifySound: 'app' }, false), M.playsSystemSound({ notifySound: 'none' }, false)], [true, false, true, false, false]);
eq('notification keys', [M.notifyKey('chat'), M.notifyKey('../x'), M.appOfNotifyKey('lordos-web.chat'), M.appOfNotifyKey('lordos-web.chat.work'), M.appOfNotifyKey('linux:firefox')],
  ['lordos-web.chat', null, 'chat', null, null]);

// ---- profile colors, window titles ----
const colored = { ...chat, profiles: [{ id: 'personal', name: 'Personal' }, { id: 'work', name: 'trabajo', color: 'blue' }, { id: 'x', name: 'X', color: 'none' }, { id: 'y', name: 'Y' }] };
eq('a profile\'s color', [M.profileColor(colored, 'personal'), M.profileColor(colored, 'work'), M.profileColor(colored, 'x'), M.profileColor(colored, 'y'), M.profileColor(colored, 'ghost'),
  M.profileColor(chat, 'work')], [null, 'blue', null, 'blue', null, 'orange']);
eq('colors\' hex', [M.profileHex('orange'), M.profileHex('pink'), M.profileHex(null)], ['#ff9f0a', null, null]);
eq('a profile\'s initial', [M.profileInitial({ name: 'trabajo' }), M.profileInitial({ name: ' ñandú' }), M.profileInitial({ name: '' }), M.profileInitial(null)], ['T', 'Ñ', '', '']);
eq('window titles with two profiles', M.webWindowTitles(colored, 'work', 'WhatsApp Business'),
  { frame: 'Chat — trabajo', menu: 'WhatsApp Business — trabajo', color: 'blue' });
eq('window titles with one profile', M.webWindowTitles(solo, 'personal', '(3) Notes'), { frame: '(3) Notes', menu: '(3) Notes', color: null });
eq('window titles without a page title', M.webWindowTitles(colored, 'personal', ''), { frame: 'Chat — Personal', menu: 'Chat — Personal', color: null });
eq('the Dock menu\'s profiles are actions (no "running" mark to show as a check)', M.profileMenu(chat, [{ app: 'chat', profile: 'work' }]).map((r) => r.profile), ['work']);

// ---- Settings → Web apps: measuring each profile's data again (a profile added during the visit had none) ----
const k0 = M.sizesKey(chat, []);
eq('the key changes when a profile starts', [k0, M.sizesKey(chat, [{ app: 'chat', profile: 'work' }]), M.sizesKey(chat, [{ app: 'other', profile: 'work' }])],
  ['personal,work', 'personal,work*', 'personal,work']);
eq('and when one is added', M.sizesKey({ ...chat, profiles: [...chat.profiles, { id: 'new', name: 'New' }] }, []), 'personal,work,new');

console.log(bad ? `webapps-model: ${bad} of ${n} failed` : `webapps-model: ${n}/${n} ok`);
process.exit(bad ? 1 : 0);
