// fs.js — files in the home folder and on the user's mounted disks (/media/$USER,
// /run/media/$USER); the server rejects paths outside them. Writing is atomic and cannot touch
// ~/.local/share/applications, ~/.config/autostart, ~/.local/share/axia/apps or
// ~/.config/axia/settings.json (see server/src/ui/js_sys.c and server/src/fsops.c).
import { useEffect, useState } from 'react';

export const home = () => lord.fs.home();

export function list(path, { hidden = false } = {}) {
  const entries = lord.fs.list(path).filter((e) => hidden || !e.name.startsWith('.'));
  return entries.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name));
}

export const join = (...parts) => parts.filter(Boolean).join('/').replace(/\/+/g, '/');
export const parent = (path) => path.split('/').slice(0, -1).join('/');
export const basename = (path) => String(path).split('/').filter(Boolean).pop() || '/';

// Paths relative to the home folder: '' = home.
export function useDirectory(path, options) {
  const [state, setState] = useState({ entries: [], error: null });
  const [version, setVersion] = useState(0);
  useEffect(() => {
    try { setState({ entries: list(path || '.', options), error: null }); }
    catch (e) { setState({ entries: [], error: e.message }); }
  }, [path, version, options?.hidden]);
  return { ...state, reload: () => setVersion((v) => v + 1) };
}

// Contents of a file (max. 8 MB): UTF-8 text or, with { binary: true }, an ArrayBuffer.
export const read = (path, { binary = false } = {}) => lord.fs.read(path, binary ? 'binary' : 'text');

// Creates or replaces a file (text or ArrayBuffer, up to 32 MB). Keeps the permissions.
export const write = (path, data) => lord.fs.write(path, data);
export const mkdir = (path) => lord.fs.mkdir(path);
// Move or rename on the same disk (doesn't overwrite). Across disks, move() with a job.
export const rename = (from, to) => lord.fs.rename(from, to);

// Copy and move in the background (a server thread): they return the job id; the
// progress is read with jobs() → [{ id, src, dst, move, done, failed, error, bytes, total, files }].
// Recursive, keeps permissions and dates, doesn't overwrite and copies links as links.
export const copy = (from, to) => lord.fs.copy(from, to, false);
export const move = (from, to) => lord.fs.copy(from, to, true);
export const jobs = () => lord.fs.jobs();
export const cancelJob = (id) => lord.fs.jobCancel(id);
export const forgetJob = (id) => lord.fs.jobForget(id);

// Trash (freedesktop: ~/.local/share/Trash, and $mount/.Trash-$uid on disks).
// Permanent deletion is only possible inside the Trash.
export const trash = (path) => lord.fs.trash(path);
export const trashList = () => lord.fs.trashList();          // [{ id, name, original, deleted, dir, size }]
export const restore = (id) => lord.fs.restore(id);
export const removeFromTrash = (id) => lord.fs.removeFromTrash(id);
export const emptyTrash = () => lord.fs.emptyTrash();

// Info: { name, path, dir, link, target, size, mode, user, group, mtime, atime, ctime, btime, writable }
export const stat = (path) => lord.fs.stat(path);
export const du = (path, ms) => lord.fs.du(path, ms);          // { size, files, dirs, complete } (bounded)
export const free = (path) => lord.fs.free(path);              // { total, free } of the disk
export const search = (dir, text, opts) => lord.fs.search(dir, text, opts);   // by name, no hidden files
export const recent = (limit = 60, days = 30) => lord.fs.recent(limit, days);
export const volumes = () => lord.fs.volumes();                // [{ path, name, fs, total, free }]
