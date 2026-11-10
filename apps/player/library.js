// library.js — the Player's music and video library.
//
// Walks ~/Music and ~/Videos (bounded), reads the tags with media.probe (lord-media, in the
// background) and stores what it read in ~/.cache/lordos/music.json (by path, size and date), so
// next time nothing that did not change is read again.
import { fs, media, userDirs, createStore } from '@lordos/sdk';
import { compareNames } from '../../sdk/collate.js';

export const AUDIO = /\.(mp3|ogg|oga|opus|flac|wav|m4a|aac|wma|aif|aiff|ape|mka)$/i;
export const VIDEO = /\.(mp4|mkv|webm|avi|mov|m4v|wmv|flv|mpg|mpeg|ogv|ts|3gp)$/i;
const CACHE = '.cache/lordos/music.json';

export const library = createStore({ tracks: [], videos: [], scanning: false, done: 0, total: 0 });

function walk(dir, re, out, depth = 0) {
  if (depth > 6 || out.length > 5000) return;
  let list = [];
  try { list = fs.list(dir); } catch (e) { return; }
  for (const e of list) {
    if (e.dir) walk(e.path || `${dir}/${e.name}`, re, out, depth + 1);
    else if (re.test(e.name)) out.push({ path: e.path || `${dir}/${e.name}`, name: e.name, size: e.size, mtime: e.mtime });
  }
}

const baseName = (p) => String(p).split('/').pop().replace(/\.[^.]+$/, '');

// Track from the probe result (or the file name if there are no tags).
export function trackFrom(f, info) {
  const tags = (info && info.tags) || {};
  const n = parseInt(String(tags.track || '').split('/')[0], 10);
  return {
    path: f.path, size: f.size, mtime: f.mtime,
    title: tags.title || baseName(f.path),
    artist: tags.artist || tags.album_artist || '',
    album: tags.album || '',
    albumArtist: tags.album_artist || tags.artist || '',
    track: Number.isFinite(n) ? n : 0,
    year: String(tags.date || '').slice(0, 4),
    genre: tags.genre || '',
    duration: (info && info.duration) || 0,
    cover: !!(info && info.streams && info.streams.some((s) => s.type === 'video' && s.cover)),
  };
}

let running = false;
export async function scan() {
  if (running) return;
  running = true;
  const dirs = userDirs();
  const files = [], vids = [];
  walk(dirs.music, AUDIO, files);
  walk(dirs.videos, VIDEO, vids);
  let cache = {};
  try { cache = JSON.parse(fs.read(CACHE)); } catch (e) { /* first time */ }
  const key = (f) => `${f.size}:${Math.round(f.mtime || 0)}`;
  const tracks = [];
  const todo = [];
  for (const f of files) {
    const c = cache[f.path];
    if (c && c.k === key(f)) tracks.push(c.t);
    else todo.push(f);
  }
  const videos = vids.map((f) => ({ path: f.path, title: baseName(f.path), size: f.size, mtime: f.mtime }));
  library.set({ tracks: [...tracks], videos, scanning: todo.length > 0, done: 0, total: todo.length });
  const next = {};
  for (const tr of tracks) next[tr.path] = cache[tr.path];
  for (let i = 0; i < todo.length; i++) {
    const f = todo[i];
    const info = await media.probe(f.path);
    const tr = trackFrom(f, info);
    tracks.push(tr);
    next[f.path] = { k: key(f), t: tr };
    if (i % 8 === 7 || i === todo.length - 1) library.set({ tracks: [...tracks], done: i + 1 });
  }
  library.set({ scanning: false });
  if (todo.length || Object.keys(cache).length !== Object.keys(next).length) {
    try { fs.mkdir('.cache/lordos'); } catch (e) { /* already there */ }
    try { fs.write(CACHE, JSON.stringify(next)); } catch (e) { /* no disk */ }
  }
  running = false;
}

// Grouping: artists → albums → tracks.
export function albumsOf(tracks) {
  const map = new Map();
  for (const t of tracks) {
    const k = `${t.albumArtist}\u0000${t.album}`;
    if (!map.has(k)) map.set(k, { key: k, album: t.album, artist: t.albumArtist, year: t.year, tracks: [] });
    map.get(k).tracks.push(t);
  }
  const list = [...map.values()];
  for (const a of list) a.tracks.sort((x, y) => (x.track - y.track) || compareNames(x.title, y.title));
  return list.sort((a, b) => compareNames(a.artist, b.artist) || compareNames(a.album, b.album));
}

export function artistsOf(tracks) {
  const map = new Map();
  for (const t of tracks) {
    const k = t.albumArtist || t.artist;
    if (!map.has(k)) map.set(k, []);
    map.get(k).push(t);
  }
  return [...map.entries()].map(([name, list]) => ({ name, tracks: list })).sort((a, b) => compareNames(a.name, b.name));
}

// .srt subtitles: [{ start, end, text }] in ms.
export function parseSrt(text) {
  const out = [];
  const ms = (s) => { const m = /(\d+):(\d+):(\d+)[,.](\d+)/.exec(s); return m ? ((+m[1] * 60 + +m[2]) * 60 + +m[3]) * 1000 + +m[4].padEnd(3, '0').slice(0, 3) : 0; };
  for (const block of String(text).replace(/\r/g, '').split(/\n\s*\n/)) {
    const lines = block.split('\n').filter((l) => l.trim() !== '');
    const i = lines.findIndex((l) => l.includes('-->'));
    if (i < 0) continue;
    const [a, b] = lines[i].split('-->');
    const body = lines.slice(i + 1).join('\n').replace(/<[^>]+>/g, '').replace(/\{\\[^}]*\}/g, '');
    if (body) out.push({ start: ms(a), end: ms(b), text: body });
  }
  return out;
}
