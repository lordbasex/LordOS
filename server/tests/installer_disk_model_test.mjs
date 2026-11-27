// node tests/installer_disk_model_test.mjs — apps/installer/diskmodel.js, GitHub #280: install-disks' lines. A disk
// line that doesn't parse (the helper used to cut a seven-partition disk's JSON at 1200 bytes) is counted as
// unreadable instead of vanishing silently, so the Disk step can say so instead of "No disk to install on was found".
import assert from 'node:assert/strict';
import { diskCollector, validDisk } from '../../apps/installer/diskmodel.js';

const part = (i) => ({ name: `sda${i}`, fstype: 'ext4', label: '', size: 1e9, num: i, start: 2048, end: 4096, esp: false, fs: 'ext4' });
const disk = (parts) => ({ name: 'sda', path: '/dev/sda', size: 256060514304, model: 'SanDisk', vendor: 'ATA', transport: 'sata',
  removable: false, usable: true, ptable: 'gpt', freeOk: false, manualOk: true, parts });

{
  const c = diskCollector();
  const whole = JSON.stringify(disk([1, 2, 3, 4, 5, 6, 7].map(part)));
  c.add(whole);
  c.add('a progress line, not a disk');
  c.add(whole.slice(0, 1000));                                    // #280: cut by the helper
  c.add(JSON.stringify({ name: 'sdb' }));                          // parses, but isn't a disk
  c.add('{"name":"sdc","path":"/dev/sdc","size":-1,"model":"","vendor":"","transport":"","removable":false,"usable":true,"parts":[]}');
  const { disks, unreadable } = c.result();
  assert.equal(disks.length, 1);
  assert.equal(disks[0].parts.length, 7);
  assert.equal(unreadable, 3);
}

// the helper's stand-in for a disk with more partitions than a line holds: listed, unusable, with its reason
{
  const c = diskCollector();
  c.add('{"name":"sda","path":"/dev/sda","size":100,"model":"m","vendor":"v","transport":"sata","removable":false,"usable":false,'
    + '"freeOk":false,"manualOk":false,"parts":[],"listWhy":"install.diskTooManyParts"}');
  const { disks, unreadable } = c.result();
  assert.equal(unreadable, 0);
  assert.equal(disks[0].listWhy, 'install.diskTooManyParts');
}

assert.equal(validDisk(null), false);
assert.equal(validDisk([]), false);
assert.equal(validDisk({ ...disk([part(1)]), parts: [{ name: 'sda1' }] }), false);
console.log('installer-disk-model-test: ok');
