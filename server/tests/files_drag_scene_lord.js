// files_drag_scene_lord.js — imported first by tests/files_drag_scene.mjs: lord.fs over menubar_test.mjs's stub lord,
// with a home folder of 2000 files and 20 folders. lord.fs.home calls are counted: Files' main component reads the
// home folder while it renders (the sidebar's Home, the start place), so the count tells how often it drew again.
const N_FILES = 2000, N_DIRS = 20, HOME = '/home/test';
export const fsCalls = { home: 0 };
const big = [
  ...Array.from({ length: N_DIRS }, (_, i) => ({ name: `Folder ${String(i).padStart(2, '0')}`, dir: true, size: 0, mtime: 1700000000 })),
  ...Array.from({ length: N_FILES }, (_, i) => ({ name: `file-${String(i).padStart(4, '0')}.txt`, dir: false, size: 100 + i, mtime: 1700000000 + i })),
];
const api = {
  home: () => { fsCalls.home++; return HOME; },
  list: (p) => (p === HOME ? big.map((e) => ({ ...e })) : []),
  stat: (p) => {
    if (p === HOME) return { name: 'test', path: p, dir: true, size: 0, mtime: 0 };
    const e = big.find((x) => `${HOME}/${x.name}` === p);
    if (!e) throw new Error('ENOENT');
    return { ...e, path: p };
  },
  volumes: () => [], mounts: () => [], recent: () => [], trashList: () => [], jobs: () => [],
  free: () => ({ free: 1e11, total: 2e11 }),
};
// the window is 1100 × 700: the icon view lays out its grid in rows of several cards
globalThis.lord.getLayout = () => [0, 0, 1100, 700];
globalThis.lord.fs = new Proxy(api, { get: (o, k) => (k in o ? o[k] : () => undefined) });
