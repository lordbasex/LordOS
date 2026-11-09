// remotedirs.js — the listings Files shows of folders on slow file systems (a Cloud account, a Connect to Server or
// SMB/NFS share), read off the interface's thread (fs.listAsync, server/src/fsasync.c) and kept between visits.
//
// Until 0.8.8.6 Files listed every folder with lord.fs.list on lord-server's only thread: deleting a file in an iCloud
// folder, or going back to its parent, froze the whole desktop until rclone answered. Here:
//   - get(dir) answers at once with what is known ({ entries, error, busy }) and lists the folder in the background
//     the first time; a folder seen before shows its last listing while a new one comes (going back is instant);
//   - refresh(dir) asks for a new listing; asked while one runs, one more follows it (never two at once per folder);
//   - hide(paths) takes items off the shown listing at once while they are being deleted or moved to the Trash, and
//     settle(paths) lists their folders again when that ends: they come back only if they are still there.
// Pure logic (no React, no lord.*): `list(dir)` returns a Promise of entries, `onChange()` is called when an answer
// arrives. tests: server/tests/files_remote_test.mjs.

const parentOf = (p) => { const i = p.lastIndexOf('/'); return i > 0 ? p.slice(0, i) : '/'; };

export function createRemoteDirs({ list, onChange = () => {}, max = 64 }) {
  const dirs = new Map();       // dir → { entries, error, busy, asked, listed, startedAt, used }
  const hidden = new Map();     // path → the `asked` count of its folder after which a new listing may show it again
  let tick = 0;

  const state = (dir) => {
    let s = dirs.get(dir);
    if (!s) {
      s = { entries: null, error: null, busy: false, asked: 1, listed: 0, started: 0, used: 0 };
      dirs.set(dir, s);
      if (dirs.size > max) {
        // the folder unused longest goes (never one being listed)
        let old = null;
        for (const [k, v] of dirs) if (!v.busy && k !== dir && (!old || v.used < dirs.get(old).used)) old = k;
        if (old) dirs.delete(old);
      }
    }
    s.used = ++tick;
    return s;
  };

  const start = (dir, s) => {
    s.busy = true;
    s.started = s.asked;
    let p;
    try { p = Promise.resolve(list(dir)); } catch (e) { p = Promise.reject(e); }
    p.then((entries) => { s.entries = Array.isArray(entries) ? entries : []; s.error = null; },
      (e) => { s.error = (e && e.message) || String(e); if (!s.entries) s.entries = []; })
      .then(() => {
        s.busy = false;
        s.listed = s.started;
        // what was hidden until a listing asked after its operation ended: this listing tells whether it is still there
        for (const [path, after] of hidden) if (after !== null && after <= s.listed && parentOf(path) === dir) hidden.delete(path);
        if (s.asked > s.listed && dirs.get(dir) === s) start(dir, s);       // asked again meanwhile
        onChange(dir);
      });
  };

  const view = (dir, s) => {
    const e = s.entries;
    if (!e || !hidden.size) return { entries: e, error: s.error, busy: s.busy };
    const pre = dir === '/' ? '/' : `${dir}/`;
    return { entries: e.filter((x) => !hidden.has(pre + x.name)), error: s.error, busy: s.busy };
  };

  return {
    get(dir) {
      const s = state(dir);
      if (!s.busy && s.asked > s.listed) start(dir, s);
      return view(dir, s);
    },
    has: (dir) => !!(dirs.get(dir) && dirs.get(dir).entries),
    refresh(dir) {
      const s = dirs.get(dir);
      if (!s) return;
      s.asked++;
      if (!s.busy) start(dir, s);
    },
    refreshAll() { for (const dir of dirs.keys()) this.refresh(dir); },
    hide(paths) {
      for (const p of paths) hidden.set(p, null);
      onChange(null);
    },
    // the operation on these paths ended (well or not): their folders are listed again, and each comes back if it is
    // still there
    settle(paths) {
      const folders = new Set();
      for (const p of paths) {
        const dir = parentOf(p);
        const s = dirs.get(dir);
        if (!s) { hidden.delete(p); continue; }
        if (hidden.has(p)) hidden.set(p, s.asked + 1);
        folders.add(dir);
      }
      for (const dir of folders) this.refresh(dir);
      onChange(null);
    },
    hidden: (p) => hidden.has(p),
    forget(dir) { if (!dirs.get(dir)?.busy) dirs.delete(dir); },
  };
}
