// netplaces.js — what Files → Network lists, no screen (issue #59). First the places this person is connected to: shares
// and remote folders mounted now, servers kept in Locations (connected again only when clicked, so nothing reaches the
// network when Files starts) and the Cloud accounts; then the machines found on the network that offer a way in. A
// Wi-Fi repeater or a printer has nothing to open here, so it is left out unless every device is asked for.

// Every way into a computer, best first (network.jsx, hostWays). What mounts comes before a remote screen.
export const WAY_ORDER = { smb: 0, nfs: 1, sftp: 2, ssh: 3, webdavs: 4, webdav: 5, ftps: 6, ftp: 7, screen: 8, rdp: 9 };
// file sharing that is announced but can't be opened from here is still worth seeing
const SHARING = ['afp'];

const SHARE_FS = ['cifs', 'smb3', 'nfs', 'nfs4', 'fuse.rclone'];
export const isShareFs = (fs) => SHARE_FS.includes(fs);
const isCloudPath = (path, home) => !!home && String(path).startsWith(`${home}/Cloud/`);

// a machine with something to open: shared folders, exports, a folder over sftp, WebDAV or FTP, or a remote screen
export function offersWay(h) {
  if (!h) return false;
  if (h.smb || h.nfs || (h.shares && h.shares.length) || (h.exports && h.exports.length)) return true;
  return (h.services || []).some((s) => WAY_ORDER[s.kind] !== undefined || SHARING.includes(s.kind));
}

// { shown, hidden }: the machines to list, and how many were left out
export function foundHosts(hosts, all = false) {
  const list = (hosts || []).filter((h) => h && h.addr);
  const shown = all ? list : list.filter(offersWay);
  return { shown, hidden: list.length - shown.length };
}

// The connected places, as entries of the Network page. Their path isn't the folder's own (`place:`), so nothing that
// works on files (trash, rename, drag, copy) can reach a mount point from here; opening one goes to its folder.
//   volumes: fs.volumes(); pins: Files' kept servers ({ address, path, name }); accounts: Cloud's ({ id, name, mount, state })
export function connectedEntries({ volumes = [], pins = [], accounts = [], hiddenAccounts = [], home = '' } = {}) {
  const out = [];
  const seen = new Set();
  for (const v of volumes || []) {
    if (!v || !isShareFs(v.fs) || isCloudPath(v.path, home) || seen.has(v.path)) continue;
    seen.add(v.path);
    out.push({ name: v.name, path: `place:${v.path}`, dir: false, size: 0, mtime: 0, netPlace: { kind: 'share', path: v.path, fs: v.fs, name: v.name } });
  }
  for (const p of pins || []) {
    if (!p || !p.path || seen.has(p.path)) continue;
    seen.add(p.path);
    out.push({ name: p.name, path: `place:${p.path}`, dir: false, size: 0, mtime: 0, netPlace: { kind: 'pin', path: p.path, pin: p } });
  }
  for (const a of accounts || []) {
    if (!a || !a.mount || (hiddenAccounts || []).includes(a.id) || seen.has(a.mount)) continue;
    seen.add(a.mount);
    out.push({ name: a.name, path: `place:${a.mount}`, dir: false, size: 0, mtime: 0, netPlace: { kind: 'cloud', path: a.mount, id: a.id, state: a.state } });
  }
  return out;
}

// what a mounted folder is, for its row: smb, nfs or a remote folder (rclone's "<kind> on <host>")
export function shareKind(name, fs) {
  if (fs === 'cifs' || fs === 'smb3') return 'smb';
  if (fs === 'nfs' || fs === 'nfs4') return 'nfs';
  const m = /^(sftp|ftps?|webdav|davs) on /.exec(name || '');
  return m ? m[1] : 'remote';
}

// The page in two groups, connected first, whatever the sort; a single group needs no title.
export function netGroups(sorted, t) {
  const connected = sorted.filter((e) => e.netPlace);
  const found = sorted.filter((e) => !e.netPlace);
  if (!connected.length || !found.length) return [{ key: 'all', label: '', items: sorted }];
  return [{ key: 'connected', label: t('files.net.group.connected'), items: connected }, { key: 'found', label: t('files.net.group.found'), items: found }];
}
