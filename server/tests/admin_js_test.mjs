// node tests/admin_js_test.mjs — sdk/admin.js: how the privileged helper's lines settle a request. A PROGRESS line
// (wallpaper-get, studio-bg-get) is progress, never the answer: before 0.8.9.6.2 the first "PROGRESS 0" rejected the
// request with the text "0", so "Download and use" downloaded the picture and never used it.
import assert from 'node:assert/strict';

let next = 0;
globalThis.lord = { admin: { request: () => ++next, available: () => true }, locale: { catalog: () => null, get: () => ({ lang: 'en' }) }, settings: { load: () => null } };
const { admin } = await import('../../sdk/admin.js');
const say = (id, kind, text) => globalThis.__lordAdmin(id, kind, text);

// a download: progress 0 → 17 → 100, then OK with the sha
{
  const seen = [];
  const p = admin.run('wallpaper-get', 'anime-x 2160.jpeg', (l) => seen.push(l), { quiet: true });
  const id = next;
  say(id, 'PROGRESS', '0');
  say(id, 'PROGRESS', '17');
  say(id, 'LOG', 'a log line');
  say(id, 'PROGRESS', '100');
  let settled = false;
  p.then(() => { settled = true; }, () => { settled = true; });
  await new Promise((r) => setTimeout(r, 0));
  assert.equal(settled, false, 'PROGRESS and LOG never settle the request');
  say(id, 'OK', 'abc123');
  assert.equal(await p, 'abc123', 'OK resolves with its text');
  assert.deepEqual(seen, ['PROGRESS 0', 'PROGRESS 17', 'a log line', 'PROGRESS 100'], 'progress reaches onLog as "PROGRESS <n>"');
}

// an error after progress still rejects
{
  const p = admin.run('wallpaper-get', 'x y', null, { quiet: true });
  const id = next;
  say(id, 'PROGRESS', '40');
  say(id, 'ERR', 'wallpaper.downloadFailed');
  await assert.rejects(p, (e) => e.code === 'wallpaper.downloadFailed', 'ERR rejects with its code');
}

// a reply for a request nobody waits for is ignored
say(9999, 'OK', 'x');
console.log('admin-js-test: ok');
