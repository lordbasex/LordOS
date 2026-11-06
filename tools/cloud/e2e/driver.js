// driver.js — test-only: bundled into themes/default/theme.e2e.bundle.js by build-e2e.mjs, never into a release.
// Lets the bench drive a real headless session's Cloud through the same SDK the interface uses (sdk/cloud.js),
// as the session user, with no new channel into lord-server: the bench (lc.py, as that user) writes
// ~/.e2e/cmd-<n>.js, the body of an async function; this polls for it with lord.fs, runs it with
// { cloud, cloudState, wm, lord, sleep, log } and writes ~/.e2e/out-<n>.json: { ok, value } or { ok: false, error }.
import { cloud, cloudState } from '../../../sdk/cloud.js';
import { wm } from '../../../sdk/windows.js';
import * as cloudcheck from '../../../sdk/cloudcheck.js';

// Check This Account exactly as Settings runs it (apps/settings/Cloud.jsx: checkFs and env)
globalThis.__e2eCheck = cloudcheck;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const AsyncFunction = Object.getPrototypeOf(async () => {}).constructor;

// every reply lord-server sends (req, result), so the bench sees answers even when a call's { req } was lost
const replies = [];
function hookReplies() {
  const prev = globalThis.__lordCloudReply;
  globalThis.__lordCloudReply = (req, res) => { replies.push({ req, res, at: Date.now() }); if (replies.length > 200) replies.shift(); if (prev) prev(req, res); };
}

function start() {
  hookReplies();
  if (typeof lord === 'undefined' || !lord.fs) return;
  const dir = `${lord.fs.home()}/.e2e`;
  let n = 1;
  // commands run concurrently (one waiting for a Promise doesn't hold the next), each writes its own answer
  const runOne = async (id, src) => {
    const mine = [];
    const log = (...a) => { mine.push(a.map((x) => (typeof x === 'string' ? x : JSON.stringify(x))).join(' ')); };
    let out;
    try {
      const fn = new AsyncFunction('cloud', 'cloudState', 'wm', 'lord', 'sleep', 'log', 'replies', src);
      const value = await fn(cloud, cloudState, wm, lord, sleep, log, replies);
      out = { ok: true, value: value === undefined ? null : value, logs: mine };
    } catch (e) {
      out = { ok: false, error: String((e && (e.code || e.message)) || e), detail: e && e.stack ? String(e.stack) : null, logs: mine };
    }
    try { lord.fs.write(`${dir}/out-${id}.json`, JSON.stringify(out)); } catch (e) { /* the bench times out */ }
  };
  const tick = () => {
    for (;;) {
      let src = null;
      try { src = lord.fs.read(`${dir}/cmd-${n}.js`); } catch (e) { src = null; }
      if (src == null) break;
      runOne(n++, src);
    }
    setTimeout(tick, 200);
  };
  setTimeout(tick, 500);
}
start();
