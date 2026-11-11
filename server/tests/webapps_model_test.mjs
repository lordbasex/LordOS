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
eq('section without lord-web', M.storeSection({ available: false, installed: [] }), { view: 'runtime', canAdd: false });
eq('section with lord-web', M.storeSection({ available: true, installed: [chat] }), { view: 'list', canAdd: true, count: 1 });
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
eq('shipped catalog: lowercase keywords (the search lowers the query only)', shipped.apps.flatMap((a) => a.keywords || []).filter((k) => k !== k.toLowerCase()), []);

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

console.log(bad ? `webapps-model: ${bad} of ${n} failed` : `webapps-model: ${n}/${n} ok`);
process.exit(bad ? 1 : 0);
