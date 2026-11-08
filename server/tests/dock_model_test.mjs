// dock_model_test.mjs — the Dock's pinned-list model (sdk/dockmodel.js): typed ids, migration of
// untyped lists, move/pin/unpin edge cases, hidden (uninstalled) items and quick actions.
//
//   node tests/dock_model_test.mjs        (from server/; run_suites.sh runs it when node exists)
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const M = await import(path.join(here, '../../sdk/dockmodel.js'));
const { parseDockId, dockId, toTypedId, normalizePinned, moveItem, pinItem, unpinItem, cleanActions } = M;

let n = 0, bad = 0;
const eq = (name, got, want) => {
  n++;
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { bad++; console.log(`✗ ${name}: want ${w}, got ${g}`); }
};

// ---- typed ids ----
eq('parse app', parseDockId('app:files'), { kind: 'app', key: 'files' });
eq('parse lpk', parseDockId('lpk:hello-world'), { kind: 'lpk', key: 'hello-world' });
eq('parse desktop', parseDockId('desktop:org.mozilla.firefox'), { kind: 'desktop', key: 'org.mozilla.firefox' });
eq('parse desktop underscore', parseDockId('desktop:chromium_browser'), { kind: 'desktop', key: 'chromium_browser' });
for (const bad of ['files', 'app:', ':files', 'app:Files', 'app:-x', 'lpk:a', 'lpk:Up', 'lpk:a_b', 'desktop:..x', 'desktop:a..b',
  'desktop:.hidden', 'desktop:-x', 'desktop:lpk-notes', 'desktop:a/b', 'desktop:a b', 'desktop:' + 'x'.repeat(63), 'linux:foot',
  'web:x', 'app:files:x', 'APP:files', null, 42, undefined, {}]) eq(`parse rejects ${JSON.stringify(bad)}`, parseDockId(bad), null);
eq('dockId ok', dockId('lpk', 'notes'), 'lpk:notes');
eq('dockId bad kind', dockId('web', 'x'), null);
eq('dockId bad key', dockId('desktop', '../x'), null);
eq('desktop 62 chars ok', parseDockId('desktop:' + 'x'.repeat(62))?.kind, 'desktop');

// ---- migration of untyped lists ----
const classify = (id) => ({ files: 'app', terminal: 'app', hello: 'lpk', notes: 'lpk' })[id] || null;
eq('typed stays', toTypedId('desktop:firefox', classify), 'desktop:firefox');
eq('untyped native', toTypedId('files', classify), 'app:files');
eq('untyped package', toTypedId('hello', classify), 'lpk:hello');
eq('untyped unknown → app', toTypedId('gone', classify), 'app:gone');
eq('untyped invalid dropped', toTypedId('Bad Id', classify), null);
eq('linux: dropped', toTypedId('linux:foot', classify), null);
eq('migrate', normalizePinned(['files', 'desktop:firefox', 'hello', 'linux:x', 'files', 'app:files', 'terminal', 7, ''], classify),
  { list: ['app:files', 'desktop:firefox', 'lpk:hello', 'app:terminal'], changed: true });
eq('already typed: unchanged', normalizePinned(['app:files', 'lpk:notes'], classify), { list: ['app:files', 'lpk:notes'], changed: false });
eq('not a list', normalizePinned('files', classify), { list: [], changed: true });
eq('null (theme defaults)', normalizePinned(null, classify), { list: [], changed: false });
eq('empty', normalizePinned([], classify), { list: [], changed: false });

