// cloudcheck.js — Check This Account (docs/CLOUD.md 19): one account's files checked end to end through its folder
// in ~/Cloud with lord.fs and cloud.* only, and the report people can send to mark a provider verified. Pure: the
// runner takes what it uses (env) so the tests run it against stand-ins (server/tests/cloud_model_test.mjs).
//
//   runCheck(account, env, onStep) → Promise<[{ id, result: 'pass'|'fail'|'skipped', ms, note }]>
//   buildReport({ version, arch, rclone, kind, label, verified, ownApp, aboutFields, linksSupported, steps }) → text
//   issueUrl({ label, report, version }) → the GitHub issue form's address, under ISSUE_URL_MAX characters
//
// The report never carries anything personal: no account or file names but the check's own, no email, no paths,
// no links, no tokens. A note is a fixed word, never an error's text (which can hold a path).

export const CHECK_STEPS = ['about', 'list', 'folder', 'write', 'up', 'free', 'read', 'link', 'delete', 'deleteUp'];
export const CHECK_SIZE = 1024 * 1024;
const UP_TIMEOUT = 120e3, FREE_TIMEOUT = 40e3, DELETE_TIMEOUT = 60e3, ABOUT_TIMEOUT = 6e3, JOB_TIMEOUT = 60e3;

// "random" bytes: xorshift from a seed (the check needs bytes the cloud can't have seen, not secrecy)
export function randomBytes(n, seed) {
  let x = (seed >>> 0) || 0x9e3779b9;
  const out = new Uint8Array(n);
  for (let i = 0; i < n; i++) { x ^= x << 13; x ^= x >>> 17; x ^= x << 5; out[i] = x & 255; }
  return out;
}
// a 64-bit hash (two FNV-1a 32 with different seeds), as hex
export function hashBytes(bytes) {
  const b = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  let h1 = 0x811c9dc5, h2 = 0xcbf29ce4;
  for (let i = 0; i < b.length; i++) { h1 = Math.imul(h1 ^ b[i], 0x01000193) >>> 0; h2 = Math.imul(h2 ^ b[i] ^ 0x5a, 0x01000193) >>> 0; }
  return `${h1.toString(16).padStart(8, '0')}${h2.toString(16).padStart(8, '0')}:${b.length}`;
}

// Where the check folder goes: the account's root, but in object storage the root lists buckets, so inside the first
// one (a new bucket only when there is none: a valid bucket name, no dot first).
export function checkFolder(acc, rootEntries, tag) {
  const dirs = (rootEntries || []).filter((e) => e.dir && !String(e.name).startsWith('.')).map((e) => e.name).sort();
  if (acc.group === 'object') return dirs.length ? { path: `${acc.mount}/${dirs[0]}/.lordos-check-${tag}`, bucket: false } : { path: `${acc.mount}/lordos-check-${tag}`, bucket: true };
  return { path: `${acc.mount}/.lordos-check-${tag}`, bucket: false };
}

const code = (e) => { const c = e && (e.code || (typeof e === 'string' ? e : '')); return /^[a-z][A-Za-z]*\.[A-Za-z.]+$/.test(String(c || '')) ? c : 'error'; };

