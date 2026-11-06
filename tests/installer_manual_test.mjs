// The installer's manual partitioning model (apps/installer/manualmodel.js): node tests/installer_manual_test.mjs
import assert from 'node:assert/strict';
import { emptyManual, actionsFor, setAction, addPartition, removeAdded, freeGaps, placeIn, rows, review } from '../apps/installer/manualmodel.js';

const MIB = 2048, GIB = 2097152;               // sectors of 512 bytes
// 64 GiB: 1 EFI (100 MiB), 3 another system (20 GiB, ntfs), 4 Linux (10 GiB, ext4), the rest free
const disk = {
  name: 'sda', lsec: 512, first: 34, last: 64 * GIB - 34, tableTag: 'tag-1',
  parts: [
    { name: 'sda1', num: 1, start: MIB, end: 101 * MIB - 1, esp: true, fs: 'vfat', loaders: ['other'] },
    { name: 'sda3', num: 3, start: 101 * MIB, end: 101 * MIB + 20 * GIB - 1, esp: false, fs: 'ntfs' },
    { name: 'sda4', num: 4, start: 101 * MIB + 20 * GIB, end: 101 * MIB + 30 * GIB - 1, esp: false, fs: 'ext4' },
  ],
};
const FREE0 = 101 * MIB + 30 * GIB;

// the menus
assert.deepEqual(actionsFor(disk.parts[0]), ['keep', 'esp', 'espFormat', 'delete']);
assert.ok(!actionsFor(disk.parts[1]).includes('home'));        // ntfs can't be kept as /home
assert.ok(actionsFor(disk.parts[2]).includes('home'));

// nothing chosen: / and the EFI partition are missing, nothing changes
let m = emptyManual();
let r = review(disk, m, 0);
assert.deepEqual(r.errors, ['install.manual.noRoot', 'install.manual.noEsp']);
assert.equal(r.changes.length, 0);
assert.equal(r.destructive, false);

// the free space at the end, aligned
let gaps = freeGaps(disk, m);
assert.equal(gaps.length, 1);
assert.equal(gaps[0].start, FREE0);
assert.equal((gaps[0].end + 1) % MIB, 0);

// a reinstall: format 4 as /, keep the EFI partition
m = setAction(setAction(m, 4, 'root'), 1, 'esp');
r = review(disk, m, 2048);
assert.deepEqual(r.errors, []);
assert.equal(r.destructive, true);
assert.deepEqual(r.fields, { mode: 'manual', tableTag: 'tag-1', delete: '', create: '', rootPart: 4, homePart: 0, swapPart: 0, espPart: 1, homeFormat: false, espFormat: false });

// one role in one place: / moves to a new partition and leaves 4
const g = freeGaps(disk, m)[0];
const place = placeIn(disk, g, 20 * 1024);
assert.equal(place.start, FREE0);
assert.equal(place.end, FREE0 + 20 * GIB - 1);
m = addPartition(m, 'root', place.start, place.end);
assert.equal(m.acts[4], undefined);
r = review(disk, m, 0);
assert.deepEqual(r.errors, []);
assert.equal(r.fields.create, `root:${FREE0}:${FREE0 + 20 * GIB - 1}`);
assert.equal(r.fields.rootPart, 0);
assert.equal(r.destructive, false);            // only free space is used
// and back: choosing 4 as / removes the new one
assert.equal(setAction(m, 4, 'root').adds.length, 0);

// all of a gap when no size (or too big a size) is asked
assert.equal(placeIn(disk, g, 0).end, g.end);
assert.equal(placeIn(disk, g, 10 ** 9).end, g.end);

// deleting makes room, and a deleted partition's space shows as free
m = setAction(m, 3, 'delete');
gaps = freeGaps(disk, m);
assert.equal(gaps.length, 2);
assert.equal(gaps[0].start, 101 * MIB);
const sw = placeIn(disk, gaps[0], 2048);
m = addPartition(m, 'swap', sw.start, sw.end);
r = review(disk, m, 0);
assert.deepEqual(r.errors, []);
assert.equal(r.fields.delete, '3');
assert.equal(r.fields.create.split(',').length, 2);
assert.ok(r.fields.create.startsWith('swap:'));    // in disk order
assert.equal(r.destructive, true);
assert.deepEqual(r.changes.map((c) => c.kind), ['delete', 'create', 'create', 'reuse']);
// a swap partition and a swap file are not both possible
assert.ok(review(disk, m, 2048).errors.includes('install.manual.twoSwaps'));
// the screen's rows, in disk order: the deleted partition, then its free space
const kinds = rows(disk, m).map((x) => x.kind + (x.p ? x.p.num : ''));
assert.deepEqual(kinds, ['part1', 'part3', 'new', 'free', 'part4', 'new', 'free']);
m = removeAdded(m, 0);
assert.equal(m.adds.length, 1);

// sizes
let s = setAction(setAction(emptyManual(), 1, 'esp'), 4, 'root');
const tiny = placeIn(disk, freeGaps(disk, s)[0], 500);
assert.ok(review(disk, addPartition(s, 'home', tiny.start, tiny.end), 0).errors.includes('install.manual.homeTooSmall'));
assert.ok(review(disk, addPartition(s, 'swap', tiny.start, tiny.start + 100 * MIB - 1), 0).errors.includes('install.manual.swapSize'));
const small = { ...disk, parts: disk.parts.map((p) => (p.num === 4 ? { ...p, end: p.start + 8 * GIB - 2 } : p)) };
assert.ok(review(small, s, 0).errors.includes('install.manual.rootTooSmall'));
s = setAction(emptyManual(), 4, 'root');
const e = placeIn(disk, freeGaps(disk, s)[0], 50);
assert.ok(review(disk, addPartition(s, 'esp', e.start, e.end), 0).errors.includes('install.manual.espTooSmall'));
// formatting the EFI partition is a destructive change of its own, and asked for explicitly
s = setAction(setAction(emptyManual(), 4, 'root'), 1, 'espFormat');
r = review(disk, s, 0);
assert.equal(r.fields.espFormat, true);
assert.ok(r.changes.some((c) => c.kind === 'format' && c.act === 'espFormat'));
// a kept /home vs a formatted one
s = setAction(setAction(setAction(emptyManual(), 3, 'root'), 1, 'esp'), 4, 'home');
r = review(disk, s, 0);
assert.equal(r.fields.homePart, 4);
assert.equal(r.fields.homeFormat, false);
assert.equal(setAction(s, 4, 'homeFormat').acts[4], 'homeFormat');
assert.equal(review(disk, setAction(s, 4, 'homeFormat'), 0).fields.homeFormat, true);
// unknown actions are ignored
assert.equal(setAction(s, 4, 'format-everything'), s);
console.log('installer-manual-test: ok');