// ---- move ----
const L = ['app:a', 'app:b', 'app:c', 'app:d'];
eq('move to front', moveItem(L, 'app:c', 0), ['app:c', 'app:a', 'app:b', 'app:d']);
eq('move to end', moveItem(L, 'app:a', 3), ['app:b', 'app:c', 'app:d', 'app:a']);
eq('move past end', moveItem(L, 'app:a', 99), ['app:b', 'app:c', 'app:d', 'app:a']);
eq('move negative', moveItem(L, 'app:d', -5), ['app:d', 'app:a', 'app:b', 'app:c']);
eq('move same place keeps identity', moveItem(L, 'app:b', 1) === L, true);
eq('move unknown id', moveItem(L, 'app:zz', 0) === L, true);
eq('move fractional index', moveItem(L, 'app:d', 0.6), ['app:a', 'app:d', 'app:b', 'app:c']);
eq('move NaN → end', moveItem(L, 'app:a', NaN), ['app:b', 'app:c', 'app:d', 'app:a']);
eq('move single', moveItem(['app:a'], 'app:a', 0), ['app:a']);
// hidden items (uninstalled) keep their place and don't count
const H = ['app:a', 'lpk:gone', 'app:b', 'app:c'];
const shown = (id) => id !== 'lpk:gone';
eq('move with hidden: index counts shown only', moveItem(H, 'app:c', 1, shown), ['app:a', 'lpk:gone', 'app:c', 'app:b']);
eq('move with hidden to front', moveItem(H, 'app:b', 0, shown), ['app:b', 'app:a', 'lpk:gone', 'app:c']);
eq('move with hidden to end', moveItem(H, 'app:a', 2, shown), ['lpk:gone', 'app:b', 'app:c', 'app:a']);

// ---- pin ----
eq('pin at end', pinItem(L, 'desktop:firefox'), [...L, 'desktop:firefox']);
eq('pin at index', pinItem(L, 'lpk:notes', 1), ['app:a', 'lpk:notes', 'app:b', 'app:c', 'app:d']);
eq('pin at 0', pinItem(L, 'lpk:notes', 0), ['lpk:notes', ...L]);
eq('pin past end', pinItem(L, 'lpk:notes', 50), [...L, 'lpk:notes']);
eq('pin already pinned, no index: unchanged', pinItem(L, 'app:b') === L, true);
eq('pin already pinned, with index: moves', pinItem(L, 'app:d', 0), ['app:d', 'app:a', 'app:b', 'app:c']);
eq('pin invalid id refused', pinItem(L, 'files') === L, true);
eq('pin linux: refused', pinItem(L, 'linux:foot', 0) === L, true);
eq('pin into empty', pinItem([], 'app:files', 3), ['app:files']);
eq('pin with hidden', pinItem(H, 'app:x', 1, shown), ['app:a', 'lpk:gone', 'app:x', 'app:b', 'app:c']);

// ---- unpin ----
eq('unpin', unpinItem(L, 'app:b'), ['app:a', 'app:c', 'app:d']);
eq('unpin missing keeps identity', unpinItem(L, 'app:zz') === L, true);
eq('unpin last', unpinItem(['app:a'], 'app:a'), []);

// ---- locked items (launcher, Files, Settings) ----
const { LOCKED, LAUNCHER, ensureItems, isLocked } = M;
eq('launcher id parses', parseDockId(LAUNCHER), { kind: 'shell', key: 'launcher' });
eq('only one shell item', parseDockId('shell:trash'), null);
eq('locked set', LOCKED, ['app:files', 'shell:launcher', 'app:settings']);
eq('locked: files', isLocked('app:files'), true);
eq('not locked: terminal', isLocked('app:terminal'), false);
eq('unpin files refused', unpinItem(['app:files', 'shell:launcher', 'app:x'], 'app:files'), ['app:files', 'shell:launcher', 'app:x']);
eq('unpin launcher refused', unpinItem(['shell:launcher', 'app:x'], 'shell:launcher'), ['shell:launcher', 'app:x']);
eq('unpin settings refused', unpinItem(['app:settings'], 'app:settings'), ['app:settings']);
eq('launcher can move', moveItem(['app:files', 'shell:launcher', 'app:x'], 'shell:launcher', 2), ['app:files', 'app:x', 'shell:launcher']);
eq('settings can move', moveItem(['app:files', 'app:x', 'app:settings'], 'app:settings', 1), ['app:files', 'app:settings', 'app:x']);
// Files moves like any other app (issue #62), but can't be removed
const F = ['app:files', 'shell:launcher', 'app:a', 'app:b'];
eq('files moves', moveItem(F, 'app:files', 2), ['shell:launcher', 'app:a', 'app:files', 'app:b']);
eq('files moves to the end', moveItem(F, 'app:files', 3), ['shell:launcher', 'app:a', 'app:b', 'app:files']);
eq('launcher before files (move)', moveItem(F, 'shell:launcher', 0), ['shell:launcher', 'app:files', 'app:a', 'app:b']);
eq('app before files (pin)', pinItem(F, 'lpk:notes', 0), ['lpk:notes', 'app:files', 'shell:launcher', 'app:a', 'app:b']);
eq('pin files again with an index: moves', pinItem(F, 'app:files', 1), ['shell:launcher', 'app:files', 'app:a', 'app:b']);
eq('a moved files keeps its place in ensure', ensureItems(['app:a', 'app:files'], ['app:files'], []), ['app:a', 'app:files']);
eq('a missing files comes back first', ensureItems(['app:a'], ['app:files'], ['app:files', 'app:a']), ['app:files', 'app:a']);
eq('moved files survives ensure with every rule', ensureItems(['shell:launcher', 'app:settings', 'app:files'], LOCKED, ['app:files', 'shell:launcher', 'app:settings']),
  ['shell:launcher', 'app:settings', 'app:files']);
