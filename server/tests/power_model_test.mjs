// power_model_test.mjs — what Restart… and Shut Down…'s countdown does when it runs out (sdk/powermodel.js): LordOS's
// own confirms; one a community theme started is cancelled (it can't end the session as if the person had).
import assert from 'node:assert/strict';
import { countdownEnds } from '../../sdk/powermodel.js';
assert.equal(countdownEnds({ action: 'reboot', state: 'ask' }), 'confirm');
assert.equal(countdownEnds({ action: 'poweroff', state: 'ask', byTheme: false }), 'confirm');
assert.equal(countdownEnds({ action: 'reboot', state: 'ask', byTheme: true }), 'cancel');
assert.equal(countdownEnds({ action: 'poweroff', state: 'ask', byTheme: true }), 'cancel');
assert.equal(countdownEnds({ action: 'reboot', state: 'ask', byAgent: true }), 'cancel');   // an AI agent's (Hands) never confirms itself
assert.equal(countdownEnds({ action: 'reboot', state: 'sent', byTheme: true }), 'none');
assert.equal(countdownEnds(null), 'none');
console.log('power-model-test: 6 ok');
