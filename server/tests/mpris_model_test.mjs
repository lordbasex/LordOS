// mpris_model_test.mjs — where the media keys go and what Now Playing lists, with LordOS's Player and the
// Linux players (MPRIS) side by side (sdk/mprismodel.js).
import { normalize, mergePlayers, touch, positionAt, seekTo, keyTarget, tileOrder, MAX_PLAYERS } from '../../sdk/mprismodel.js';

let ok = 0, bad = 0;
const eq = (label, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) ok++;
  else { bad++; console.log(`FAIL ${label}: ${JSON.stringify(got)} != ${JSON.stringify(want)}`); }
};

const P = (name, status, extra = {}) => ({ name: `org.mpris.MediaPlayer2.${name}`, identity: name, status, title: `${name} track`,
  artist: '', album: '', art: '', length: 200000, pos: 1000, canPlay: true, canPause: true, canNext: true, canPrev: true, canSeek: true, ...extra });
const N = (name) => `org.mpris.MediaPlayer2.${name}`;

// ---- normalize: everything checked again ----
eq('normalize: no name', normalize({ status: 'playing' }), null);
eq('normalize: not an object', normalize('x'), null);
const n1 = normalize({ name: N('a'), status: 'dancing', title: 'x'.repeat(400), art: 'https://example.org/a.png', length: -5, pos: 9e9, canSeek: true, canPlay: 1 });
eq('normalize: unknown status is stopped', n1.status, 'stopped');
eq('normalize: long title cut', n1.title.length, 255);
eq('normalize: remote art dropped', n1.art, '');
eq('normalize: negative length is none', n1.length, 0);
eq('normalize: no seek without a length', n1.canSeek, false);
eq('normalize: only true is true', n1.canPlay, false);
eq('normalize: position clamped to the length', normalize(P('b', 'paused', { pos: 999999 })).pos, 200000);
eq('normalize: NaN position', normalize(P('b', 'paused', { pos: NaN })).pos, 0);
eq('normalize: local art kept', normalize(P('b', 'paused', { art: '/tmp/a.png' })).art, '/tmp/a.png');

// ---- mergePlayers: when each one was last active ----
let list = mergePlayers([], [P('a', 'paused'), P('b', 'playing')], 1000);
eq('merge: a paused player has no activity yet', list[0].active, 0);
eq('merge: a playing one is active from now', list[1].active, 1000);
list = mergePlayers(list, [P('a', 'playing'), P('b', 'playing')], 2000);
eq('merge: starting to play moves it', list[0].active, 2000);
eq('merge: still playing keeps when it started', list[1].active, 1000);
list = mergePlayers(list, [P('a', 'paused')], 3000);
eq('merge: a player that left is gone', list.map((p) => p.identity), ['a']);
eq('merge: pausing keeps when it was active', list[0].active, 2000);
eq('merge: duplicates dropped', mergePlayers([], [P('a', 'paused'), P('a', 'playing')], 1).length, 1);
eq('merge: bounded', mergePlayers([], Array.from({ length: 40 }, (_, i) => P(`p${i}`, 'paused')), 1).length, MAX_PLAYERS);
eq('merge: not a list', mergePlayers([], null, 1), []);
eq('touch: a control makes it the active one', touch(list, N('a'), 9000)[0].active, 9000);

// ---- positions and seeking ----
const pl = mergePlayers([], [P('a', 'playing', { pos: 10000 })], 5000)[0];
eq('position: moves on while playing', positionAt(pl, 7500), 12500);
eq('position: never past the end', positionAt(pl, 5000 + 999999), 200000);
eq('position: still while paused', positionAt({ ...pl, status: 'paused' }, 9000), 10000);
eq('seek: clamped to the length', seekTo(pl, 500000), 200000);
eq('seek: clamped to zero', seekTo(pl, -40), 0);
eq('seek: rounded', seekTo(pl, 1234.6), 1235);
eq('seek: a player that cannot seek', seekTo({ ...pl, canSeek: false }, 10), null);
eq('seek: not a number', seekTo(pl, 'x'), null);

// ---- keyTarget: where the media keys go ----
const own = (playing) => ({ title: 'ours', playing });
const lp = (players, now = 100) => mergePlayers([], players, now);
eq('keys: nothing at all', keyTarget(null, 0, []), null);
eq('keys: our Player while it plays, even with a Linux one playing', keyTarget(own(true), 1, lp([P('a', 'playing')], 500)), { kind: 'own' });
eq('keys: a playing Linux player over our paused one', keyTarget(own(false), 900, lp([P('a', 'playing')], 500)), { kind: 'linux', name: N('a') });
let two = mergePlayers(lp([P('a', 'playing')], 100), [P('a', 'playing'), P('b', 'playing')], 200);
eq('keys: of two playing, the one that started last', keyTarget(null, 0, two), { kind: 'linux', name: N('b') });
two = mergePlayers(two, [P('a', 'paused'), P('b', 'paused')], 300);
eq('keys: nothing playing: the last active Linux player', keyTarget(null, 0, two), { kind: 'linux', name: N('b') });
eq('keys: nothing playing: our Player when it was active later', keyTarget(own(false), 250, two), { kind: 'own' });
eq('keys: nothing playing: a Linux player active after ours', keyTarget(own(false), 150, two), { kind: 'linux', name: N('b') });
eq('keys: a tie goes to our Player', keyTarget(own(false), 200, two), { kind: 'own' });
eq('keys: a player with no controls is skipped',
  keyTarget(null, 0, lp([P('a', 'playing', { canPlay: false, canPause: false, canNext: false, canPrev: false }), P('b', 'paused')])),
  { kind: 'linux', name: N('b') });
eq('keys: only our paused Player', keyTarget(own(false), 0, []), { kind: 'own' });

// ---- tileOrder: what Control Center lists ----
const three = mergePlayers(lp([P('a', 'paused'), P('b', 'playing')], 100), [P('a', 'paused'), P('b', 'playing'), P('c', 'stopped', { title: '' }), P('d', 'paused')], 200);
eq('tile: the key target first, then ours, then the others', tileOrder(own(false), 50, three),
  [{ kind: 'linux', name: N('b') }, { kind: 'own' }, { kind: 'linux', name: N('a') }]);
eq('tile: a stopped player with no track is not listed', tileOrder(null, 0, three, 9).map((x) => x.name), [N('b'), N('a'), N('d')]);
eq('tile: our playing Player first', tileOrder(own(true), 0, three, 2), [{ kind: 'own' }, { kind: 'linux', name: N('b') }]);
eq('tile: nothing', tileOrder(null, 0, []), []);

console.log(`mpris-model: ${ok} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