eq('files still locked', unpinItem(['app:a', 'app:files'], 'app:files'), ['app:a', 'app:files']);
eq('isMovable', [M.isMovable('app:files'), M.isMovable('shell:launcher'), M.isMovable('app:settings'), M.isMovable('desktop:x'), M.isMovable('x')], [true, true, true, true, false]);
const DEF = ['app:files', 'shell:launcher', 'app:terminal', 'app:settings', 'app:about'];
eq('ensure: old list without any', ensureItems(['app:terminal', 'desktop:firefox'], LOCKED, DEF),
  ['app:files', 'shell:launcher', 'app:terminal', 'app:settings', 'desktop:firefox']);
eq('ensure: settings after its default predecessor', ensureItems(['app:files', 'shell:launcher', 'app:about', 'app:terminal'], LOCKED, DEF),
  ['app:files', 'shell:launcher', 'app:about', 'app:terminal', 'app:settings']);
eq('ensure: complete list unchanged', ensureItems(DEF, LOCKED, DEF), DEF);
eq('ensure: moved launcher stays', ensureItems(['app:files', 'app:settings', 'shell:launcher'], LOCKED, DEF), ['app:files', 'app:settings', 'shell:launcher']);
eq('ensure: empty list', ensureItems([], LOCKED, DEF), ['app:files', 'shell:launcher', 'app:settings']);
eq('ensure: not in defaults → end', ensureItems(['app:x'], ['app:y'], []), ['app:x', 'app:y']);

// ---- quick actions ----
eq('actions ok', cleanActions([{ id: 'network', label: 'settings.section.network', args: { section: 'network' } }]),
  [{ id: 'network', label: 'settings.section.network', args: { section: 'network' }, new: false }]);
eq('actions drop malformed', cleanActions([
  { id: '', label: 'x' }, { id: 'a b', label: 'x' }, { id: 'ok', label: '' }, { id: 'nested', label: 'x', args: { a: { b: 1 } } },
  { id: 'arr', label: 'x', args: [1] }, { id: 'nonce', label: 'x', args: { nonce: 1 } }, { id: 'fn', label: 'x', args: { f: () => 1 } },
  null, 'str', { id: 'good', label: 'x', new: 1 }, { id: 'good', label: 'dup' },
]), [{ id: 'good', label: 'x', args: {}, new: true }]);
eq('actions not a list', cleanActions({ id: 'x' }), []);
eq('actions max', cleanActions(Array.from({ length: 60 }, (_, i) => ({ id: `a${i}`, label: 'x' }))).length, 40);

console.log(bad ? `dock-model: ${bad} of ${n} failed` : `dock-model: ${n}/${n} ok`);
process.exit(bad ? 1 : 0);
