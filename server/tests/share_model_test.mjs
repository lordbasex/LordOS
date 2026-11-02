// share_model_test.mjs — sdk/sharemodel.js: what the screen-sharing prompt asks, and for which app.
import assert from 'node:assert/strict';
import { shareAppId, shareAskList, staleGrants } from '../../sdk/sharemodel.js';

assert.equal(shareAppId('desktop:go-link'), 'go-link');
assert.equal(shareAppId('lpk:a.b'), 'a.b');
assert.equal(shareAppId(null), '');
assert.deepEqual(shareAskList({ ask: ['remote-input', 'screen-share'] }), ['screen-share', 'remote-input']);
assert.deepEqual(shareAskList({ ask: ['root', 'screen-share'] }), ['screen-share']);
assert.deepEqual(shareAskList({ ask: 'screen-share' }), []);
assert.deepEqual(shareAskList({}), []);
assert.deepEqual(shareAskList(null), []);
const g = [{ id: 'lpk:old' }, { id: 'lpk:old' }, { id: 'lpk:here' }, { id: 'desktop:gone' }, { id: 'desktop:app' }];
assert.deepEqual(staleGrants(g, new Set(['here']), new Set(['app'])), ['lpk:old', 'desktop:gone']);
assert.deepEqual(staleGrants(g, new Set(), null), ['lpk:old', 'lpk:here']);          // no package left: none of theirs stays
assert.deepEqual(staleGrants(g, null, null), []);                                     // lists that couldn't be read: nothing
assert.deepEqual(staleGrants(null, new Set(), new Set()), []);
console.log('share-model-test: ok');
