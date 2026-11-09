// mountmodel.js — which mount a path lies on (pure; sdk/fs.js isRemote, tests/files_remote_test.mjs).
//
// table: lord.fs.mounts() → [{ path, remote }]. The longest mount point that holds the path decides, as the kernel
// does: a local disk mounted inside a share is local, a Cloud account mounted in the home folder is remote.
export function remoteOf(path, table) {
  let best = -1, remote = false;
  for (const m of table || []) {
    const p = m.path;
    const inside = p === '/' || path === p || path.startsWith(`${p}/`);
    if (inside && p.length > best) { best = p.length; remote = !!m.remote; }
  }
  return remote;
}
