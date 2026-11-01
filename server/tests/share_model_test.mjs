// share_model_test.mjs — sdk/sharemodel.js: what the screen-sharing prompt asks, and for which app.
import assert from 'node:assert/strict';
import { shareAppId, shareAskList } from '../../sdk/sharemodel.js';

assert.equal(shareAppId('desktop:go-link'), 'go-link');
assert.equal(shareAppId('lpk:a.b'), 'a.b');
assert.equal(shareAppId(null), '');
assert.deepEqual(shareAskList({ ask: ['remote-input', 'screen-share'] }), ['screen-share', 'remote-input']);
assert.deepEqual(shareAskList({ ask: ['root', 'screen-share'] }), ['screen-share']);
assert.deepEqual(shareAskList({ ask: 'screen-share' }), []);
assert.deepEqual(shareAskList({}), []);
assert.deepEqual(shareAskList(null), []);
console.log('share-model-test: ok');
