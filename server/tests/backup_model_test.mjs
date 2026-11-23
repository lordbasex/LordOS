// Backups' exclusions from the file dialog or typed (#200), from server/: node tests/backup_model_test.mjs
import assert from 'node:assert/strict';
import { exclusionFor, addExclusion } from '../../apps/backup/model.js';
import { refuseCloud } from '../../sdk/filepickermodel.js';

const H = '/home/u';
// the dialog gives absolute paths: inside the home folder they become relative to it
assert.deepEqual(exclusionFor('/home/u/Videos/Large', H), { rel: 'Videos/Large' });
assert.deepEqual(exclusionFor('/home/u/Videos/', `${H}/`), { rel: 'Videos' });
// typed: "~/…" or relative, extra slashes dropped
assert.deepEqual(exclusionFor('~/Music', H), { rel: 'Music' });
assert.deepEqual(exclusionFor('  Videos//Large/ ', H), { rel: 'Videos/Large' });
assert.deepEqual(exclusionFor('.cache/x', H), { rel: '.cache/x' });
// nothing typed
assert.equal(exclusionFor('', H), null);
assert.equal(exclusionFor('   ', H), null);
assert.equal(exclusionFor(undefined, H), null);
// outside the backed-up folder: another user's home, a disk, a look-alike prefix, "." or ".."
for (const p of ['/home/u2/Videos', '/media/u/Disk/x', '/', '/etc', '../u2', 'a/../../b', './Videos', '~/../x', '/home/u/../u2'])
  assert.deepEqual(exclusionFor(p, H), { error: 'backup.excludeOutside' }, p);
assert.deepEqual(exclusionFor('/x', ''), { error: 'backup.excludeOutside' });
// the home folder itself
for (const p of ['/home/u', '/home/u/', '~', '~/'])
  assert.deepEqual(exclusionFor(p, H), { error: 'backup.excludeHome' }, p);

// adding: once, in order; garbage in the saved list dropped
assert.deepEqual(addExclusion(['A'], 'B'), ['A', 'B']);
assert.deepEqual(addExclusion(['A', 'B'], 'A'), ['A', 'B']);
assert.deepEqual(addExclusion(undefined, 'A'), ['A']);
assert.deepEqual(addExclusion(['A', 3, ''], ''), ['A']);

// the destination dialog (cloud={false}): a Cloud account's folder, or one in it, is never chosen (#208)
assert.equal(refuseCloud({ pick: '/home/u/Cloud/Drive' }, H), null);
assert.equal(refuseCloud({ pick: '/home/u/Cloud/Drive/Backups' }, H), null);
assert.deepEqual(refuseCloud({ pick: '/home/u/Cloud' }, H), { pick: '/home/u/Cloud' });
assert.deepEqual(refuseCloud({ pick: '/media/u/Disk' }, H), { pick: '/media/u/Disk' });
assert.deepEqual(refuseCloud({ go: '/home/u/Cloud/Drive' }, H), { go: '/home/u/Cloud/Drive' });
assert.equal(refuseCloud(null, H), null);

console.log('backup-model-test: ok');
