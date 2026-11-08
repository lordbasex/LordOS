// Mail's account passwords and the session keyring (issue #63), from server/: node tests/mail_auth_model_test.mjs
import assert from 'node:assert/strict';
import { keyringTrouble, askUnlock, authAfterStatus } from '../../sdk/mailauthmodel.js';

// the SDK's short keyring codes (sdk/vault.js KR_SHORT, and lord-vault's own on the session side)
assert.equal(keyringTrouble(undefined), null);
assert.equal(keyringTrouble(''), null);
assert.equal(keyringTrouble('locked'), 'locked');           // automatic login: it exists, the prompt opens it
assert.equal(keyringTrouble('no-keyring'), 'missing');      // never created: only a login with the password makes it
for (const c of ['not-found', 'unavailable', 'save', 'bad-request', 'crash', 'error']) assert.equal(keyringTrouble(c), 'error', c);

// the system prompt only for a keyring it can open, and once per session (Cancel is an answer)
assert.equal(askUnlock('locked', false), true);
assert.equal(askUnlock('locked', true), false);
assert.equal(askUnlock('missing', false), false);
assert.equal(askUnlock('error', false), false);
assert.equal(askUnlock(null, false), false);

// the "password needed" mark goes away once the account is connected (the session answered it after
// the keyring opened); other statuses leave it, and an unmarked account keeps the same object
const marked = { work: true, home: true };
assert.deepEqual(authAfterStatus(marked, 'work', 'idle'), { work: false, home: true });
assert.deepEqual(authAfterStatus(marked, 'work', 'sync'), { work: false, home: true });
for (const s of ['connecting', 'auth', 'cert', 'offline']) assert.equal(authAfterStatus(marked, 'work', s), marked, s);
const clean = { work: false };
assert.equal(authAfterStatus(clean, 'work', 'idle'), clean);
assert.equal(authAfterStatus(clean, 'other', 'idle'), clean);
assert.deepEqual(marked, { work: true, home: true });        // never changed in place

console.log('mail-auth-model: ok');
