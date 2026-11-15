// diskplaces.js — which disks and partitions Files and Disks offer to mount or unmount (pure logic over
// lord.disks.list(); tests: server/tests/diskplaces_test.mjs). The helper decides again as root
// (disk-mount, disk-unmount: server/src/disk/disk_admin.c and diskuse.c); this only keeps the UI from
// offering what it would refuse.
//
// Since #122 a partition of the system disk can be mounted too, by an administrator, when it is free:
// not mounted anywhere, not held by LUKS/LVM/RAID, not an active swap (`inuse`), and not a table entry the
// system keeps for itself, such as the EFI partition, BIOS boot, Microsoft Reserved or Windows Recovery
// (`reserved`). The system disk itself is never formatted, erased or ejected.

// file systems that are never mounted as such
export const NO_MOUNT = ['', 'swap', 'crypto_LUKS', 'LVM2_member', 'linux_raid_member'];

export const inMedia = (path) => typeof path === 'string' && (path.startsWith('/media/') || path.startsWith('/run/media/'));

// The partitions of a disk, or the disk itself when it holds a file system without a table.
const partsOf = (d) => (d.partitions?.length ? d.partitions : [d]);

// Whether part (a partition of disk, or disk itself) can be offered for disk-mount.
export function partMountable(disk, part) {
  if (!disk || !part || part.mountpoint || NO_MOUNT.includes(part.fstype || '')) return false;
  if (!disk.system) return true;
  // the system disk: only one of its partitions, free and not kept by its table
  return part !== disk && !part.inuse && !part.reserved;
}

// Whether disk-unmount can be offered for part: a system disk's partition only when it is mounted under
// /media (what disk-mount made; the helper refuses anything mounted elsewhere).
export function partUnmountable(disk, part) {
  if (!disk || !part || !part.mountpoint) return false;
  return !disk.system || (part !== disk && inMedia(part.mountpoint));
}

// Files' sidebar: every partition that can be mounted, on any disk.
export function unmountedDisks(list) {
  const out = [];
  for (const d of list || []) {
    for (const p of partsOf(d)) {
      if (partMountable(d, p)) out.push({ name: p.name, label: p.label, size: p.size, removable: !!d.removable && !d.system, fstype: p.fstype });
    }
  }
  return out;
}

// What ⏏ does for a volume mounted at path: a removable disk is ejected whole (disk-eject), another disk's
// partition, or a system disk's partition mounted under /media, is unmounted (disk-unmount). null: none.
export function diskRelease(list, path) {
  for (const d of list || []) {
    const p = partsOf(d).find((x) => x.mountpoint === path);
    if (!p) continue;
    if (d.system) return partUnmountable(d, p) ? { action: 'disk-unmount', name: p.name } : null;
    return d.removable ? { action: 'disk-eject', name: d.name } : { action: 'disk-unmount', name: p.name };
  }
  return null;
}
