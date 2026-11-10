// mprismodel.js — what Now Playing shows and where the media keys go (sdk/media.js): LordOS's own
// Player and the Linux players on the session bus (MPRIS, server/src/hw/mpris.c). Pure logic, no lord.*;
// tested by server/tests/mpris_model_test.mjs.
//
// The keys go to our own Player while it plays; otherwise to the Linux player that plays, the one that
// started most recently if several do; with nothing playing, to whichever was active last (our Player
// wins a tie), so a paused track resumes where it was.

export const MAX_PLAYERS = 16;          // as many as lord-server follows (MPRIS_MAX_PLAYERS)
export const TILE_MAX = 3;              // players Control Center shows at once
const LIMIT = { name: 255, identity: 95, title: 255, artist: 255, album: 255, art: 1023 };
const STATUS = new Set(['playing', 'paused', 'stopped']);

const str = (v, n) => (typeof v === 'string' ? v.slice(0, n) : '');
const ms = (v) => (typeof v === 'number' && Number.isFinite(v) && v > 0 ? Math.floor(v) : 0);

// One player from lord-server's list, with every field checked again (it came from another program).
export function normalize(p) {
  if (!p || typeof p !== 'object' || !str(p.name, LIMIT.name)) return null;
  const length = ms(p.length);
  const pos = length ? Math.min(ms(p.pos), length) : ms(p.pos);
  return {
    name: str(p.name, LIMIT.name), identity: str(p.identity, LIMIT.identity), status: STATUS.has(p.status) ? p.status : 'stopped',
    title: str(p.title, LIMIT.title), artist: str(p.artist, LIMIT.artist), album: str(p.album, LIMIT.album),
    art: str(p.art, LIMIT.art).startsWith('/') ? str(p.art, LIMIT.art) : '', length, pos,
    canPlay: p.canPlay === true, canPause: p.canPause === true, canNext: p.canNext === true, canPrev: p.canPrev === true,
    canSeek: p.canSeek === true && length > 0,
  };
}

// The new list, keeping when each one was last active: `active` (ms) moves when a player starts playing,
// `at` is when its position was read (to move it on while it plays).
export function mergePlayers(prev, list, now) {
  const old = new Map((prev || []).map((p) => [p.name, p]));
  const out = [];
  for (const raw of Array.isArray(list) ? list : []) {
    const p = normalize(raw);
    if (!p || out.some((x) => x.name === p.name)) continue;
    const was = old.get(p.name);
    const started = p.status === 'playing' && (!was || was.status !== 'playing');
    out.push({ ...p, at: now, active: started ? now : was ? was.active : 0 });
    if (out.length >= MAX_PLAYERS) break;
  }
  return out;
}

// After a control from the person: that player is the active one.
export const touch = (players, name, now) => players.map((p) => (p.name === name ? { ...p, active: now } : p));

// Where a player's track is now.
export function positionAt(p, now) {
  if (!p) return 0;
  const pos = p.status === 'playing' ? p.pos + Math.max(0, now - (p.at || now)) : p.pos;
  return p.length ? Math.min(pos, p.length) : pos;
}

// A seek within the track: the position clamped to [0, length], or null when that player can't seek.
export function seekTo(p, to) {
  if (!p || !p.canSeek || !p.length || typeof to !== 'number' || !Number.isFinite(to)) return null;
  return Math.max(0, Math.min(p.length, Math.round(to)));
}

const controllable = (p) => p.canPlay || p.canPause || p.canNext || p.canPrev;
const newest = (a, b) => b.active - a.active;

// Where the media keys go: { kind: 'own' } (LordOS's Player), { kind: 'linux', name } or null.
// own: the Player's nowPlaying (or null); ownActive: when it last started playing (ms).
export function keyTarget(own, ownActive, players) {
  if (own && own.playing) return { kind: 'own' };
  const list = (players || []).filter(controllable);
  const playing = list.filter((p) => p.status === 'playing').sort(newest);
  if (playing.length) return { kind: 'linux', name: playing[0].name };
  const last = [...list].sort(newest)[0];
  if (own && (!last || (ownActive || 0) >= last.active)) return { kind: 'own' };
  return last ? { kind: 'linux', name: last.name } : null;
}

// What Control Center lists: the one the keys go to first, then the others that have a track or a
// state (playing before paused, the newest first), at most `max`.
export function tileOrder(own, ownActive, players, max = TILE_MAX) {
  const target = keyTarget(own, ownActive, players);
  const shown = (players || []).filter((p) => p.title || p.status !== 'stopped');
  const rank = (p) => (p.status === 'playing' ? 0 : 1);
  const rest = shown.filter((p) => !(target && target.kind === 'linux' && target.name === p.name))
    .sort((a, b) => rank(a) - rank(b) || newest(a, b)).map((p) => ({ kind: 'linux', name: p.name }));
  const out = [];
  if (target) out.push(target);
  if (own && !(target && target.kind === 'own')) out.push({ kind: 'own' });
  for (const r of rest) out.push(r);
  return out.slice(0, max);
}
