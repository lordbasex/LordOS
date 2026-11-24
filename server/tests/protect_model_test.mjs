// protect_model_test.mjs — protected windows' flag (sdk/protectmodel.js) and Machines' choice of protected machines
// (apps/machines/model.js machineProtected). docs/PRIVACY.md "Protected windows".
import assert from 'node:assert';
import { protectOnOpen, protectOnReopen, protectPatch, protectLabel, PROTECT_LABEL_MAX } from '../../sdk/protectmodel.js';
import { machineProtected, machineProtectedOf, ANONYMITY_PROFILE } from '../../apps/machines/model.js';

// opened protected: by the option or by the native app's manifest, and locked
assert.deepEqual(protectOnOpen({ protected: true }, null), { protected: true, protectLocked: true });
assert.deepEqual(protectOnOpen({}, { id: 'passwords', protected: true }), { protected: true, protectLocked: true });
assert.equal(protectOnOpen({}, { id: 'notes' }), null);
assert.equal(protectOnOpen(undefined, undefined), null);
// only true itself: a truthy string or number from args isn't the flag
assert.equal(protectOnOpen({ protected: 'yes' }, null), null);
assert.equal(protectOnOpen({ protected: 1 }, { protected: 'true' }), null);

// open again: it can only become protected
assert.deepEqual(protectOnReopen({ id: 'a' }, { protected: true }, null), { protected: true, protectLocked: true });
assert.deepEqual(protectOnReopen({ id: 'a', protected: true }, { protected: true }, null), { protected: true, protectLocked: true });
assert.equal(protectOnReopen({ id: 'a', protected: true, protectLocked: true }, { protected: true }, null), null);
assert.equal(protectOnReopen({ id: 'a', protected: true, protectLocked: true }, {}, null), null);    // never less
assert.equal(protectOnReopen({ id: 'a' }, {}, null), null);

// setProtected: on and off, but a locked window stays protected
assert.deepEqual(protectPatch({ id: 'a' }, true), { protected: true });
assert.deepEqual(protectPatch({ id: 'a', protected: true }, false), { protected: false });
assert.equal(protectPatch({ id: 'a', protected: true }, true), null);
assert.equal(protectPatch({ id: 'a' }, false), null);
assert.equal(protectPatch({ id: 'a', protected: true, protectLocked: true }, false), null);
assert.equal(protectPatch({ id: 'a', protected: true, protectLocked: true }, true), null);
assert.equal(protectPatch(null, true), null);
// lock: on and locked at once, and then for good
assert.deepEqual(protectPatch({ id: 'a' }, true, true), { protected: true, protectLocked: true });
assert.deepEqual(protectPatch({ id: 'a', protected: true }, true, true), { protected: true, protectLocked: true });
assert.equal(protectPatch({ id: 'a', protected: true, protectLocked: true }, true, true), null);
assert.equal(protectPatch({ id: 'a' }, false, true), null);

// the label: printable, at most 95 bytes of UTF-8, cut on a character
assert.equal(protectLabel('Protected window'), 'Protected window');
assert.equal(protectLabel('a\nb\u0001c\u007f'), 'abc');
assert.equal(protectLabel(null), '');
const long = protectLabel('é'.repeat(200));
assert.equal(long, 'é'.repeat(47));                       // 94 bytes: the 48th would make 96
assert.ok(Buffer.byteLength(long) <= PROTECT_LABEL_MAX);
assert.equal(Buffer.byteLength(protectLabel('漢'.repeat(40))), 93);
assert.equal(protectLabel('🔒'.repeat(30)), '🔒'.repeat(23));

// Machines: the anonymity profile (the Whonix members' machine.json profile_lock, or the profile it was made with)
assert.equal(ANONYMITY_PROFILE, 'anonymity');
assert.equal(machineProtected({ profileLock: 'anonymity' }), true);
assert.equal(machineProtected({ meta: { profile: 'anonymity' } }), true);
assert.equal(machineProtected({ group: { kind: 'whonix' } }), true);
assert.equal(machineProtected({ meta: { profile: 'productivity' } }), false);
assert.equal(machineProtected({ profileLock: '' }), false);
assert.equal(machineProtected({}), false);
assert.equal(machineProtected(null), false);

// the shared question (meta.js isProtectedMachine): the list entry and app.json together, each enough alone, and a
// protected home window protects its apps-mode windows whatever the rest says
assert.equal(machineProtectedOf({ dir: '/m', profileLock: 'anonymity' }, {}), true);          // machine.json only
assert.equal(machineProtectedOf({ dir: '/m' }, { profile: 'anonymity' }), true);              // app.json only
assert.equal(machineProtectedOf({ dir: '/m', group: { kind: 'whonix' } }, null), true);       // a group member
assert.equal(machineProtectedOf(null, { profile: 'anonymity' }), true);                       // no list entry
assert.equal(machineProtectedOf({ dir: '/m', meta: { profile: 'anonymity' } }, null), true);  // the entry's own meta
assert.equal(machineProtectedOf({ dir: '/m', profileLock: 'anonymity' }, { profile: 'productivity' }), true);
assert.equal(machineProtectedOf({ dir: '/m' }, { profile: 'productivity' }), false);
assert.equal(machineProtectedOf({ dir: '/m' }, {}, true), true);                              // the home window
assert.equal(machineProtectedOf(null, null), false);
assert.equal(machineProtectedOf('x', null), false);

console.log('protect-model-test: ok');
