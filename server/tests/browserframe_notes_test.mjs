// Settings → Appearance → "Browsers: tabs in the title bar": the notes under the switch (sdk/browserframe.js,
// browserFrameNotes). #222: a change that worked says nothing; what needs the person still shows.
// node server/tests/browserframe_notes_test.mjs
import { browserFrameNotes } from '../../sdk/browserframe.js';

let ok = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { console.log(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); process.exitCode = 1; } else ok++; };
const keys = (r, p) => browserFrameNotes(r, p).map((n) => n.key);

eq('nothing yet', browserFrameNotes(null, false), []);
eq('a change that worked says nothing (#222)', keys({ chromium: { changed: 2 }, firefox: { changed: 1 } }, false), []);
eq('turned back off: still nothing', keys({ chromium: { changed: 2 }, firefox: { changed: 0 } }, false), []);
eq('no Done text anywhere', browserFrameNotes({ chromium: { changed: 1 } }, false).some((n) => /done/.test(n.text)), false);
eq('a browser open', browserFrameNotes({ chromium: { changed: 0, running: true } }, true), [{ key: 'open', tone: 'warn', text: 'settings.browserTabs.chromiumOpen' }]);
eq('failures counted', browserFrameNotes({ chromium: { failed: 1 }, firefox: { failed: 2 } }, false),
  [{ key: 'failed', tone: 'error', text: 'settings.browserTabs.failed', vars: { count: 3 } }]);
eq('firefox user.js kept', keys({ firefox: { kept: 1, changed: 1 } }, false), ['kept']);
eq('all three', keys({ chromium: { failed: 1 }, firefox: { kept: 1 } }, true), ['open', 'failed', 'kept']);

console.log(process.exitCode ? 'browserframe-notes-test: FAILED' : `browserframe-notes-test: ${ok} checks ok`);
