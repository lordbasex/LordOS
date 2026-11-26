// theme_keys_model_test.mjs — when a community theme's key listeners hear a key (sdk/themekeysmodel.js): never while
// a native LordOS window, a text field, LordOS's search, a sheet or a system prompt takes it, held keyboard or not.
import assert from 'node:assert/strict';
import { themeHearsKeys as k } from '../../sdk/themekeysmodel.js';
const base = { layerOpen: false, textFocused: false, focusedApp: null, holds: 0 };
assert.equal(k(base), true, 'nothing in front: the desktop is the theme\'s');
assert.equal(k({ ...base, holds: 1, focusedApp: 'terminal' }), false, 'held keyboard, the Terminal in front');
assert.equal(k({ ...base, holds: 1, focusedApp: 'settings' }), false, 'held keyboard, Settings in front');
assert.equal(k({ ...base, holds: 1, textFocused: true }), false, 'held keyboard, a focused native TextInput');
assert.equal(k({ ...base, holds: 1, layerOpen: true }), false, 'held keyboard, LordOS\'s search or a sheet open');
assert.equal(k({ ...base, layerOpen: true }), false, 'a system prompt');
assert.equal(k({ ...base, holds: 1, focusedApp: 'linux' }), true, 'its menu held over a Linux app');
assert.equal(k({ ...base, holds: 0, focusedApp: 'linux' }), false, 'a Linux app in front, not held');
assert.equal(k({ ...base, holds: 1, focusedApp: 'linux', textFocused: true }), false);
console.log('theme-keys-model-test: 9 ok');
