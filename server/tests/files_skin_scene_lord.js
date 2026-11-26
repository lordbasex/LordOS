// files_skin_scene_lord.js — imported first by tests/files_skin_scene.mjs: lord.fs over menubar_test.mjs's stub lord, with a
// small home folder (Documents with a file, Desktop, two files) and a record of every call that would change a disk, so
// the scene can tell what Files did when a skin asked for an action.
const HOME = '/home/test';
export const fsLog = [];
const tree = {
  [HOME]: [
    { name: 'Desktop', dir: true }, { name: 'Documents', dir: true },
    { name: 'notes.txt', dir: false, size: 12 }, { name: 'photo.png', dir: false, size: 2048 },
  ],
  [`${HOME}/Documents`]: [{ name: 'report.pdf', dir: false, size: 4096 }],
  [`${HOME}/Desktop`]: [],
  '/': [{ name: 'etc', dir: true }, { name: 'home', dir: true }],
};
const entry = (dir, e) => ({ ...e, path: `${dir === '/' ? '' : dir}/${e.name}`, mtime: 1700000000, mode: e.dir ? 0o755 : 0o644 });
const find = (p) => {
  if (tree[p]) return { name: p.split('/').pop() || '/', path: p, dir: true, size: 0, mtime: 1700000000, writable: p.startsWith(HOME) };
  const dir = p.split('/').slice(0, -1).join('/') || '/';
  const e = (tree[dir] || []).find((x) => x.name === p.split('/').pop());
  return e ? { ...entry(dir, e), writable: p.startsWith(HOME) } : null;
};
const api = {
  home: () => HOME,
  list: (p) => (tree[p] || []).map((e) => entry(p, e)),
  stat: (p) => { const e = find(p); if (!e) throw new Error('ENOENT'); return e; },
  exists: (p) => !!find(p),
  read: () => { throw new Error('ENOENT'); },
  volumes: () => [], mounts: () => [], recent: () => [], trashList: () => [], jobs: () => [],
  free: () => ({ free: 1e11, total: 2e11 }),
  trash: (p) => { fsLog.push(`trash ${p}`); return true; },
  mkdir: (p) => { fsLog.push(`mkdir ${p}`); return true; },
  rename: (a, b) => { fsLog.push(`rename ${a} ${b}`); return true; },
  emptyTrash: () => { fsLog.push('emptyTrash'); return true; },
};
globalThis.lord.getLayout = () => [0, 0, 1100, 700];
globalThis.lord.fs = new Proxy(api, { get: (o, k) => (k in o ? o[k] : () => undefined) });
