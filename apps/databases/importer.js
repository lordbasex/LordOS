// importer.js — finding Sequel Ace's and Sequel Pro's Favorites.plist in the home folder and on the user's mounted
// disks (another computer's disk: /media/<user>/<disk>/Users/<name>/…), without holding the desktop: the folders are
// listed and the files looked up with the asynchronous lord.fs calls, and only a file that is there and not on a network
// share is read (docs/DATABASES.md, "Importing").
import { fs } from '@lordos/sdk';
import { candidatePaths, importSequel } from './plist.js';

const MAX_PLIST = 4 * 1024 * 1024;

async function dirs(path) {
  try { return (await fs.listAsync(path)).filter((e) => e.dir).map((e) => e.name); } catch (e) { return []; }
}

// → [{ source, path, disk, text }] for every file found
export async function findSequelFiles() {
  let home = '';
  try { home = fs.home(); } catch (e) { return []; }
  const user = home.split('/').filter(Boolean).pop() || '';
  const mounts = [];
  for (const base of [`/media/${user}`, `/run/media/${user}`]) {
    for (const name of await dirs(base)) mounts.push({ path: `${base}/${name}`, name });
  }
  const users = {};
  for (const m of mounts) users[m.path] = await dirs(`${m.path}/Users`);
  const found = [];
  for (const c of candidatePaths(home, mounts, users)) {
    let st = null;
    try { st = await fs.statAsync(c.path); } catch (e) { st = null; }
    if (!st || st.dir || (st.size && st.size > MAX_PLIST)) continue;
    if (fs.isRemote(c.path)) continue;
    let text = '';
    try { text = fs.read(c.path); } catch (e) { continue; }
    if (typeof text !== 'string' || !text) continue;
    found.push({ ...c, text });
  }
  return found;
}

// what each file would add to favorites.json now: [{ ...file, imp | error }]
export function previewImports(files, existing) {
  return files.map((f) => {
    try { return { ...f, imp: importSequel(f.text, { source: f.source, existing }) }; } catch (e) { return { ...f, error: e.code || 'syntax' }; }
  });
}
