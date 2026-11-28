// admin_sync_test.mjs — sdk/adminsyncmodel.js: a setting mirrored by a closed administrator action (browser-restore,
// video-optimize) runs it once per new choice, and a failed one isn't tried again on every other settings change for
// RETRY_MS (BUG-10b of the ws170 QA: an agent's clicks in Settings each brought refused requests it never asked for).
//   node tests/admin_sync_test.mjs   (from server/)
import { syncStep, syncDone, SYNC_START, RETRY_MS } from '../../sdk/adminsyncmodel.js';

let n = 0, bad = 0;
const eq = (name, a, b) => { n++; if (JSON.stringify(a) !== JSON.stringify(b)) { bad++; console.log(`✗ ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); } };
let s = SYNC_START;
let r = syncStep(s, 'on', 1000); s = r.state;
eq('a first choice runs', r.run, true);
s = syncDone(s, 'on', true, 1100);
r = syncStep(s, 'on', 2000); s = r.state;
eq('the same choice, applied: nothing', r.run, false);
r = syncStep(s, 'off', 3000); s = r.state;
eq('a new choice runs', r.run, true);
s = syncDone(s, 'off', false, 3100);
const tries = [4000, 9000, 30000, 60000].map((t) => { const x = syncStep(s, 'off', t); s = x.state; return x.run; });
eq('failed: not again on every other settings change for a while', tries, [false, false, false, false]);
r = syncStep(s, 'off', 3100 + RETRY_MS); s = r.state;
eq('failed: tried again after RETRY_MS', r.run, true);
s = syncDone(s, 'off', false, 3100 + RETRY_MS + 10);
r = syncStep(s, 'on', 3100 + RETRY_MS + 20); s = r.state;
eq('another choice right after a failure runs at once', r.run, true);
s = syncDone(s, 'on', true, 3100 + RETRY_MS + 30);
eq('applied after success, the failure forgotten', [s.applied, s.failed], ['on', '']);
eq('a clock gone back never blocks', syncStep({ applied: '', failed: 'x', failedAt: 5000 }, 'x', 1000).run, true);
console.log(`admin-sync-test: ${n} checks, ${bad} failures`);
process.exit(bad ? 1 : 0);