export async function runCheck(acc, env, onStep = () => {}) {
  const { fs, cloud, sleep, now } = env;
  const steps = CHECK_STEPS.map((id) => ({ id, result: null, ms: 0, note: '' }));
  const set = (id, result, ms, note = '') => { const s = steps.find((x) => x.id === id); Object.assign(s, { result, ms, note }); onStep(steps.map((x) => ({ ...x }))); };
  const account = () => (cloud.state().accounts || []).find((a) => a.id === acc.id) || acc;
  const until = async (fn, ms, every = 500) => { const end = now() + ms; for (;;) { if (await fn()) return true; if (now() >= end) return false; await sleep(every); } };
  const run = async (id, fn) => {
    const t0 = now();
    try { const r = await fn(); set(id, r === 'skipped' || (r && r.skipped) ? 'skipped' : r === false ? 'fail' : 'pass', now() - t0, (r && r.note) || ''); return r !== false; }
    catch (e) { set(id, 'fail', now() - t0, code(e)); return false; }
  };
  const skipRest = (from) => { for (const s of steps.slice(CHECK_STEPS.indexOf(from))) if (!s.result) set(s.id, 'skipped', 0, 'notReached'); };
  const seed = Math.floor(now() % 0xffffffff) ^ 0x5bd1e995;
  const tag = hashBytes(randomBytes(8, seed)).slice(0, 10);
  let root = [];
  let folder = null, file = null, made = false;

  await run('about', async () => {
    cloud.refreshAbout(acc.id);
    const got = await until(() => !!account().about, ABOUT_TIMEOUT);
    return got ? true : { skipped: true, note: 'noSpaceInfo' };
  });
  if (!await run('list', () => { root = fs.list(acc.mount); return Array.isArray(root); })) { skipRest('folder'); return steps; }
  const f = checkFolder(account(), root, tag);
  folder = f.path; file = `${folder}/check.bin`;
  if (!await run('folder', () => { fs.mkdir(folder); made = true; return !!fs.stat(folder).dir; })) { skipRest('write'); return steps; }
  const bytes = randomBytes(CHECK_SIZE, seed ^ 0x1234567);
  const want = hashBytes(bytes);
  let wrote = await run('write', () => { fs.write(file, bytes.buffer); return fs.stat(file).size === CHECK_SIZE; });
  if (wrote) {
    const upOk = await run('up', async () => {
      const seen = { up: false };
      const ok = await until(() => {
        const s = (cloud.status([file]) || {})[file];
        if (s && s.st === 'up') seen.up = true;
        if (s && s.st === 'error') throw Object.assign(new Error(''), { code: s.error || 'cloud.transferFailed' });
        return !!s && (s.st === 'here' || s.st === 'kept');
      }, UP_TIMEOUT, 1000);
      return ok ? { note: seen.up ? '' : 'upNotSeen' } : (() => { throw Object.assign(new Error(''), { code: 'cloud.checkTimeout' }); })();
    });
    if (upOk) {
      await run('free', async () => {
        let last = null;
        // a folder kept on this computer (Keep on This Computer on the account, #71) is never freed: that is what the
        // person asked for, so the step doesn't apply (it failed with cloud.notFreed.kept after 40 s until 0.8.8.7)
        const kept = () => (((cloud.status([file]) || {})[file] || {}).st === 'kept') || ((last && last.skipped) || []).some((s) => s && s.reason === 'kept');
        if (kept()) return { skipped: true, note: 'kept' };
        // rclone holds a file open for a few seconds after the last write: "open" is tried again
        const ok = await until(() => { last = cloud.free([file]) || {}; return last.freed > 0 || ((cloud.status([file]) || {})[file] || {}).st === 'cloud' || kept(); }, FREE_TIMEOUT, 2000);
        if (ok && !(last && last.freed > 0) && kept()) return { skipped: true, note: 'kept' };
        if (!ok) throw Object.assign(new Error(''), { code: `cloud.notFreed.${((last && last.skipped && last.skipped[0]) || {}).reason || 'unknown'}` });
        return true;
      });
      await run('read', () => {
        const got = hashBytes(new Uint8Array(fs.read(file, 'binary')));
        if (got !== want) throw Object.assign(new Error(''), { code: 'cloud.checkMismatch' });
        return true;
      });
      await run('link', async () => {
        if (!account().linksSupported) return { skipped: true, note: 'noLinks' };
        const r = await cloud.link(file, '1d');
        if (r && r.error === 'unsupported') return { skipped: true, note: 'noLinks' };
        if (!r || !r.url) throw Object.assign(new Error(''), { code: r && r.error });
        await cloud.unlink(file);
        return true;
      });
    } else skipRest('free');
  } else skipRest('up');
  // always clean up what the check made (lord.fs has no delete: the folder moves out of the cloud into a scratch
  // folder of the home, then to the Trash and out of it)
  if (made) {
    const okDel = await run('delete', async () => {
      const scratch = `${fs.home()}/.cache/lordos-cloud-check-${tag}`;
      const job = fs.move(folder, scratch);
      const done = await until(() => !(fs.jobs() || []).some((j) => j.id === job && !j.done && !j.failed), JOB_TIMEOUT);
      const j = (fs.jobs() || []).find((x) => x.id === job);
      if (j) fs.forgetJob(job);
      if (!done || (j && j.failed)) throw Object.assign(new Error(''), { code: 'cloud.checkDeleteFailed' });
      try {
        fs.trash(scratch);
        const t = (fs.trashList() || []).find((x) => x.original === scratch);
        if (t) fs.removeFromTrash(t.id);
      } catch (e) { /* the scratch copy is only in the home */ }
      let gone = false;
      try { fs.stat(folder); } catch (e) { gone = true; }
      return gone;
    });
    // gone from a fresh listing of its parent, and nothing of it waiting to go up (another file of a busy account may)
    const parent = folder.slice(0, folder.lastIndexOf('/')), leaf = folder.slice(folder.lastIndexOf('/') + 1);
    if (okDel) {
      await run('deleteUp', async () => (await until(() => {
        const s = (cloud.status([folder]) || {})[folder];
        return !(fs.list(parent) || []).some((e) => e.name === leaf) && !(s && s.st === 'up');
      }, DELETE_TIMEOUT, 1000)) || (() => { throw Object.assign(new Error(''), { code: 'cloud.checkTimeout' }); })());
    } else skipRest('deleteUp');
  } else skipRest('delete');
  return steps;
}

// ---- the report ----

