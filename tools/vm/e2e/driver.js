// driver.js — test-only: bundled into themes/default/theme.vm-e2e.bundle.js by build-e2e.mjs, never into a release.
// Lets the bench (lc.py, as the session user) run a command in a real headless session through the same SDK the
// interface uses, with no new channel into lord-server (the Cloud bench's approach): it writes ~/.e2e/cmd-<n>.js, the
// body of an async function; this polls for it with lord.fs, runs it with { lord, wm, admin, launchApp, sleep, log }
// and writes ~/.e2e/out-<n>.json: { ok, value } or { ok: false, error }.
import { windows } from '../../../sdk/windows.js';
import { launchApp } from '../../../sdk/apps.js';
import { admin } from '../../../sdk/admin.js';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const AsyncFunction = Object.getPrototypeOf(async () => {}).constructor;
const wm = { windows: () => windows.get().map((w) => ({ id: w.id, app: w.appId || w.app, title: w.title, x: w.x, y: w.y, w: w.w, h: w.h, minimized: w.minimized })) };

function start() {
  if (typeof lord === 'undefined' || !lord.fs) return;
  const dir = `${lord.fs.home()}/.e2e`;
  let n = 1;
  const runOne = async (id, src) => {
    const mine = [];
    const log = (...a) => { mine.push(a.map((x) => (typeof x === 'string' ? x : JSON.stringify(x))).join(' ')); };
    let out;
    try {
      const fn = new AsyncFunction('lord', 'wm', 'admin', 'launchApp', 'sleep', 'log', src);
      const value = await fn(lord, wm, admin, launchApp, sleep, log);
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
