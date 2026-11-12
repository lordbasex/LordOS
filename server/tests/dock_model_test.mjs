// dock_model_test.mjs — the Dock's pinned-list model (sdk/dockmodel.js): typed ids, migration of
// untyped lists, move/pin/unpin edge cases, hidden (uninstalled) items, quick actions, hiding (#46) and icon sizes (#128).
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
  'web:X', 'web:-x', 'web:a_b', 'web:a.b', 'web:' + 'x'.repeat(41), 'www:x', 'app:files:x', 'APP:files', null, 42, undefined, {}]) eq(`parse rejects ${JSON.stringify(bad)}`, parseDockId(bad), null);
eq('dockId ok', dockId('lpk', 'notes'), 'lpk:notes');
// a web app (docs/WEBAPPS.md): the registry's id rule
eq('parse web', parseDockId('web:notes-online'), { kind: 'web', key: 'notes-online' });
eq('dockId web', dockId('web', '0chat'), 'web:0chat');
eq('dockId bad kind', dockId('www', 'x'), null);
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

// ---- hiding: the three modes and "a window covers the Dock" (issue #46) ----
const { dockHideMode, dockHideSettings, dockCovered, DOCK_HIDE_MODES } = M;
eq('hide: defaults never', dockHideMode({}), 'never');
eq('hide: autohide off → never (intellihide kept)', dockHideMode({ dockAutohide: false, dockIntellihide: true }), 'never');
eq('hide: autohide → always', dockHideMode({ dockAutohide: true }), 'always');
eq('hide: autohide + intellihide → overlap', dockHideMode({ dockAutohide: true, dockIntellihide: true }), 'overlap');
eq('hide: junk is never', dockHideMode({ dockAutohide: 'yes', dockIntellihide: 1 }), 'never');
eq('hide: no prefs', dockHideMode(null), 'never');
for (const m of DOCK_HIDE_MODES) eq(`hide: ${m} round trip`, dockHideMode({ dockIntellihide: true, ...dockHideSettings(m) }), m);
eq('hide: never keeps the chosen hiding mode for the shortcut', dockHideSettings('never'), { dockAutohide: false });
eq('hide: unknown mode → never', dockHideSettings('sometimes'), { dockAutohide: false });

