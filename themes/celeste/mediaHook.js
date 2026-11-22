// mediaHook.js: what is playing, for the dashboard, in one shape: { title, artist, image, playing, pos, duration,
// toggle, next, prev } or null. A LordOS player (sdk/media.js nowPlaying) first, else a Linux player over MPRIS
// (linuxPlayers). Its position is moved forward once a second only while something plays and the dashboard is out.
import { useEffect, useState } from 'react';
import { useLinuxPlayers, useNowPlaying, useThumb } from '@lordos/sdk';

function ctl(name, cmd) {
  try { if (typeof lord !== 'undefined' && lord.mpris) lord.mpris.control(name, cmd, 0); } catch (e) { /* no session bus */ }
}

export function useMediaEntry() {
  const own = useNowPlaying();
  const players = useLinuxPlayers();
  const p = own ? null : players.find((x) => x.status === 'playing') || players[0] || null;
  const ticking = !own && !!p && p.status === 'playing';
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    if (!ticking) return undefined;
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, [ticking]);

  let e = null;
  if (own) {
    e = { title: own.title, artist: own.artist, art: '', cover: own.cover || null, playing: !!own.playing, pos: own.pos || 0,
      duration: own.duration || 0, toggle: own.toggle || null, next: own.next || null, prev: own.prev || null };
  } else if (p) {
    const playing = p.status === 'playing';
    let pos = playing ? p.pos + Math.max(0, now - (p.at || now)) : p.pos;
    if (p.length) pos = Math.min(pos, p.length);
    e = { title: p.title, artist: p.artist, art: p.art || '', cover: null, playing, pos, duration: p.length || 0,
      toggle: (playing ? p.canPause : p.canPlay || p.canPause) ? () => ctl(p.name, 'toggle') : null,
      next: p.canNext ? () => ctl(p.name, 'next') : null,
      prev: p.canPrev ? () => ctl(p.name, 'prev') : null };
  }
  const thumb = useThumb(e && e.art ? e.art : null, 256);
  if (!e) return null;
  return { ...e, image: e.cover || thumb || null };
}
