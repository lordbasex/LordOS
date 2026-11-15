// diskplaces_test.mjs — what Files' sidebar and the Disks app offer to mount or unmount (sdk/diskplaces.js),
// with a system disk that also holds Windows (#122). The helper decides again as root (tests/disk_test.c).
import assert from 'node:assert';
import { unmountedDisks, diskRelease, partMountable, partUnmountable, inMedia } from '../../sdk/diskplaces.js';

const part = (name, o) => ({ name, label: '', size: 1, fstype: 'ntfs', mountpoint: '', inuse: false, reserved: false, ...o });
const sys = {
  name: 'nvme0n1', system: true, removable: false,
  partitions: [
    part('nvme0n1p1', { fstype: 'vfat', mountpoint: '/boot/efi', inuse: true, reserved: true }),
    part('nvme0n1p2', { fstype: 'ext4', mountpoint: '/', inuse: true }),
    part('nvme0n1p3', { fstype: 'swap', inuse: true }),
    part('nvme0n1p4', { fstype: '', reserved: true }),                         // Microsoft Reserved
    part('nvme0n1p5', { label: 'Windows' }),                                   // C:
    part('nvme0n1p6', { reserved: true }),                                     // Windows Recovery
    part('nvme0n1p7', { fstype: 'crypto_LUKS', inuse: true }),
    part('nvme0n1p8', { fstype: 'ext4', label: 'Data', inuse: true }),       // held: in use without a mount point
    part('nvme0n1p9', { label: 'Games', mountpoint: '/media/ana/Games', inuse: true }),
  ],
};
const usb = { name: 'sdb', system: false, removable: true, partitions: [part('sdb1', { fstype: 'exfat', label: 'STICK' }), part('sdb2', { fstype: 'vfat', reserved: true })] };
const hdd = { name: 'sdc', system: false, removable: false, partitions: [part('sdc1', { fstype: 'ext4', mountpoint: '/media/ana/Backup', inuse: true })] };
const bare = { name: 'sdd', system: false, removable: true, fstype: 'vfat', label: 'CARD', mountpoint: '', partitions: [] };
const sysBare = { name: 'vda', system: true, removable: false, fstype: 'ext4', mountpoint: '', partitions: [] };
const list = [sys, usb, hdd, bare, sysBare];

// the sidebar: Windows' C: on the system disk, and every other disk as before (another disk's ESP stays offered)
assert.deepEqual(unmountedDisks(list).map((p) => p.name), ['nvme0n1p5', 'sdb1', 'sdb2', 'sdd']);
assert.equal(unmountedDisks(list)[0].removable, false);
assert.equal(unmountedDisks([{ ...sys, removable: true }])[0].removable, false, 'a live stick: still not "removable" (no eject)');

// mount: never the system disk itself, nor one of its partitions that is in use or kept by the table
assert.equal(partMountable(sys, sys), false);
assert.equal(partMountable(sysBare, sysBare), false);
for (const n of ['nvme0n1p1', 'nvme0n1p2', 'nvme0n1p3', 'nvme0n1p4', 'nvme0n1p6', 'nvme0n1p7', 'nvme0n1p8', 'nvme0n1p9']) {
  assert.equal(partMountable(sys, sys.partitions.find((p) => p.name === n)), false, n);
}
assert.equal(partMountable(sys, sys.partitions[4]), true);
assert.equal(partMountable(usb, usb.partitions[0]), true);
assert.equal(partMountable(bare, bare), true);
assert.equal(partMountable(null, null), false);

// unmount: a system disk's partition only under /media; other disks as before
assert.equal(partUnmountable(sys, sys.partitions[8]), true);
assert.equal(partUnmountable(sys, sys.partitions[0]), false, '/boot/efi');
assert.equal(partUnmountable(sys, sys.partitions[1]), false, '/');
assert.equal(partUnmountable(sys, sys.partitions[4]), false, 'not mounted');
assert.equal(partUnmountable(hdd, hdd.partitions[0]), true);
const sysBareMounted = { ...sysBare, mountpoint: '/media/ana/x' };
assert.equal(partUnmountable(sysBareMounted, sysBareMounted), false, 'the system disk itself never');

// ⏏ in Files: a system disk's partition is unmounted, never ejected; / and /boot/efi have none
assert.deepEqual(diskRelease(list, '/media/ana/Games'), { action: 'disk-unmount', name: 'nvme0n1p9' });
assert.equal(diskRelease(list, '/'), null);
assert.equal(diskRelease(list, '/boot/efi'), null);
assert.deepEqual(diskRelease([{ ...sys, removable: true }], '/media/ana/Games'), { action: 'disk-unmount', name: 'nvme0n1p9' });
assert.deepEqual(diskRelease(list, '/media/ana/Backup'), { action: 'disk-unmount', name: 'sdc1' });
assert.deepEqual(diskRelease([{ ...usb, partitions: [part('sdb1', { mountpoint: '/media/ana/STICK' })] }], '/media/ana/STICK'), { action: 'disk-eject', name: 'sdb' });
assert.equal(diskRelease(list, '/nowhere'), null);

assert.equal(inMedia('/media/ana/x'), true);
assert.equal(inMedia('/run/media/ana/x'), true);
assert.equal(inMedia('/mediax'), false);
assert.equal(inMedia(undefined), false);

console.log('diskplaces-test: ok');
