// driver.js — test-only: bundled into themes/default/theme.e2e.bundle.js by build-e2e.mjs, never into a release.
// Lets the bench drive a real headless session's Cloud through the same SDK the interface uses (sdk/cloud.js),
// as the session user, with no new channel into lord-server: the bench (lc.py, as that user) writes
// ~/.e2e/cmd-<n>.js, the body of an async function; this polls for it with lord.fs, runs it with
// { cloud, cloudState, wm, lord, sleep, log } and writes ~/.e2e/out-<n>.json: { ok, value } or { ok: false, error }.
import { cloud, cloudState } from '../../../sdk/cloud.js';
import { wm } from '../../../sdk/windows.js';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const logs = [];
const log = (...a) => { logs.push(a.map((x) => (typeof x === 'string' ? x : JSON.stringify(x))).join(' ')); };
const AsyncFunction = Object.getPrototypeOf(async () => {}).constructor;

function start() {
  if (typeof lord === 'undefined' || !lord.fs) return;
  const dir = `${lord.fs.home()}/.e2e`;
  let n = 1;
  const tick = async () => {
    let src = null;
    try { src = lord.fs.read(`${dir}/cmd-${n}.js`); } catch (e) { src = null; }
    if (src != null) {
      const id = n++;
      let out;
      logs.length = 0;
      try {
        const fn = new AsyncFunction('cloud', 'cloudState', 'wm', 'lord', 'sleep', 'log', src);
        const value = await fn(cloud, cloudState, wm, lord, sleep, log);
        out = { ok: true, value: value === undefined ? null : value, logs: logs.slice() };
      } catch (e) {
        out = { ok: false, error: String((e && (e.code || e.message)) || e), detail: e && e.stack ? String(e.stack) : null, logs: logs.slice() };
      }
      try { lord.fs.write(`${dir}/out-${id}.json`, JSON.stringify(out)); } catch (e) { /* the bench times out */ }
    }
    setTimeout(tick, 200);
  };
  setTimeout(tick, 500);
}
start();