// a bottom Dock on a 1920×1080 main display at 0,0 (and a second display to its right)
const R = { x0: 700, x1: 1220, y0: 1000, y1: 1080 };
const win = (o) => ({ id: 'w', x: 100, y: 100, w: 800, h: 600, minimized: false, ...o });
eq('cover: nothing open', dockCovered([], 1, R), false);
eq('cover: a window well above', dockCovered([win()], 1, R), false);
eq('cover: a window reaching into it', dockCovered([win({ y: 500 })], 1, R), true);
eq('cover: touching its top edge only', dockCovered([win({ y: 400, h: 600 })], 1, R), false);
eq('cover: one pixel into it', dockCovered([win({ y: 401, h: 600 })], 1, R), true);
eq('cover: beside it, same height', dockCovered([win({ x: 0, w: 700, y: 900 })], 1, R), false);
eq('cover: minimized does not count', dockCovered([win({ y: 500, minimized: true })], 1, R), false);
eq('cover: another desktop does not count', dockCovered([win({ y: 500, desktop: 2 })], 1, R), false);
eq('cover: on the desktop in view', dockCovered([win({ y: 500, desktop: 2 })], 2, R), true);
eq('cover: no desktop = the first', dockCovered([win({ y: 500 })], 1, R), true);
eq('cover: picture-in-picture does not count', dockCovered([win({ y: 500, pip: { x: 800, y: 950, w: 320, h: 180 } })], 1, R), false);
eq('cover: a window on the second display', dockCovered([win({ x: 1920, y: 500 })], 1, R), false);
// a maximized window is drawn over its work area (frameOf), not at its stored place
const frameOf = (w) => (w.maximized ? { x: 0, y: 28, w: 1920, h: 1052 } : w);
eq('cover: maximized fills the work area', dockCovered([win({ maximized: true })], 1, R, { frameOf }), true);
// fullscreen covers its display (displayOf), whatever its stored place
const displayOf = (w) => (w.x >= 1920 ? { x: 1920, y: 0, w: 1920, h: 1080 } : { x: 0, y: 0, w: 1920, h: 1080 });
eq('cover: fullscreen on the Dock\'s display', dockCovered([win({ fullscreen: true })], 1, R, { displayOf }), true);
eq('cover: fullscreen on the other display', dockCovered([win({ x: 2000, fullscreen: true })], 1, R, { displayOf }), false);
eq('cover: fullscreen with no display known', dockCovered([win({ fullscreen: true })], 1, R), false);
eq('cover: any window of several', dockCovered([win(), win({ id: 'v', x: 1000, y: 900, w: 300, h: 300 })], 1, R), true);
// a Linux (Wayland or X11) window is a window like any other: its frame is in the list
eq('cover: a Linux window', dockCovered([win({ id: 'linux#3', args: { surface: 7 }, y: 700 })], 1, R), true);
// left Dock
const LR = { x0: 0, x1: 80, y0: 200, y1: 880 };
eq('cover: left Dock, window at the left edge', dockCovered([win({ x: 0 })], 1, LR), true);
eq('cover: left Dock, window right of it', dockCovered([win({ x: 80 })], 1, LR), false);
eq('cover: no rect', dockCovered([win({ y: 500 })], 1, null), false);
eq('cover: empty rect', dockCovered([win({ y: 500 })], 1, { x0: 5, x1: 5, y0: 0, y1: 10 }), false);
eq('cover: zero-size window', dockCovered([win({ y: 1010, w: 0 })], 1, R), false);
eq('cover: junk entries', dockCovered([null, 3, win({ y: 500 })], 1, R), true);

// ---- icon sizes (issue #128: the minimum went from 36 to 20 px) ----
const { dockSizes, dockBadgeMetrics, DOCK_SIZE_MIN, DOCK_SIZE_MAX, DOCK_MAGNIFY_MAX } = M;
eq('size: limits', [DOCK_SIZE_MIN, DOCK_SIZE_MAX, DOCK_MAGNIFY_MAX], [20, 80, 128]);
eq('size: defaults', dockSizes(undefined, undefined), { dockSize: 54, dockMagnifySize: 86 });
eq('size: the new minimum', dockSizes(20, 40), { dockSize: 20, dockMagnifySize: 40 });
eq('size: 24 kept (was raised to 36)', dockSizes(24, 86).dockSize, 24);
eq('size: below the minimum', dockSizes(8, 86).dockSize, 20);
eq('size: above the maximum', dockSizes(200, 300), { dockSize: 80, dockMagnifySize: 128 });
eq('size: rounded', dockSizes(30.6, 50.2), { dockSize: 31, dockMagnifySize: 50 });
eq('size: junk', dockSizes('x', null), { dockSize: 54, dockMagnifySize: 86 });
eq('size: string number', dockSizes('28', '60'), { dockSize: 28, dockMagnifySize: 60 });
eq('size: magnified never smaller than the icon', dockSizes(40, 22), { dockSize: 40, dockMagnifySize: 40 });
eq('size: old minimum still valid', dockSizes(36, 86), { dockSize: 36, dockMagnifySize: 86 });
eq('badge: usual size', dockBadgeMetrics(54), { height: 18, font: 11, pad: 5 });
eq('badge: magnified', dockBadgeMetrics(128).height, 18);
eq('badge: smallest Dock', dockBadgeMetrics(20), { height: 12, font: 9, pad: 3 });
eq('badge: 32 px', dockBadgeMetrics(32), { height: 14, font: 10, pad: 3 });
eq('badge: never more than 60 % of the icon at the minimum', dockBadgeMetrics(20).height <= 20 * 0.6, true);

console.log(bad ? `dock-model: ${bad} of ${n} failed` : `dock-model: ${n}/${n} ok`);
process.exit(bad ? 1 : 0);
