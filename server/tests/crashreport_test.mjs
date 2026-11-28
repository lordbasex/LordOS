// node tests/crashreport_test.mjs — the text an app's crash notice copies for an issue (sdk/crashreport.js)
import assert from 'node:assert/strict';
import { crashReport } from '../../sdk/crashreport.js';

const err = new Error('Minified React error #300');
err.stack = 'Error: Minified React error #300\n    at Kf (/usr/share/lordos/theme.bundle.js:8:120)\n    at /home/alice/x.js:1:2';
const r = crashReport({ app: 'machines', name: 'Machines', version: '0.8.9.6.2-beta', theme: 'default', error: err,
  componentStack: '\n    at Assistant\n    at AppBoundary\n' });
assert.match(r, /^App: Machines \(machines\)\nLordOS: 0\.8\.9\.6\.2-beta\nTheme: default\nError: Minified React error #300\n/);
assert.match(r, /Stack:\n {2}at Kf \(\/usr\/share\/lordos\/theme\.bundle\.js:8:120\)/);
assert.ok(!r.includes('alice'), 'home folder names are hidden');
assert.ok(r.includes('~/x.js:1:2'));
assert.match(r, /Components:\n {2}at Assistant\n {2}at AppBoundary$/);
assert.equal(r.split('\n').filter((l) => l.startsWith('Error:')).length, 1, 'the message is not repeated from the stack');

// a thrown string, no stack, no version
assert.equal(crashReport({ app: 'notes', error: 'boom' }), 'App: notes\nLordOS: unknown\nError: boom');
assert.equal(crashReport({ app: 'x' }), 'App: x\nLordOS: unknown\nError: unknown error');

// long stacks are cut
const long = new Error('e'); long.stack = Array.from({ length: 40 }, (_, i) => `at f${i}`).join('\n');
assert.equal(crashReport({ app: 'a', error: long }).split('\n').filter((l) => l.startsWith('  at f')).length, 12);
console.log('crashreport: ok');