export const ABOUT_FIELDS = ['total', 'used', 'free', 'trashed', 'other'];
export const aboutFields = (about) => ABOUT_FIELDS.filter((k) => about && about[k] != null);

const pad = (s, n) => (String(s) + ' '.repeat(n)).slice(0, Math.max(n, String(s).length));
// A step skipped for one of these is not a failure (#81: "link skipped noLinks" read as an error). The screen says it
// in the person's words (cloud.check.note.<note>); the report, which goes to GitHub in English, says it after the word.
export const SKIP_NOTES = ['noSpaceInfo', 'noLinks', 'notReached', 'kept'];
const REPORT_NOTE = { noSpaceInfo: 'the provider gives no space information, not a failure', noLinks: 'the provider has no links, not a failure',
  notReached: 'not run: an earlier step failed', kept: 'the account keeps its files on this computer, so nothing is freed; not a failure' };
// facts: only these fields are read; anything else passed in (the account's name, its folder, an email…) is ignored
export function buildReport(facts) {
  const f = facts || {};
  const yn = (v) => (v ? 'yes' : 'no');
  const steps = (f.steps || []).filter((s) => CHECK_STEPS.includes(s.id));
  const n = (r) => steps.filter((s) => s.result === r).length;
  const word = (w) => (/^[a-z][A-Za-z.]*$/.test(String(w || '')) ? String(w) : '');
  return [
    'LordOS Cloud provider check',
    `LordOS: ${word(f.version) || String(f.version || '').replace(/[^\w. ·()+-]/g, '').slice(0, 40)} (${String(f.arch || '').replace(/[^\w-]/g, '').slice(0, 16)})`,
    `rclone: ${String(f.rclone || '').replace(/[^\w.-]/g, '').slice(0, 24)}`,
    `Provider: ${String(f.label || '').replace(/[\r\n]/g, ' ').slice(0, 60)} (${String(f.kind || '').replace(/[^\w-]/g, '').slice(0, 24)})`,
    `Verified on LordOS: ${yn(f.verified)}`,
    `Own app: ${yn(f.ownApp)}`,
    `Space info: ${(f.aboutFields || []).filter((k) => ABOUT_FIELDS.includes(k)).join(', ') || 'none'}`,
    `Links supported: ${yn(f.linksSupported)}`,
    'Steps:',
    ...steps.map((s) => `  ${pad(s.id, 9)} ${pad(s.result || 'skipped', 8)} ${pad(`${Math.max(0, Math.round(s.ms || 0))} ms`, 10)}${word(s.note) ? ` ${word(s.note)}` : ''}${s.result === 'skipped' && REPORT_NOTE[s.note] ? ` (${REPORT_NOTE[s.note]})` : ''}`.trimEnd()),
    `Result: ${n('pass')} passed, ${n('fail')} failed, ${n('skipped') + steps.filter((s) => !s.result).length} skipped`,
  ].join('\n');
}
export const checkWorks = (steps) => {
  const fails = (steps || []).filter((s) => s.result === 'fail').length;
  return fails === 0 ? 'yes' : fails >= 3 ? 'no' : 'partly';
};

// ---- GitHub ----

export const ISSUE_BASE = 'https://github.com/lordbasex/LordOS/issues/new';
// The browser gets the address through lord.apps.launchUri, which takes at most 3000 characters for a web address
// (APPS_URI_WEB_MAX in server/src/apps.h) and none of ! ' ( ) * (encodeURIComponent leaves those as they are, and the
// report has "(x86_64)"): until #79 the button did nothing, the address was refused.
export const ISSUE_URL_MAX = 3000;
const TRUNC = '\n[truncated]';
export const encodeQuery = (v) => encodeURIComponent(v).replace(/[!'()*]/g, (c) => `%${c.charCodeAt(0).toString(16).toUpperCase()}`);
export function issueUrl({ label, report = '', version = '' } = {}) {
  const lab = String(label || '').slice(0, 80);
  const q = (k, v) => `&${k}=${encodeQuery(v)}`;
  const head = `${ISSUE_BASE}?template=cloud-provider.yml${q('title', `Cloud provider: ${lab}`)}${q('provider', lab)}${version ? q('version', String(version).slice(0, 40)) : ''}`;
  if (!report) return head;
  let r = String(report);
  let url = head + q('report', r);
  if (url.length <= ISSUE_URL_MAX) return url;
  // cut whole lines from the end until it fits, then say so
  const lines = r.split('\n');
  while (lines.length > 1) {
    lines.pop();
    r = lines.join('\n') + TRUNC;
    url = head + q('report', r);
    if (url.length <= ISSUE_URL_MAX) return url;
  }
  let s = lines[0] || '';
  while (s.length && (head + q('report', s + TRUNC)).length > ISSUE_URL_MAX) s = s.slice(0, -50);
  return head + q('report', s + TRUNC);
}
